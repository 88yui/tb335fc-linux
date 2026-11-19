#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>
#include <errno.h>
#include <time.h>
int main(int argc,char**argv){
  const char *path=argc>1?argv[1]:"/dev/video3";
  int want=argc>2?atoi(argv[2]):12;
  int fd=open(path,O_RDWR|O_NONBLOCK); if(fd<0){perror("open");return 1;}
  struct v4l2_capability cap; if(ioctl(fd,VIDIOC_QUERYCAP,&cap)==0) printf("driver=%s card=%s caps=%x device_caps=%x\n",cap.driver,cap.card,cap.capabilities,cap.device_caps);
  struct v4l2_format f; memset(&f,0,sizeof f); f.type=V4L2_BUF_TYPE_VIDEO_CAPTURE;
  if(ioctl(fd,VIDIOC_G_FMT,&f)<0) perror("G_FMT");
  printf("fmt %ux%u fourcc=%08x bpl=%u size=%u\n",f.fmt.pix.width,f.fmt.pix.height,f.fmt.pix.pixelformat,f.fmt.pix.bytesperline,f.fmt.pix.sizeimage);
  struct v4l2_requestbuffers rb; memset(&rb,0,sizeof rb); rb.count=4; rb.type=V4L2_BUF_TYPE_VIDEO_CAPTURE; rb.memory=V4L2_MEMORY_MMAP;
  if(ioctl(fd,VIDIOC_REQBUFS,&rb)<0){perror("REQBUFS");return 1;}
  printf("reqbufs count=%u\n",rb.count);
  void *ptrs[8]; size_t lens[8];
  for(unsigned i=0;i<rb.count;i++){
    struct v4l2_buffer b; memset(&b,0,sizeof b); b.type=V4L2_BUF_TYPE_VIDEO_CAPTURE; b.memory=V4L2_MEMORY_MMAP; b.index=i;
    if(ioctl(fd,VIDIOC_QUERYBUF,&b)<0){perror("QUERYBUF");return 1;}
    lens[i]=b.length;
    ptrs[i]=mmap(NULL,b.length,PROT_READ|PROT_WRITE,MAP_SHARED,fd,b.m.offset);
    if(ptrs[i]==MAP_FAILED){perror("mmap");return 1;}
    if(ioctl(fd,VIDIOC_QBUF,&b)<0){perror("QBUF");return 1;}
  }
  int type=V4L2_BUF_TYPE_VIDEO_CAPTURE;
  if(ioctl(fd,VIDIOC_STREAMON,&type)<0){perror("STREAMON");return 1;}
  printf("STREAMON ok, waiting %d frames...\n",want); fflush(stdout);
  int got=0; time_t t0=time(NULL);
  while(got<want && time(NULL)-t0 < 25){
    fd_set rfd; FD_ZERO(&rfd); FD_SET(fd,&rfd);
    struct timeval tv={1,0};
    int sel=select(fd+1,&rfd,NULL,NULL,&tv);
    if(sel<=0) continue;
    struct v4l2_buffer b; memset(&b,0,sizeof b); b.type=V4L2_BUF_TYPE_VIDEO_CAPTURE; b.memory=V4L2_MEMORY_MMAP;
    if(ioctl(fd,VIDIOC_DQBUF,&b)<0){ perror("DQBUF"); usleep(20000); continue; }
    unsigned char *p=ptrs[b.index];
    printf("DQBUF idx=%u seq=%u bytesused=%u ts=%ld.%06ld first=%02x %02x %02x\n",b.index,b.sequence,b.bytesused,(long)b.timestamp.tv_sec,(long)b.timestamp.tv_usec,p[0],p[1],p[2]);
    fflush(stdout);
    got++;
    if(ioctl(fd,VIDIOC_QBUF,&b)<0) perror("QBUF2");
  }
  ioctl(fd,VIDIOC_STREAMOFF,&type);
  printf("GOT %d frames\n",got);
  close(fd); return 0;
}
