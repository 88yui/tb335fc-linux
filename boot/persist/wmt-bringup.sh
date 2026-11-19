#!/bin/sh
# WMT（MTK connectivity）bring-up · v2
# 关键修正：/dev/stpwmt 是 **190:0**（wmt_drv 的 STP main），不是 192:0（mtk_stp_bt_chrdev）
# 用法：sh /persist/wmt-bringup.sh [--probe-only]
LOG=/persist/wmt-bringup.log
log() { echo "[$(date +%H:%M:%S)] $*" >> "$LOG"; }

mknode() {
    name=$1
    hard=$2
    [ -e "/dev/$name" ] && return 0
    dev=$(cat "/sys/devices/virtual/$name/$name/dev" 2>/dev/null)
    [ -z "$dev" ] && dev=$hard
    if [ -n "$dev" ]; then
        mknod "/dev/$name" c "${dev%%:*}" "${dev##*:}" 2>/dev/null && \
            chmod 660 "/dev/$name" && log "node /dev/$name = $dev"
    else
        log "WARN: node $name impossible"
    fi
}

mknode wmtdetect 154:0
mknode wmtWifi   491:0
mknode stpwmt    190:0
log "nodes: $(ls /dev/wmtdetect /dev/wmtWifi /dev/stpwmt 2>/dev/null | wc -l)/3"

echo "=== IRQ 状况（CONNSYS 主中断是否注册） ===" | tee -a "$LOG"
grep -iE 'conn|wmt|btif|GICv3 16[0-9]' /proc/interrupts | tee -a "$LOG"

echo "=== probe 日志（重启后第一次运行才有） ===" | tee -a "$LOG"
dmesg | grep -iE 'get irq|eirq|BGF|CONN2AP|wmt.*probe|consys' | tail -20 | tee -a "$LOG"

if [ "$1" = "--probe-only" ]; then
    exit 0
fi

log "run wmt_loader"
/vendor/bin/wmt_loader >>"$LOG" 2>&1
log "loader rc=$?"

dmesg | grep -iE 'wmt|consys|patch|eirq' | tail -30 >> "$LOG"
echo "--- iface ---" >> "$LOG"
ip -br link | grep -i wlan >> "$LOG" 2>&1 || echo "(no wlan)" >> "$LOG"
tail -40 "$LOG"
