# 08 · 雷区清单（会坏机的操作都在这）

> 本项目全部结论都是在一台真实零售机上试错得来的。这里每一条都真的发生过。

## A. 会让设备自复位 / 重启的操作

1. **整窗扫寄存器**：一次读 0x11C80000 起 64 KiB（16384 次读）⇒ 设备自复位，uptime 归 0。
   只读手挑的、确认在已上电域里的地址。
2. **整表读** /sys/kernel/debug/clk/clk_summary 或 pm_genpd_summary ⇒ 曾直接把设备读重启。
3. **在域未上电 / 已 suspend 时碰该域寄存器**（CSI-RX、CAM、CAMSV 都要先由平台驱动上电）。
   先让 MTK camsensor 驱动 OPEN 传感器，再写寄存器。
4. **前置连续模式（0x210=0x21）** 会自复位 —— 前置只能 immediate。

## B. 会卡死系统的操作

5. **在 V4L2 ioctl handler 里再锁 vdev->lock**：video_ioctl2() 已持锁 ⇒ 进程永久 D 状态，
   kill -9 无效，rmmod refcount 不降，只能 sync; reboot。
6. **单独跑一个 `sh -i` 在救援包装器里** ⇒ 它忽略 SIGTERM，关机要等 90 s。

## C. 会让相机不出图的坑（按出现频率）

7. DB_EN 直接写 0x120074（没有 0→1 边沿）⇒ 配置不装载，IMGO 一字节不写。
8. FBC 没关（0x0E00/0x0E08 ≠ 0）⇒ 输出是压缩流。
9. CAM_A_INNER（0x1A038000）没镜像 ⇒ IMGO 只写 3968 B（25 行）。
10. IMGO base 写了物理地址（不是 IOVA）⇒ P1 崩。
11. 环里 0x214 没指回自己的环 ⇒ 只首帧真跑。
12. seninf mux +0xD00 没写 ⇒ 满帧进不了 CAM。
13. 前置数据影子用了 Android 抓的污染版 ⇒ 环垃圾写进 P1。
14. SET_DRIVER payload 不是 36 字节 ⇒ 所有 FEAT 调用 rc=0 但传感器根本不上电。

## D. 会让画质翻车的坑

15. tone 高光用 100% 分位 ⇒ 整幅压暗发灰（必须 99.5%）。
16. RAW10 用 uint32 解包 ⇒ 全花（40 bit 小端打包，必须 uint64）。
17. Bayer 顺序按 RGGB ⇒ R/B 互换（实际 BGGR）。
18. 低光高增益不降增益 ⇒ 彩线（行相关噪声混叠色度）。
19. 逐通道套 gamma ⇒ 掉饱和（色度要用线性域 channel/Y 比例重建）。

## E. 操作纪律

20. **每轮实验先记 btime/uptime**：设备一旦自复位，整轮作废，基线要重录。
21. **开关机三件套必须齐**，否则可能停在半状态需要救援。
22. **改寄存器前后都读回，倒序还原**，RESTORE_MISMATCH 必须 0（W1C 除外）。
23. **ssh 命令里的 $( )、反引号、模板字符串会在本地被展开** ⇒ 一律本地写 .sh → scp → ssh 执行。
24. **传 .ko / 二进制前后比 md5**。
25. 单条命令有超时上限（约 120 s），长任务放后台或分段。
26. 没有 USB 主机时 init_boot 会卡住（已修复，见 docs/07）；救援依赖 fastboot + 好镜像，请先准备好。

## F. 2026-11-19 新踩到的坑

27. **整机跑久了 30 MB 相机缓冲分配失败**（`IOC_ALLOC: Cannot allocate memory`、
    `ABORT_ALLOC_GROUP s=0 want=0x1E00000`），只能 reboot 恢复 —— 根因是 ZONE_DMA32 碎片化后
    buddy 连 order-10 都拿不到。已修：`camreg_isp.c` 先试 GFP_DMA32，失败后从普通内存重试
    （iova 由 IOMMU 固定映射到 0x68000000 起，与 PA 无关）。实测 `insmod … force_nodma32=1`
    时 PA 落在 0x168800000（>4 GB）仍满帧 9,987,840 B、drops=0。
28. **这台机器上 fstab 挂不出来**：systemd 收不到 udev 设备事件 ⇒ device 单元永远 inactive，
    `*.mount` 被 "Condition check …skipped" 跳过。见 docs/07 第 9.1 节，改用 oneshot 服务
    `tb335-data-mount.service` 按 LABEL 挂载。
