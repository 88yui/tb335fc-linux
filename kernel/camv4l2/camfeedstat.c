#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
struct camfeed_stats { unsigned long long fed, dropped, seq; unsigned int streaming, queued; };
#define CAMFEED_STATS _IOR('F', 3, struct camfeed_stats)
int main(void){
  int fd=open("/dev/camfeed",O_RDWR);
  struct camfeed_stats s;
  if(fd<0){ printf("-1 0 0 0 0\n"); return 1; }
  memset(&s,0,sizeof s);
  if(ioctl(fd,CAMFEED_STATS,&s)<0){ printf("-1 0 0 0 0\n"); close(fd); return 1; }
  printf("%u %llu %llu %llu %u\n", s.streaming, s.fed, s.dropped, s.seq, s.queued);
  close(fd);
  return 0;
}