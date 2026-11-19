# boot · 板级系统补丁（systemd 服务 / 辅助脚本 / 恢复脚本）

本目录是**设备上实际在跑**的系统层补丁，来源是联想 TB335FC（MT6835 + Debian，内核 5.15.185-android13-8-g365af54093bd）
的一台实机，2026-11-19 全量拉取并逐文件 md5 核对：**83 个文件与设备上的对应文件全部 md5 相同**
（清单见 MD5SUMS，核对结果见 ../VERSIONS.md 第 5 节）。

它们解决的问题都属于「这台机器特有」的坑：MTK 充电/关机模式、A9b initramfs 冷启动、
GPU/DRM 节点缺失、声卡与耳机插孔、屏幕键盘、亮度、WiFi/BT 固件、相机按需起流等。
在别的机器上不具备通用性，移植前请先读 ../docs/08-pitfalls.md。

## 目录

    initramfs/    A9b initramfs 修复脚本（生成 release/init_boot_v4.img 的源）
    systemd/      20 个 unit（tb335-*.service + camdaemon.service），与 /etc/systemd/system 一致
    sbin/         27 个 /usr/local/sbin/tb335-* 与 a9b-*
    bin/          20 个 /usr/local/bin/tb335-* 与 cammirror
    persist/      15 个 /persist 下的恢复/切换/WiFi/音频/输入辅助脚本
    MD5SUMS       上面 83 个文件的 md5

## 各文件作用

### systemd/（20 个 unit，内容与设备逐字节一致）

    tb335-modules.service            开机顺序加载 220 个内核模块（tb335-load-modules.sh）
    tb335-powerkey.service           电源键守护（tb335-powerkey.py）
    tb335-wait-gpu.service           等 Mali render 节点可用再起显示管理器（tb335-wait-gpu.sh）
    tb335-mali0-node.service         补 /dev/mali0 misc 节点（内核按设计不注册）
    tb335-panel60-guard.service      60 Hz 面板模块守卫：300 s 内没有 OK 标记就回滚并重启
    tb335-initboot-guard.service     init_boot 自愈（见 docs/07 第 2.4 节）
    tb335-drm-compat.service         显示驱动加载前插入兼容垫片模块（tb335-drm-compat-load.sh）
    tb335-hide-dp.service            隐藏误报连接的 writeback DP
    tb335-usb-host.service           挂上有电 dock 时保持 USB data role = host
    tb335-audio.service              声卡 bring-up（/persist/audio-up.sh）
    tb335-audio-state.service        把可用的混音器状态写回去（tb335-audio-state.sh）
    tb335-mic-route.service          内置麦克风采集路由（UCM 不可用，直接写 mixer）
    tb335-snd-watch.service          保持 /dev/snd 目录可遍历（0755）
    tb335-jack-watch.service         3.5 mm 插孔自动切换（ALSA jack -> HPL/HPR mux）
    tb335-devperm.service            /dev/snd、/dev/rfkill 节点权限
    tb335-input-node.service         /dev/input 节点同步（本机 udev 热插拔不可用，/persist/input-node-sync.sh）
    tb335-wifi.service               MTK connsys WiFi bring-up（/persist/wifi-up.sh）
    tb335-wifi-dhcp.service          wlan0 关联后取 DHCP（/persist/wifi-dhcp.sh）
    tb335-bt.service                 蓝牙桥（stpbt -> HCI H4）与 hci0 上电（/persist/bt-bridge.py）
    camdaemon.service                相机按需起流（streaming=1 才启动 camvid；脚本 /root/camdaemon.sh）

### sbin/（27）

    a9b-kpoc-normalize.sh      充电器启动（KPOC, bootmode 8/9）时自动转一次正常重启，避免「拔线就断电」
    a9b-rescue-cleanup.sh      关机前清掉 A9b 救援 shell（/bin/sh -i 忽略 SIGTERM，会把 poweroff 拖到 90 s）
    a9b-shutmark.sh            关机/重启打时间戳到 /persist/a9b/shut.log
    tb335-load-modules.sh      220 个模块的顺序显式加载表（每行一个 insmod）
    tb335-mali0-node.sh        创建 /dev/mali0
    tb335-wait-gpu.sh          等 vendor DRM + Mali 节点就绪
    tb335-panel60-guard.sh     60 Hz 面板守卫
    tb335-initboot-guard.sh    发现 init_boot 被写回原版（无主机时 ttyGS0 阻塞）就重新刷 v4
    tb335-hide-dp.sh           隐藏误报 DP 连接器
    tb335-drm-compat-load.sh / tb335-drm-rollback.sh / tb335-drm-trialok.sh        显示驱动重编试验的自愈三件套
    tb335-bringup-force-revert.sh / tb335-bringup-rollback.sh / tb335-bringup-trialok.sh   启动期补丁的试验/回滚状态机
    tb335-kmsg-capture.sh / tb335-kmsg-persist.py      内核日志落盘（给会硬复位的试验取证）
    tb335-kms                  （二进制）DRM/KMS 直显测试工具，源码不在本仓库
    tb335-coldreboot.sh        RTC 闹钟 + 优雅关机 + 到点自动上电（热重启会丢 panfrost）
    tb335-poweroff.sh          长按强制断电
    tb335-powerkey.py          电源键 + 背光守护（与 bin/ 下同名文件同源）
    tb335-cursor-autohide.sh   有指针设备显示光标，否则隐藏
    tb335-usb-host-watch.sh    USB 数据角色切换
    tb335-media-rescan.sh      等声卡出现（最多 180 s）并重新扫描
    tb335-mic-route.sh         麦克风路由
    tb335-post-v7-check.sh     启动后自检（守卫状态 + 给 MFGSYS 上电）
    tb335-jack-force.sh / tb335-jack-watch.py     插孔强制/自动切换

