#!/usr/bin/env python3
# synthetic RGB24 feeder for /dev/camfeed (camv4l2 test) - fast version
import os, sys, time, struct, fcntl, errno
W,H=1632,1224
FOURCC=0x33424752  # V4L2_PIX_FMT_RGB24
BPL=W*3
SIZE=BPL*H
def IOW(t,nr,size): return (1<<30)|(size<<16)|(ord(t)<<8)|nr
def IOR(t,nr,size): return (2<<30)|(size<<16)|(ord(t)<<8)|nr
SET_FMT=IOW('F',1,20); GET_FMT=IOR('F',2,20); STATS=IOR('F',3,32)
n=int(sys.argv[1]) if len(sys.argv)>1 else 60
fd=os.open('/dev/camfeed', os.O_WRONLY|os.O_NONBLOCK)
try:
    fcntl.ioctl(fd, SET_FMT, struct.pack('IIIII', W,H,FOURCC,BPL,SIZE))
    print('FEED set_fmt ok')
except OSError as e:
    if e.errno == 16:
        cur=struct.unpack('IIIII', fcntl.ioctl(fd, GET_FMT, bytes(20)))
        print('FEED EBUSY (streaming); current fmt=%s'%(cur,))
        if cur[0]!=W or cur[1]!=H or cur[2]!=FOURCC:
            print('FEED FATAL fmt mismatch'); raise SystemExit(2)
    else:
        raise
row=bytearray(BPL)
for x in range(W):
    v=(x*255)//W
    row[x*3+0]=v; row[x*3+1]=v; row[x*3+2]=255-v
base=bytearray(row*H)
ok=0; eagain=0; err=0
t0=time.time()
# wait until V4L2 consumer has started streaming (up to 20s)
for _w in range(200):
    try:
        st=fcntl.ioctl(fd, STATS, bytes(32))
        fed,dropped,seq,streaming,queued=struct.unpack('QQQII', st)
        if streaming:
            print('FEED consumer streaming, queued=%d'%queued); break
    except OSError as e:
        print('STATS err', e); break
    time.sleep(0.1)
for i in range(n):
    x0=(i*37)%(W-40)
    o0=x0*3
    bar=b'\xff'*120
    for y in range(0,H):
        s=y*BPL+o0
        base[s:s+120]=bar
    try:
        os.write(fd, bytes(base))
        ok+=1
    except OSError as e:
        if e.errno in (errno.EAGAIN, errno.EWOULDBLOCK):
            eagain+=1
        else:
            err+=1
            print('WRITE_ERR', e)
            break
    time.sleep(0.02)
try:
    st=fcntl.ioctl(fd, STATS, bytes(32))
    fed,dropped,seq,streaming,queued=struct.unpack('QQQII', st)
    print('FEED ok=%d eagain=%d err=%d dt=%.2fs fed=%d dropped=%d seq=%d streaming=%d queued=%d'%(ok,eagain,err,time.time()-t0,fed,dropped,seq,streaming,queued))
except OSError as e:
    print('FEED ok=%d eagain=%d err=%d STATS_FAILED %s'%(ok,eagain,err,e))
os.close(fd)
