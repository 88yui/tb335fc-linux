/* isp_lib.c — 单帧 RAW10 40bit LE -> RGB24 视频快路 (2x2 bin, 1632x1224)
 * v2 移植 C-2 细化（outputs/gold/w266_c2_refine.md）：
 *   - AWB v2 估计器：减黑后 8x8 块统计，gray/patch/hybrid + 标定常数（修正旧实现随亮度漂移 9%）
 *   - AWB IIR 时域平滑：init/hold(<45% 有效块)/reset(>30%)/fast(>8%)/iir(alpha)
 *   - tone v2：线性域有理曲线（暗部 lifts + 高光 shoulder，不硬剪）+ 通道/Y 比例重建（避免去饱和）
 *     + 高光去饱和 + 显示域对比度
 * 环境变量：CAMREC_VFAST=1 快路（跳 R/B 平滑、AWB 每 5 帧统计）
 *           CAMREC_ISP_LEGACY=1 退回旧 tone（逐通道 LUT + 旧 AWB），用于 A/B 回归
 *           CAMREC_ISPDBG=1 打印阶段耗时
 *           CAMREC_ISP_AWB=patch|gray|hybrid 覆盖 AWB 模式；CAMREC_ISP_ALPHA 覆盖 IIR 系数
 */
#define _GNU_SOURCE
#include "isp_lib.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#ifdef _OPENMP
#include <omp.h>
#endif

#define W_FULL 3264
#define H_FULL 2448
#define ROW_BYTES 4080
#define MAXV 1023
#define BLKSZ 8

static inline int cvi(int v){ return v<0?0:(v>MAXV?MAXV:v); }
static inline float cvf(float v,float a,float b){ return v<a?a:(v>b?b:v); }
static inline void dec5(const uint8_t *p, uint32_t *o){
  uint64_t w=(uint64_t)p[0]|((uint64_t)p[1]<<8)|((uint64_t)p[2]<<16)|((uint64_t)p[3]<<24)|((uint64_t)p[4]<<32);
  o[0]=(uint32_t)(w&0x3FF); o[1]=(uint32_t)((w>>10)&0x3FF);
  o[2]=(uint32_t)((w>>20)&0x3FF); o[3]=(uint32_t)((w>>30)&0x3FF);
}
/* camreg mmap 缓冲是 uncached 语义：逐字节读 10MB 会慢到 60ms+（实测 63ms）。
 * 改成 16 字节 NEON 宽读（一次覆盖 3 个 5 字节组）后解包，实测 10ms。 */
#ifdef __aarch64__
#include <arm_neon.h>
static inline void ld16(const uint8_t *p, uint8_t *t){ vst1q_u8(t, vld1q_u8(p)); }
static inline uint64_t w5(const uint8_t *t,int i){
  return (uint64_t)t[i]|((uint64_t)t[i+1]<<8)|((uint64_t)t[i+2]<<16)|((uint64_t)t[i+3]<<24)|((uint64_t)t[i+4]<<32);
}
static inline void dec5x3(const uint8_t *t,uint32_t o[3][4]){
  for(int k=0;k<3;k++){ uint64_t w=w5(t,k*5);
    o[k][0]=(uint32_t)(w&0x3FF); o[k][1]=(uint32_t)((w>>10)&0x3FF);
    o[k][2]=(uint32_t)((w>>20)&0x3FF); o[k][3]=(uint32_t)((w>>30)&0x3FF); }
}
#endif
static int g_ispfast=0,g_ispcnt=0,g_ispdbg=0,g_legacy=0;
static int g_hifloor=0;
static int g_hflip=0;
static int g_tnr=1;
static double g_tnr_alpha=0.55;
static double g_tdec2=0,g_ttot=0; static long g_dn2=0;
static double g_alpha=0.35;
/* H0：AWB 鲁棒性（偏色止血） */
static double g_wbmin=0.45,g_wbmax=2.45;      /* 单通道增益绝对限幅（实测暖光 gB≈1.94） */
static double g_rbrmin=0.40,g_rbrmax=2.10;    /* gR/gB 色温限幅（实测 2700K 暖光 ≈0.477） */
static int    g_holdmax=40;                   /* 连续低置信 hold 上限（超过则向中性缓释） */
static double g_tdec=0,g_tblur=0,g_tstat=0,g_tfin=0; static long g_dn=0;

