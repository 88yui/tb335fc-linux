#!/bin/bash
# 开机把可用混音器状态写回去，并确保 mt6835-mt6377 声卡已注册
LOG=/run/tb335-audio-state.log
: > "$LOG"
for i in $(seq 1 45); do
    if grep -q mt6835 /proc/asound/cards 2>/dev/null; then break; fi
    modprobe mt6835-mt6377 >>"$LOG" 2>&1
    sleep 2
done
if ! grep -q mt6835 /proc/asound/cards 2>/dev/null; then
    echo "card not present after wait" >>"$LOG"; exit 0
fi
# 补齐 /dev/snd 节点（防止 udev 未建）
for d in /sys/class/sound/*; do
    dev=$(cat "$d/dev" 2>/dev/null) || continue
    maj=${dev%%:*}; min=${dev##*:}
    case "$maj" in 116) ;; *) continue;; esac
    [ -e "/dev/snd/${d##*/}" ] || mknod "/dev/snd/${d##*/}" c "$maj" "$min"
done
find /dev/snd -maxdepth 1 -type c -exec chgrp audio {} \; 2>/dev/null
find /dev/snd -maxdepth 1 -type c -exec chmod 660 {} \; 2>/dev/null
chmod 755 /dev/snd /dev/snd/by-path 2>/dev/null
amixer -c 0 cset name='Ext_Speaker_Amp Switch' off >>"$LOG" 2>&1
amixer -c 0 cset name='LOL Mux' 2                 >>"$LOG" 2>&1
amixer -c 0 cset name='HPL Mux' 5                 >>"$LOG" 2>&1
amixer -c 0 cset name='HPR Mux' 5                 >>"$LOG" 2>&1
amixer -c 0 cset name='Headset Volume' 18,18      >>"$LOG" 2>&1
amixer -c 0 cset name='Lineout Volume' 18,18      >>"$LOG" 2>&1
amixer -c 0 cset name='Ext_Speaker_Amp Switch' on >>"$LOG" 2>&1
echo "done uptime=$(cut -d' ' -f1 /proc/uptime)" >>"$LOG"
