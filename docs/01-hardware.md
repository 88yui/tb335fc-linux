# 01 · 硬件：寄存器窗口、传感器、前端配方

## 1. 相关地址空间

| 窗口 | 基址 | 长度 | 内容 |
| --- | --- | --- | --- |
| camsys / seninf / cam_a / camsv | 0x1A000000 | 1 MiB | TOP(0x1A004000)、SENINF(0x1A006000..)、CAM_A(0x1A030000)、CAM_A_INNER(0x1A038000)、CAMSV2(0x1A092000..) |
| imgsys | 0x15000000 | 192 KiB | DIP/ISP 相关（本项目基本没直接用） |
| CSI-RX 模拟前端 ANA | 0x11C80000 | 64 KiB | CD-PHY：port0 A 0x11C84000 / B 0x11C85000，port2 A 0x11C88000 / B 0x11C89000 |
| DPHY 顶层 | 0x11C86000（port0） | 4 KiB | LANE_EN / LANE_SELECT / HS_RX_EN_SW |
| TOPCKGEN | 0x10000000 | 4 KiB | CLK_CFG_2 / CLK_CFG_UPDATE（验证 24 MHz MCLK 是否当选） |

camreg 内核模块（kernel/camreg/camreg_isp.c）把前四个窗口 ioremap 后通过 /dev/camreg 暴露读写；
**不在这四个窗口内的地址一律 -EINVAL**。

CAM_A 内部常用偏移（相对 0x1A030000）：

| 偏移 | 含义 |
| --- | --- |
| 0x0000–0x00FF | 显示/控制（不要整块剔掉，里面有必需的 TG 起点） |
| 0x0054 | DB_EN：配置表装载使能，**必须走 0→1 边沿**否则不装载 |
| 0x0074 | CQ kick（写 1 立即执行当前环） |
| 0x0200 | CQ 模式（0x112 = 本项目的配置） |
| 0x0210 | CQ 执行模式：0x11 = immediate（每帧 CPU kick），0x21 = 连续（TG 自动重放） |
| 0x0214 | 下一环地址（由环内命令写；必须指回自己的环） |
| 0x0218 | 环长度（0xD98 = 435 条 × 8 B） |
| 0x0E00 / 0x0E08 | FBC 压缩开关（本项目必须关：0） |
| 0x1F3C | TG 状态：byte1 的 0x02 = TG 在跑，0x04 = CAM 接管出流 |
| 0x1F78 | 一帧像素数（3264×2448 = 0x0079EC00） |
| 0x1F7C | 宽高：(0x0CC0<<16)|0x0990 |
| 0x47B0 / 0x4808 | META（DMA 元数据）基址 |
| 0x4820 / 0x4878 | IMGO 主/副输出基址（双写，都指向当前帧缓冲 IOVA） |
| 0x484C | FH（frame header）地址 |

CAM_A_INNER（0x1A038000）是同一组寄存器的内部镜像窗口，**0x4820 等关键项两边都要写**。

## 2. 前端配方（30 条静态写）

上电顺序：传感器 OPEN（走 MTK camsensor 内核驱动，FEAT ioctl）→ 写 DPHY/CSI2 → 写 CAM_A 前端 → DB_EN 边沿 → 环 kick → TG 起流。
核心写序（实机已验证多次复现）：

    ; ---- CSI-RX PHY：DPHY A/B 两套 ----------------
    0x11C840F0 = 0x000200E2      ; 0x11C850F0 同值
    0x11C84008 = 0x44001111      ; 0x11C85008
    0x11C8400C = 0x02001111      ; 0x11C8500C
    0x11C84018 = 0x00090909      ; 0x11C85018
    0x11C8401C = 0x00090909      ; 0x11C8501C
    0x11C84024 = 0x30030000      ; 0x11C85024
    ; BG 脉冲：先清低 2 位再置回（0x00074713 & ~3 -> 0x00074711 -> 0x00074713）
    0x11C84020 = 0x203F413F      ; 均衡器（另一组 0x203F611B 在 ANA 的另一页）

    ; ---- lane / HS 使能（port0 DPHY 顶层） ---------
    0x11C86020..0x11C8602C = 0x30201802   ; 4 条 data lane 的 HS 时序
    0x11C86004 = 0x80403102               ; LANE_SELECT（bit31 = CK_DATA_MUX_EN）
    0x11C86000 = 0x00000F01               ; LANE_EN：4 data + 1 clock

    ; ---- SENINF3 + CSI2（后摄） ------------------
    0x1A006A10 = 0x00000106      ; CSI2 lane 数 / 数据格式（RAW10）
    0x1A006A00 = 0x0000000F      ; CSI2 使能（4 lane）
    0x1A006210 = 1               ; SENINF mux
    0x1A006200 = 1
    0x1A004064 = 1               ; TOP：PHY_CTRL 对应位（DPHY_EN）
    0x1A006D00 = 1               ; mux 使能（**漏写则满帧进不了 CAM**）
    0x1A006D04 = 0x001F0208

