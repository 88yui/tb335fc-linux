#!/usr/bin/env python3
"""raw2video.py -- 把 RAW10 帧序列渲染成 PNG 帧 (AE 阶跃演示), 固定/平滑 WB + 新 tone + RAW 域 TNR.

用法 (旧默认行为逐字节保持):
  python raw2video.py --dir <raw_dir> --prefix f- --start 0 --end 29 --out <png_dir>
                       [--ref 12] [--width 1280] [--expo 0.9] [--contrast 1.10] [--csup 0.12]

新增能力 (任一非默认即走新渲染路径):
  --awb-est v2 --awb-smooth iir --tnr-alpha 0.35 --tnr-norm p995 --tone v2
旧默认: 固定 WB/tone 取自 --ref 帧 (避免逐帧 AWB/tone 抖动), 不做 TNR.
"""
import sys, os, argparse, numpy as np
from PIL import Image
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import autoisp as A

H0,W0,ROWB,BL = 2448,3264,4080,61.0
GH,GW = H0//2, W0//2

def unpack_half(path):
    b=np.fromfile(path,dtype=np.uint8)[:ROWB*H0].reshape(H0,816,5).astype(np.uint64)
    w=b[:,:,0]|(b[:,:,1]<<np.uint64(8))|(b[:,:,2]<<np.uint64(16))|(b[:,:,3]<<np.uint64(24))|(b[:,:,4]<<np.uint64(32))
    p=np.empty((H0,816,4),np.uint16)
    p[:,:,0]=w&np.uint64(0x3FF); p[:,:,1]=(w>>np.uint64(10))&np.uint64(0x3FF)
    p[:,:,2]=(w>>np.uint64(20))&np.uint64(0x3FF); p[:,:,3]=(w>>np.uint64(30))&np.uint64(0x3FF)
    bayer=p.reshape(H0,W0).astype(np.float32)
    def q(a): return a.reshape(a.shape[0]//2,2,a.shape[1]//2,2).mean((1,3))
    Bs=q(bayer[0::2,0::2]); Rs=q(bayer[1::2,1::2])
    Gs=(q(bayer[0::2,1::2])+q(bayer[1::2,0::2]))*0.5
    hb=np.empty((GH,GW),np.float32)
    hb[0::2,0::2]=Bs; hb[1::2,1::2]=Rs
    hb[0::2,1::2]=Gs; hb[1::2,0::2]=Gs
    return hb

def fixed_tone(Rf,Gf,Bf, lo, hi, expo, contrast, csup):
    Y=0.299*Rf+0.587*Gf+0.114*Bf
    if csup>0:
        t=np.clip(Y/max(csup*hi,1e-3),0,1); f=t*t*(3-2*t)
        Rf=Y+(Rf-Y)*f; Gf=Y+(Gf-Y)*f; Bf=Y+(Bf-Y)*f
    scale=255.0/max(hi-lo,1.0)
    out=np.stack([np.clip((Rf-lo)*scale,0,255)/255.0,
                  np.clip((Gf-lo)*scale,0,255)/255.0,
                  np.clip((Bf-lo)*scale,0,255)/255.0],-1)
    out=np.clip(out,0,1)**expo
    out=np.clip(out,0,1)**(1/2.2)
    out=np.clip(0.45+(out-0.45)*contrast,0,1)
    return (out*255).astype(np.uint8)

def main():
    ap=argparse.ArgumentParser()
    ap.add_argument('--dir',required=True); ap.add_argument('--prefix',default='f-')
    ap.add_argument('--start',type=int,default=0); ap.add_argument('--end',type=int,default=29)
    ap.add_argument('--out',required=True); ap.add_argument('--ref',type=int,default=12)
    ap.add_argument('--width',type=int,default=1280)
    ap.add_argument('--expo',type=float,default=0.9); ap.add_argument('--contrast',type=float,default=1.10)
    ap.add_argument('--csup',type=float,default=0.12); ap.add_argument('--sharpen',type=float,default=0.5)
    # ---- AWB 时域平滑 (新) ----
    ap.add_argument('--awb',default='patch',choices=['gray','patch','hybrid'],help='AWB 估计模式')
    ap.add_argument('--awb-est',default='v1',choices=['v1','v2'])
    ap.add_argument('--awb-smooth',default='off',choices=['off','iir','median','median-iir'])
    ap.add_argument('--awb-alpha',type=float,default=0.35)
    ap.add_argument('--awb-median',type=int,default=5)
    ap.add_argument('--awb-thresh',type=float,default=0.08)
    ap.add_argument('--awb-fast',type=float,default=0.85)
    ap.add_argument('--awb-reset',type=float,default=0.30)
    ap.add_argument('--awb-hold',type=float,default=0.45)
    # ---- RAW 域时域降噪 (新) ----
    ap.add_argument('--tnr-alpha',type=float,default=None,help='IIR 新帧权重; 不给=等权平均; 需配合 --tnr-norm')
    ap.add_argument('--tnr-norm',default='none',choices=['none','p995','mean'])
    ap.add_argument('--tnr-reset',type=float,default=16.0,help='相邻帧亮度尺度比超限则重置 (默认 16 = 允许 4 档线性 AE 阶跃)')
    ap.add_argument('--tnr-sad',type=float,default=0.06,help='归一化域内容突变(SAD)重置门限')
    ap.add_argument('--tnr-fit',default='affine',choices=['bl','affine','none'],
                    help='曝光归一化增益估计: affine=原始域最小二乘(默认)/bl=减黑电平域过原点/none=p99.5 比值')
    ap.add_argument('--tnr-denorm',default='scale',choices=['fit','scale'],
                    help='曝光尺度还原: scale=p99.5 尺度比(默认, 亮度贴合单帧)/fit=拟合增益比')
    ap.add_argument('--tnr-log',default='',help='把逐帧 TNR/AWB 事件写到该 json 路径')
    # ---- tone v2 / 色度降噪 (新) ----
    ap.add_argument('--tone',default='legacy',choices=['legacy','v2'])
    ap.add_argument('--tone-lo-pct',type=float,default=0.5)
    ap.add_argument('--tone-hi-pct',type=float,default=100.0)
    ap.add_argument('--shadow-lift',type=float,default=1.0)
    ap.add_argument('--tone-contrast',type=float,default=1.08)
    ap.add_argument('--tone-pivot',type=float,default=0.5)
    ap.add_argument('--tone-gamma',type=float,default=1.0)
    ap.add_argument('--detail-gain',type=float,default=0.65)
    ap.add_argument('--detail-range',type=float,default=0.35)
    ap.add_argument('--clarity',type=float,default=0.15)
    ap.add_argument('--tone-sat',type=float,default=1.0)
    ap.add_argument('--highlight-desat',type=float,default=0.8)
    ap.add_argument('--tone-base-window',type=int,default=15)
    ap.add_argument('--chroma-nr',type=int,default=1,help='色度中值半径 0..3 (legacy=1)')
    ap.add_argument('--demosaic-chroma',type=int,default=4,help='色度下采样因子 (legacy=4)')
    a=ap.parse_args()
    os.makedirs(a.out,exist_ok=True)
    A.H,A.W=GH,GW
    new_awb = (a.awb_est!='v1') or (a.awb_smooth!='off')
    new_tnr = (a.tnr_alpha is not None) or (a.tnr_norm!='none')
    new_tone = (a.tone!='legacy')
    modern = new_awb or new_tnr or new_tone or a.chroma_nr!=1 or a.demosaic_chroma!=4
    ref=unpack_half(os.path.join(a.dir,'%s%04d.raw'%(a.prefix,a.ref)))
    # ---- 参考帧 WB ----
    if new_awb:
        gR,gB,ainfo=A.awb_estimate(ref,a.awb,a.awb_est)
    else:
        gR,gB,_=A.awb_gains(ref,'patch'); ainfo={}
    sm=A.AwbSmoother(mode=a.awb_smooth,alpha=a.awb_alpha,median=a.awb_median,
                     thresh=a.awb_thresh,fast=a.awb_fast,reset=a.awb_reset,hold_frac=a.awb_hold) if new_awb else None
    st=A.TnrStream(alpha=a.tnr_alpha,norm=a.tnr_norm,reset_ratio=a.tnr_reset,reset_sad=a.tnr_sad,
                   fit_mode=a.tnr_fit,denorm_mode=a.tnr_denorm) if new_tnr else None
    if sm is not None:
        sm.update(gR,gB,ainfo.get('valid_frac',1.0))
    # ---- 参考帧 tone 窗口 ----
    img=np.maximum(ref-BL,0.0)
    Rf,Gf,Bf=A.demosaic(img,gR,gB,chroma=a.demosaic_chroma,cnr=a.chroma_nr)
    Y=0.299*Rf+0.587*Gf+0.114*Bf
    if new_tone:
        lo,hi=np.percentile(Y,[a.tone_lo_pct,a.tone_hi_pct])
    else:
        lo,hi=np.percentile(Y,[0.5,99.7])
    print('ref=%d gR=%.4f gB=%.4f lo=%.1f hi=%.1f mode=%s'%(
        a.ref,gR,gB,lo,hi,'modern' if modern else 'legacy'),flush=True)
    logs=[]
    for i in range(a.start,a.end+1):
        p=os.path.join(a.dir,'%s%04d.raw'%(a.prefix,i))
        if not os.path.exists(p) or os.path.getsize(p)<ROWB*H0:
            print('skip',i,flush=True); continue
        hb=unpack_half(p)
        if st is not None:
            st.update(hb); hb=st.denorm()
        if sm is not None:
            eR,eB,ainfo2=A.awb_estimate(hb,a.awb,a.awb_est)
            gnew,ev=sm.update(eR,eB,ainfo2.get('valid_frac',1.0))
            gR,gB=gnew
            logs.append(dict(frame=i,awb=ev.get('event')))
        if st is not None: logs.append(dict(frame=i,tnr=st.log[-1]['event'],scale=st.log[-1]['scale'],sad=st.log[-1]['sad']))
        Rf,Gf,Bf=A.demosaic(np.maximum(hb-BL,0.0),gR,gB,chroma=a.demosaic_chroma,cnr=a.chroma_nr)
        if new_tone:
            rgb=A.tone_v2(Rf,Gf,Bf,expo=a.expo,lo_pct=a.tone_lo_pct,hi_pct=a.tone_hi_pct,
                          shadow_lift=a.shadow_lift,contrast=a.tone_contrast,pivot=a.tone_pivot,
                          gamma=a.tone_gamma,detail_gain=a.detail_gain,detail_range=a.detail_range,
                          clarity=a.clarity,csup=a.csup,sat=a.tone_sat,desat_hi=a.highlight_desat,
                          base_window=a.tone_base_window,lo_fix=lo,hi_fix=hi)
        else:
            rgb=fixed_tone(Rf,Gf,Bf,lo,hi,a.expo,a.contrast,a.csup)
        if a.sharpen>0: rgb=A.sharpen(rgb,a.sharpen)
        im=Image.fromarray(rgb).resize((a.width,int(a.width*GH/GW)),Image.LANCZOS)
        im.save(os.path.join(a.out,'v%04d.png'%i))
        print('frame',i,flush=True)
    if a.tnr_log and logs:
        import json; json.dump(logs,open(a.tnr_log,'w'),indent=1)
    print('DONE')

if __name__=='__main__': main()
