# 04 · 用户态 ISP

源文件：userspace/{isp_lib.h, isp_lib.c, camisp.c, isp_front.c, camfront.c}

目标：在 CPU 上把 RAW10 变成好看的照片/视频帧，不依赖任何闭源 tuning。

## 1. RAW10 解码（最容易错的一步）

SC820ACS 的 RAW10 **不是** MIPI 标准打包，而是 **40 bit 小端位打包**（5 字节装 4 像素，低位在前）：

    uint64_t W = b0 | b1<<8 | b2<<16 | b3<<24 | (uint64_t)b4<<32;
    p0 =  W        & 0x3FF;
    p1 = (W >> 10) & 0x3FF;
    p2 = (W >> 20) & 0x3FF;
    p3 = (W >> 30) & 0x3FF;

W 必须是 uint64：用 uint32 会丢掉第 5 个字节，图会整片花。
行 4080 B（3264 像素 × 10 bit / 8），帧 9,987,840 B；**Bayer 顺序 BGGR**（按 RGGB 解会 R/B 互换）。

前置 GC05A2 走 `isp_half_set_geom(w,h,rowb,mipi=1)` 的 MIPI 分支：行 3248 B、有效 3240 B、行尾 8 B 填充。

## 2. 处理管线

    解码 -> 减黑电平 -> 8x8 块统计 -> AWB v2 -> 去马赛克(G 底比值 9x9) -> tone v2 -> TNR -> Y/Cb/Cr 4:4 -> 缩放 -> YUYV/RGB24

要点：

* **去马赛克**：G 通道用 9×9 加权、R/B 按比值（避免色噪直接放大成彩斑）。
* **色度**：4×4 下采样 + 3×3 中值。低光高增益下的彩线就是「行相关噪声被去马赛克混叠成色度条纹」，这两步是主要解药。
* **tone v2**：4096 项有理曲线 LUT（k=1），高光去饱和 0.8；色度用线性域的 channel/Y 比例重建。
  逐通道套 gamma 会掉饱和，不要那么做。
* **TNR**：G 采样曝光归一化（k 限幅 [1/8, 8]），运动度量 d < 14 混 α、d > 72 不动。默认 α 0.55，视频链路用 0.30~0.45。

## 3. AWB

8×8 块统计：有效块 = bG > 10 且 max < 940；patch 模式取 bG 的 99% 分位做增益基准。
标定常数：

| 模式 | R gain | B gain |
| --- | --- | --- |
| patch | 0.92519 | 0.94922 |
| gray | 0.83926 | 0.88441 |
| hybrid | 0.88394 | 0.91930 |

增益裁到 [0.55, 2.2]；IIR 时域平滑默认 α 0.35，并带置信度 / hold / 跳变二次确认（IspHalf 里的 wb_conf、wb_spread、wb_pend_*）。

## 4. 两个致命的百分位坑

1. **tone 的高光分位必须用 99.5%，不能用 100%**。用最大值会被单点高光（灯、反光）拉到 1023，导致整幅压暗发灰。
   isp_lib 与 camisp 的默认值都是 hipct = 0.995（camisp 有 --tone-hi-pct 可调）。
2. daemon 里还有 CAMREC_HI_FLOOR（tone 高光下限，daemon 用 175）修短曝光画面灰白。

## 5. 性能（MT6835，单帧）

| 路径 | 耗时 | 说明 |
| --- | --- | --- |
| 朴素解码 | 66 ms | camreg mmap 是 uncached 语义，逐字节读 |
| 优化解码 | 12.3 ms | 先 memcpy 两行到栈（8 KB）再解包 |
| 全分辨率 ISP 总 | 49.6–51.6 ms | 1280×960 输出 |
| 1280×960 融合快路 | 32.5–33.6 ms | 30 fps 可用（VTHREADS=2） |
| 1632×1224 融合快路 | 21.1–21.7 ms | dec 12.7–13.8 + fin 5.7–5.9 |

融合快路的思路：解码/统计只做一次，**直接在目标尺寸上做 tone + 打包 YUYV**，省掉全分辨率 RGB 缓冲和二次缩放；
目标尺寸 == 源尺寸时再走 ident 快路跳过插值。

## 6. API

    typedef struct { ... } IspHalf;
    int  isp_half_init(IspHalf *s, int black, const char *awbmode);
    int  isp_half_process(IspHalf *s, const uint8_t *raw, int black, const char *awbmode,
                          double manR, double manB, double csup, double contrast,
                          double gamma, double sharpen, uint8_t **rgbout);
    int  isp_half_process_yuyv(... int ow, int oh, int dst_stride, uint8_t *dst);
    void isp_half_free(IspHalf *s);
    void isp_half_set_geom(int w, int h, int rowb, int mipi);
    void isp_half_set_tnr(int on);

## 7. 离线工具

| 工具 | 用途 |
| --- | --- |
| camisp | 把一帧 RAW 转 PNG/PPM，可调 --scale/--awb/--tone v2/--threads/--rgbout |
| tools/raw2png.py | 最简 RAW -> PNG（PC 端） |
| tools/autoisp.py | AWB patch/gray/hybrid、TNR、tone v2、USM 的离线实验平台 |
| tools/raw2video.py | RAW 序列 -> 视频 |
| tools/isptest.c | 新旧解码结果逐字节比对（正确性回归） |

## 8. 画质现状与已知短板

* 细节可以超过 Android 参考 JPEG；主要差距在**噪点**（约 5 倍），需要多帧降噪（MFNR）才能补。
* 后摄第一瓶颈其实是**对焦**（近景梯度 0.0033 vs 参考 0.0316）—— AF 已在另一分支实现（VCM GT9764AF，i2c-8 7-bit 0x0C，DAC 10 位），
  但**当前合并的 camvid.c 里没有 AF 代码**，camdaemon 传 --af 会被静默忽略。
* MTK 的 CCM blob 已破解格式（2256 B = 6×376 B，13-bit 有符号 + Q9，行和恒 512），但不能 1:1 套用：
  它属于 MTK 自己的 AWB 域，直接套 dE 反而从 22 变 36。
* 白平衡/曝光调好之后，tone 分位匹配能把 dE 从 22 压到 ~18（配合 AWB 增益网格搜索可到 17.9）。