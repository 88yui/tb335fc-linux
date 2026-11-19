#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
struct camfeed_fmt { unsigned int width,height,fourcc,bpl,sizeimage; };
#define CAMFEED_GET_FMT _IOR('F',2,struct camfeed_fmt)
int main(int argc,char**argv){
  const char *p = argc>1?argv[1]:"/dev/camfeed";
  int fd=open(p,O_RDWR);
  if(fd<0){ printf("open fail\n"); return 1; }
  struct camfeed_fmt f; memset(&f,0,sizeof f);
  if(ioctl(fd,CAMFEED_GET_FMT,&f)!=0){ printf("ioctl fail\n"); close(fd); return 2; }
  printf("%u %u %08X %u %u\n", f.width, f.height, f.fourcc, f.bpl, f.sizeimage);
  close(fd); return 0;
}