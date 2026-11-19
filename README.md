# TB335FC Linux 相机栈（MT6835 / ISP6s）

> 联想 TB335FC 平板（MT6835，ISP6s，Android 13 原厂固件）上，把**后摄 SC820ACS（3264×2448 RAW10）**与**前置 GC05A2（2592×1944）**
> 在自编 Debian Linux（内核 5.15.185-android13-8）里完整跑起来的一套补丁、内核模块与用户态工具。

**当前状态（2026-11-19）**

| 能力 | 状态 |
| --- | --- |
| 后摄全分辨率 RAW10 采集（CAM_A pass1 → IMGO） | 可用，3264×2448 / 9,987,840 B 整帧 |
| 后摄连续采集 | 可用，CQ 环硬件自轮转，15 fps / 30 fps 实测 3000 帧 0 drop |
| 实时链路（含 ISP） | 可用，1280×960 与 1632×1224 在 30 fps 下稳定 |
| 标准 V4L2 节点给普通应用（微信 / QQ / Chromium / ffmpeg / GStreamer） | 可用，双节点（后摄 / 前置）+ 私有推流口 |
| 前置摄像头 | 可用，2592×1944 满帧；连续模式需每帧 CPU kick（见 docs/06） |
| 自动对焦（后摄 VCM GT9764AF） | 可用，AF 状态机已合并进 camvid.c；暗光低帧率下按 max(4 帧, 250 ms) 等 VCM 稳定 |
| AWB / tone / TNR / 去马赛克 | 用户态实现，参数已固化进 camdaemon |
| CCM / GGM / 原厂 tuning | 未采用（见 docs/09-roadmap.md） |
| 冷启动 / 关机 / 熄屏 / 开机时间 | 修复已合入（init_boot v4、kpoc、关机清理、29.3 s 启动） |

---

## 1. 这是什么（以及不是什么）

原厂 Android 的相机链路由 MTK 的闭源 HAL（libcam / isptuning）驱动：它写一大串 ISP 寄存器、
把「金标 CQ 环」（command queue ring）交给 CQ 引擎硬件重放，再由 DIP/ISP 出图。这条路径在 Linux 下没有驱动可用。
本项目做的事：

1. **在 Linux 里直接操作 ISP 寄存器窗口**（自写 camreg 内核模块做 ioremap + IOMMU 分配 + mmap）；
2. 用「前端配方（30 条）+ 金标 CQ 环重放（41 条最小环）」把 CAM_A 的 pass1 跑起来，IMGO 直接 DMA 整帧 RAW10 到内存；
3. 用户态完成 RAW10 解包、去马赛克、AWB、tone、TNR、缩放与格式转换；
4. 自写 camv4l2 内核模块把用户态帧喂成一个标准 V4L2 capture 节点，普通应用无感使用；
5. 附带让这台机器能「正常当电脑用」的启动 / 关机 / 电源键 / 亮度 / 开机 logo 补丁。

**不是**：不是通用驱动，不是上游可合入的干净实现。所有寄存器值都是从**这一台机器**的 Android 上抓取、
重放、试错得到的，换机型 / 换固件版本必然不通用。

> ⚠️ **风险**：直接写 ISP / PHY / 时钟 / 电源域寄存器。做错了会设备自复位、白屏、甚至无法开机（本项目就踩过多次）。
> 只在自己的机器上做；先读 docs/08-pitfalls.md，务必准备好救援镜像与 fastboot。

---

## 2. 硬件与软件基线

| 项 | 值 |
| --- | --- |
| SoC | MediaTek MT6835，ISP6s（camsys / seninf / cam_a / camsv） |
| 后摄 | SC820ACS（Sunwin），3264×2448 RAW10，4-lane MIPI，BGGR，带 VCM（GT9764AF）+ EEPROM |
| 前置 | GC05A2，2592×1944 RAW10，2-lane MIPI，无 AF |
| 内核 | 5.15.185-android13-8-g365af54093bd，arm64，LLVM/Clang 交叉编译 |
| 发行版 | Debian（arm64），gcc 14.2，无 numpy / libjpeg（用户态全部手写 C） |
| 设备访问 | root over SSH（USB-NCM，192.168.55.2），拔数据线即断链 |

---

## 3. 目录结构

    kernel/camreg/      ISP 寄存器窗口 + DMA/IOMMU 分配模块（/dev/camreg），以及 /proc 版 camreg_proc.c
    kernel/camv4l2/     用户态推流 → 标准 V4L2 节点（/dev/camfeed + /dev/videoN，双节点）
    kernel/afreg/       VCM 对焦马达（GT9764AF）i2c 访问模块（/proc/afreg）
    kernel/regwin/      通用物理窗口只读取证模块（/proc/regwin，排障用）
    userspace/          camrec5/6（采集）、camvid/camfront（实时链路）、camisp（离线转图）、isp_lib/isp_front（ISP 库）
    userspace/tools/    raw2png / autoisp / camfeedfmt / camfeedstat / 测试工具
    boot/               initramfs 补丁（init_boot v4）+ 82 个板级系统补丁（20 个 systemd 单元、
                        27 个 /usr/local/sbin、20 个 /usr/local/bin、15 个 /persist 脚本，含 MD5SUMS）
    docs/               技术文档（寄存器机制、CQ 环、ISP、V4L2、前置、启动、雷区、路线图）
    release/            可直接刷入的镜像与校验和（init_boot v4）