static int hist_pct(const uint32_t *h,int nb,double pct){
  uint64_t tot=0,acc=0; for(int i=0;i<nb;i++) tot+=h[i];
  if(!tot) return 0;
  if(pct>=0.99999){ for(int i=nb-1;i>=0;i--) if(h[i]) return i; return nb-1; }
  uint64_t target=(uint64_t)(tot*pct);
  for(int i=0;i<nb;i++){ acc+=h[i]; if(acc>=target) return i; }
  return nb-1;
}
static void box_blur(float *plane,float *tmp,int w,int h,int rad){
  #pragma omp parallel for schedule(static)
  for(int y=0;y<h;y++){
    float *p=plane+(size_t)y*w,*t=tmp+(size_t)y*w; float s=0;
    for(int x=-rad;x<=rad;x++){ int xx=x<0?0:(x>=w?w-1:x); s+=p[xx]; }
    float inv=1.0f/(2*rad+1);
    for(int x=0;x<w;x++){
      t[x]=s*inv;
      int xa=x-rad; if(xa<0)xa=0; if(xa>=w)xa=w-1;
      int xb=x+rad+1; if(xb<0)xb=0; if(xb>=w)xb=w-1;
      s-=p[xa]; s+=p[xb];
    }
  }
  #pragma omp parallel for schedule(static)
  for(int y=0;y<h;y++){
    int y0=y-rad; if(y0<0)y0=0; int y1=y+rad; if(y1>=h)y1=h-1; int cnt=y1-y0+1;
    const float *r=tmp+(size_t)y0*w; float *o=plane+(size_t)y*w;
    for(int x=0;x<w;x++) o[x]=r[x];
    for(int yy=y0+1;yy<=y1;yy++){ const float *q=tmp+(size_t)yy*w; for(int x=0;x<w;x++) o[x]+=q[x]; }
    float inv=1.0f/cnt; for(int x=0;x<w;x++) o[x]*=inv;
  }
}
int isp_half_init(IspHalf *s,int black,const char *awbmode){
  (void)black;(void)awbmode;
  memset(s,0,sizeof(*s));
  s->w=W_FULL/2; s->h=H_FULL/2;
  size_t n=(size_t)s->w*s->h;
  s->R=(float*)malloc(n*4); s->G=(float*)malloc(n*4); s->B=(float*)malloc(n*4);
  s->tmp=(float*)malloc(n*4);
  s->pR=(float*)malloc(n*4); s->pG=(float*)malloc(n*4); s->pB=(float*)malloc(n*4);
  s->alpha=0.35; s->inited=0; s->wbr=1.0; s->wbb=1.0; s->last_event="off";
  s->tnr_alpha=0.55; s->tnr_on=1;
  g_ispfast = getenv("CAMREC_VFAST")?1:0;
  g_ispdbg = getenv("CAMREC_ISPDBG")?1:0;
  g_legacy = getenv("CAMREC_ISP_LEGACY")?1:0;
  { const char *a=getenv("CAMREC_ISP_ALPHA"); if(a) g_alpha=atof(a); }
  { const char *a=getenv("CAMREC_ISP_WBMIN");  if(a) g_wbmin=atof(a); }
  { const char *a=getenv("CAMREC_ISP_WBMAX");  if(a) g_wbmax=atof(a); }
  { const char *a=getenv("CAMREC_ISP_RBRMIN"); if(a) g_rbrmin=atof(a); }
  { const char *a=getenv("CAMREC_ISP_RBRMAX"); if(a) g_rbrmax=atof(a); }
  { const char *a=getenv("CAMREC_ISP_HOLDMAX");if(a) g_holdmax=atoi(a); }
  { const char *a=getenv("CAMREC_TNR_ALPHA"); if(a) g_tnr_alpha=atof(a); }
  if(getenv("CAMREC_TNR_OFF")) g_tnr=0;
  if(getenv("CAMREC_HI_FLOOR")) g_hifloor=atoi(getenv("CAMREC_HI_FLOOR"));
  if(getenv("CAMREC_HFLIP")) g_hflip=atoi(getenv("CAMREC_HFLIP"));
  if(!s->R||!s->G||!s->B||!s->tmp||!s->pR||!s->pG||!s->pB) return -1;
  s->tnr_alpha=g_tnr_alpha;
  printf("ISP flags fast=%d legacy=%d tnr=%d alpha=%.2f\n",g_ispfast,g_legacy,g_tnr,g_tnr_alpha);
  return 0;
}
void isp_half_set_tnr(int on){ g_tnr = on?1:0; }
void isp_half_free(IspHalf *s){ free(s->R);free(s->G);free(s->B);free(s->tmp);
  free(s->pR);free(s->pG);free(s->pB);
  free(s->blkR);free(s->blkG);free(s->blkB); memset(s,0,sizeof(*s)); }

/* ---------- AWB v2 估计器：8x8 块域（已减黑） ---------- */
static int block_stats(IspHalf *s){
  int w=s->w,h=s->h,bw=w/BLKSZ,bh=h/BLKSZ;
  if(bw<=0||bh<=0) return -1;
  if(!s->blkB){
    size_t nb=(size_t)bw*bh;
    s->blkR=(float*)malloc(nb*4); s->blkG=(float*)malloc(nb*4); s->blkB=(float*)malloc(nb*4);
    if(!s->blkR||!s->blkG||!s->blkB) return -1;
    s->bw=bw; s->bh=bh;
  }
  if(s->bw!=bw||s->bh!=bh) return -1;
  const float inv=1.0f/(BLKSZ*BLKSZ);
  #pragma omp parallel for schedule(static)
  for(int by=0;by<bh;by++){
    for(int bx=0;bx<bw;bx++){
      float sb=0,sr=0,sg=0;
      for(int yy=0;yy<BLKSZ;yy++){
        size_t o=(size_t)(by*BLKSZ+yy)*w+bx*BLKSZ;
        const float *pr=s->R+o,*pg=s->G+o,*pb=s->B+o;
        for(int xx=0;xx<BLKSZ;xx++){ sr+=pr[xx]; sg+=pg[xx]; sb+=pb[xx]; }
      }
      size_t i=(size_t)by*bw+bx;
      s->blkR[i]=sr*inv; s->blkG[i]=sg*inv; s->blkB[i]=sb*inv;
    }
  }
  return 0;
}
/* 返回 0 成功；-1 极少有效块（调用方 hold）；mode: patch/gray/hybrid
 * H0：额外输出置信度 *outConf(0..1000) 与离散度 *outSpread(0..1000)；
 *     对 gR/gB 做色温限幅、对单通道增益做绝对限幅（防极端偏色）。 */
