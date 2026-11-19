# VERSIONS · 源码 ↔ 实机二进制对照

核对时间 2026-11-19（设备 uptime 2963 s，内核 5.15.185-android13-8-g365af54093bd）。
下表全部用 `md5sum` 实测比对，不是估算。

## 1. 用户态源码：仓库 == 设备 /root（7/7 完全一致）

| 仓库文件 | 设备路径 | MD5 |
| --- | --- | --- |
| userspace/camvid.c | /root/camvid.c | b6f37ab288778c1ea973c4f137f08721 |
| userspace/camrec5.c | /root/camrec5.c | 73ed608d85ad3a575145ba4c8ab122e6 |
| userspace/camrec6.c | /root/camrec6.c | 5d83f10dcb1450919a9bbcf54f162b87 |
| userspace/camisp.c | /root/camisp.c | 2de38f5473f0e92a29c0e74fd989e633 |
| userspace/isp_lib.c | /root/isp_lib.c | 54aeb23a82a379fe9f4d3a5b0c03a92a |
| userspace/isp_lib.h | /root/isp_lib.h | b6d3a2b4e919e9a1d299527b6072217f |
| userspace/camdaemon.sh | /root/camdaemon.sh | 2900b2db8de66dabbe6110bc74b59a9e |

## 2. 内核模块：仓库源码 == WSL 构建副本 == 设备上正在跑的 .ko

| 仓库源码 | 构建副本 | 产出的 .ko（release/modules/） | MD5 |
| --- | --- | --- | --- |
| kernel/camreg/camreg_isp.c | /home/qiji_/camreg_isp/camreg_isp.c（+ 临时/camreg） | camreg_isp_top.ko | 817d89245b74cc792000616c9292b717 |
| kernel/camv4l2/camv4l2.c | /home/qiji_/kmod_camv4l/camv4l2.c | camv4l2.ko | 36037b0052ee1c8af2a79cae184b6b70 |
| kernel/afreg/afreg.c | /home/qiji_/afreg/afreg.c | afreg.ko | 6feb00d6caee3a61bfde3f9c7bb098dc |
| kernel/regwin/regwin.c | /home/qiji_/regwin/regwin.c | regwin.ko | f1e474e832793167e3691678e98ec8fd（仅取证用，设备未装） |

三个模块的 vermagic 全部为：

    5.15.185-android13-8-g365af54093bd SMP preempt mod_unload modversions aarch64

license=GPL。上面的 .ko 直接可用于**同版本内核**的设备。

> 注：`kernel/camreg/camreg_isp.c` 与 `kernel/camreg/camreg.c` 是同一模块的两份快照（后者为早期）。

## 3. 设备上的用户态二进制

| 程序 | 日期 | MD5 前 8 位 |
| --- | --- | --- |
| /root/camvid | 2026-11-19 04:51 | 08d2c058 |
| /root/camfront | 2026-11-18 21:56 | 1c6dad40 |
| /root/camrec6 | 2026-11-19 03:33 | 8e8993ce |
| /root/camrec5 | — | 86d9e3e2 |
| /root/camisp | — | a394768f |
| camfeedfmt | — | fb3169a1 |
| camfeedstat | — | 14297f6c |
| camvid_af（AF 分支） | 2026-11-18 21:39 | b5fe5050 |

## 4. 镜像

| 文件 | 大小 | MD5 |
| --- | --- | --- |
| release/init_boot_v4.img | 8,388,608 | bc8e7e9995caedd3040d3c8e96f0896f |
| release/deb_init_boot_v4.img | 8,388,608 | bc8e7e9995caedd3040d3c8e96f0896f |
| （原厂 init_boot 备份，本仓库不含） | 8,388,608 | 9ef20ef76dc8deb877247e6a2bf6dd83 |

## 5. 板级系统补丁：仓库 boot/ == 设备（82/82 完全一致）

2026-11-19 从设备整包拉取（tar 打包 /etc/systemd/system、/usr/local/sbin、/usr/local/bin、/persist），
按目录落到 boot/ 后逐文件 md5 比对，结果 **82 个文件全部相同**（清单 boot/MD5SUMS）：

| 仓库目录 | 设备路径 | 文件数 | 说明 |
| --- | --- | --- | --- |
| boot/systemd/ | /etc/systemd/system/ | 20 | 19 个 tb335-*.service + camdaemon.service |
| boot/sbin/ | /usr/local/sbin/ | 27 | tb335-*.sh/*.py、a9b-*.sh、tb335-kms（二进制） |
| boot/bin/ | /usr/local/bin/ | 20 | tb335-* 工具 + cammirror |
| boot/persist/ | /persist/ | 15 | 切安卓/回 Debian、WiFi/音频/蓝牙/输入 bring-up 脚本 |

核对方式（在仓库根目录）：

    cd boot && md5sum -c MD5SUMS

已知的历史差异都已消除：

* /usr/local/sbin/a9b-rescue-cleanup.sh 设备版只有前 4 行注释是乱码（代码完全相同），
  已把仓库里的干净版本回写设备，现在两边都是 b08397afbdf5b5c735d8354dd34879f0；
* 早期试验期的 tb335-rotate-guard.service / tb335-session-heal.service 对应的脚本在现网设备上已不存在，未收录；
* userspace/cammirror 与 boot/bin/cammirror 是同一文件（32fa089a270446aedccfca047a77912e），保留两份仅为目录完整性。

## 6. 已知分支差异

* **AF**：`camvid_af.c`（127,148 B，2026-11-18）含自动对焦状态机与 --af/--af-iv；
  本仓库的 `camvid.c`（122,119 B，2026-11-19）**没有** AF 代码 —— 这是当前唯一的功能性缺口。
  设备上 /root/camvid_af 是能用的 AF 二进制。
* **前置 ISP**：`camfront.c` + `isp_front.c` 是前置专用副本（含 hflip 与前置几何），与后摄 isp_lib 同源不同版。

## 7. 环境

| 项 | 值 |
| --- | --- |
| 设备内核 | 5.15.185-android13-8-g365af54093bd |
| 设备 gcc | 14.2 |
| 模块编译 | WSL + LLVM/Clang 交叉（ARCH=arm64 LLVM=1），KDIR 指向设备同版本内核构建目录 |