---

## 4. 快速开始

### 4.1 用户态（在设备上原生编译，最快）

    # 设备侧（Debian arm64，gcc 14.2）
    mkdir -p /root/cam && cd /root/cam
    # 把本仓库 userspace/ 拷进来后：
    make            # 产出 camrec6 camvid camfront camisp camfeedfmt camfeedstat
    sudo make install   # 装到 /usr/local/bin（可选）

单帧抓一张全分辨率 RAW，并转成 PNG：

    # 1) 内核模块与节点（camdaemon 会做，这里手动来一遍）
    insmod /root/camreg_isp_top.ko devname=1a030000.cam1_legacy
    minor=$(awk "/camreg_isp/{print \$1}" /proc/misc); mknod /dev/camreg c 10 $minor
    # 2) 抓 1 帧（-p 前缀，-v/-e/-g 为 VTS/曝光/增益）
    ./camrec6 -n 1 -p /root/cap_ -v 5000 -e 4900 -g 0
    # 3) 解包 + ISP + PNG（-o 输出，--scale 1 = 全分辨率）
    ./camisp /root/cap__0001.raw -o /root/cap.png --scale 1 --awb-est v2 --tone v2

### 4.2 实时链路 + V4L2（推荐，等价于出厂体验）

    # 采集 + ISP + 推流到 /dev/camfeed，由 camv4l2 暴露成 /dev/videoN
    insmod /root/camv4l2.ko video_nr=3
    ./camvid -n 100000 -L /dev/camfeed -F 30 -v 6000 -e 2400 -g 0 -A -t 330 \
             --vcontrast 1.25 --vgamma 2.0
    # 另一个终端，用任何 V4L2 应用取流
    ffplay -f v4l2 -input_format yuyv422 -video_size 1280x960 /dev/video3

生产环境下由 camdaemon（userspace/camdaemon.sh）守护：应用开流才起管线，关流即停，自动选后摄/前置节点。
把 camdaemon.service 装进 systemd（见 kernel/camv4l2/camdaemon.service）。

### 4.3 内核模块（在 x86 主机 + WSL 里交叉编译）

需要与设备同版本的 Android 内核源码树与已编译产物（KBUILD 输出目录）：

    export KDIR=/path/to/kb_out_cfi        # 已编译的内核树（含 Module.symvers）
    export ARCH=arm64 LLVM=1
    cd kernel/camv4l2 && ./build_wsl.sh    # 或：make -C $KDIR M=$PWD ARCH=arm64 LLVM=1 modules
    cd kernel/camreg   && ./build_camreg_isp.sh
    cd kernel/afreg    && make -C $KDIR M=$PWD ARCH=arm64 LLVM=1 modules

模块必须与设备内核 vermagic 完全一致，否则 insmod 报 -EINVAL / unknown symbol。

---

## 5. 一次性系统补丁（boot/）