static int awb_v2(IspHalf *s,const char *mode,double *outR,double *outB,long *validQ,
                  double *outConf,double *outSpread){
  int bw=s->bw,bh=s->bh; if(!s->blkR||bw<=0) return -1;
  size_t nb=(size_t)bw*bh;
  long valid=0;
  uint32_t hb[1024]; memset(hb,0,sizeof(hb));
  for(size_t i=0;i<nb;i++){
    float bB=s->blkB[i],bR=s->blkR[i],bG=s->blkG[i];
    float mx=bB; if(bR>mx)mx=bR; if(bG>mx)mx=bG;
    if(mx<940.f && bG>10.f){ valid++; int v=(int)(bG+0.5f); if(v>MAXV)v=MAXV; if(v<0)v=0; hb[v]++; }
  }
  *validQ = (long)((double)valid*1000.0/(double)nb);
  if(valid<64) return -1;
  long target=(long)ceil((double)valid*0.01);
  long acc=0; int thr=MAXV;
  for(int v=MAXV;v>=0;v--){ acc+=hb[v]; if(acc>=target){ thr=v; break; } }
  double sg=0,sgr=0,sgb=0,spr=0,spg=0,spb=0; long pv=0;
  double mr=0,mb=0; long rn=0;
  for(size_t i=0;i<nb;i++){
    float bB=s->blkB[i],bR=s->blkR[i],bG=s->blkG[i];
    float mx=bB; if(bR>mx)mx=bR; if(bG>mx)mx=bG;
    if(!(mx<940.f && bG>10.f)) continue;
    sg+=bG; sgr+=bR; sgb+=bB;
    mr+=(double)bR/(double)bG; mb+=(double)bB/(double)bG; rn++;
    if(bG>=(float)thr){ spg+=bG; spr+=bR; spb+=bB; pv++; }
  }
  if(sgr<=0||sgb<=0) return -1;
  double grayR=sg/sgr, grayB=sg/sgb;
  double patchR=1,patchB=1;
  if(pv>=32 && spr>0 && spb>0){ patchR=spg/spr; patchB=spg/spb; }
  else { patchR=grayR; patchB=grayB; }
  /* H0 场景彩色度：有效块 R/G、B/G 比值的 MAD（灰卡场景小、大面积彩色大）
   * gray-world 假设在彩色场景失效，MAD 大时要降低 AWB 更新强度。 */
  double spread=0.0;
  if(rn>0){
    mr/=rn; mb/=rn;
    double dar=0,dab=0;
    for(size_t i=0;i<nb;i++){
      float bB=s->blkB[i],bR=s->blkR[i],bG=s->blkG[i];
      float mx=bB; if(bR>mx)mx=bR; if(bG>mx)mx=bG;
      if(!(mx<940.f && bG>10.f)) continue;
      dar+=fabs((double)bR/(double)bG-mr); dab+=fabs((double)bB/(double)bG-mb);
    }
    spread=0.5*(dar/(double)rn+dab/(double)rn);
  }
  double gR,gB;
  if(!strcmp(mode,"gray")){ gR=grayR*0.83926; gB=grayB*0.88441; }
  else if(!strcmp(mode,"hybrid")){
    double hR=0.65*grayR+0.35*patchR, hB=0.65*grayB+0.35*patchB;
    gR=hR*0.88394; gB=hB*0.91930;
  } else { gR=patchR*0.92519; gB=patchB*0.94922; }  /* patch 默认 */
  /* H0 置信度：有效块占比 × 场景彩色度惩罚（彩色场景 gray-world 不可靠） */
  double cValid=(double)(*validQ)/700.0; if(cValid>1.0)cValid=1.0; if(cValid<0.0)cValid=0.0;
  double cSpread=1.0-spread/0.45; if(cSpread<0.0)cSpread=0.0; if(cSpread>1.0)cSpread=1.0;
  double conf=1000.0*cValid*(0.45+0.55*cSpread);
  if(pv<32) conf*=0.85;                    /* 无近白块 ⇒ patch 退化为 gray，可信度打折 */
  /* H0 色温限幅：gR/gB 限制在物理光照范围内，超出按边界钳制并降置信 */
  double ratio=gR/gB;
  if(ratio<g_rbrmin){ gR=gB*g_rbrmin; conf*=0.6; }
  else if(ratio>g_rbrmax){ gR=gB*g_rbrmax; conf*=0.6; }
  /* H0 绝对限幅 */
  { double g1=cvf((float)gR,(float)g_wbmin,(float)g_wbmax), g2=cvf((float)gB,(float)g_wbmin,(float)g_wbmax);
    if(fabs(g1-gR)>1e-6||fabs(g2-gB)>1e-6) conf*=0.7;
    gR=g1; gB=g2; }
  if(conf<0)conf=0; if(conf>1000)conf=1000;
  *outR=gR; *outB=gB; *outConf=conf;
  if(outSpread){ double sp=spread/0.6*1000.0; if(sp<0)sp=0; if(sp>1000)sp=1000; *outSpread=sp; }
  return 0;
}
#define AWB_SET(tag) do{ s->last_event=(tag); }while(0)
/* H0 v3：置信度门控 + 死区 + 二次确认 reset + 长时低置信向中性缓释 */
static void awb_smooth(IspHalf *s,double gR,double gB,long validQ,double conf){
  s->wb_conf=conf;
  if(!s->inited){
    if(conf<350.0){ AWB_SET("initwait"); return; }   /* 前几帧不可信：保持中性 1.0 */
    s->wbr=gR; s->wbb=gB; s->inited=1; s->wb_holdn=0; AWB_SET("init"); return;
  }
  if(validQ<450 || conf<250.0){                      /* 极暗/彩色场景：估计不可信 */
    s->wb_holdn++;
    if(s->wb_holdn>(long)g_holdmax){                 /* 长时间不可信：缓慢回中性，避免卡在错误增益 */
      double a=0.02; s->wbr=s->wbr*(1-a)+a; s->wbb=s->wbb*(1-a)+a; AWB_SET("decay");
    } else AWB_SET("hold");
    return;
  }
  double relR=fabs(gR/s->wbr-1.0), relB=fabs(gB/s->wbb-1.0);
  double rel=relR>relB?relR:relB;
  if(rel<0.02){ s->wb_holdn=0; s->wb_pend_n=0; AWB_SET("deadband"); return; }
  if(rel>0.30){
    /* 大幅跳变需连续两帧一致才提交（防单帧彩色物体把 WB 带飞） */
    if(s->wb_pend_n>0 &&
       fabs(gR-s->wb_pend_r)<0.10*s->wb_pend_r+0.02 &&
       fabs(gB-s->wb_pend_b)<0.10*s->wb_pend_b+0.02){
      s->wbr=gR; s->wbb=gB; s->wb_pend_n=0; s->wb_holdn=0; AWB_SET("reset2"); return;
    }
    s->wb_pend_r=gR; s->wb_pend_b=gB; s->wb_pend_n=1;
    { double a=0.10; s->wbr=s->wbr*(1-a)+gR*a; s->wbb=s->wbb*(1-a)+gB*a; }  /* 待确认期小幅跟随防僵死 */
    AWB_SET("resetpend"); return;
  }
  s->wb_pend_n=0;
  if(rel>0.08){ double a=0.45; s->wbr=s->wbr*(1-a)+gR*a; s->wbb=s->wbb*(1-a)+gB*a; AWB_SET("fast"); }
  else { double a=g_alpha; s->wbr=s->wbr*(1-a)+gR*a; s->wbb=s->wbb*(1-a)+gB*a; AWB_SET("iir"); }
  s->wb_holdn=0;
}


