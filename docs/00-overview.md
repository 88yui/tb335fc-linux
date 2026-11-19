# 00 · 总览：这条链路是怎么跑起来的

## 1. 数据流

    SC820ACS (MIPI 4-lane)
        │  RAW10 3264×2448 @ 30 fps
        ▼
    CSI-RX PHY (0x11C84000/0x11C85000 ANA, 0x11C86000 DPHY) → SENINF3 (0x1A006000)
        │
        ▼
    CAM_A pass1 (0x1A030000)  ←── CQ 引擎每帧重放 41 条命令环
        │                         （环由 CPU 在 Linux 里直接写内存构造）
        ▼  IMGO DMA
    内存块（IOMMU IOVA，8 槽 × 10 MB 轮转，软件影子每帧改 0x4820/0x4878）
        │
        ├── camrec6：落盘 RAW（调试 / 离线处理）
        ▼
    用户态 ISP（isp_lib.c）：RAW10 40 bit 解包 → 黑电平 → AWB → 去马赛克 → tone → TNR → 缩放 → YUYV/RGB
        │
        ├── camvid -V：ffmpeg 管道出 mp4
        ▼
    /dev/camfeed（write 帧）→ camv4l2 内核模块（vb2_vmalloc）→ /dev/video3（后摄）/ /dev/videoN（前置）
        │
        ▼
    普通应用：ffmpeg / GStreamer / Chromium / 微信 / QQ

## 2. 为什么必须「重放 CQ 环」

MTK 的相机不是「写几个寄存器就出图」。CAM_A 的 pass1 由 **CQ（command queue）引擎**驱动：
环里每条命令 8 字节（低 16 位 = 目标寄存器偏移，高 16 位 = 命令码），CQ 引擎按顺序执行，
其中 0x1805 命令还会顺带触发「DB 装载」（把配置表推进 ISP 内部影子寄存器）。

原厂 HAL 把这条环放在 DMA 内存里，并在每帧 VSYNC 时让硬件自动重放。
我们的做法：在 Linux 用户态把这 435 条金标环（Android 上抓的）整理成 **41 条最小环**，
自己用 camreg 分配 IOVA 内存、写环、写影子数据、发 kick，让 CQ 引擎照常跑。
这条路线成立之后，采集就与「Linux 有没有相机驱动」无关了 —— 只与寄存器序列有关。

## 3. 关键结论清单

1. **CQ 引擎在 Linux 下可执行**：0x210=0x11（immediate）+ 写 CAM_A+0x074=1 kick 即可执行整条环。
2. **连续模式可行**：0x210=0x21 时由 TG 的 VSYNC 自动每帧重放整条环，CPU 零干预（后摄）；
   前置例外，0x21 会自复位，只能 0x11 + 每帧 CPU kick。
3. **IMGO base 必须写 IOVA**（camreg ALLOC 返回值，0x68000000 起）。写物理地址会让 P1 崩。
4. **环里有一条「写 0x214（下一环地址）」**，不改回自己的环，CQ 只真跑首帧（表现为 IMGO base 被锁存）。
   采集器用 8 份环 + 8 份影子 + 8 个 next-ring cell 做硬件轮转。
5. **41 条是最小环**（再减就 FAIL），其中 6 条是硬承重；41 条 + END、影子 11 KB、0x200=0x112、0x218=0xD98。
6. **RAW10 是 40 bit 小端位打包**（5 字节装 4 像素），不是 MIPI 标准打包 —— 解错过一次，图会全花。
7. **Bayer 顺序 BGGR**（按 RGGB 解会 R/B 互换）。
8. **tone 的 hi 必须用 99.5% 分位**（用 100% 会被单点高光拉到 1023，整幅压暗发灰）。
9. **V4L2 代理**：应用要什么尺寸/格式就喂什么（camvid 先 GET_FMT），且必须实现 enum_input/g_input/s_input，
   否则 ffmpeg 报 ioctl(VIDIOC_G_INPUT): Inappropriate ioctl for device。
10. **启动修复的关键是 init_boot 里 /init 的日志函数**：它把日志 echo 到 /dev/ttyGS0，
    没有 USB 主机时 write 永久阻塞 PID1 —— 这就是「必须插数据线才能进桌面」的真正原因。

## 4. 两种使用姿势

| 姿势 | 入口 | 适用 |
| --- | --- | --- |
| 采集调试 | camrec6 | 抓 RAW 整帧、验证寄存器/环改动、长稳 soak |
| 实时显示 / 应用 | camvid + camv4l2 + camdaemon | 当摄像头用（视频通话、录像、截图） |
| 离线处理 | camisp / raw2png.py / autoisp.py | 用已存的 RAW 调 ISP 参数，不用占设备 |

## 5. 时间线（要点）

* 2026-11-13 ~ 11-14：Linux 直接写寄存器，卡在「IMGO 只写 3968 B 就永久背压」；
  转入「金标 CQ 环重放」，首次拿到整帧 9,987,840 B。
* 11-14：最小环 41 条验证；采集器 8 槽硬件轮转，3000 帧 0 drop。
* 11-15：ISP 自研 + 30 fps 实时链路 + 自写 camv4l2 模块，普通应用可用。
* 11-16 ~ 11-18：前置 GC05A2 打通（端口 2 / seninf5），前置 CQ 环重建（Android 抓的 data 被环缓冲污染，重建后满帧）。
* 11-18 ~ 11-19：启动 / 关机 / 电源键 / logo / 冷启动卡 logo 全部修复；相机硬件路线（闭源 tuning）放弃，转用户态优化。