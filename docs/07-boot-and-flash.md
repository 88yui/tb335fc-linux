# 07 · 系统：开机、关机、电源键、启动 logo

## 1. 开机时间

| | systemd-analyze | 说明 |
| --- | --- | --- |
| 优化前 | 105.2 s | 模块逐个晚加载 + 多个 wait |
| 优化后 | 29.3 s | kernel 15.8 s + userspace 13.5 s |
| 按电源到桌面 | 约 32–34 s | 其中 bootloader（preloader+bl2+tfa+lk）约 15.2 s 是平台固有 |

做的事：

* 模块加载重排：boot/sbin/tb335-load-modules.sh（220 个模块一遍过；15 个 bundle 前移到 pos102）。
  注意：**用 ssh 一条命令跑 220 个 insmod 会被截断**，必须在设备端用脚本/python 执行。
* 去掉 tb335-modules.service 里的 `sleep 3`。
* tb335-mali0-node.sh 先判断 misc 节点是否已存在（避免重复创建失败拖慢）。
* `systemctl mask plymouth-quit-wait`（它没有 [Install] 段，`disable` 无效，必须 mask）。

## 2. init_boot 与「不插数据线进不了桌面」

### 2.1 initramfs 格式铁律

init_boot 分区里的 initramfs **必须是 LZ4 legacy 容器**：

    0x184C2102 | u32 chunk_len | chunk ... （可多块，每块 ≤ 8 MB）| 0x00000000 结束

直接塞一个未压缩的 newc cpio 会 `invalid header` → 无限重启。

### 2.2 真正的 bug

initramfs 的 /init 里日志函数 O() 用 `echo ... > /dev/ttyGS0`。没有 USB 主机时这个 write **永久阻塞 PID1**
（发生在 rootfs 挂载成功后的第一条 O()），插上电脑枚举出 ttyGS0 后才继续 —— 所以「必须插数据线才能进桌面」。

修复（boot/initramfs/patch-a9b-init-v4.sh）：

* 所有 ttyGS0 写操作全部后台化；
* switch_root 的 stdio 改成 /dev/null；
* 兜底日志改 /dev/console。

产物：release/init_boot_v4.img / deb_init_boot_v4.img（各 8,388,608 B，md5 bc8e7e9995caedd3040d3c8e96f0896f）。

### 2.3 刷写与恢复

    # Debian 侧（root）：
    dd if=init_boot_v4.img of=/dev/sdc31 bs=1M conv=fsync

    # 救援（卡 logo 时）：设备进 fastboot，用 fastboot 刷回已知好的镜像
    fastboot flash init_boot_a init_boot_a9b.img
    fastboot reboot

**原厂 init_boot 备份 md5 = 9ef20ef76dc8deb877247e6a2bf6dd83**，请自己另存一份（本仓库不含）。

### 2.4 自愈守护

boot/sbin/tb335-initboot-guard.sh + boot/systemd/tb335-initboot-guard.service（oneshot，After=local-fs.target）在每次开机时检查：

* /dev/sdc31 前 8 MB 的 md5 是否 == 9ef20ef7…（原版）且 /persist/ib_current.img 是 v4；
* 是则自动把 v4 刷回去，日志写 /persist/initboot_guard.log。

## 3. 关机 90 秒

故障：poweroff 要等约 90 s。原因：A9b 救援包装器里跑着 `/bin/sh -i`，它忽略 SIGTERM（SigIgn=0x4006），
systemd-shutdown 只能等满 90 s 超时。

修复：/usr/local/sbin/a9b-rescue-cleanup.sh + service，在关机前主动杀掉那个 shell ⇒ poweroff 约 6.4 s。
（源文件：boot/sbin/a9b-rescue-cleanup.sh，与设备 md5 b08397afbdf5b5c735d8354dd34879f0 一致）

## 4. 「一拔充电线就关机」

MTK 的 kpoc_power_off_check 在 `boot_mode == 8 或 9` 且 `vbus < 2500` 时直接断电。
boot/sbin/a9b-kpoc-normalize.sh 读 /proc/device-tree/chosen/atag,boot 偏移 8 的 u32（arm64 小端），
发现是 8/9 就打标记 reboot 回正常模式。

## 5. 电源键

boot/bin/tb335-powerkey.py（设备上的 /usr/local/bin/tb335-powerkey.py）：

* 长按 3 s 弹关机菜单；
* 按住 8 s 走 sysrq poweroff（硬关机兜底）；
* 轻按熄屏 + 650 ms 背光淡出（用户验收「现在对了」）。

## 6. 启动 logo

位置 /dev/sdc43（16 MB），偏移 0x200 处是 121 条偏移表，每条指向一个 zlib 流；
帧尺寸 1600×2560×4 **按竖屏存储**，LK 显示时旋转 90°。

关键细节：

* **字节序是 BGRA**（按 RGBA 写，Lenovo 字样会变蓝）；
* frame0 槽位固定 69,137 B（基础帧本身 23,081 B，能塞的额外图约 46 KB）；
* 其余 120 帧务必逐字节不动；
* 描边必须先量化再二值叠加，否则糊。

回滚：

    dd if=/root/logo_a.img of=/dev/sdc43 bs=1M conv=fsync

## 7. 系统服务清单

设备上实际 enable 的 20 个 unit 全部收录在 boot/systemd/（与 /etc/systemd/system 逐字节一致），
完整文件清单与逐文件说明见 boot/README.md，md5 清单见 boot/MD5SUMS。
脚本全部在 boot/sbin/（→ /usr/local/sbin）与 boot/bin/（→ /usr/local/bin），
/persist 下的辅助脚本在 boot/persist/。

