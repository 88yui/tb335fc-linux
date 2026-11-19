#!/usr/bin/env python3
"""autoisp.py -- RAW10(40bit LE, BGGR) -> sRGB PNG with AE-frame averaging, AWB, demosaic, chroma NR, tone.

用法 (legacy, 输出与旧版逐字节一致):
  python autoisp.py -o out_prefix [--preview 1280] [--awb gray|patch|hybrid] [--frames N]
                    [--expo 0.9] [--sat 1.0] [--contrast 1.0] [--csup 0.10] f1.raw [f2.raw ...]

新增 (全部 opt-in, 默认关闭/保持 legacy):
  --awb-est v2            在减黑电平后的块均值上估计 AWB (消除增益随亮度漂移), 已用标定系数对齐旧工作点
  --awb-smooth iir|median|median-iir    AWB 时域平滑, --awb-alpha/--awb-median/--awb-thresh 调参
  --tnr-norm p995|mean    RAW 域时域降噪的曝光归一化方式 (+ --tnr-alpha/--tnr-frames/--tnr-reset)
  --tnr-fit affine|bl|none 曝光归一化增益估计方式 (默认 affine) ; --tnr-denorm scale|fit 尺度还原
  --tone v2               新 tone (有理 shoulder 曲线 + 暗部细节保护 + 高光保留 + 色度降噪强度可调)
"""
import argparse, numpy as np, os, json
from PIL import Image

W,H,ROWB,BL = 3264,2448,4080,61.0

# ---- AWB: v2 估计器把"减黑电平后块均值"的估计乘以下列标定系数, 对齐已验收的 legacy 工作点 ----
AWB_CAL = {  # mode -> (calR, calB):  legacy_gain / v2_gain  在 ae2 8帧平均上测得
    'patch':  (0.92519, 0.94922),
    'gray':   (0.83926, 0.88441),
    'hybrid': (0.88394, 0.91930),
}

def unpack(path):
    b=np.fromfile(path,dtype=np.uint8)[:ROWB*H].reshape(H,816,5).astype(np.uint64)
    w=b[:,:,0]|(b[:,:,1]<<np.uint64(8))|(b[:,:,2]<<np.uint64(16))|(b[:,:,3]<<np.uint64(24))|(b[:,:,4]<<np.uint64(32))
    p=np.empty((H,816,4),np.uint16)
    p[:,:,0]=w&np.uint64(0x3FF); p[:,:,1]=(w>>np.uint64(10))&np.uint64(0x3FF)
    p[:,:,2]=(w>>np.uint64(20))&np.uint64(0x3FF); p[:,:,3]=(w>>np.uint64(30))&np.uint64(0x3FF)
    return p.reshape(H,W).astype(np.float32)

def planes(bayer):
    return (bayer[0::2,0::2], bayer[1::2,1::2], bayer[1::2,0::2])  # B, R, G  (BGGR: (0,0)=B,(1,1)=R)

def box(a,k):
    pad=k//2; q=np.pad(a,pad,mode='reflect')
    c=np.cumsum(np.cumsum(q,0),1); c=np.pad(c,((1,0),(1,0)))
    return (c[k:,k:]-c[:-k,k:]-c[k:,:-k]+c[:-k,:-k])/(k*k)

def fin(ch,mask,k): return box(ch*mask,k)/np.maximum(box(mask.astype(np.float32),k),1e-6)

def block_stats(bayer, blk=8):
    """downsample planes into blk x blk blocks -> mean per plane, valid mask"""
    B,R,G = planes(bayer)
    def bm(a):
        h,w=a.shape; h2,w2=h//blk,w//blk
        return a[:h2*blk,:w2*blk].reshape(h2,blk,w2,blk).mean((1,3))
    bB,bR,bG = bm(B),bm(R),bm(G)
    mx = np.maximum(np.maximum(bB,bR),bG)
    mn = np.minimum(np.minimum(bB,bR),bG)
    return bB,bR,bG,mx,mn

