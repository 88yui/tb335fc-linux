/* isptest.c — 离线跑 isp_half_process 在若干 RAW10 帧上，输出 PPM 并打印统计
 * 用法: isptest <outprefix> <raw1> [raw2 ...]
 * 环境: CAMREC_ISP_LEGACY=1 旧路径；CAMREC_VFAST=1 快路
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "isp_lib.h"

int main(int argc,char**argv){
  if(argc<3){ fprintf(stderr,"usage: %s outprefix raw1 [raw2 ...]\n",argv[0]); return 1; }
  const char *op=argv[1];
  IspHalf isp;
  if(isp_half_init(&isp,61,"patch")!=0){ fprintf(stderr,"isp init fail\n"); return 2; }
  size_t fsz=(size_t)isp.w*isp.h;
  size_t rsz=4080UL*2448UL;
  unsigned char *raw=(unsigned char*)malloc(rsz);
  if(!raw){ fprintf(stderr,"oom\n"); return 2; }
  for(int k=2;k<argc;k++){
    FILE *f=fopen(argv[k],"rb");
    if(!f){ fprintf(stderr,"open fail %s\n",argv[k]); continue; }
    size_t got=fread(raw,1,rsz,f); fclose(f);
    if(got<rsz){ fprintf(stderr,"short %s %zu\n",argv[k],got); continue; }
    uint8_t *rgb=NULL;
    if(isp_half_process(&isp,raw,61,"patch",1.0,1.0,0.12,1.08,2.2,0,&rgb)!=0){ fprintf(stderr,"proc fail\n"); continue; }
    long sumr=0,sumg=0,sumb=0,clip=0;
    for(size_t i=0;i<fsz;i++){ int R=rgb[i*3],G=rgb[i*3+1],B=rgb[i*3+2];
      sumr+=R;sumg+=G;sumb+=B; int y=(299*R+587*G+114*B)/1000; if(y>=254)clip++; }
    char fn[512]; snprintf(fn,sizeof(fn),"%s_%02d.ppm",op,k-2);
    FILE *o=fopen(fn,"wb"); if(o){ fprintf(o,"P6\n%d %d\n255\n",isp.w,isp.h); fwrite(rgb,1,fsz*3,o); fclose(o); }
    printf("FRAME %d %s wb=%.3f/%.3f lo=%d hi=%d ev=%s meanR=%.1f meanG=%.1f meanB=%.1f clip=%.3f%% -> %s\n",
      k-2,argv[k],isp.wbr,isp.wbb,isp.lo,isp.hi,isp.last_event?isp.last_event:"-",
      (double)sumr/fsz,(double)sumg/fsz,(double)sumb/fsz,100.0*(double)clip/fsz,fn);
  }
  free(raw); isp_half_free(&isp);
  return 0;
}
