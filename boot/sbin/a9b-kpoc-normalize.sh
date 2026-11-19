#!/bin/sh
# A9b: 检测「充电器启动(KPOC)」模式 => 自动转一次正常重启
# 背景：MTK 在 bootmode 8/9（KPOC / LOW_POWER_OFF_CHARGING）下，内核 mtk_charger.c 的
#       kpoc_power_off_check() 一旦发现充电器被拔掉(vbus<2500) 就 kernel_power_off()
#       => 表现为「一拔充电线平板自己关机」。
# 处理：开机检测到 8/9 就主动 reboot 一次（reboot 原因=normal -> 下次 bootmode=0 正常模式），
#       并用 /persist/a9b/kpoc_reboot_done 防止万一的循环重启。
LOG=/persist/a9b/kpoc.log
log() { echo "$(date '+%F %T') $*" >> "$LOG" 2>/dev/null; }

BM=""
if [ -r /proc/device-tree/chosen/atag,boot ]; then
    raw=$(od -An -tu4 -j8 -N4 /proc/device-tree/chosen/atag,boot 2>/dev/null | tr -d " ")
    if [ -n "$raw" ]; then
        if [ "$raw" -le 1000 ]; then BM=$raw; else BM=$(( (raw >> 24) & 255 )); fi
    fi
fi
[ -z "$BM" ] && [ -r /proc/boot_mode ] && BM=$(cat /proc/boot_mode 2>/dev/null)
[ -z "$BM" ] && [ -r /sys/class/BOOT/BOOT/boot/boot_mode ] && BM=$(cat /sys/class/BOOT/BOOT/boot/boot_mode 2>/dev/null)
[ -z "$BM" ] && [ -r /proc/bootmode ] && BM=$(cat /proc/bootmode 2>/dev/null)

log "boot_mode=$BM"

case "$BM" in
  8|9)
    if [ -f /persist/a9b/kpoc_reboot_done ]; then
        log "KPOC boot_mode=$BM 但已自动重启过一次，为避免循环，本次不再重启（拔线会关机，注意）"
        exit 0
    fi
    mkdir -p /persist/a9b 2>/dev/null
    touch /persist/a9b/kpoc_reboot_done
    log "检测到充电器启动模式(KPOC boot_mode=$BM) -> 主动 reboot 转正常启动模式"
    /bin/sleep 2
    /bin/systemctl reboot
    ;;
  *)
    if [ -f /persist/a9b/kpoc_reboot_done ]; then rm -f /persist/a9b/kpoc_reboot_done; log "正常启动模式，清除循环保护标记"; fi
    ;;
esac
exit 0
