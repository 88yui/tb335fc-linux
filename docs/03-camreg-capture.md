# 03 · camreg 驱动 + 采集器（camrec5 / camrec6）

## 1. camreg：一个「能读写任意相机寄存器 + 能分配 IOVA 内存」的模块

源文件：kernel/camreg/{camreg_isp.c, camreg.c, camregtool.c}

为什么需要它：

* 用户态不能直接 ioremap 这些窗口（且这些地址在 Linux 里没有驱动认领）；
* 更重要的是 **IMGO 要写的内存必须是 IOMMU 认得、CQ 引擎能访问的 IOVA**，所以需要内核帮忙分配。

### 1.1 ioctl ABI

    magic = 'r'
    CAMREG_RD     = _IOWR('r', 1, struct camreg_req)     /* addr/val */
    CAMREG_WR     = _IOW ('r', 2, struct camreg_req)
    CAMREG_ALLOC  = _IOWR('r', 3, struct camreg_alloc)   /* size -> handle/iova/paddr */
    CAMREG_FREE   = _IOW ('r', 4, u32 handle)
    CAMREG_READPA = _IOWR('r', 5, struct camreg_pa)      /* 读物理内存（调试） */
    CAMREG_TRANS  = _IOWR('r', 6, ...)                   /* 物理 -> IOVA */

    struct camreg_req   { u32 addr, val; };
    struct camreg_alloc { u32 size, handle; u64 iova, paddr; };
    struct camreg_pa    { u64 addr, val; };

限制：最多 8 块缓冲、每块最大 4 MB、单次 ALLOC 合计 ≤32 MB。
可访问窗口（表 g_regions）：0x1A000000/1 MiB（isp）、0x15000000/192 KiB（imgsys）、
0x11C80000/64 KiB（ana）、0x10000000/4 KiB（topckgen）。窗口外一律 -EINVAL。

读 DRAM（READPA）用单页 ioremap_cache，范围 0x40000000–0x4,00000000。

### 1.2 设备节点

    insmod camreg_isp_top.ko devname=1a030000.cam1_legacy   # 绑定 CAM_A 的 platform device
    # 若 devname 不匹配，模块仍会注册 misc 设备，取 /proc/misc 里的 minor：
    mknod /dev/camreg c 10 <minor>

绑定 device 的意义：让 MTK 平台驱动把 CAM_A 域上电（这样才能安全碰寄存器）。

## 2. camrec6：满帧采集器

源文件：userspace/camrec6.c（衍生自 camrec5；camrec6 额外开 AAO 硬件统计）

它做的事，按顺序：

1. 确保电源/时钟：等 MTK camsensor 驱动 OPEN 传感器（FEAT ioctl），必要时强制 LARB power/control=on。
2. camreg ALLOC：8 个 10 MB 帧槽（SLOT_STRIDE 0x00A00000）+ 1 个 1 MB AUX（环/影子/cell/FH/META/AAO）。
3. 构造 8 份环 + 8 份影子：环 i 的 0x214 命令指向 cell_i；影子按寄存器偏移镜像。
4. 写前端 30 条 → 静态组（0x4820/0x4878/0x484C/0x4808/0x47B0/0x0E00/0x0E08/0x014）。
5. DB_EN 0→1 边沿；0x200=0x112、0x210=0x11（或 0x21）、0x214、0x218。
6. 每帧：patch_slot() 把影子 +0x4820 与 +0x4878 改成当前槽的 IOVA → kick（0x074=1）→ 等帧完成 → 读下一槽。
   连续模式下 0x210=0x21，软件只做轮转与取帧，不需要每帧 kick。

### 2.1 命令行

    camrec6 -n 300 -p /tmp/f -v 5000 -e 4900 -g 0x18      # 抓 300 帧 RAW
    camrec6 -n 600 -z -v 2500 -e 2400                     # 只测吞吐不落盘（30 fps 验证）
    camrec6 -n 2900 -z -A -t 300                          # AE 闭环长稳
    camrec6 -k 60 ...                                     # 每 60 帧存一张

| 选项 | 含义 |
| --- | --- |
| -n N | 采集帧数 |
| -p prefix | 输出前缀（写 prefix_0001.raw） |
| -v VTS | 帧长（2500 ≈ 30 fps，5000 ≈ 15 fps，16383 ≈ 4.6 fps） |
| -e EXP | 曝光行数，必须 < VTS−16 |
| -g gain | 增益码（0x3E07） |
| -A / -t / -m | AE 闭环 / 目标 p995 / 最大增益 |
| -z | 不落盘（纯吞吐测试） |
| -k N | 每 N 帧存盘 |

环境变量：CAMREC_NOLARB、CAMREC_NOSEN、CAMREC_EXTEST、CAMREC_GSW、CAMREC_AE_*、CAMREC_DIAG、CAMREC_VTHREADS 等（见源码开头）。

### 2.2 输出

文件里就是裸 RAW10（9,987,840 B/帧），行 4080 B，Bayer BGGR。
配合 userspace/tools/raw2png.py 或 camisp 转 PNG。

## 3. AE 闭环

控制律（camrec4 起）：以 p995 为目标亮度，s = p995 − 61，
**长曝优先**：先把 VTS 加到 16383 / 曝光加到 VTS−16，再补增益；增益用码表 GMAP（0x3E07 线性单调）。

实测：15 fps 下 p995 硬上限约 147（曝光被 VTS 卡住），4.6 fps 长曝能更亮。
长稳：4.58 fps AE 长曝 2900 帧 drops=0 lag=0 bad=0；15 fps 3000 帧 drops=0 dtavg 66.7 ms dtmax 81.0 ms。

## 4. 踩过的坑

1. **DB_EN 必须走 0→1 边沿**：直接写 0x120074 不产生上升沿，配置不装载（IMGO 一个字节都不写）。
2. **FBC 必须关**（0x0E00 = 0x0E08 = 0），否则 IMGO 输出是压缩流不是 RAW。
3. **CAM_A_INNER（0x1A038000）要镜像**同样的关键寄存器。
4. **IMGO base 必须是 IOVA**，写物理地址会把 P1 打崩。
5. 环里那条写 0x214 的命令不改回自己的环 → 只有首帧真跑。
6. camvid / camrec 与 ffmpeg 抢 CPU 时，采集循环会被拖慢导致失步；异步 worker 线程 + 融合快路解决。

## 5. 排障：camregtool

kernel/camreg/camregtool.c 是最小读写/分配示例（也是排障入口）：

    camregtool rd 0x1A030000 0x1F78
    camregtool wr 0x1A030000 0x0054 0x120074
    camregtool alloc 10485760

注意：**不要整窗扫寄存器**（一次扫 64 KiB 会让设备自复位）；恢复寄存器要倒序并读回校验（W1C 除外）。