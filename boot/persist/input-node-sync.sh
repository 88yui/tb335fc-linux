#!/bin/sh
# /dev 是 tmpfs 且 udev 不为热插拔输入设备落节点
# 本守护每秒对账 sysfs 与 /dev/input，缺则 mknod，多余则清理
LOG=/persist/a9b/input-node-sync.log
mkdir -p /dev/input /persist/a9b
log() { echo "[$(date +%H:%M:%S)] $*" >> "$LOG"; }

log "start"
while true; do
    # 1) 补齐：sysfs 有、/dev 缺
    for e in /sys/class/input/event*; do
        [ -e "$e" ] || continue
        n=$(basename "$e")
        [ -e "/dev/input/$n" ] && continue
        dev=$(cat "$e/dev" 2>/dev/null)
        [ -n "$dev" ] || continue
        maj=${dev%%:*}; min=${dev##*:}
        if mknod "/dev/input/$n" c "$maj" "$min" 2>/dev/null; then
            chmod 660 "/dev/input/$n" 2>/dev/null
            chgrp input "/dev/input/$n" 2>/dev/null
            nm=$(cat "$e/device/name" 2>/dev/null)
            log "created $n ($maj:$min) $nm"
        fi
    done
    # 2) 清理：/dev 有、sysfs 无（设备已拔）
    for d in /dev/input/event*; do
        [ -e "$d" ] || continue
        n=$(basename "$d")
        if [ ! -e "/sys/class/input/$n" ]; then
            rm -f "$d" 2>/dev/null && log "removed stale $n"
        fi
    done
    sleep 1
done