def awb_gains(bayer, mode='hybrid'):
    """legacy AWB (v1): 直接在带黑电平的块均值上估计"""
    bB,bR,bG,mx,mn = block_stats(bayer)
    valid = (mx<1000)&(bG>BL+12)      # 非饱和、非死黑
    vmid  = valid & (bG<0.6*1000)     # 中低亮度块
    if mode=='patch':
        # 白块法：取最亮 1.0% 有效块的各通道均值作为白点
        cand = valid & (bG>np.percentile(bG[valid],99.0))
        mB,mR,mG = bB[cand].mean(), bR[cand].mean(), bG[cand].mean()
    elif mode=='gray':
        mB,mR,mG = bB[vmid].mean(), bR[vmid].mean(), bG[vmid].mean()
    else:  # hybrid: 灰世界为主体，白块只少量牵引
        mB,mR,mG = bB[vmid].mean(), bR[vmid].mean(), bG[vmid].mean()
        cand = valid & (bG>np.percentile(bG[valid],99.0))
        wB,wR,wG = bB[cand].mean(), bR[cand].mean(), bG[cand].mean()
        a=0.35
        mB=(1-a)*mB+a*wB; mR=(1-a)*mR+a*wR; mG=(1-a)*mG+a*wG
    gR=float(np.clip(mG/max(mR,1e-3),0.55,2.2))
    gB=float(np.clip(mG/max(mB,1e-3),0.55,2.2))
    return gR,gB,(float(mR),float(mG),float(mB))

def awb_gains_v2(bayer, mode='patch', bl=BL, calibrate=True):
    """v2 AWB: 先减黑电平再统计, 消除低亮度下黑电平平台导致的增益漂移.

    返回 (gR, gB, info); info 含 valid_frac (有效块比例, 低光下用于 hold) 与块均值.
    """
    x = np.maximum(bayer-bl,0.0)
    bB,bR,bG,mx,mn = block_stats(x)
    valid = (mx<940.0)&(bG>10.0)          # 减黑后: 非饱和、非死黑
    nv = int(valid.sum())
    if nv < 128:                           # 极暗/异常帧: 放宽门限
        valid = (mx<940.0)&(bG>1e-3)
        nv = int(valid.sum())
    if nv == 0:
        return 1.0,1.0,dict(est='v2',mode=mode,valid_frac=0.0,means=(1.0,1.0,1.0),cal=(1.0,1.0))
    vf = float(valid.mean())
    vmid = valid&(bG<611.0)                # 0.65*940
    if mode=='patch':
        cand = valid&(bG>np.percentile(bG[valid],99.0))
    else:
        cand = vmid
    if int(cand.sum())<16: cand = valid
    mB,mR,mG = float(bB[cand].mean()),float(bR[cand].mean()),float(bG[cand].mean())
    if mode=='hybrid':
        wc = valid&(bG>np.percentile(bG[valid],99.0))
        if int(wc.sum())>=16:
            a=0.35
            mB=(1-a)*mB+a*float(bB[wc].mean())
            mR=(1-a)*mR+a*float(bR[wc].mean())
            mG=(1-a)*mG+a*float(bG[wc].mean())
    cal = AWB_CAL.get(mode,(1.0,1.0)) if calibrate else (1.0,1.0)
    gR=float(np.clip(mG/max(mR,1e-3)*cal[0],0.55,2.2))
    gB=float(np.clip(mG/max(mB,1e-3)*cal[1],0.55,2.2))
    return gR,gB,dict(est='v2',mode=mode,valid_frac=vf,means=(mR,mG,mB),cal=(cal[0],cal[1]))

def awb_estimate(bayer, mode='patch', est='v1'):
    """统一入口: 返回 (gR,gB,info)"""
    if est=='v1':
        gR,gB,means = awb_gains(bayer,mode)
        return gR,gB,dict(est='v1',mode=mode,valid_frac=1.0,means=[float(z) for z in means],cal=(1.0,1.0))
    return awb_gains_v2(bayer,mode)

