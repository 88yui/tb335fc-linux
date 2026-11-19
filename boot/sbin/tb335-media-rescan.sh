#!/bin/bash
# 强化版：等声卡出现（最多 180 秒，因为 tb335-modules 顺序加载要 ~100 秒）
L=/persist/media-rescan.log
log() { echo "[$(date +%H:%M:%S)] $*" >> "$L"; }
: >> "$L"

U() { runuser -u desk -- env XDG_RUNTIME_DIR=/run/user/1000 \
        DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus "$@"; }

log "start uptime=$(cut -d' ' -f1 /proc/uptime)"

i=0
while [ $i -lt 60 ]; do
    grep -q mt6835 /proc/asound/cards 2>/dev/null && break
    modprobe mt6835-mt6377 2>/dev/null
    i=$((i+1)); sleep 3
done

if ! grep -q mt6835 /proc/asound/cards 2>/dev/null; then
    log "FAIL: no sound card after $((i*3))s"
    exit 0
fi
log "sound card OK after $((i*3))s"

mkdir -p /dev/snd
for c in /sys/class/sound/*/; do
    n=$(basename "$c"); dev=$(cat "$c/dev" 2>/dev/null) || continue
    [ -n "$dev" ] || continue
    [ -e "/dev/snd/$n" ] && continue
    mknod "/dev/snd/$n" c "${dev%%:*}" "${dev##*:}" 2>/dev/null
done
chgrp audio /dev/snd/* 2>/dev/null
chmod 660 /dev/snd/* 2>/dev/null
log "/dev/snd nodes=$(ls /dev/snd 2>/dev/null | wc -l)"

i=0
while [ $i -lt 60 ]; do
    [ -S /run/user/1000/bus ] && break
    i=$((i+1)); sleep 2
done
if [ ! -S /run/user/1000/bus ]; then
    log "FAIL: no desk session bus"
    exit 0
fi

U systemctl --user restart wireplumber >>"$L" 2>&1 && log "wireplumber restarted"
sleep 6
U wpctl status 2>/dev/null | sed -n '/Audio/,/Video/p' >> "$L"

log "done"