/* ---------- RAW 域时域降噪（TNR）：运动自适应 IIR ----------
 * 曝光归一化：用 G 平面采样均值比 k=cur/prev（限幅 1/8..8）
 * 运动度量：d = max(|R-pR*k|, |G-pG*k|, |B-pB*k|)（10bit 域）
 * 混合权重：d<=LO 全混合 alpha；LO<d<HI 线性衰减；d>=HI 不混合（防拖影）
 */
static void tnr_blend(IspHalf *s,int lo,int hi,double alpha){
  int w=s->w,h=s->h;
  double sum=0; long cnt=0;
  for(int y=0;y<h;y+=4)for(int x=0;x<w;x+=4){ sum+=s->G[(size_t)y*w+x]; cnt++; }
  double cur=cnt?sum/cnt:0;
  if(!s->have_prev || s->pmean<1.0){ s->pmean=cur; s->have_prev=1;
    memcpy(s->pR,s->R,(size_t)w*h*4); memcpy(s->pG,s->G,(size_t)w*h*4); memcpy(s->pB,s->B,(size_t)w*h*4);
    return; }
  double k=cur/s->pmean; if(k<0.125)k=0.125; if(k>8.0)k=8.0;
  const float kf=(float)k, af=(float)alpha;
  #pragma omp parallel for schedule(static)
  for(int y=0;y<h;y++){
    size_t o=(size_t)y*w;
    for(int x=0;x<w;x++){
      size_t i=o+x;
      const float cr=s->R[i], cg=s->G[i], cb=s->B[i];
      const float pr=s->pR[i]*kf, pg=s->pG[i]*kf, pb=s->pB[i]*kf;
      float dr=cr-pr, dg=cg-pg, db=cb-pb;
      float d=fabsf(dr); float t=fabsf(dg); if(t>d)d=t; t=fabsf(db); if(t>d)d=t;
      float wgt;
      if(d<=(float)lo) wgt=af;
      else if(d>=(float)hi) wgt=0.f;
      else wgt=af*((float)hi-d)/(float)(hi-lo);
      float nr=cr+(pr-cr)*wgt, ng=cg+(pg-cg)*wgt, nb=cb+(pb-cb)*wgt;
      s->R[i]=nr; s->G[i]=ng; s->B[i]=nb;
      s->pR[i]=nr; s->pG[i]=ng; s->pB[i]=nb;
    }
  }
  s->pmean=cur;
}