判据（写完后读回，任何一项不对就不要继续）：

* CSI2 包计数 0x1A006AC8 != 0（有包进来）
* MUX 尺寸 0x1A006D30 = 0x09900CC0（（高<<16）|宽）
* MUX 像素 0x1A006D34 = 0x0079EC00（= 7,990,272 = 3264×2448）
* CAM_A 0x1F78 同上、0x1F7C = 0x0CC00990、0x1F3C 的 byte1 出现 0x02 → 0x04

这些寄存器值全部来自本机 Android 出流时的实抓（camreg 只读 dump）+ 逐条重放验证，不是手册值。

## 3. 传感器：SC820ACS

走原厂 **MTK camsensor 内核驱动**（/dev/kd_camera_hw，major 497:0）打开/关流，不自己写 i2c 时序：

    struct { int32_t cam, fid; uint64_t buf, plen; } f;   // ioctl FEAT = 0xC018690F
    FeatureId: SET_DRIVER=3107, OPEN=3105, STREAM_ON=3104, STREAM_OFF=3103, CLOSE=3106
    SET_DRIVER 的 payload 必须是 36 字节：struct { u32 id; char name[32]; }
    （长度不足 36 时所有调用都 rc=0 但传感器根本不上电 —— 这是个很贵的坑）

出流后的曝光 / 增益 / 帧长（直接 i2c 或经 /proc/driver/camsensor2）：

| 项 | 寄存器 | 编码 |
| --- | --- | --- |
| 曝光 | 0x3E00 / 0x3E01 / 0x3E02 | exp>>11 / exp>>3 / (exp&7)<<5 |
| 帧长 VTS | 0x320E / 0x320F | 高/低字节 |
| 增益 | 0x3E07 | 线性单调（AE 只用它；0x3E08 非单调，不要用） |

约束：**曝光必须 < VTS − 16**。实测帧率：VTS 16383 → 4.6 fps，5000 → 15 fps，2500 → 30 fps。

## 4. CQ 引擎

* 环条目 8 字节：word0 = (cmd << 16) | reg_offset，word1 = 数据（通常是影子数组内的地址，或立即数）。
* cmd 0x18xx = 写寄存器；**0x1805 = 写寄存器 + 触发 DB 装载**（关键机制）。
* 环里还有 4 条表命令（0x19FD×3、0x18FB×1），重放时要跳过（它们是给原厂 HAL 的元数据）。
* 0x210 决定执行模式；0x074 是手动 kick；0x214 是下一环地址（环内自带一条写它）。

## 5. 时钟 / 电源域雷区

* CSI-RX / CAM / CAMSV 分属不同 power domain 与 SMI LARB。域未上电时**碰该域寄存器会让整机自复位**。
  正确的上电由 MTK 平台驱动在传感器 OPEN 时完成；不要手写 SMI/GCB 寄存器。
* 强制 LARB power/control=on（camrec6 的 CAMREC_NOLARB//sysfs 路径）只是排障手段，不是常规步骤。
* 读 clk_summary / pm_genpd_summary 这类整表文件曾直接把设备读重启 —— **禁止整表读**。
* 恢复寄存器必须**倒序**写回并读回校验；W1C 位除外。RESTORE_MISMATCH 应为 0。
* 绝对不要整窗扫寄存器（0x11C80000 整 64 KiB 扫一遍 = 设备自复位）。只读手挑的地址。