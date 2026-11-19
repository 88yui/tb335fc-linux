#!/usr/bin/env python3
"""RAW10(40bit LE packed) -> sRGB PNG preview with Bayer demosaic + chroma LPF + gray-world WB.

用法:
  python raw2png.py -o out_prefix [--preview 1280] f1.raw [f2.raw ...]
多帧输入会先在 RAW 域做时域平均（静态场景降噪；行噪声随机，平均 N 帧降 sqrt(N)）。
约定: 3264x2448, 每行 4080B = 816 组 x 5B, W=b0|b1<<8|b2<<16|b3<<24|b4<<32
      p0=W&1023 p1=(W>>10)&1023 p2=(W>>20)&1023 p3=(W>>30)&1023, Bayer=BGGR, 黑电平 61
"""
import argparse, numpy as np
from PIL import Image
W,H,ROWB,BL=3264,2448,4080,61.0
def unpack(path):
    b=np.fromfile(path,dtype=np.uint8)[:ROWB*H].reshape(H,816,5).astype(np.uint64)
    w=b[:,:,0]|(b[:,:,1]<<8)|(b[:,:,2]<<16)|(b[:,:,3]<<24)|(b[:,:,4]<<32)
    p=np.empty((H,816,4),np.uint16)
    p[:,:,0]=w&np.uint64(0x3FF); p[:,:,1]=(w>>np.uint64(10))&np.uint64(0x3FF)
    p[:,:,2]=(w>>np.uint64(20))&np.uint64(0x3FF); p[:,:,3]=(w>>np.uint64(30))&np.uint64(0x3FF)
    return p.reshape(H,W).astype(np.float32)
def box(a,k):
    pad=k//2; q=np.pad(a,pad,mode='reflect')
    c=np.cumsum(np.cumsum(q,0),1); c=np.pad(c,((1,0),(1,0)))
    return (c[k:,k:]-c[:-k,k:]-c[k:,:-k]+c[:-k,:-k])/(k*k)
def fin(ch,mask,k): return box(ch*mask,k)/np.maximum(box(mask.astype(np.float32),k),1e-6)
def render(A,f=4,expo=0.9):
    im=np.maximum(A-BL,0.0)
    yy,xx=np.mgrid[0:H,0:W]
    mR=((yy%2==1)&(xx%2==1)); mB=((yy%2==0)&(xx%2==0)); mG=~mR&~mB
    Gf=fin(im,mG,5)
    kR=fin(np.where(mR,im/np.maximum(Gf,1e-3),0.0),mR,9)
    kB=fin(np.where(mB,im/np.maximum(Gf,1e-3),0.0),mB,9)
    Rf=Gf*kR; Bf=Gf*kB
    gR=float(im[mR].mean()); gG=float(im[mG].mean()); gB=float(im[mB].mean())
    Rf*=min(max(gG/max(gR,1e-3),0.5),3.0); Bf*=min(max(gG/max(gB,1e-3),0.5),3.0)
    Y=0.299*Rf+0.587*Gf+0.114*Bf
    Cr=Rf-Y; Cb=Bf-Y
    def down(a):
        h,w=a.shape; return a.reshape(h//f,f,w//f,f).mean((1,3))
    def med3(a):
        st=np.stack([np.roll(np.roll(a,d,0),e,1) for d in (-1,0,1) for e in (-1,0,1)],0)
        return np.median(st,0)
    def up(a): return np.array(Image.fromarray(a.astype(np.float32)).resize((W,H),Image.BILINEAR))
    Crf=up(med3(down(Cr))); Cbf=up(med3(down(Cb)))
    R2=Y+Crf; B2=Y+Cbf; G2=(Y-0.299*R2-0.114*B2)/0.587
    lo=float(np.percentile(Y,0.5)); hi=float(np.percentile(Y,99.8))
    x=np.clip((np.stack([R2,G2,B2],-1)-lo)/max(hi-lo,1e-3),0,1)**expo
    return (np.clip(x,0,1)**(1/2.2)*255).astype(np.uint8)
def main():
    ap=argparse.ArgumentParser()
    ap.add_argument("-o",default="out"); ap.add_argument("--preview",type=int,default=1280)
    ap.add_argument("--chroma",type=int,default=4); ap.add_argument("files",nargs="+")
    a=ap.parse_args()
    acc=None; n=0
    for f in a.files:
        fr=unpack(f); acc=fr if acc is None else acc+fr; n+=1
        print("loaded",f,"mean=%.2f"%fr.mean())
    acc/=n
    print("frames=%d  raw mean=%.2f"%(n,acc.mean()))
    img=render(acc,f=a.chroma)
    Image.fromarray(img).save(a.o+"_full.png")
    pw=int(a.preview); ph=int(round(pw*H/W))
    Image.fromarray(img).resize((pw,ph),Image.BOX).save(a.o+"_preview.png")
    print("saved",a.o+"_full.png",a.o+"_preview.png")
if __name__=="__main__": main()