static double g_last_w0=0,g_last_w3=0;
/* 解码 + TNR + (可选 R/B 平滑) + AWB/tone 统计：RGB 慢路与 YUYV 融合快路共用 */
static int isp_analyze(IspHalf *s,const uint8_t *raw,int black,
                       const char *awbmode,double manR,double manB){
  int w=s->w,h=s->h;
  double w0=0; if(g_ispdbg){ w0=omp_get_wtime(); } g_last_w0=w0;
  const int ng=w/2;              /* 每行 5 字节组数（816） */
  /* camreg mmap 缓冲是 uncached 语义：直接逐字节读 10MB 要 60~96ms（实测）。
   * 改为「每线程先 memcpy 两行到栈（宽读，约 1.3ms/线程）→ 在 L1 覆盖内解包」：
   * 实测流水线内 decode 从 66~96ms 降到 ~15ms。 */
  #pragma omp parallel for schedule(static)
  for(int k=0;k<h;k+=2){
    uint8_t buf[4*ROW_BYTES];           /* 4 输入行 = 2 输出行，一次宽读 16KB */
    memcpy(buf,raw+(size_t)(2*k)*ROW_BYTES,4*ROW_BYTES);
    uint32_t a[4],b[4];
    for(int j=0;j<2;j++){
      if(k+j>=h) break;
      const uint8_t *p0=buf+(size_t)(2*j)*ROW_BYTES,*p1=p0+ROW_BYTES;
      size_t y=(size_t)k+j, ob=y*(size_t)w;
      for(int g=0;g<ng;g++){
        dec5(p0+g*5,a); dec5(p1+g*5,b);
        size_t o=ob+(size_t)g*2;
        s->B[o]=(float)cvi((int)a[0]-black); s->G[o]=0.5f*(cvi((int)a[1]-black)+cvi((int)b[0]-black)); s->R[o]=(float)cvi((int)b[1]-black);
        s->B[o+1]=(float)cvi((int)a[2]-black); s->G[o+1]=0.5f*(cvi((int)a[3]-black)+cvi((int)b[2]-black)); s->R[o+1]=(float)cvi((int)b[3]-black);
      }
    }
  }
  double w1=0; if(g_ispdbg){ w1=omp_get_wtime(); g_tdec+=w1-w0; }
  if(g_tnr && !g_legacy){
    double t0=omp_get_wtime();
    tnr_blend(s,14,72,0.55);
    if(g_ispdbg){ g_tdec2+=omp_get_wtime()-t0; g_dn2++; }
    s->tnr_applied++;
  }
  double wb=0; if(g_ispdbg){ wb=omp_get_wtime(); }
  if(!g_ispfast){ box_blur(s->R,s->tmp,w,h,1); box_blur(s->B,s->tmp,w,h,1); }
  double w2=0; if(g_ispdbg){ w2=omp_get_wtime(); g_tblur+=w2-wb; }

  int do_stats = (!g_ispfast) || (!s->inited) || ((s->frame_no%10)==0);
  if(do_stats){
    double wr=s->wbr,wb=s->wbb; int lo=s->lo,hi=s->hi;
    const char *mode=awbmode;
    { const char *a=getenv("CAMREC_ISP_AWB"); if(a) mode=a; }
    if(!strcmp(mode,"manual")){
      if(s->inited){ double a=g_alpha; wr=wr*(1-a)+manR*a; wb=wb*(1-a)+manB*a; }
      else { wr=manR; wb=manB; }
      AWB_SET("manual");
    } else {
      double gR=1,gB=1; long validQ=0; double conf=0,spread=0;
      if(block_stats(s)==0 && awb_v2(s,mode,&gR,&gB,&validQ,&conf,&spread)==0){
        awb_smooth(s,gR,gB,validQ,conf);
        wr=s->wbr; wb=s->wbb;
      } else { AWB_SET("hold"); wr=s->wbr; wb=s->wbb; }
      s->valid_q = validQ; s->wb_spread = spread;
    }
    s->wbr=wr; s->wbb=wb;
    {
      uint32_t hl[1024]; memset(hl,0,sizeof(hl));
      for(int y=0;y<h;y+=2)for(int x=0;x<w;x+=2){ size_t i=(size_t)y*w+x;
        double yv=0.299*s->R[i]*wr+0.587*s->G[i]+0.114*s->B[i]*wb; int v=cvi((int)(yv+0.5)); hl[v]++; }
      double hipct = g_legacy?0.997:0.995;
      int nlo=hist_pct(hl,1024,0.005), nhi=hist_pct(hl,1024,hipct);
      if(g_hifloor>0 && nhi<g_hifloor) nhi=g_hifloor;
      if(nhi<=nlo+32) nhi=nlo+32;
      if(!s->inited){ s->lo=nlo; s->hi=nhi; }
      else { double a=0.25;
        s->lo=(int)(s->lo*(1-a)+nlo*a); s->hi=(int)(s->hi*(1-a)+nhi*a);
        if(s->hi<=s->lo+32) s->hi=s->lo+32; }
    }
    s->inited=1;
  }
  s->frame_no++;
  double w3e=0; if(g_ispdbg){ w3e=omp_get_wtime(); g_tstat+=w3e-w2; g_last_w3=w3e; }
  return 0;
}

