# 预编译内核模块

这些 .ko 是从本仓库 kernel/ 下的源码编译出来的，**就是设备上正在跑的那一份**（MD5 见 VERSIONS.md）。

| 文件 | 模块名 | 用途 |
| --- | --- | --- |
| camreg_isp_top.ko | camreg_isp | 寄存器读写 + IOVA 分配（/dev/camreg），相机链路必需 |
| camv4l2.ko | camv4l2 | 用户态推流 → 标准 V4L2 节点（/dev/camfeed → /dev/videoN） |
| afreg.ko | afreg | VCM 对焦马达 i2c 直通（/proc/afreg） |

vermagic：

    5.15.185-android13-8-g365af54093bd SMP preempt mod_unload modversions aarch64

**只能用于同内核版本**。内核一变就必须用 kernel/ 下源码重编（见 docs/10-build-deploy.md）。

安装：

    insmod camreg_isp_top.ko devname=1a030000.cam1_legacy
    insmod camv4l2.ko video_nr=3
    insmod afreg.ko bus=8 addr=0x0C