| 服务 | 作用 |
| --- | --- |
| tb335-modules.service | 开机顺序加载 220 个模块（→ sbin/tb335-load-modules.sh） |
| tb335-powerkey.service | 电源键 + 背光守护（→ bin/tb335-powerkey.py） |
| tb335-wait-gpu.service | 等 Mali render 节点可用再起显示管理器 |
| tb335-mali0-node.service | 补 /dev/mali0 misc 节点 |
| tb335-panel60-guard.service | 60 Hz 面板模块守卫（300 s 未确认则回滚） |
| tb335-initboot-guard.service | init_boot 自愈（见 2.4） |
| tb335-drm-compat.service | 显示驱动前置兼容垫片 |
| tb335-hide-dp.service | 隐藏误报的 writeback DP |
| tb335-usb-host.service | USB data role 切换（dock=host） |
| tb335-audio / tb335-audio-state | 声卡 bring-up 与混音器状态恢复 |
| tb335-mic-route.service | 内置麦克风路由（UCM 不可用） |
| tb335-snd-watch.service | /dev/snd 目录 0755 守卫 |
| tb335-jack-watch.service | 3.5 mm 插孔自动切换 |
| tb335-devperm.service | /dev/snd、/dev/rfkill 权限 |
| tb335-input-node.service | /dev/input 节点同步 |
| tb335-wifi / tb335-wifi-dhcp / tb335-bt | WiFi bring-up + DHCP、蓝牙桥 |
| camdaemon.service | 相机按需起流（见 05） |

早期试验期装过 tb335-rotate-guard.service / tb335-session-heal.service 两个 unit，其脚本在现网设备上已不存在
（旋转改由 bin/tb335-rotate.sh 手动切换），故本仓库不再收录。

## 8. WiFi / 蓝牙

曾因 /vendor 被清空导致 WMT 用户态（wmt_loader / wmt_launcher / conninfra_loader）与固件缺失，
WMT 从未启动 ⇒ 无 WiFi 无蓝牙。修复：把 vendor/bin + firmware（81 个文件 4.24 MB）放回 /，
并加 /persist/wifi-up.sh。

注意：**不要整表读** /sys/kernel/debug/clk/clk_summary 或 pm_genpd_summary —— 本机曾因此直接重启。
## 9. GPT 分区布局与「把 Android 分区还给 Linux」

本机存储 119.2 GB，GPT 分区（lsblk PARTLABEL，2026-11-19 实测）：

| 分区 | 大小 | 标签 | 现状 / 用途 |
| --- | --- | --- | --- |
| /dev/sdc13 | 64 M | metadata | f2fs，Android 元数据 |
| /dev/sdc31 | 8 M | init_boot_a | **本项目的 A9b 修复镜像位置**（v4，md5 bc8e7e99…） |
| /dev/sdc43 | 16 M | logo_a | 开机 logo（121 帧 zlib，见第 6 节） |
| /dev/sdc52/53/54 | 16/64/64 M | lk_b, boot_b, vendor_boot_b | Android B 槽引导（保持原样） |
| /dev/sdc61 | 11.5 M | init_boot_b | Android B 槽 init_boot |
| /dev/sdc62 | 11 G | super | 已于 2026-11-19 格式化为 ext4（linux_super）→ /mnt/super |
| /dev/sdc69 | 60 G | debian_root | Debian 根：/ = 子目录 sdroot，/persist = 该分区根（里面放救援镜像） |
| /dev/sdc72 | 46.7 G | userdata | 已于 2026-11-19 格式化为 ext4（linux_userdata）→ /data |

### 9.1 已执行：super + userdata 全部回收（2026-11-19 22:58）

用户确认放弃 Android 后，两个分区都 mkfs.ext4 并挂进 Debian：

| 分区 | 新文件系统 | 标签 | UUID | 挂载点 |
| --- | --- | --- | --- | --- |
| /dev/sdc62（原 super，11 G） | ext4 | linux_super | a5cf3263-cfe9-4c7e-8d29-ed76809079d9 | /mnt/super |
| /dev/sdc72（原 userdata，46.7 G） | ext4 | linux_userdata | a03ce644-899a-466b-8d24-8da6b12d0093 | /data |

执行命令（在 Debian 里直接做，两个分区当时都未挂载）：

    sfdisk -d /dev/sdc > /persist/gpt_sdc_backup.txt      # 先备份 GPT
    mkfs.ext4 -F -L linux_super   -m 1 /dev/sdc62
    mkfs.ext4 -F -L linux_userdata -m 1 /dev/sdc72
    mkdir -p /data /mnt/super
    # /etc/fstab 追加（nofail 防止分区异常时卡启动）
    UUID=a03ce644-899a-466b-8d24-8da6b12d0093  /data       ext4 defaults,nofail,noatime 0 2
    UUID=a5cf3263-cfe9-4c7e-8d29-ed76809079d9  /mnt/super  ext4 defaults,nofail,noatime 0 2
    mount /data && mount /mnt/super && chown desk:desk /data /mnt/super

结果：/data 46 G、/mnt/super 11 G 可用，GPT 原始表保存在 /persist/gpt_sdc_backup.txt（也随仓库 docs 记录）。

### 9.2 注意事项

* **Android 已不可启动**（super 被格式化）：要回去只能 fastboot 重刷原厂固件。
  切安卓/回 Debian 的 switch_android.sh、restore_debian*.sh 已无用（且原在 /data/local/tmp，随 userdata 一起没了）。
* 不要动 sdc31（init_boot_a，A9b 修复镜像）、sdc43（logo_a）与 misc 分区，否则无限重启 / 变砖；
* 重跑 mkfs 会把对应数据全毁：sdc69 是 Debian 本体（/ 与 /persist 都在里面），绝对不能碰；
* 设备号（/dev/sdcN）由 GPT 编号决定，格式化不改编号。