class AwbSmoother:
    """AWB 增益时域平滑器.

    mode: off | iir | median | median-iir
    alpha: IIR 新样本权重 (默认 0.35); median: 滑动中值窗口 (默认 5)
    thresh: 相对变化超过该值 -> 用 fast 权重快速跟上 (默认 0.08)
    reset:  相对变化超过该值 -> 直接重置 (默认 0.30)
    hold_frac: 估计器 valid_frac 低于该值 -> hold 旧增益 (默认 0.45)
    """
    def __init__(self, mode='off', alpha=0.35, median=5, thresh=0.08, fast=0.85,
                 reset=0.30, hold_frac=0.45):
        self.mode=mode; self.alpha=float(alpha); self.median=max(1,int(median))
        self.thresh=float(thresh); self.fast=float(fast); self.reset=float(reset)
        self.hold_frac=float(hold_frac)
        self.g=None; self.hist=[]; self.n=0

    def update(self, gR, gB, valid_frac=1.0):
        g=(float(gR),float(gB))
        if self.mode=='off':
            self.g=g; self.n+=1
            return g, dict(event='off', raw=g)
        if self.g is None:
            self.g=g; self.hist=[g]; self.n=1
            return g, dict(event='init', raw=g)
        raw=g; ev='hold'
        if valid_frac < self.hold_frac:
            self.n+=1
            return self.g, dict(event='hold', raw=raw, value=self.g)
        self.hist.append(g)
        if len(self.hist)>self.median: self.hist.pop(0)
        if self.mode=='median' or self.mode=='median-iir':
            h=np.array(self.hist)
            est=(float(np.median(h[:,0])),float(np.median(h[:,1])))
        else:
            est=g
        rel=max(abs(est[0]/max(self.g[0],1e-6)-1.0), abs(est[1]/max(self.g[1],1e-6)-1.0))
        if rel > self.reset:
            self.g=est; ev='reset'
        elif rel > self.thresh:
            a=self.fast
            self.g=(self.g[0]*(1-a)+est[0]*a, self.g[1]*(1-a)+est[1]*a); ev='fast'
        elif self.mode=='median':
            self.g=est; ev='median'
        else:
            a=self.alpha
            self.g=(self.g[0]*(1-a)+est[0]*a, self.g[1]*(1-a)+est[1]*a); ev='iir'
        self.n+=1
        return self.g, dict(event=ev,raw=raw,est=est,value=self.g,rel=rel)

# ---------------- RAW 域时域降噪 ----------------
def frame_scale(bayer, norm='p995', bl=BL):
    """单帧亮度尺度, 用于 RAW 域多帧曝光归一化"""
    if norm=='none': return 1.0
    if norm=='mean': return float(max(float(bayer.mean())-bl,1e-3))
    return float(max(np.percentile(bayer[::4,::4],99.5)-bl,1e-3))

