/* isp_lib.h — 视频快路 ISP (RAW10 40bit LE -> RGB24 1632x1224)
 * v2: AWB v2 估计器(减黑+标定) + AWB IIR 时域平滑 + tone v2(有理曲线/通道-Y 比例重建/高光去饱和)
 * 参考 outputs/gold/w266_c2_refine.md（C-2 细化）
 */
#ifndef ISP_LIB_H
#define ISP_LIB_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
  int w,h;             /* 输出尺寸 (1632x1224) */
  float *R,*G,*B;      /* 平面缓冲（已减黑电平） */
  float *tmp;
  float *blkR,*blkG,*blkB; /* 8x8 块均值（AWB v2 统计） */
  float *pR,*pG,*pB;   /* 上一帧平面（RAW 域时域降噪 TNR） */
  double pmean;        /* 上一帧亮度均值（曝光归一化） */
  int have_prev;       /* pR/pG/pB 有效 */
  int tnr_on;          /* TNR 开 */
  double tnr_alpha;    /* TNR 最大混合系数 */
  long tnr_applied;    /* 已应用 TNR 帧数 */
  int bw,bh;           /* 块数 */
  double wbr,wbb;      /* 当前白平衡增益（时域平滑后的输出） */
  int lo,hi;           /* tone 映射区间（线性域 Y） */
  int inited;
  double alpha;        /* AWB IIR 系数 0..1 (默认 0.35) */
  int frame_no;
  long valid_q;        /* 上一帧 AWB 有效块占比 *1000 */
  const char *last_event; /* 最后一次 AWB 事件: init/initwait/iir/fast/reset2/resetpend/hold/decay/deadband/off */
  /* H0：AWB 鲁棒性（偏色止血） */
  double wb_conf;      /* 上一帧 AWB 置信度 0..1000 */
  double wb_spread;    /* 上一帧块 R/G、B/G 比值离散度 0..1000（越大场景越彩色） */
  long   wb_holdn;     /* 连续低置信 hold 次数 */
  double wb_pend_r,wb_pend_b; /* 待二次确认的大幅跳变 */
  int    wb_pend_n;
} IspHalf;

int  isp_half_init(IspHalf *s, int black, const char *awbmode);
int  isp_half_process(IspHalf *s, const uint8_t *raw, int black,
                      const char *awbmode, double manR, double manB,
                      double csup, double contrast, double gamma, double sharpen,
                      uint8_t **rgbout);
/* 融合快路：解码/统计一次，直接在目标尺寸上做 tone + YUYV（省掉全分辨率 RGB 缓冲与二次缩放） */
int  isp_half_process_yuyv(IspHalf *s, const uint8_t *raw, int black,
                      const char *awbmode, double manR, double manB,
                      double csup, double contrast, double gamma,
                      int ow, int oh, int dst_stride, uint8_t *dst);
void isp_half_free(IspHalf *s);
/* 运行前设置传感器几何/打包（w,h=全分辨率；rowb=行 stride；mipi=1 用 MIPI RAW10） */
void isp_half_set_geom(int w,int h,int rowb,int mipi);
/* 运行时开关 TNR：30fps 时单帧预算 33ms，TNR 约 +12ms，按到达间隔自适应关闭 */
void isp_half_set_tnr(int on);
#ifdef __cplusplus
}
#endif
#endif
