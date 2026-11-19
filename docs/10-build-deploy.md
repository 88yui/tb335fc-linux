# 10 · 编译、部署、排障

## 1. 两种编译环境

| 目标 | 在哪编 | 工具链 |
| --- | --- | --- |
| 内核模块（.ko） | WSL（/home/qiji_/） | LLVM 交叉：ARCH=arm64 LLVM=1 |
| 用户态程序 | 直接在设备上编 | 设备自带 gcc 14.2 / make |

内核版本必须与设备一致：**5.15.185-android13-8-g365af54093bd**（vermagic 必须完全一致，否则 insmod 失败）。

### 1.1 内核模块

    # WSL 里，用设备同版本内核的构建目录
    make -C /home/qiji_/kernel_work/kb_out_cfi M=$PWD ARCH=arm64 LLVM=1 modules

    # 5.15 源码树 + 独立输出目录的写法
    make -C /home/qiji_/kernel_work/kernel-5.15 O=/home/qiji_/kb_out_vt M=$PWD \
         ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- LLVM=1 LLVM_IAS=1 modules

验证：

    strings camv4l2.ko | grep ^vermagic      # 必须等于设备 uname -r
    modinfo camv4l2.ko

### 1.2 用户态

设备上：

    gcc -O3 -march=native -pthread -o camvid camvid.c isp_lib.c -lm
    gcc -O2 -o camrec6 camrec6.c -pthread
    gcc -O3 -o camfront camfront.c isp_front.c -lm -pthread

（具体见各源文件顶部的编译命令注释；isp_lib.c 需要 -lm。）

## 2. 部署

    # 传文件前后都比 md5，避免半截文件
    scp -i C:/Users/qiji_/.ssh/id_ed25519_hinix -o BatchMode=yes \
        -o StrictHostKeyChecking=no camvid.c root@192.168.55.2:/root/
    ssh root@192.168.55.2 "md5sum /root/camvid.c"

模块加载顺序（相机）：

    insmod /root/camreg_isp_top.ko devname=1a030000.cam1_legacy
    mknod /dev/camreg c 10 $(awk '$2=="camreg"{print $1}' /proc/misc)   # 首次/重启后
    insmod /root/camv4l2.ko video_nr=3
    ln -sf /dev/video3 /dev/video0

板级系统补丁（systemd 服务 + /usr/local 脚本 + /persist 脚本，共 82 个文件）：

    cd boot
    cp sbin/* /usr/local/sbin/ ; cp bin/* /usr/local/bin/ ; cp persist/* /persist/
    cp systemd/* /etc/systemd/system/
    chmod +x /usr/local/sbin/* /usr/local/bin/* /persist/*.sh /persist/*.py
    systemctl daemon-reload
    systemctl enable --now tb335-modules tb335-powerkey tb335-wait-gpu camdaemon ...
    md5sum -c MD5SUMS        # 与本项目实机逐字节核对

逐文件说明见 boot/README.md。

## 3. 常见故障对照表

| 现象 | 原因 | 处理 |
| --- | --- | --- |
| IMGO 一个字节都不写 | DB_EN 没有 0→1 边沿 / FBC 没关 / INNER 没镜像 | 见 03 的坑 1~3 |
| IMGO 只写 3968 B 就停 | 没有每帧用 0x1805 重装配置 | 用 41 条环，不要静态直写 |
| 只有首帧更新 | 环里 0x214 没指回自己的环 | 8 环 + 8 cell 轮转 |
| 帧内容像乱码/全花 | RAW10 解包用了 uint32 / Bayer 顺序错 | 40 bit 小端打包 + BGGR |
| 画面整体发灰 | tone 高光取了 100% 分位 | 改成 99.5% |
| 低光彩线 | 高增益行相关噪声混叠成色度 | 长曝优先降增益 + 色度 4×4 + 3×3 中值 |
| ffmpeg: ioctl(VIDIOC_G_INPUT) 不支持 | 模块没实现 enum_input/g_input/s_input | 补三个回调 |
| insmod: invalid module format | vermagic 不一致 | 用设备同版本内核重编 |
| 进程 D 状态杀不掉 | ioctl 里重复锁 vdev->lock | 去掉锁，只能重启恢复 |
| 碰相机寄存器后设备重启 | 电源域没上电 / 整窗扫 | 先 OPEN 传感器再写；只读手挑地址 |
| 不插数据线进不了桌面 | init_boot 里 ttyGS0 阻塞 PID1 | 刷 release/init_boot_v4.img |
| poweroff 卡 90 s | 救援 shell 忽略 SIGTERM | 装 a9b-rescue-cleanup |
| 拔充电线自动关机 | kpoc boot_mode 8/9 | 装 a9b-kpoc-normalize |

## 4. 排障纪律（血泪）

1. 动任何东西之前先记 `cat /proc/uptime` / boot time —— **设备一旦自复位，整轮实验作废**，要重录基线。
2. 不要整窗读寄存器、不要整表读 clk_summary/pm_genpd_summary。
3. 改寄存器前后都读回，倒序还原，RESTORE_MISMATCH 必须为 0（W1C 位除外）。
4. 关机 / 开机务必三件套齐（否则可能停在半状态需要救援）。
5. ssh 命令里的 $( )、反引号、模板字符串容易在本地就被展开 —— **一律本地写 .sh → scp → ssh 执行**。
6. run_code / 单条命令有 120 s 上限，长任务放后台或分段。