def _blk(a,k=8):
    """8x8 块均值 (SAD/场景变化检测用, 抑制像素噪声)"""
    h,w=a.shape
    return a[:h//k*k,:w//k*k].reshape(h//k,k,w//k,k).mean((1,3))

class TnrStream:
    """RAW 域时域降噪流式状态机 (autoisp 多帧 / raw2video 逐帧共用).

    alpha: 新帧权重; None -> 等权平均 (legacy 多帧平均)
    norm:  'p995'|'mean' 曝光归一化尺度; 'none' 不归一化
    reset_ratio: 相邻帧亮度尺度比 (max(ratio/prev, prev/ratio)) 超限 -> 归一化失效, 重置
                 (默认 16.0 ≈ 4 档: 允许线性 AE 大阶跃由归一化吸收, 只挡灾难性跳变)
    reset_sad:   归一化域 8x8 块均值 SAD (去除全局偏置后, 相对参考亮度) 超限 -> 场景内容突变, 重置
                 (去偏是为了容忍 p99.5 尺度估计在极暗帧留下的黑电平/偏置失配)
    累加器 out 位于参考帧(=首帧或 reset 帧)曝光尺度; denorm() 可还原到当前帧尺度.
    """
    def __init__(self, alpha=None, norm='p995', reset_ratio=16.0, reset_sad=0.06, bl=BL,
                 fit_affine=True, fit_mode=None, denorm_mode='scale'):
        self.alpha=alpha; self.norm=norm; self.reset_ratio=reset_ratio
        self.reset_sad=reset_sad; self.bl=bl; self.fit_affine=fit_affine
        # fit_mode: 'bl'   -> (b-BL) ~ k*(out-BL) 过原点拟合 (推荐; 由高光/中亮段主导, 非线性暗端不参与)
        #           'affine'-> b ~ k*out + c 原始域最小二乘 (暗端块多, 极暗帧 k 偏低)
        #           'none'  -> 只用 p99.5 尺度比 (legacy 行为)
        if fit_mode is None: fit_mode='affine' if fit_affine else 'none'
        self.fit_mode=fit_mode
        # denorm_mode: 'fit'   用逐帧拟合增益比 k 还原到当前帧曝光尺度 (与归一化一致)
        #              'scale' 用 p99.5 尺度比还原 (显示亮度更贴近单帧渲染, 对极暗帧不低估)
        self.denorm_mode=denorm_mode
        self.out=None; self.ref=None; self.prev=1.0; self.n=0; self.log=[]; self.last_scale=1.0
        self.last_k=1.0; self.events=0
    def _fit_gain(self, b):
        """用 8x8 块均值对 (当前帧, 累加器) 估计增益比 k 与黑电平偏置 c.

        返回 (k, c, ok). 归一化 x=(b-c)/k 的作用: 不同 gain/曝光下黑电平会漂移几个 DN,
        只按 p99.5 比值缩放会把该偏置放大 (极暗帧可放大到 ~18DN), 造成混合后亮度瞬变;
        拟合同时吸收偏置 c. 'bl' 模式在减黑电平域过原点回归, 对极暗帧的非线性暗端
        (veiling glare/黑电平漂移) 不敏感, 与高光曝光比一致.
        """
        bo=_blk(self.out); bx=_blk(b)
        m=(bo>self.bl+0.03*max(self.ref,1.0))&(bo<900.0)&(bx>self.bl+1.0)
        if int(m.sum())<32: return None,0.0,False
        if self.fit_mode=='bl':
            xb=bo[m]-self.bl; yb=bx[m]-self.bl
            den=float(np.dot(xb,xb))
            if den<1e-3: return None,0.0,False
            k=float(np.dot(xb,yb)/den)
            if k<=1e-6: return None,0.0,False
            return k, float(self.bl*(1.0-k)), True
        bv=bo[m]-bo[m].mean(); av=bx[m]-bx[m].mean()
        den=float(np.dot(bv,bv))
        if den<1e-3: return None,0.0,False
        k=float(np.dot(av,bv)/den); c=float(bx[m].mean()-k*bo[m].mean())
        if k<=1e-6: return None,0.0,False
        return k,c,True
    def update(self, b, name=None):
        b=np.asarray(b,dtype=np.float32)
        s=frame_scale(b,self.norm,self.bl)
        self.last_scale=s
        if self.out is None:
            self.out=np.array(b,dtype=np.float32); self.ref=s; self.prev=1.0; self.n=1; self.last_k=1.0
            e=dict(frame=name,scale=s,ratio=1.0,k=1.0,sad=0.0,event='init',alpha=1.0)
            self.log.append(e); return self.out, e
        if self.norm=='none':
            x=b.copy(); k=s/max(self.ref,1e-6)
        else:
            k=c=None
            if self.fit_mode!='none':
                k,c,ok=self._fit_gain(b)
                if not ok: k=None
            if k is not None and (1.0/32.0)<=k<=32.0:
                x=(b-c)/k                       # 仿射归一化: 斜率 + 黑电平偏置
            else:
                k=s/max(self.ref,1e-6)
                x=(b-self.bl)*(self.ref/max(s,1e-6))+self.bl   # 回退: 只按 p99.5 缩放
        ratio=s/max(self.ref,1e-6)
        scale=max(self.ref,1.0)
        diff=_blk(x)-_blk(self.out)
        d=float(np.mean(np.abs(diff-np.mean(diff))))/scale   # 去全局偏置: 只惩罚结构差异
        jump=max(k/max(self.prev,1e-6), max(self.prev,1e-6)/max(k,1e-6))
        ev='blend'; a=self.alpha
        if self.reset_ratio>0 and jump>self.reset_ratio: ev='reset-bright'
        elif self.reset_sad>0 and d>self.reset_sad: ev='reset-scene'
        if ev=='blend':
            self.n+=1
            a=self.alpha if self.alpha is not None else 1.0/self.n
            self.out=self.out*(1.0-a)+x*a
        else:
            self.out=x; self.n=1; a=1.0; self.prev=k; self.last_k=k; self.events+=1
            e=dict(frame=name,scale=s,ratio=ratio,k=k,sad=d,event=ev,alpha=a)
            self.log.append(e); return self.out, e
        self.prev=k; self.last_k=k
        e=dict(frame=name,scale=s,ratio=ratio,k=k,sad=d,event=ev,alpha=a)
        self.log.append(e); return self.out, e
    def denorm(self, scale=None, k=None):
        """累加器(参考曝光尺度) -> 当前帧曝光尺度.

        denorm_mode='scale' 时用 p99.5 尺度比 (显示亮度与单帧渲染一致, 极暗帧不被拟合 k 低估);
        否则优先用逐帧拟合增益比 k, 回退 p99.5 比值.
        """
        if self.denorm_mode=='scale':
            s=self.last_scale if scale is None else scale
            f=(s/max(self.ref,1e-6))
        elif k is not None: f=k
        elif scale is not None: f=(scale/max(self.ref,1e-6))
        else: f=self.last_k
        if f<=0: return self.out
        return (self.out-self.bl)*f+self.bl

def tnr_blend(frames, alpha=None, norm='p995', reset_ratio=16.0, reset_sad=0.06, bl=BL,
              fit_mode='affine', denorm_mode='scale'):
    """批量接口: 对帧序列做 RAW 域 TNR, 返回 (参考尺度下的累加结果, 逐帧日志)"""
    st=TnrStream(alpha,norm,reset_ratio,reset_sad,bl,fit_mode=fit_mode,denorm_mode=denorm_mode)
    for name,b in frames: st.update(b,name)
    return np.asarray(st.out,np.float32), st.log

def tnr_denorm(out, ref_scale, cur_scale, bl=BL):
    """把参考曝光尺度下的 TNR 输出还原到当前帧曝光尺度 (保持 AE 亮度轨迹不变)"""
    if ref_scale<=0 or cur_scale<=0: return out
    k=cur_scale/ref_scale
    return (out-bl)*k+bl

# ---------------- demosaic / tone ----------------
def demosaic(im, gR, gB, chroma=4, cnr=1):
    """chroma=下采样因子(legacy=4); cnr=色度中值半径(0/1/2/3, legacy=1)"""
    yy,xx=np.mgrid[0:H,0:W]
    mR=((yy%2==1)&(xx%2==1)); mB=((yy%2==0)&(xx%2==0)); mG=~mR&~mB
    Gf=fin(im,mG,5)
    kR=fin(np.where(mR,im/np.maximum(Gf,1e-3),0.0),mR,9)
    kB=fin(np.where(mB,im/np.maximum(Gf,1e-3),0.0),mB,9)
    Rf=Gf*kR*gR; Bf=Gf*kB*gB
    Y=0.299*Rf+0.587*Gf+0.114*Bf
    Cr=Rf-Y; Cb=Bf-Y
    def down(a):
        h,w=a.shape; return a.reshape(h//chroma,chroma,w//chroma,chroma).mean((1,3))
    def med(a,r):
        if r<=0: return a
        st=np.stack([np.roll(np.roll(a,d,0),e,1) for d in range(-r,r+1) for e in range(-r,r+1)],0)
        return np.median(st,0)
    def up(a):
        im_=Image.fromarray(a.astype(np.float32),mode='F').resize((W,H),Image.BILINEAR)
        return np.array(im_)
    Cr=up(med(down(Cr),cnr)); Cb=up(med(down(Cb),cnr))
    return np.clip(Y+Cr,0,None), np.clip(Y,0,None), np.clip(Y+Cb,0,None)

def tone(Rf,Gf,Bf, expo=0.9, sat=1.0, contrast=1.0, csup=0.10):
    """legacy tone (保持逐字节兼容)"""
    Y=0.299*Rf+0.587*Gf+0.114*Bf
    lo,hi=np.percentile(Y[::8,::8],[0.5,99.7])
    hi=max(hi,lo+1)
    # chroma suppression in shadows (linear): scale chroma by smoothstep(Y/(csup*hi))
    if csup>0:
        t=np.clip(Y/max(csup*hi,1e-3),0,1)
        f=t*t*(3-2*t)
        Rf=Y+(Rf-Y)*f
        Gf=Y+(Gf-Y)*f
        Bf=Y+(Bf-Y)*f
    scale=255.0/(hi-lo)
    R=np.clip((Rf-lo)*scale,0,255)/255.0
    G=np.clip((Gf-lo)*scale,0,255)/255.0
    B=np.clip((Bf-lo)*scale,0,255)/255.0
    out=np.stack([R,G,B],-1)
    if abs(sat-1.0)>1e-3:
        Yn=0.299*out[...,0]+0.587*out[...,1]+0.114*out[...,2]
        out=Yn[...,None]+(out-Yn[...,None])*sat
        out=np.clip(out,0,1)
    out=np.clip(out,0,1)**expo
    out=np.clip(out,0,1)**(1/2.2)
    if abs(contrast-1.0)>1e-3:
        # gentle S-curve around 0.45, keeps highlights from clipping hard
        x=out
        out=np.clip(0.45+(x-0.45)*contrast,0,1)
        out=np.clip(out,0,1)**(1.0 if contrast<=1.0 else 1.0)
    return (out*255).astype(np.uint8)

def tone_v2(Rf,Gf,Bf, expo=1.0, lo_pct=0.5, hi_pct=100.0, shadow_lift=1.0,
            contrast=1.08, pivot=0.5, gamma=1.0, detail_gain=0.65, detail_range=0.35,
            clarity=0.15, csup=0.12, sat=1.0, desat_hi=0.8, base_window=15,
            lo_fix=None, hi_fix=None):
    """新 tone:
    1) 黑/白点取 [lo_pct, hi_pct] 百分位; 白点可取到 100% 以保住高光层次(旧版 99.7 直接硬剪)
    2) 亮度分两带: Yb=两次 box 平滑的 base, Yd=Y-Yb 细节
    3) 有理曲线 u=t*(1+k)/(1+k*t): 暗部提升(k)+高光 shoulder, 单调无剪切
    4) 细节按亮度给增益(detail_gain 暗部 <1, 高光 1.0): 暗部提亮但不放大噪点
    5) clarity: 细节带额外锐化(大半径 USM)
    6) tone 只作用于 luma, 色度按线性域 channel/Y 比例重建 -> 不被 gamma 压成灰(旧版发灰的主因)
    7) 显示域: contrast -> saturation -> 高光去饱和; 暗部 csup 抑制色噪
    """
    Y=0.299*Rf+0.587*Gf+0.114*Bf
    if lo_fix is not None and hi_fix is not None:
        lo,hi=float(lo_fix),max(float(hi_fix),float(lo_fix)+1.0)  # 视频: 固定窗口防闪烁
    else:
        lo,hi=np.percentile(Y[::8,::8],[lo_pct,hi_pct])
    hi=max(hi,lo+1.0)
    scale=hi-lo
    if csup>0:
        tt=np.clip(Y/max(csup*hi,1e-3),0,1)
        f=tt*tt*(3-2*tt)
        Rf=Y+(Rf-Y)*f
        Gf=Y+(Gf-Y)*f
        Bf=Y+(Bf-Y)*f
    Yb=box(box(Y,base_window),base_window)
    Yd=Y-Yb
    if clarity>0:
        Yd=Yd*(1.0+clarity)
    t=(Yb-lo)/scale
    u=t*(1.0+shadow_lift)/(1.0+shadow_lift*t)
    u=np.clip(u,0.0,1.0)
    dg=detail_gain+(1.0-detail_gain)*np.clip(t/max(detail_range,1e-3),0.0,1.0)
    yn=np.clip(u+Yd*dg/scale,0.0,1.0)
    # luma tone -> display domain (gamma), then contrast around pivot
    d=yn**(expo*gamma/2.2)
    if abs(contrast-1.0)>1e-3:
        d=np.clip(pivot+(d-pivot)*contrast,0.0,1.0)
    yy=np.maximum(Y,1e-3)
    out=np.stack([(Rf/yy),(Gf/yy),(Bf/yy)],-1)*d[...,None]
    if abs(sat-1.0)>1e-3:
        out=np.clip(d[...,None]+(out-d[...,None])*sat,0.0,None)
    if desat_hi>0:
        t2=np.clip((d-0.6)/0.4,0.0,1.0)
        f2=1.0-desat_hi*(t2*t2*(3-2*t2))
        out=np.clip(d[...,None]+(out-d[...,None])*f2[...,None],0.0,None)
    out=np.clip(out,0,1)
    return (out*255).astype(np.uint8)

def sharpen(rgb, amount=0.6, radius=2):
    im=Image.fromarray(rgb)
    blur=im.filter(__import__('PIL.ImageFilter',fromlist=['x']).GaussianBlur(radius))
    a=np.array(im).astype(np.float32); b=np.array(blur).astype(np.float32)
    o=np.clip(a+(a-b)*amount,0,255).astype(np.uint8)
    return o

def render(bayer, awb='patch', expo=0.9, sat=1.0, contrast=1.0, csup=0.10):
    """legacy 单图渲染 (保持兼容)"""
    im=np.maximum(bayer-BL,0.0)
    gR,gB,means=awb_gains(bayer,awb)
    Rf,Gf,Bf=demosaic(im,gR,gB)
    return tone(Rf,Gf,Bf,expo,sat,contrast,csup), dict(awb=awb,gR=gR,gB=gB,means=[float(z) for z in means])

def render_new(bayer, gR, gB, a, awb_info=None):
    """新渲染: 由 main 传入已求好的 AWB 增益与参数空间 a"""
    im=np.maximum(bayer-BL,0.0)
    Rf,Gf,Bf=demosaic(im,gR,gB,chroma=a.demosaic_chroma,cnr=a.chroma_nr)
    if a.tone=='legacy':
        rgb=tone(Rf,Gf,Bf,a.expo,a.sat,a.contrast,a.csup)
        tinfo=dict(tone='legacy')
    else:
        rgb=tone_v2(Rf,Gf,Bf,expo=a.expo,lo_pct=a.tone_lo_pct,hi_pct=a.tone_hi_pct,
                    shadow_lift=a.shadow_lift,contrast=a.tone_contrast,pivot=a.tone_pivot,
                    gamma=a.tone_gamma,detail_gain=a.detail_gain,detail_range=a.detail_range,
                    clarity=a.clarity,csup=a.csup,sat=a.tone_sat,desat_hi=a.highlight_desat,
                    base_window=a.tone_base_window)
        tinfo=dict(tone='v2')
    info=dict(awb=awb_info or {}, tone=tinfo, gR=gR, gB=gB)
    return rgb, info

def main():
    ap=argparse.ArgumentParser()
    ap.add_argument('files',nargs='+')
    ap.add_argument('-o','--out',default='out')
    ap.add_argument('--preview',type=int,default=1280)
    # ---- legacy 参数 ----
    ap.add_argument('--awb',default='patch',choices=['gray','patch','hybrid'])
    ap.add_argument('--expo',type=float,default=0.9)
    ap.add_argument('--sat',type=float,default=1.0)
    ap.add_argument('--contrast',type=float,default=1.0)
    ap.add_argument('--csup',type=float,default=0.10)
    ap.add_argument('--sharpen',type=float,default=0.6)
    ap.add_argument('--frames',type=int,default=0,help='use last N frames (default all)')
    # ---- AWB ----
    ap.add_argument('--awb-est',default='v1',choices=['v1','v2'])
    ap.add_argument('--awb-smooth',default='off',choices=['off','iir','median','median-iir'])
    ap.add_argument('--awb-alpha',type=float,default=0.35)
    ap.add_argument('--awb-median',type=int,default=5)
    ap.add_argument('--awb-thresh',type=float,default=0.08)
    ap.add_argument('--awb-fast',type=float,default=0.85)
    ap.add_argument('--awb-reset',type=float,default=0.30)
    ap.add_argument('--awb-hold',type=float,default=0.45)
    # ---- TNR ----
    ap.add_argument('--tnr-alpha',type=float,default=None,help='RAW 帧 IIR 新帧权重; 不给=等权平均(legacy)')
    ap.add_argument('--tnr-frames',type=int,default=0,help='用最后 N 帧做 TNR (0=全部)')
    ap.add_argument('--tnr-norm',default='none',choices=['none','p995','mean'])
    ap.add_argument('--tnr-fit',default='affine',choices=['bl','affine','none'],
                    help='曝光归一化增益估计: affine=原始域最小二乘(推荐)/bl=减黑电平域过原点/none=p99.5 比值')
    ap.add_argument('--tnr-denorm',default='scale',choices=['fit','scale'],
                    help='曝光尺度还原方式: scale=p99.5 尺度比(推荐, 亮度贴合单帧)/fit=拟合增益比')
    ap.add_argument('--tnr-reset',type=float,default=16.0,help='亮度尺度跳变重置门限 (16≈4档; 0=关闭)')
    ap.add_argument('--tnr-sad',type=float,default=0.06,help='内容突变重置门限 (归一化域 8x8 块 SAD)')
    # ---- 新 tone / chroma ----
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
    fs=a.files[-a.frames:] if a.frames>0 else a.files
    if a.tnr_frames>0: fs=fs[-a.tnr_frames:]
    new_awb = (a.awb_est!='v1') or (a.awb_smooth!='off')
    new_tnr = (a.tnr_alpha is not None) or (a.tnr_norm!='none')
    sm = AwbSmoother(a.awb_smooth,a.awb_alpha,a.awb_median,a.awb_thresh,a.awb_fast,a.awb_reset,a.awb_hold)
    awb_log=[]; last={'g':(None,None)}
    if new_tnr:
        def gen():
            for f in fs:
                b=unpack(f)
                gR_,gB_,inf=awb_estimate(b,a.awb,a.awb_est)
                g,ev=sm.update(gR_,gB_,inf['valid_frac'])
                awb_log.append(dict(frame=os.path.basename(f),gR=float(g[0]),gB=float(g[1]),
                                    raw=[float(gR_),float(gB_)],event=ev['event'],valid_frac=inf['valid_frac']))
                last['g']=g
                yield (os.path.basename(f),b)
        bayer,tlog=tnr_blend(gen(),alpha=a.tnr_alpha,norm=('p995' if a.tnr_norm=='none' else a.tnr_norm),
                             reset_ratio=a.tnr_reset,reset_sad=a.tnr_sad,fit_mode=a.tnr_fit)
        lg=last['g']
        gR,gB=lg if lg[0] is not None else (1.0,1.0)
    else:
        acc=None
        for f in fs: b=unpack(f); acc=b if acc is None else acc+b
        bayer=acc/len(fs)
        if new_awb:
            for f in fs:
                b=unpack(f)
                gR_,gB_,inf=awb_estimate(b,a.awb,a.awb_est)
                g,ev=sm.update(gR_,gB_,inf['valid_frac'])
                awb_log.append(dict(frame=os.path.basename(f),gR=float(g[0]),gB=float(g[1]),
                                    raw=[float(gR_),float(gB_)],event=ev['event'],valid_frac=inf['valid_frac']))
                gR,gB=g
        else:
            gR,gB,means=awb_gains(bayer,a.awb)
    rgb,info=render_new(bayer,gR,gB,a,awb_info=dict(gR=float(gR),gB=float(gB),est=a.awb_est,smooth=a.awb_smooth))
    if a.sharpen>0: rgb=sharpen(rgb,a.sharpen)
    Image.fromarray(rgb).save(a.out+'_full.png')
    pv=Image.fromarray(rgb).resize((a.preview,int(a.preview*H/W)),Image.LANCZOS)
    pv.save(a.out+'_preview.png')
    out=dict(out=a.out,frames=len(fs),gR=float(gR),gB=float(gB),est=a.awb_est,
             smooth=a.awb_smooth,tone=a.tone,
             tnr=dict(alpha=a.tnr_alpha,norm=a.tnr_norm,fit=a.tnr_fit),
             awb_log=awb_log)
    print(json.dumps(out,ensure_ascii=False))

if __name__=='__main__':
    main()