这些补丁解决「装完 Debian 后不能当电脑用」的问题。**82 个文件与作者实机逐字节一致**（md5 清单见 boot/MD5SUMS）：

    boot/sbin/tb335-load-modules.sh        # 220 个模块按依赖顺序批量加载（开机 105 s → 29 s 的关键之一）
    boot/bin/tb335-powerkey.py             # 电源键：短按熄屏、长按 3 s 菜单、长按 8 s 强制关机
    boot/sbin/tb335-initboot-guard.sh      # 守护 init_boot 分区，被覆盖时自动刷回 v4（冷启动修复）
    boot/sbin/a9b-kpoc-normalize.sh        # 修「拔充电线就关机」
    boot/sbin/a9b-rescue-cleanup.sh        # 修「关机卡 90 s」
    boot/systemd/*.service                 # 20 个 unit：模块加载、GPU/DRM、声卡、插孔、WiFi/BT、相机守护…
    boot/persist/*                         # 15 个 /persist 脚本：切安卓/回 Debian、WiFi/音频/蓝牙/输入 bring-up
    boot/initramfs/patch-a9b-init-v4.sh    # 生成 init_boot v4 的 initramfs 补丁脚本

逐文件说明与部署命令见 boot/README.md；这些补丁依赖本机设备名，换机前先读 docs/08-pitfalls.md。

镜像（release/）：

| 文件 | 大小 | MD5 | 用途 |
| --- | --- | --- | --- |
| release/init_boot_v4.img | 8 MiB | bc8e7e9995caedd3040d3c8e96f0896f | 冷启动卡 logo 修复（LZ4 legacy initramfs） |
| release/deb_init_boot_v4.img | 8 MiB | 同左 | 同上（Debian 侧备份副本） |
| release/modules/camreg_isp_top.ko | 36 KB | 817d89245b74cc792000616c9292b717 | 寄存器/IOVA 模块（设备在用） |
| release/modules/camv4l2.ko | 40 KB | 36037b0052ee1c8af2a79cae184b6b70 | V4L2 代理模块（设备在用） |
| release/modules/afreg.ko | 23 KB | 6feb00d6caee3a61bfde3f9c7bb098dc | VCM 对焦 i2c 模块（设备在用） |

刷入：

    # 设备上（root，谨慎）：init_boot 在 /dev/sdc31
    dd if=init_boot_v4.img of=/dev/sdc31 bs=1M conv=fsync
    # 或 fastboot（切到 fastboot 后）
    fastboot flash init_boot_a init_boot_v4.img

详细原理与救援流程见 docs/07-boot-and-flash.md。

---

## 6. 文档索引

| 文档 | 内容 |
| --- | --- |
| docs/00-overview.md | 整条链路的数据流与关键结论（先读这个） |
| docs/01-hardware.md | ISP 寄存器窗口、传感器、CQ 引擎、时钟/电源雷区 |
| docs/02-cq-ring.md | 金标 CQ 环重放：41 条最小环、6 条硬承重、0x1805 机制 |
| docs/03-camreg-capture.md | camreg 模块 ABI、多环硬件轮转采集器 camrec6、AAO 统计 |
| docs/04-isp-pipeline.md | RAW10 40 bit 解包、AWB / tone / TNR、性能、参数、画质短板 |
| docs/05-v4l2-camfeed.md | /dev/camfeed ABI、双 V4L2 节点、camdaemon、应用兼容与踩坑 |
| docs/06-front-camera.md | 前置 GC05A2：端口定位、寄存器配方、限制 |
| docs/07-boot-and-flash.md | 启动加速、init_boot v4、关机/电源键/logo、WiFi/BT |
| docs/08-pitfalls.md | 雷区清单（会坏机的操作都在这里） |
| docs/09-roadmap.md | 已完成 / 未完成 / 建议的下一步（含已放弃路线的原因） |
| docs/10-build-deploy.md | 编译、部署、故障对照表、排障纪律 |
| VERSIONS.md | 源码 ↔ 设备实机二进制的 MD5 对照、分支差异 |
| boot/README.md | 82 个板级系统补丁的逐文件说明与部署步骤 |

---

## 7. 许可

* 用户态程序与文档：MIT（LICENSE）
* 内核模块（kernel/ 下）：GPL-2.0-only（LICENSE-GPL-2.0，与 Linux 内核一致）

寄存器表 / CQ 环数据来自本机 Android 固件的运行时抓取，仅作互操作与研究用途；
不包含任何原厂二进制、厂商 tuning 文件或反编译产物。

## 8. 致谢与免责

本项目是在一台真实零售机上反复试错做出来的：烧过 init_boot、死过机、失手过整窗扫寄存器。
所有结论都以「实测可复现」为准，未验证的推测在文档里会明确标注。
按本文档操作导致的任何设备损坏、数据丢失，由操作者自行承担。

---

## 9 · 仓库核对情况（2026-11-19）

仓库内容已与实机逐项 md5 核对（2026-11-19，详见 VERSIONS.md）：

* 用户态源码 7/7 与设备 /root 完全一致：camvid.c / camrec5.c / camrec6.c / camisp.c / isp_lib.c / isp_lib.h / camdaemon.sh；
* 内核模块源码 == WSL 构建副本，且 `release/modules/` 里的三个 .ko 与设备上正在跑的 **MD5 完全相同**
  （camreg_isp_top.ko 817d8924… / camv4l2.ko 36037b00… / afreg.ko 6feb00d6…，vermagic 均为 5.15.185-android13-8-g365af54093bd）；
* release/init_boot_v4.img 与设备 init_boot 分区内容一致（md5 bc8e7e99…）；
* 设备侧确认在跑：camreg_isp / camv4l2 / afreg 已加载，/dev/camreg、/dev/camfeed、/dev/video3 齐全，camdaemon 等服务 active。
* 板级系统补丁 82/82 与设备 md5 完全相同：boot/{sbin,bin,persist,systemd} 共 80 个脚本/unit（对照 /usr/local/sbin、/usr/local/bin、/persist、/etc/systemd/system），清单在 boot/MD5SUMS；
  其中 boot/sbin/a9b-rescue-cleanup.sh 设备上的注释曾被写坏（功能相同），已把仓库里的干净版本回写设备并复验。

## 10 · 不含什么

* 不含原厂固件、vendor 镜像、MTK tuning 二进制或反编译产物；
* 不含原厂 init_boot 备份（仅提供其 MD5 9ef20ef7…，请自行备份）；
* 不含相机标定/EEPROM 数据；
* boot/ 下的补丁是针对**本机原厂固件**的，换固件版本需要重新对齐。

## 11 · 安全提醒

直接写 ISP / PHY / 时钟 / 电源域寄存器是危险操作。开始之前请确保：

1. 有可用的 fastboot 与好镜像（release/init_boot_v4.img + 原厂备份）；
2. 已读 docs/08-pitfalls.md；
3. 能接受机器变砖、数据丢失的风险。
