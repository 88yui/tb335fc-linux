/*
 * camisp.c v4 — CAM_A(SC820ACS) RAW10(40bit LE) 快速 ISP -> PPM / RGB24
 *  scale=2 视频快路 1632x1224 / scale=1 照片路 3264x2448
 *  OpenMP + 行式滑动窗; 直方图按 1/4 采样; 多帧在解码域平均
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <time.h>
#ifdef _OPENMP
#include <omp.h>
#endif

#define W_FULL 3264
#define H_FULL 2448
#define ROW_BYTES 4080
#define FRAME_BYTES (2448*4080)
#define MAXV 1023

static int g_black=61, g_scale=2, g_threads=0;
static const char *g_awb="patch";
static double g_wbr=0,g_wbb=0;
static double g_csup=0.12, g_contrast=1.10, g_gamma=2.2, g_sharpen=0.0;
static int g_rgbout=0;
static int g_tone_v2=0;
static int g_awb_v2=0;
static double g_hipct=0.0; /* 0 => 由 tone 模式决定 */

static double now_ms(void){ struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts); return ts.tv_sec*1000.0+ts.tv_nsec/1e6; }
static inline int cvi(int v){ return v<0?0:(v>MAXV?MAXV:v); }
static inline float cvf(float v,float a,float b){ return v<a?a:(v>b?b:v); }

