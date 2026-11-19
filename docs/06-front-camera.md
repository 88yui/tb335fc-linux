# 06 · 前置摄像头 GC05A2

前置比后摄麻烦：它没有 Android 上的「金标配方」可以直接照抄，端口 / PHY / seninf 全是靠实测定位出来的。

## 1. 硬件事实

| 项 | 值 |
| --- | --- |
| 传感器 | GC05A2（id 0x5A2），2592×1944 RAW10，2-lane，MCLK 24 MHz，无 AF |
| i2c | @11ed2000，7-bit 地址 0x1a（由 MTK camsensor 内核驱动管理） |
| CSI-RX 端口 | **端口 2**：ANA 0x11C88000 / 0x11C89000，DPHY 顶层 0x11C8A000 |
| SENINF | seninf5 = 0x1A008000 |
| 出流尺寸 | 2592×1944，一帧 6,298,560 B（= 2592×1944×1.25） |
| 行格式 | stride 3248 B，有效 3240 B，行尾 8 B 填充（与后摄同样的 40 bit 打包） |

## 2. 前端配方

    ; ---- 端口 2 CSI-RX PHY ----------------
    ANA 0x11C88000 / 0x11C89000：与后摄同一套 DPHY 写序（见 docs/01 第 2 节）
    DPHY 0x11C8A000 + 0x0000 = 0x00000F01     ; LANE_EN：2-lane 版本
    DPHY 0x11C8A000 + 0x0004 = 0x80403102     ; LANE_SELECT
    DPHY 0x11C8A000 + 0x0020.. = 0x10201802   ; DL0..DL3
    CL0/CL1 = 0x10000000

    ; ---- seninf5 (0x1A008000) --------------
    +0x0A00 = 0x00000003
    +0x0A10 = 0x00000106
    +0x0200 = 1
    +0x0210 = 1
    +0xD00  = 1                ; ★ seninf mux 使能，漏写满帧也进不了 CAM
    +0xD04  = 0x001F0200
    +0xD10  = 0x80000000

    ; ---- TOP (0x1A004000) ------------------
    0x1A004068 = 1             ; 端口 2 的 PHY_CTRL（DPHY_EN）
    0x1A004010 = 0x03020104    ; MUX_CTRL：源选择

判据：CAM_A 0x1F78 = 0x004CE300（= 2592×1944 = 5,038,848）。

## 3. 踩过的坑（按代价排序）

1. **端口定位不能靠 DT**：设备 DT 只有 seninf1..8，没有 port/mux 属性，vendor 配置里也没有 gc05a2。
   必须靠「改一处 → 看 CSI2 包计数 0x1A006AC8 是否变化」一个个试。
2. **2-lane 的 LANE_SELECT/LANE_EN 与 4-lane 不同**（0x80403102 / 0x00000F01）。
3. **seninf mux 必须写 +0xD00=1**（定位阶段一直漏这条，导致「满帧但 CAM 收不到」）。
4. **前置的「数据影子」在 Android 上抓下来是被污染的**：0x1000 以上区域其实是 CQ 环缓冲区的拷贝，
   环条目会把 0x18000494 / 0xF7350000 之类垃圾写进 P1 配置。
   解法：用 Android 出流时的 live 全窗寄存器重建 DATA 区（0x1A030000..0x4FFF，共 5120 条）。
5. **连续模式 0x210=0x21 会让前置自复位**，只能 immediate 0x11 + 每帧 CPU kick（约 30 fps 需要 CPU 参与）。
6. 前置自拍需要水平镜像：isp_front 侧 g_hflip / CAMREC_HFLIP，配合 /persist/cam_flip 与 cammirror。

## 4. 当前实现

userspace/camfront.c + userspace/isp_front.c：

    isp_half_set_geom(2592, 1944, 3248, 0)   /* 0 = 40bit 打包（非 MIPI 标准打包） */
    isp_half_init(&g_isp, 61, "patch")       /* 黑电平 61，AWB patch 模式 */

输出推到双节点 V4L2 的 camv4l2-front（见 docs/05）。

## 5. 已知限制

* 前置没有 AF、没有独立 3A 调参；
* 每帧 kick 带来 CPU 占用，30 fps 时更明显；
* 与后摄不能真正「同时高帧率」（共用 CQ/ISP 资源，靠 camv_owner 串行化）。