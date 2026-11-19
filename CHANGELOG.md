# 更新日志（CHANGELOG）

本项目的所有结论都来自一台真实 TB335FC（MT6835 / ISP6s）上的实测，按时间倒序记录里程碑。

## v1.0.0 — 2026-11-19

首个可分发快照。内容：

* **后摄 SC820ACS**：CAM_A pass1 → IMGO 整帧 RAW10（9,987,840 B），41 条最小 CQ 环 + 8 槽硬件轮转采集器，
  15/30 fps 长稳实测 3000 帧 drops=0。
* **前置 GC05A2**：端口 2 / seninf5 定位，2592×1944 满帧（连续模式需每帧 CPU kick）。
* **用户态 ISP**：40 bit 小端 RAW10 解包、AWB v2、tone v2、TNR、去马赛克、缩放；
  1280×960 与 1632×1224 实时 30 fps。
* **标准 V4L2 输出**：自写 camv4l2 内核模块 + /dev/camfeed 私有 ABI，双节点、camdaemon 按需起流；
  ffmpeg / GStreamer / Chromium / 微信 / QQ 可用。
* **后摄自动对焦**：VCM GT9764AF，AF 状态机并入 camvid.c（粗扫 9 点 + 细扫 step24 + 抛物线亚步长 + 置信度门限），
  暗光按 max(4 帧, 250 ms) 等 VCM 稳定。
* **板级系统补丁**：83 个文件（21 个 systemd unit、27 个 /usr/local/sbin、20 个 /usr/local/bin、15 个 /persist 脚本），
  逐文件 md5 与实机一致（boot/MD5SUMS）。含启动加速（105 s → 29.3 s）、电源键/熄屏、关机 90 s 卡顿、
  kpoc 拔线关机、冷启动卡 logo 修复（init_boot v4）、WiFi/BT 修复。
* **系统可靠性**：camreg 30 MB 相机缓冲在 ZONE_DMA32 碎片化时回落普通内存（靠 IOMMU 映射，PA>4GB 也出图）；
  /data 与 /mnt/super 用 oneshot 服务挂载（本机 systemd 收不到 udev 事件，fstab 挂载单元不生效）。
* **分区**：Android super(11G)/userdata(46.7G) 已回收为 Linux ext4，GPT 备份见 docs/gpt_sdc_backup.txt。

已知短板见 docs/09-roadmap.md（噪点缺 MFNR、CCM/GGM 未采用、V4L2 无 CID 控件、空闲不下电）。

## 2026-11-18 ～ 11-19

* 后摄自动对焦落地：afreg 模块（/proc/afreg）、VCM 粗扫/细扫状态机，真实应用链路验证通过。
* camreg 缓冲分配修复（碎片化导致 IOC_ALLOC 失败、只能重启）。
* Android 分区回收 + 挂载服务 + 文档/版本表更新。

## 2026-11-14 ～ 11-17

* CQ 环重放在 Linux 首次跑通（0x210=0x11 immediate + kick 0x074=1），IMGO 出整帧；
  最小环从 439 条减到 41 条（6 条硬承重）。
* camrec3/5/6 采集器（8 环 + 8 影子 + 8 next-ring cell 硬件轮转）与长稳实测。
* 实时链路与 V4L2 代理模块（camv4l2 + camfeed），应用兼容性验证。
* 前置摄像头端口定位（CSI 2 / seninf5），金标环与后摄仅 4 条不同。

## 2026-10-12 ～ 11-13

* Debian 启动、桌面、GPU（panfrost）、USB-NCM 网络基线。
* 前端 30 条配方、DPHY 模拟前端上电、CSI2/SENINF/MUX3 打通，CSI2 开始出包。
* 寄存器取证工具链（camreg / regwin）、从 Android 抓取金标寄存器与 CQ 环。