static inline void dec5(const uint8_t *p, uint32_t *o){
  uint64_t w=(uint64_t)p[0]|((uint64_t)p[1]<<8)|((uint64_t)p[2]<<16)|((uint64_t)p[3]<<24)|((uint64_t)p[4]<<32);
  o[0]=(uint32_t)(w&0x3FF); o[1]=(uint32_t)((w>>10)&0x3FF);
  o[2]=(uint32_t)((w>>20)&0x3FF); o[3]=(uint32_t)((w>>30)&0x3FF);
}
static int hist_pct(const uint32_t *h,int nb,double pct){
  uint64_t tot=0,acc=0; for(int i=0;i<nb;i++) tot+=h[i];
  if(!tot) return 0;
  uint64_t target=(uint64_t)(tot*pct);
  for(int i=0;i<nb;i++){ acc+=h[i]; if(acc>=target) return i; }
  return nb-1;
}
/* 掩码水平滑动 + 垂直行和 (半径小, 直接累加) */
static void masked_box(float *plane,float *tmp,int w,int h,int rad){
  #pragma omp parallel for schedule(static)
  for(int y=0;y<h;y++){
    float *p=plane+(size_t)y*w,*t=tmp+(size_t)y*w;
    float s=0; int c=0;
    for(int x=-rad;x<=rad;x++){ int xx=x<0?0:(x>=w?w-1:x); float v=p[xx]; if(v>0){s+=v;c++;} }
    for(int x=0;x<w;x++){
      t[x]= c? s/c : 0;
      int xa=x-rad; if(xa<0)xa=0; if(xa>=w)xa=w-1;
      int xb=x+rad+1; if(xb<0)xb=0; if(xb>=w)xb=w-1;
      float va=p[xa]; if(va>0){s-=va;c--;}
      float vb=p[xb]; if(vb>0){s+=vb;c++;}
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
/* 普通盒式(无掩码) */
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
/* 双线性 2x 上采样 half(w,hh) -> full(2w,2hh) */
static void up2(const float *hs,int w,int hh,float *f){
  #pragma omp parallel for schedule(static)
  for(int Y=0;Y<hh*2;Y++){
    int y0=Y>>1; float dy=(Y&1)?0.5f:0.0f; int y1=y0+1<hh?y0+1:y0;
    float *o=f+(size_t)Y*2*w;
    for(int X=0;X<w;X++){
      int x1=X+1<w?X+1:X; float dx=0.5f;
      float a=hs[(size_t)y0*w+X], b=hs[(size_t)y0*w+x1], c=hs[(size_t)y1*w+X], d=hs[(size_t)y1*w+x1];
      float top=a+(b-a)*dx, bot=c+(d-c)*dx;
      o[2*X]=a+(c-a)*dy;          /* x 无偏移: a..c */
      o[2*X+1]=top+(bot-top)*dy;  /* x 偏移 0.5 时: top 与 bot 之间 */
    }
  }
}
static void build_lut(uint8_t *lut,int lo,int hi,double contrast,double gamma){
  for(int i=0;i<1024;i++){
    double v=(i-lo)/(double)(hi-lo); if(v<0)v=0; if(v>1)v=1;
    double s=0.5+(v-0.5)*contrast; if(s<0)s=0; if(s>1)s=1;
    lut[i]=(uint8_t)(pow(s,1.0/gamma)*255.0+0.5);
  }
}
/* 统计 + AWB；返回 lo/hi/wbr/wbb */
static void stats_awb(const float *R,const float *G,const float *B,int w,int h,
                      const char *mode,double manR,double manB,
                      int *lo,int *hi,double *wbr,double *wbb){
  uint32_t hR[1024]={0},hG[1024]={0},hB[1024]={0};
  double mr=0,mg=0,mb=0; long cnt=0;
  for(int y=0;y<h;y+=2)for(int x=0;x<w;x+=2){ size_t i=(size_t)y*w+x;
    int r=cvi((int)(R[i]+0.5f)),g=cvi((int)(G[i]+0.5f)),b=cvi((int)(B[i]+0.5f));
    hR[r]++;hG[g]++;hB[b]++; mr+=R[i];mg+=G[i];mb+=B[i]; cnt++; }
  double wr=1,wb_=1;
  if(!strcmp(mode,"manual")){ wr=manR; wb_=manB; }
  else if(!strcmp(mode,"gray")){ mr/=cnt;mg/=cnt;mb/=cnt; if(mr>1)wr=mg/mr; if(mb>1)wb_=mg/mb; }
  else { int pr=hist_pct(hR,1024,0.995),pg=hist_pct(hG,1024,0.995),pb=hist_pct(hB,1024,0.995);
    if(pr>4)wr=(double)pg/pr; if(pb>4)wb_=(double)pg/pb; }
  wr=cvf((float)wr,0.3f,3.5f); wb_=cvf((float)wb_,0.3f,3.5f);
  uint32_t hl[1024]={0};
  for(int y=0;y<h;y+=2)for(int x=0;x<w;x+=2){ size_t i=(size_t)y*w+x;
    double yv=0.299*R[i]*wr+0.587*G[i]+0.114*B[i]*wb_; int v=cvi((int)(yv+0.5)); hl[v]++; }
  int l=hist_pct(hl,1024,0.005), hi_=hist_pct(hl,1024,g_hipct>0?g_hipct:(g_tone_v2?0.995:0.997)); if(hi_<=l+8) hi_=l+8;
  *lo=l; *hi=hi_; *wbr=wr; *wbb=wb_;
}
/* 生成 8bit RGB */
static void tone_rgb(const float *R,const float *G,const float *B,const float *Gb,
                     int w,int h,int lo,int hi,double wbr,double wbb,double csup,double sharpen,
                     const uint8_t *lut,uint8_t *rgb){
  #pragma omp parallel for schedule(static)
  for(int y=0;y<h;y++)for(int x=0;x<w;x++){ size_t i=(size_t)y*w+x;
    double g=G[i]; if(Gb) g=g+sharpen*(G[i]-Gb[i]);
    double r=R[i]*wbr, b=B[i]*wbb;
    double yv=0.299*r+0.587*g+0.114*b; double k=1.0;
    if(csup>0){ double t=(yv-lo)/(double)(hi-lo); if(t<0)t=0; if(t>1)t=1;
      double kk=t<0.5?(0.35+0.65*(t/0.5)):1.0; k=1.0-(1.0-kk)*csup*2.0; if(k<0.15)k=0.15; }
    r=g+(r-g)*k; b=g+(b-g)*k;
    rgb[i*3+0]=lut[cvi((int)(r+0.5))]; rgb[i*3+1]=lut[cvi((int)(g+0.5))]; rgb[i*3+2]=lut[cvi((int)(b+0.5))];
  }
}


/* tone v2（照片路）：有理曲线 LUT + 通道/Y 比例重建 + 高光去饱和（与 isp_lib 视频路一致） */
static void tone_rgb_v2(const float *R,const float *G,const float *B,const float *Gb,
                        int w,int h,int lo,int hi,double wbr,double wbb,double csup,double sharpen,
                        double contrast,double gamma,uint8_t *rgb){
  static float tlut[4096]; static int tluto=-9999,tluthi=-9999; static double tlutc=-1,tlutg=-1;
  if(tluto!=lo||tluthi!=hi||tlutc!=contrast||tlutg!=gamma){
    const double k=1.0; double span=(double)(hi-lo); if(span<32) span=32; double gp=1.0/gamma;
    for(int i=0;i<4096;i++){ double t=(i-lo)/span; if(t<0)t=0; if(t>1)t=1;
      double u=t*(1.0+k)/(1.0+k*t); double d=pow(u,gp);
      d=0.5+(d-0.5)*contrast; if(d<0)d=0; if(d>1)d=1; tlut[i]=(float)d; }
    tluto=lo; tluthi=hi; tlutc=contrast; tlutg=gamma;
  }
  const float *LUT=tlut;
  #pragma omp parallel for schedule(static)
  for(int y=0;y<h;y++)for(int x=0;x<w;x++){ size_t i=(size_t)y*w+x;
    double g=G[i]; if(Gb) g=g+sharpen*(G[i]-Gb[i]);
    double r=R[i]*wbr, b=B[i]*wbb;
    double yv=0.299*r+0.587*g+0.114*b;
    int yi=(int)(yv+0.5); if(yi<0)yi=0; if(yi>4095)yi=4095;
    double d=LUT[yi];
    double rr=(yv>1.0)?r/yv:1.0, rb=(yv>1.0)?b/yv:1.0;
    if(csup>0){ double tt=(yv-lo)/(double)(hi-lo); if(tt<0)tt=0; if(tt>1)tt=1;
      double kk=tt<0.5?(0.35+0.65*(tt/0.5)):1.0; double k=1.0-(1.0-kk)*csup*2.0; if(k<0.15)k=0.15;
      rr=1.0+(rr-1.0)*k; rb=1.0+(rb-1.0)*k; }
    if(d>0.85){ double s2=1.0-0.2*((d-0.85)/0.15);
      rr=1.0+(rr-1.0)*s2; rb=1.0+(rb-1.0)*s2; }
    int orr=(int)(rr*d*255.0+0.5), obb=(int)(rb*d*255.0+0.5), ogg=(int)(d*255.0+0.5);
    if(orr<0)orr=0; if(orr>255)orr=255; if(obb<0)obb=0; if(obb>255)obb=255; if(ogg<0)ogg=0; if(ogg>255)ogg=255;
    rgb[i*3+0]=(uint8_t)orr; rgb[i*3+1]=(uint8_t)ogg; rgb[i*3+2]=(uint8_t)obb;
  }
}

/* AWB v2（照片路）：8x8 块统计 + valid 掩码 + patch/gray/hybrid + c2-refine 标定常数 */
static void stats_awb2(const float *R,const float *G,const float *B,int w,int h,
                       const char *mode,double manR,double manB,
                       int *lo,int *hi,double *wbr,double *wbb){
  int bw=w/8, bh=h/8; size_t nb=(size_t)bw*bh; if(bw<2||bh<2){ stats_awb(R,G,B,w,h,mode,manR,manB,lo,hi,wbr,wbb); return; }
  float *bR=(float*)malloc(nb*4),*bG=(float*)malloc(nb*4),*bB=(float*)malloc(nb*4);
  unsigned char *valid=(unsigned char*)malloc(nb);
  #pragma omp parallel for schedule(static)
  for(int by=0;by<bh;by++)for(int bx=0;bx<bw;bx++){
    double sr=0,sg=0,sb=0;
    for(int y=by*8;y<by*8+8;y++){ const float *pr=R+(size_t)y*w,*pg=G+(size_t)y*w,*pb=B+(size_t)y*w;
      for(int x=bx*8;x<bx*8+8;x++){ sr+=pr[x]; sg+=pg[x]; sb+=pb[x]; } }
    size_t k=(size_t)by*bw+bx;
    bR[k]=(float)(sr/64.0); bG[k]=(float)(sg/64.0); bB[k]=(float)(sb/64.0);
    float mx=bR[k]; if(bG[k]>mx)mx=bG[k]; if(bB[k]>mx)mx=bB[k];
    valid[k]=(mx<940.0f && bG[k]>10.0f)?1:0;
  }
  double wr=1.0,wb_=1.0;
  if(!strcmp(mode,"manual")){ wr=manR; wb_=manB; }
  else {
    long nv=0; for(size_t k=0;k<nb;k++) if(valid[k]) nv++;
    if(nv>(long)(nb*0.10)){
      uint32_t hg[1024]={0};
      for(size_t k=0;k<nb;k++) if(valid[k]){ int v=(int)(bG[k]+0.5f); if(v<0)v=0; if(v>1023)v=1023; hg[v]++; }
      float p99=(float)hist_pct(hg,1024,0.99);
      double sumG=0,sumR=0,sumB=0; long nG=0;
      double pG=0,pR=0,pB=0; long nP=0;
      for(size_t k=0;k<nb;k++){ if(!valid[k]) continue;
        sumG+=bG[k]; sumR+=bR[k]; sumB+=bB[k]; nG++;
        if(bG[k]>p99){ pG+=bG[k]; pR+=bR[k]; pB+=bB[k]; nP++; } }
      double gR=1,gB=1,gRp=1,gBp=1;
      if(nG>8){ double mR=sumR/nG,mG=sumG/nG,mB=sumB/nG;
        if(mR>1) gR=mG/mR; if(mB>1) gB=mG/mB; }
      if(nP>8&&pR>1&&pB>1){ gRp=pG/pR; gBp=pG/pB; } else { gRp=gR; gBp=gB; }
      if(!strcmp(mode,"patch")){ gR=gRp; gB=gBp;
        gR*=0.92519; gB*=0.94922;
      } else if(!strcmp(mode,"gray")){
        gR*=0.83926; gB*=0.88441;
      } else { gR=0.65*gR+0.35*gRp; gB=0.65*gB+0.35*gBp;
        gR*=0.88394; gB*=0.91930; }
      wr=cvf((float)gR,0.55f,2.2f); wb_=cvf((float)gB,0.55f,2.2f);
    }
  }
  free(bR);free(bG);free(bB);free(valid);
  uint32_t hl[1024]={0};
  for(int y=0;y<h;y+=2)for(int x=0;x<w;x+=2){ size_t i=(size_t)y*w+x;
    double yv=0.299*R[i]*wr+0.587*G[i]+0.114*B[i]*wb_; int v=cvi((int)(yv+0.5)); hl[v]++; }
  int l=hist_pct(hl,1024,0.005), hi_=hist_pct(hl,1024,g_hipct>0?g_hipct:(g_tone_v2?0.995:0.997)); if(hi_<=l+8) hi_=l+8;
  *lo=l; *hi=hi_; *wbr=wr; *wbb=wb_;
}
int main(int argc,char**argv){
  const char *out=NULL; const char **files=(const char**)calloc(argc,sizeof(char*)); int nfiles=0;
  for(int i=1;i<argc;i++){
    if(!strcmp(argv[i],"-o")&&i+1<argc) out=argv[++i];
    else if(!strcmp(argv[i],"--scale")&&i+1<argc) g_scale=atoi(argv[++i]);
    else if(!strcmp(argv[i],"--awb")&&i+1<argc) g_awb=argv[++i];
    else if(!strcmp(argv[i],"--wb")&&i+1<argc){ sscanf(argv[++i],"%lf,%lf",&g_wbr,&g_wbb); g_awb="manual"; }
    else if(!strcmp(argv[i],"--black")&&i+1<argc) g_black=atoi(argv[++i]);
    else if(!strcmp(argv[i],"--csup")&&i+1<argc) g_csup=atof(argv[++i]);
    else if(!strcmp(argv[i],"--contrast")&&i+1<argc) g_contrast=atof(argv[++i]);
    else if(!strcmp(argv[i],"--gamma")&&i+1<argc) g_gamma=atof(argv[++i]);
    else if(!strcmp(argv[i],"--sharpen")&&i+1<argc) g_sharpen=atof(argv[++i]);
    else if(!strcmp(argv[i],"--tone")&&i+1<argc){ g_tone_v2 = strcmp(argv[++i],"v2")==0; }
    else if(!strcmp(argv[i],"--awb-est")&&i+1<argc){ g_awb_v2 = strcmp(argv[++i],"v2")==0; }
    else if(!strcmp(argv[i],"--tone-hi-pct")&&i+1<argc){ g_hipct=atof(argv[++i]); }
    else if(!strcmp(argv[i],"--threads")&&i+1<argc) g_threads=atoi(argv[++i]);
    else if(!strcmp(argv[i],"--rgbout")) g_rgbout=1;
    else if(argv[i][0]!='-') files[nfiles++]=argv[i];
  }
  if(nfiles<1||(!out&&!g_rgbout)){ fprintf(stderr,"usage: camisp -o out.ppm [opts] f1.raw [f2.raw...]\n"); return 1; }
  #ifdef _OPENMP
  if(g_threads>0) omp_set_num_threads(g_threads);
  #endif
  double t0=now_ms(); double tdec=0;
  int w,h; size_t n; float *R,*G,*B; uint8_t *rgb;
  int lo=0,hi=1023; double wbr=1,wbb=1;

  if(g_scale==1){
    w=W_FULL; h=H_FULL; n=(size_t)w*h;
    G=(float*)calloc(n,4); R=(float*)calloc(n,4); B=(float*)calloc(n,4);
    float *tmp=(float*)malloc(n*4);
    if(nfiles==1){
      FILE *f=fopen(files[0],"rb"); if(!f){ perror(files[0]); return 2; }
      uint8_t *raw=(uint8_t*)malloc(FRAME_BYTES);
      if(fread(raw,1,FRAME_BYTES,f)!=(size_t)FRAME_BYTES){ fprintf(stderr,"short read\n"); return 2; } fclose(f);
      #pragma omp parallel for schedule(static)
      for(int y=0;y<h;y++){
        const uint8_t *p=raw+(size_t)y*ROW_BYTES; uint32_t o4[4];
        for(int g0=0;g0<w/4;g0++){
          dec5(p+g0*5,o4); int x0=g0*4;
          for(int k=0;k<4;k++){ size_t i=(size_t)y*w+x0+k; float fv=(float)cvi((int)o4[k]-g_black);
            int xe=(x0+k)&1, ye=y&1;
            if(xe==0&&ye==0) B[i]=fv; else if(xe==1&&ye==1) R[i]=fv; else G[i]=fv; }
        }
      }
      free(raw);
    } else {
      uint32_t *acc=(uint32_t*)calloc(n,sizeof(uint32_t));
      for(int k=0;k<nfiles;k++){
        FILE *f=fopen(files[k],"rb"); if(!f){ perror(files[k]); return 2; }
        uint8_t *raw=(uint8_t*)malloc(FRAME_BYTES);
        if(fread(raw,1,FRAME_BYTES,f)!=(size_t)FRAME_BYTES){ fprintf(stderr,"short read\n"); return 2; } fclose(f);
        #pragma omp parallel for schedule(static)
        for(int y=0;y<h;y++){ const uint8_t *p=raw+(size_t)y*ROW_BYTES; uint32_t *o=acc+(size_t)y*w; uint32_t o4[4];
          for(int g0=0;g0<w/4;g0++){ dec5(p+g0*5,o4); o[g0*4]+=o4[0];o[g0*4+1]+=o4[1];o[g0*4+2]+=o4[2];o[g0*4+3]+=o4[3]; } }
        free(raw);
      }
      #pragma omp parallel for schedule(static)
      for(int y=0;y<h;y++)for(int x=0;x<w;x++){ size_t i=(size_t)y*w+x; float fv=(float)cvi((int)(acc[i]/nfiles)-g_black);
        int xe=x&1, ye=y&1; if(xe==0&&ye==0) B[i]=fv; else if(xe==1&&ye==1) R[i]=fv; else G[i]=fv; }
      free(acc);
    }
    tdec=now_ms();
    masked_box(G,tmp,w,h,1);
    { int hw=w/2,hh=h/2; size_t hn=(size_t)hw*hh;
      float *Rh=(float*)calloc(hn,4),*Bh=(float*)calloc(hn,4),*th=(float*)malloc(hn*4);
      #pragma omp parallel for schedule(static)
      for(int y=0;y<hh;y++)for(int x=0;x<hw;x++){ size_t i=(size_t)(2*y)*w+2*x;
        Rh[(size_t)y*hw+x]=R[i+w+1]; Bh[(size_t)y*hw+x]=B[i]; }
      masked_box(Rh,th,hw,hh,2); masked_box(Bh,th,hw,hh,2);
      up2(Rh,hw,hh,R); up2(Bh,hw,hh,B);
      free(Rh);free(Bh);free(th); }
    free(tmp);
    if(g_awb_v2) stats_awb2(R,G,B,w,h,g_awb,g_wbr,g_wbb,&lo,&hi,&wbr,&wbb); else stats_awb(R,G,B,w,h,g_awb,g_wbr,g_wbb,&lo,&hi,&wbr,&wbb);
    uint8_t lut[1024]; if(!g_tone_v2) build_lut(lut,lo,hi,g_contrast,g_gamma);
    float *Gb=NULL;
    if(g_sharpen>0){ Gb=(float*)malloc(n*4); memcpy(Gb,G,n*4); float *bt=(float*)malloc(n*4); box_blur(Gb,bt,w,h,2); free(bt); }
    rgb=(uint8_t*)malloc(n*3);
    if(g_tone_v2) tone_rgb_v2(R,G,B,Gb,w,h,lo,hi,wbr,wbb,g_csup,g_sharpen,g_contrast,g_gamma,rgb);
    else tone_rgb(R,G,B,Gb,w,h,lo,hi,wbr,wbb,g_csup,g_sharpen,lut,rgb);
    printf("ISP scale=1 out=%dx%d awb=%s(%s) tone=%s wb=%.3f/%.3f lo=%d hi=%d dec=%.0fms post=%.0fms\n",w,h,g_awb,g_awb_v2?"v2":"v1",g_tone_v2?"v2":"legacy",wbr,wbb,lo,hi,tdec-t0,now_ms()-tdec);
    if(g_rgbout){ fwrite(rgb,1,n*3,stdout); fflush(stdout); }
    if(out){ FILE *f=fopen(out,"wb"); if(!f){perror(out);return 2;} fprintf(f,"P6\n%d %d\n255\n",w,h); fwrite(rgb,1,n*3,f); fclose(f); }
    printf("DONE total=%.0fms frames=%d\n",now_ms()-t0,nfiles);
    if(Gb) free(Gb); free(rgb);free(R);free(G);free(B);free(files);
  } else {
    w=W_FULL/2; h=H_FULL/2; n=(size_t)w*h;
    R=(float*)malloc(n*4); G=(float*)malloc(n*4); B=(float*)malloc(n*4);
    if(nfiles==1){
      FILE *f=fopen(files[0],"rb"); if(!f){ perror(files[0]); return 2; }
      uint8_t *raw=(uint8_t*)malloc(FRAME_BYTES);
      if(fread(raw,1,FRAME_BYTES,f)!=(size_t)FRAME_BYTES){ fprintf(stderr,"short read\n"); return 2; } fclose(f);
      #pragma omp parallel for schedule(static)
      for(int y=0;y<h;y++){
        const uint8_t *p0=raw+(size_t)(2*y)*ROW_BYTES,*p1=raw+(size_t)(2*y+1)*ROW_BYTES;
        uint32_t a[4],b[4];
        for(int g0=0;g0<w/2;g0++){
          dec5(p0+g0*5,a); dec5(p1+g0*5,b);
          size_t o=(size_t)y*w+g0*2;
          B[o]=(float)cvi((int)a[0]-g_black); G[o]=0.5f*(cvi((int)a[1]-g_black)+cvi((int)b[0]-g_black)); R[o]=(float)cvi((int)b[1]-g_black);
          B[o+1]=(float)cvi((int)a[2]-g_black); G[o+1]=0.5f*(cvi((int)a[3]-g_black)+cvi((int)b[2]-g_black)); R[o+1]=(float)cvi((int)b[3]-g_black);
        }
      }
      free(raw);
    } else {
      uint32_t *acc=(uint32_t*)calloc((size_t)W_FULL*H_FULL,sizeof(uint32_t));
      for(int k=0;k<nfiles;k++){
        FILE *f=fopen(files[k],"rb"); if(!f){ perror(files[k]); return 2; }
        uint8_t *raw=(uint8_t*)malloc(FRAME_BYTES);
        if(fread(raw,1,FRAME_BYTES,f)!=(size_t)FRAME_BYTES){ fprintf(stderr,"short read\n"); return 2; } fclose(f);
        #pragma omp parallel for schedule(static)
        for(int y=0;y<H_FULL;y++){ const uint8_t *p=raw+(size_t)y*ROW_BYTES; uint32_t *o=acc+(size_t)y*W_FULL; uint32_t o4[4];
          for(int g0=0;g0<W_FULL/4;g0++){ dec5(p+g0*5,o4); o[g0*4]+=o4[0];o[g0*4+1]+=o4[1];o[g0*4+2]+=o4[2];o[g0*4+3]+=o4[3]; } }
        free(raw);
      }
      #pragma omp parallel for schedule(static)
      for(int y=0;y<h;y++)for(int x=0;x<w;x++){ size_t i0=(size_t)(2*y)*W_FULL+2*x,o=(size_t)y*w+x;
        int b0=(int)(acc[i0]/nfiles)-g_black, g1=(int)(acc[i0+1]/nfiles)-g_black, g2=(int)(acc[i0+W_FULL]/nfiles)-g_black, r1=(int)(acc[i0+W_FULL+1]/nfiles)-g_black;
        B[o]=(float)cvi(b0); G[o]=0.5f*(cvi(g1)+cvi(g2)); R[o]=(float)cvi(r1); }
      free(acc);
    }
    tdec=now_ms();
    { float *tmp=(float*)malloc(n*4); box_blur(R,tmp,w,h,1); box_blur(B,tmp,w,h,1); free(tmp); }
    if(g_awb_v2) stats_awb2(R,G,B,w,h,g_awb,g_wbr,g_wbb,&lo,&hi,&wbr,&wbb); else stats_awb(R,G,B,w,h,g_awb,g_wbr,g_wbb,&lo,&hi,&wbr,&wbb);
    uint8_t lut[1024]; if(!g_tone_v2) build_lut(lut,lo,hi,g_contrast,g_gamma);
    rgb=(uint8_t*)malloc(n*3);
    if(g_tone_v2) tone_rgb_v2(R,G,B,NULL,w,h,lo,hi,wbr,wbb,g_csup,0,g_contrast,g_gamma,rgb);
    else tone_rgb(R,G,B,NULL,w,h,lo,hi,wbr,wbb,g_csup,0,lut,rgb);
    printf("ISP scale=2 out=%dx%d awb=%s(%s) tone=%s wb=%.3f/%.3f lo=%d hi=%d dec=%.0fms post=%.0fms\n",w,h,g_awb,g_awb_v2?"v2":"v1",g_tone_v2?"v2":"legacy",wbr,wbb,lo,hi,tdec-t0,now_ms()-tdec);
    if(g_rgbout){ fwrite(rgb,1,n*3,stdout); fflush(stdout); }
    if(out){ FILE *f=fopen(out,"wb"); if(!f){perror(out);return 2;} fprintf(f,"P6\n%d %d\n255\n",w,h); fwrite(rgb,1,n*3,f); fclose(f); }
    printf("DONE total=%.0fms frames=%d\n",now_ms()-t0,nfiles);
    free(rgb);free(R);free(G);free(B);free(files);
  }
  return 0;
}