int isp_half_process(IspHalf *s,const uint8_t *raw,int black,
                     const char *awbmode,double manR,double manB,
                     double csup,double contrast,double gamma,double sharpen,
                     uint8_t **rgbout){
  (void)sharpen;
  int w=s->w,h=s->h; size_t n=(size_t)w*h;
  if(isp_analyze(s,raw,black,awbmode,manR,manB)!=0) return -1;
  double w0=g_last_w0, w3=g_last_w3;

  static uint8_t *buf=NULL; static size_t bufcap=0;
  if(bufcap!=n*3){ buf=(uint8_t*)realloc(buf,n*3); if(!buf) return -1; bufcap=n*3; }
  const double wbr=s->wbr,wbb=s->wbb; const int LO=s->lo,HI=s->hi;

  if(g_legacy){
    uint8_t lut[1024];
    for(int i=0;i<1024;i++){ double v=(i-LO)/(double)(HI-LO); if(v<0)v=0; if(v>1)v=1;
      double t=0.5+(v-0.5)*contrast; if(t<0)t=0; if(t>1)t=1;
      lut[i]=(uint8_t)(pow(t,1.0/gamma)*255.0+0.5); }
    #pragma omp parallel for schedule(static)
    for(int y=0;y<h;y++)for(int x=0;x<w;x++){ size_t i=(size_t)y*w+x; if(g_hflip) i=(size_t)y*w+(size_t)(w-1-x);
      double g=s->G[i], r=s->R[i]*wbr, b=s->B[i]*wbb;
      double yv=0.299*r+0.587*g+0.114*b; double k=1.0;
      if(csup>0){ double t=(yv-LO)/(double)(HI-LO); if(t<0)t=0; if(t>1)t=1;
        double kk=t<0.5?(0.35+0.65*(t/0.5)):1.0; k=1.0-(1.0-kk)*csup*2.0; if(k<0.15)k=0.15; }
      r=g+(r-g)*k; b=g+(b-g)*k;
      buf[i*3+0]=lut[cvi((int)(r+0.5))]; buf[i*3+1]=lut[cvi((int)(g+0.5))]; buf[i*3+2]=lut[cvi((int)(b+0.5))];
    }
  } else {
    /* tone v2：线性域有理曲线 LUT（暗部 lift + 高光 shoulder，不硬剪）+ 显示域对比度 */
    static float tlut[4096]; static int tluto=-1;
    if(tluto!=LO || tluto<0){
      const double k=1.0;
      double span=(double)(HI-LO); if(span<32) span=32;
      double gp=1.0/gamma;
      for(int i=0;i<4096;i++){
        double t=(i-LO)/span; if(t<0)t=0; if(t>1)t=1;
        double u=t*(1.0+k)/(1.0+k*t);
        double d=pow(u,gp);
        d=0.5+(d-0.5)*contrast; if(d<0)d=0; if(d>1)d=1;
        tlut[i]=(float)d;
      }
      tluto=LO;
    }
    const float *LUT=tlut;
    #pragma omp parallel for schedule(static)
    for(int y=0;y<h;y++)for(int x=0;x<w;x++){ size_t i=(size_t)y*w+x; if(g_hflip) i=(size_t)y*w+(size_t)(w-1-x);
      double r=s->R[i]*wbr, b=s->B[i]*wbb, g=s->G[i];
      double yv=0.299*r+0.587*g+0.114*b;
      int yi=(int)(yv+0.5); if(yi<0)yi=0; if(yi>4095)yi=4095;
      double d=LUT[yi];
      double rr=(yv>1.0)?r/yv:1.0, rb=(yv>1.0)?b/yv:1.0;
      if(csup>0){ double t=(yv-LO)/(double)(HI-LO); if(t<0)t=0; if(t>1)t=1;
        double kk=t<0.5?(0.35+0.65*(t/0.5)):1.0; double k=1.0-(1.0-kk)*csup*2.0; if(k<0.15)k=0.15;
        rr=1.0+(rr-1.0)*k; rb=1.0+(rb-1.0)*k; }
      if(d>0.85){ double s2=1.0-0.2*((d-0.85)/0.15);  /* 高光去饱和 0.8 */
        rr=1.0+(rr-1.0)*s2; rb=1.0+(rb-1.0)*s2; }
      int orr=(int)(rr*d*255.0+0.5), obb=(int)(rb*d*255.0+0.5), ogg=(int)(d*255.0+0.5);
      if(orr<0)orr=0; if(orr>255)orr=255; if(obb<0)obb=0; if(obb>255)obb=255; if(ogg<0)ogg=0; if(ogg>255)ogg=255;
      buf[i*3+0]=(uint8_t)orr; buf[i*3+1]=(uint8_t)ogg; buf[i*3+2]=(uint8_t)obb;
    }
  }
  *rgbout=buf;
  if(g_ispdbg){ double w4=omp_get_wtime(); g_tfin+=w4-w3; g_ttot+=(w4-w0); g_dn++;
    if(g_dn%30==0) printf("ISP perf n=%ld tnr=%.1f dec=%.1f blur=%.1f stat=%.1f fin=%.1f total=%.1f\n",g_dn,g_dn2?1000.0*g_tdec2/g_dn2:0.0,1000.0*g_tdec/g_dn,1000.0*g_tblur/g_dn,1000.0*g_tstat/g_dn,1000.0*g_tfin/g_dn,1000.0*g_ttot/g_dn); }
  return 0;
}


