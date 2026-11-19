# tb335fc-linux-camera

> 复用原厂 Android 内核，将联想 TB335FC 型号平板刷入 Linux（内核自编）。

联想 TB335FC（MT6835 / MTK ISP6s）在 Linux 下驱动前后摄像头的源码与板级补丁。

## 兼容环境

| 项目 | 值 |
| --- | --- |
| 机型 | Lenovo TB335FC（MT6835） |
| 内核 | 5.15.185-android13-8-g365af54093bd（自编，Ubuntu clang 21） |
| 发行版 | Debian 13 (trixie) arm64 |
| 后摄 | SC820ACS 3264x2448 RAW10，4-lane，CSI 端口 0/1 |
| 前摄 | GC05A2 2592x1944 RAW10，2-lane，CSI 端口 2 / seninf5 |

## 目录结构

```
boot/       板级运行文件（systemd 服务、/usr/local/bin|sbin 工具、/persist 脚本、initramfs 补丁）
  bin/        安装到 /usr/local/bin
  sbin/       安装到 /usr/local/sbin
  persist/    安装到 /persist
  systemd/    安装到 /etc/systemd/system
  modules/    相机相关模块加载清单
  initramfs/  initramfs(/init) 补丁脚本
  MD5SUMS     所有文件的校验清单
kernel/     内核模块源码
  camreg/    /dev/camreg 调试/录制字符设备（CQ ring 重放、IMGO 采集）
  camv4l2/   V4L2 代理模块（vb2_vmalloc），把 /dev/camfeed 暴露为 /dev/videoN
  afreg/     VCM(GT9764AF) 对焦马达 i2c 访问模块
  regwin/    任意物理寄存器窗口只读映射模块
userspace/  用户态程序
  camvid.c   后摄采集 + ISP + V4L2 喂帧 + AF/AE 状态机
  camfront.c 前摄采集 + ISP
  isp_lib.c  解拜耳 / AWB / tone / TNR / 缩放
  camdaemon.sh  运行时守护脚本（insmod、建节点、按 /persist/cam_which 起前后摄）
  tools/     调试工具（isptest、raw2png、raw2video、camfeedfmt 等）
```

## 构建

用户态（在设备或同版本 arm64 交叉环境）：

```sh
cd userspace && make
```

内核模块需要与设备内核同版本的构建树：

```sh
make -C <kernel_out> M=$PWD ARCH=arm64 LLVM=1 LLVM_IAS=1 CROSS_COMPILE=aarch64-linux-gnu- modules
```

## 运行时

`boot/` 下的文件按目录映射安装，启用 `tb335-camdaemon.service` 后会加载 `camreg_isp_top.ko`、
`camv4l2.ko`、`afreg.ko`，并由 `/root/camvid`（后摄）或 `/root/camfront`（前摄）输出到
`/dev/camfeed`，再由 camv4l2 提供标准 V4L2 节点。

## 许可

GPL-2.0（全文见 LICENSE）。