### bin/（20）

    tb335-powerkey.py     电源键（3 s 关机菜单 / 8 s sysrq poweroff / 轻按熄屏 + 650 ms 背光淡出）
    tb335-brightness.py   亮度调节 GUI（本机没有 /sys/class/backlight）
    tb335-kb-move.py / tb335-keyboard     屏幕键盘与拖动条
    tb335-devperm.sh      /dev 节点权限
    tb335-snd-perm.sh / tb335-snd-watch.sh     声卡节点权限
    tb335-audio-state.sh  混音器状态恢复
    tb335-jack-toggle.sh  手动切耳机/扬声器
    tb335-gpu-mode        切换 Xorg 2D 加速模式（glamor / 软件）
    tb335-gpu-warmup      预热 GPU（第一次作业会超时触发 reset）
    tb335-sink            （二进制）音频直通工具，源码不在本仓库
    tb335-play            全屏 DRM 直显播放器（脚本）
    tb335-video-browser / tb335-video-mode     浏览器视频模式 / 关开 xcompmgr 软件合成器
    tb335-rotate.sh       手动切 DSI-1 旋转（本机没有可用的加速度计驱动）
    tb335-plasma-wayland.sh     仅当 /etc/tb335-plasma-test 存在时启动 Plasma Wayland
    tb335-install-deb     双击安装 .deb（root 直接跑，desk 走免密 sudo）
    tb335-wifi-gui        WiFi 图形界面入口（/persist/wifi-gui.py）
    cammirror             相机水平镜像开关（用户态；源码见 ../userspace/cammirror，md5 相同）

### persist/（15）

    switch_android.sh         Debian -> Android：写 boot_a / init_boot_a / misc 三件套（SOP 3.1，原文照抄）
    switch_android_dev.sh     切换脚本的开发版
    restore_debian_v4.sh      Android -> Debian：同样三件套，init_boot 固定用 v4 修复版
    wifi-up.sh                MTK connsys WiFi bring-up（loader -> launcher -> /dev/wmtWifi）
    wifi-dhcp.sh              wlan0 DHCP
    wifi-gui.py               WiFi 图形界面
    wmt-bringup.sh            WMT 子系统 bring-up 辅助
    audio-up.sh               声卡 bring-up（modprobe machine driver + mknod /dev/snd）
    audio-bringup.sh          音频 bring-up 记录版
    alsactl_list.py           导出可用 mixer 控件清单
    bt-bridge.py              蓝牙 stpbt -> HCI H4 桥
    bt-power.sh               蓝牙上电
    input-node-sync.sh        手动同步 /dev/input 节点
    logd-shim.py              Android logd 兼容垫片
    virt_mouse.py             虚拟鼠标（触摸板/指针模拟）

## 部署

假设仓库在板子的 /root/tb335fc-linux-camera：

    cd /root/tb335fc-linux-camera/boot
    cp sbin/*      /usr/local/sbin/
    cp bin/*       /usr/local/bin/
    cp persist/*   /persist/
    cp systemd/*   /etc/systemd/system/
    chmod +x sbin/* bin/* persist/*
    systemctl daemon-reload
    systemctl enable --now tb335-modules tb335-powerkey tb335-wait-gpu tb335-mali0-node
    systemctl enable --now tb335-panel60-guard tb335-initboot-guard tb335-usb-host
    systemctl enable --now tb335-audio tb335-audio-state tb335-mic-route tb335-snd-watch
    systemctl enable --now tb335-jack-watch tb335-devperm tb335-input-node
    systemctl enable --now tb335-wifi tb335-wifi-dhcp tb335-bt
    systemctl enable --now camdaemon

核对是否与作者实机一致：

    cd boot && md5sum -c MD5SUMS

## 注意

* init_boot 的镜像本体不在本目录，见 ../release/ 与 docs/07；刷错 init_boot 会无限重启。
* 开关机/切安卓的三件套（boot_a / init_boot_a / misc）必须一起写，缺一个会起不来。
* sbin/tb335-kms 与 bin/tb335-sink 是预编译二进制（与设备一致），源码不在本仓库。
* 所有脚本以 root 运行；tb335-install-deb 依赖免密 sudo 配置。
* 这些脚本高度依赖本机设备名（sdc31/sdc43、1a030000.cam1_legacy、mt6835-mt6377 声卡等），换机前先读 docs/08-pitfalls.md。