/* ---------- 融合快路：一次解码/统计 → 目标尺寸 tone + YUYV ---------- */
static inline void tone2rgb_v2(const float *LUT,float wbr,float wbb,int LO,int HI,float csup,
                               float r,float g,float b,int *pr,int *pg,int *pb){
  float rr=r*wbr, bb=b*wbb, gg=g;
  float yv=0.299f*rr+0.587f*gg+0.114f*bb;
  int yi=(int)(yv+0.5f); if(yi<0)yi=0; if(yi>4095)yi=4095;
  float d=LUT[yi];
  float invy=(yv>1.0f)?(1.0f/yv):1.0f;
  float rq=rr*invy, bq=bb*invy;
  int span=HI-LO; if(span<32)span=32;
  if(csup>0){ float t=(yv-(float)LO)/(float)span; if(t<0)t=0; if(t>1)t=1;
    float kk=t<0.5f?(0.35f+0.65f*(t/0.5f)):1.0f; float k=1.0f-(1.0f-kk)*(float)csup*2.0f; if(k<0.15f)k=0.15f;
    rq=1.0f+(rq-1.0f)*k; bq=1.0f+(bq-1.0f)*k; }
  if(d>0.85f){ float s2=1.0f-0.2f*((d-0.85f)/0.15f);
    rq=1.0f+(rq-1.0f)*s2; bq=1.0f+(bq-1.0f)*s2; }
  int R=(int)(rq*d*255.0f+0.5f), B=(int)(bq*d*255.0f+0.5f), G=(int)(d*255.0f+0.5f);
  if(R<0)R=0; if(R>255)R=255; if(G<0)G=0; if(G>255)G=255; if(B<0)B=0; if(B>255)B=255;
  *pr=R; *pg=G; *pb=B;
}
static inline void tone2rgb_v2f(const float *LUT,const float *GAIN,float wbr,float wbb,int LO,int HI,float csup,
                                float r,float g,float b,int *pr,int *pg,int *pb){
  float rr=r*wbr, bb=b*wbb, gg=g;
  float yv=0.299f*rr+0.587f*gg+0.114f*bb;
  int yi=(int)(yv+0.5f); if(yi<0)yi=0; if(yi>4095)yi=4095;
  float d=LUT[yi]; float gd=GAIN[yi]; float d255=d*255.0f;
  int span=HI-LO; if(span<32)span=32;
  float kmul=1.0f;
  if(csup>0){ float t=(yv-(float)LO)/(float)span; if(t<0)t=0; if(t>1)t=1;
    float kk=t<0.5f?(0.35f+0.65f*(t/0.5f)):1.0f; kmul=1.0f-(1.0f-kk)*csup*2.0f; if(kmul<0.15f)kmul=0.15f; }
  if(d>0.85f){ float s2=1.0f-0.2f*((d-0.85f)/0.15f); kmul*=s2; }
  float dr=(rr-yv)*kmul*gd, db=(bb-yv)*kmul*gd;
  int R=(int)(d255+dr+0.5f), B=(int)(d255+db+0.5f), G=(int)(d255+0.5f);
  if(R<0)R=0; if(R>255)R=255; if(G<0)G=0; if(G>255)G=255; if(B<0)B=0; if(B>255)B=255;
  *pr=R; *pg=G; *pb=B;
}
/* fast per-pixel: WB -> tone LUT -> Y(=255*d) + chroma deltas (YUV domain) */
static inline void yuv_px(const float *LUT,const float *GAIN,float wbr,float wbb,
                          float r,float g,float b,int *Y,float *dR,float *dB){
  float rr=r*wbr, bb=b*wbb;
  float yv=0.299f*rr+0.587f*g+0.114f*bb;
  int yi=(int)(yv+0.5f); if(yi<0)yi=0; if(yi>4095)yi=4095;
  float d=LUT[yi]; float gd=GAIN[yi];
  int Yv=(int)(d*255.0f+0.5f); if(Yv<0)Yv=0; if(Yv>255)Yv=255;
  *Y=Yv; *dR=(rr-yv)*gd; *dB=(bb-yv)*gd;
}
int isp_half_process_yuyv(IspHalf *s,const uint8_t *raw,int black,
                          const char *awbmode,double manR,double manB,
                          double csup,double contrast,double gamma,
                          int ow,int oh,int dst_stride,uint8_t *dst){
  if(isp_analyze(s,raw,black,awbmode,manR,manB)!=0) return -1;
  /* fused-path timing */
  double w0f=g_ispdbg?g_last_w0:0.0; double w4f=0; if(g_ispdbg) w4f=omp_get_wtime();
  const int w=s->w,h=s->h;
  if(ow<2||oh<1||(ow&1)||dst_stride<ow*2||ow>4096) return -1;
  const double wbr=s->wbr,wbb=s->wbb; const int LO=s->lo,HI=s->hi;
  static float tlut[4096]; static float tgain[4096]; static int tluto=-999999;
  if(tluto!=LO){
    const double k=1.0; double span=(double)(HI-LO); if(span<32) span=32;
    double gp=1.0/gamma;
    for(int i=0;i<4096;i++){
      double t=(i-LO)/span; if(t<0)t=0; if(t>1)t=1;
      double u=t*(1.0+k)/(1.0+k*t);
      double d=pow(u,gp);
      d=0.5+(d-0.5)*contrast; if(d<0)d=0; if(d>1)d=1;
      tlut[i]=(float)d;
      double yv=(double)(i>1?i:1);
      double kmul=1.0;
      if(csup>1e-6){ double t2=t<0.0?0.0:(t>1.0?1.0:t);
        double kk=t2<0.5?(0.35+0.65*(t2/0.5)):1.0;
        kmul=1.0-(1.0-kk)*(double)csup*2.0; if(kmul<0.15)kmul=0.15; }
      double s2=1.0; if(d>0.85) s2=1.0-0.2*((d-0.85)/0.15);
      tgain[i]=(float)(d*255.0/yv*kmul*s2);
    }
    tluto=LO;
  }
  const float *LUT=tlut,*GAIN=tgain;
  const int ident=(ow==w && oh==h);
  static int xa[4096],xb[4096]; static float xf[4096];
  if(!ident){
    for(int ox=0;ox<ow;ox++){
      float sx=((float)ox+0.5f)*(float)w/(float)ow-0.5f;
      if(sx<0)sx=0; if(sx>(float)(w-1))sx=(float)(w-1);
      int i=(int)sx; float f=sx-(float)i; int j=i+1; if(j>=w)j=w-1;
      xa[ox]=i; xb[ox]=j; xf[ox]=f;
    }
  }
  const float cs=(float)csup;
  #pragma omp parallel for schedule(static)
  for(int oy=0;oy<oh;oy++){
    const float *Rr0,*Rr1,*Gr0,*Gr1,*Br0,*Br1; float fy=0;
    if(ident){ Rr0=Rr1=s->R+(size_t)oy*w; Gr0=Gr1=s->G+(size_t)oy*w; Br0=Br1=s->B+(size_t)oy*w; }
    else {
      float sy=((float)oy+0.5f)*(float)h/(float)oh-0.5f;
      if(sy<0)sy=0; if(sy>(float)(h-1))sy=(float)(h-1);
      int y0=(int)sy; fy=sy-(float)y0; int y1=y0+1; if(y1>=h)y1=h-1;
      Rr0=s->R+(size_t)y0*w; Rr1=s->R+(size_t)y1*w;
      Gr0=s->G+(size_t)y0*w; Gr1=s->G+(size_t)y1*w;
      Br0=s->B+(size_t)y0*w; Br1=s->B+(size_t)y1*w;
    }
    uint8_t *row=dst+(size_t)oy*dst_stride;
    for(int ox=0;ox<ow;ox+=2){
      float r0f,g0f,b0f,r1f,g1f,b1f;
      if(ident){ r0f=Rr0[ox]; g0f=Gr0[ox]; b0f=Br0[ox]; r1f=Rr0[ox+1]; g1f=Gr0[ox+1]; b1f=Br0[ox+1]; }
      else {
        int i0=xa[ox], j0=xb[ox]; float f0=xf[ox];
        int i1=xa[ox+1], j1=xb[ox+1]; float f1=xf[ox+1];
        float r0a=Rr0[i0]+(Rr0[j0]-Rr0[i0])*f0, r0b=Rr1[i0]+(Rr1[j0]-Rr1[i0])*f0;
        float g0a=Gr0[i0]+(Gr0[j0]-Gr0[i0])*f0, g0b=Gr1[i0]+(Gr1[j0]-Gr1[i0])*f0;
        float b0a=Br0[i0]+(Br0[j0]-Br0[i0])*f0, b0b=Br1[i0]+(Br1[j0]-Br1[i0])*f0;
        r0f=r0a+(r0b-r0a)*fy; g0f=g0a+(g0b-g0a)*fy; b0f=b0a+(b0b-b0a)*fy;
        float r1a=Rr0[i1]+(Rr0[j1]-Rr0[i1])*f1, r1b=Rr1[i1]+(Rr1[j1]-Rr1[i1])*f1;
        float g1a=Gr0[i1]+(Gr0[j1]-Gr0[i1])*f1, g1b=Gr1[i1]+(Gr1[j1]-Gr1[i1])*f1;
        float b1a=Br0[i1]+(Br0[j1]-Br0[i1])*f1, b1b=Br1[i1]+(Br1[j1]-Br1[i1])*f1;
        r1f=r1a+(r1b-r1a)*fy; g1f=g1a+(g1b-g1a)*fy; b1f=b1a+(b1b-b1a)*fy;
      }
      int Y0,Y1; float dR0,dB0,dR1,dB1;
      yuv_px(LUT,GAIN,(float)wbr,(float)wbb,r0f,g0f,b0f,&Y0,&dR0,&dB0);
      yuv_px(LUT,GAIN,(float)wbr,(float)wbb,r1f,g1f,b1f,&Y1,&dR1,&dB1);
      int U=128+(int)(0.246f*(dB0+dB1)+0.5f);
      int V=128+(int)(0.4385f*(dR0+dR1)+0.5f);
      if(U<0)U=0; if(U>255)U=255;
      if(V<0)V=0; if(V>255)V=255;
      int px=g_hflip?(ow-2-ox):ox;
      row[px*2+0]=(uint8_t)Y0; row[px*2+1]=(uint8_t)U;
      row[px*2+2]=(uint8_t)Y1; row[px*2+3]=(uint8_t)V;
    }
  }
  if(g_ispdbg){ double w5=omp_get_wtime(); g_tfin+=w5-w4f; g_ttot+=(w5-w0f); g_dn++;
    if(g_dn%30==0) printf("ISP perf(fused) n=%ld tnr=%.1f dec=%.1f blur=%.1f stat=%.1f fin=%.1f total=%.1f\n",g_dn,g_dn2?1000.0*g_tdec2/g_dn2:0.0,1000.0*g_tdec/g_dn,1000.0*g_tblur/g_dn,1000.0*g_tstat/g_dn,1000.0*g_tfin/g_dn,1000.0*g_ttot/g_dn); }
  return 0;
}
