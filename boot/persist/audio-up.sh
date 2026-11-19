#!/bin/sh
# TB335 音频启卡（开机自动执行，幂等）+ 建卡全过程探针
LOG=/persist/audio-bringup.log
log() { echo "[$(date +%H:%M:%S)] $*" >> "$LOG"; }

OLDLEVEL=$(cat /proc/sys/kernel/printk | awk '{print $1}')
echo 8 > /proc/sys/kernel/printk

# ---- 建卡探针：DAPM 路由/DAI widget/绑卡返回值 ----
T=/sys/kernel/tracing
if [ -d "$T" ]; then
    echo 0 > $T/tracing_on
    echo > $T/trace
    echo > $T/kprobe_events 2>/dev/null
    echo "p:addroutes snd_soc_dapm_add_routes num=\$arg3" >> $T/kprobe_events 2>/dev/null
    echo "r:addroutes_ret snd_soc_dapm_add_routes ret=\$retval" >> $T/kprobe_events 2>/dev/null
    echo "p:newdai snd_soc_dapm_new_dai_widgets" >> $T/kprobe_events 2>/dev/null
    echo "p:neww snd_soc_dapm_new_widgets" >> $T/kprobe_events 2>/dev/null
    echo "r:bind snd_soc_bind_card ret=\$retval" >> $T/kprobe_events 2>/dev/null
    echo 1 > $T/events/kprobes/enable
    echo 1 > $T/tracing_on
    log "kprobes armed"
fi

# 2026-10-30 修正：模块已加载时 modprobe 是空操作，必须 modprobe -r 再加载
# 才会重新触发 probe（开机早期 codec 拿 efuse 会 -517 EPROBE_DEFER，稍后重试可成功）。
i=0
while [ $i -lt 6 ]; do
    if grep -q mt6835 /proc/asound/cards 2>/dev/null; then
        break
    fi
    # 2026-11-18 开机优化：机驱依赖的 snd 模块由 tb335-modules 先加载完（unit 已加 After），
    # 这里首次 modprobe 通常即可建卡；不再用 sleep 2 + sleep 8 的固定等待。
    modprobe -r mt6835-mt6377 2>/dev/null
    modprobe mt6835-mt6377 2>/dev/null
    j=0
    while [ $j -lt 20 ]; do
        grep -q mt6835 /proc/asound/cards 2>/dev/null && break
        sleep 0.25
        j=$((j+1))
    done
    i=$((i+1))
done

# ---- 收探针数据 ----
if [ -d "$T" ]; then
    echo 0 > $T/tracing_on
    echo 0 > $T/events/kprobes/enable
    cp $T/trace /persist/trace-card-build.txt 2>/dev/null
    log "trace saved: $(grep -c '' /persist/trace-card-build.txt 2>/dev/null) lines"
fi

if ! grep -q mt6835 /proc/asound/cards 2>/dev/null; then
    log "FAIL: card not registered after retries"
    dmesg > /persist/dmesg-audio-fail.txt
    echo "$OLDLEVEL" > /proc/sys/kernel/printk
    exit 1
fi
log "card registered after retry $i"

mkdir -p /dev/snd
for c in /sys/class/sound/*/; do
    n=$(basename "$c")
    dev=$(cat "$c/dev" 2>/dev/null)
    [ -n "$dev" ] || continue
    [ -e "/dev/snd/$n" ] && continue
    mknod "/dev/snd/$n" c "${dev%%:*}" "${dev##*:}" 2>/dev/null
done
chmod 660 /dev/snd/* 2>/dev/null
chgrp audio /dev/snd/* 2>/dev/null
log "nodes: $(ls /dev/snd | wc -l)"

dmesg > /persist/dmesg-after-audio.txt
log "dmesg saved: $(grep -c '' /persist/dmesg-after-audio.txt) lines"
echo "$OLDLEVEL" > /proc/sys/kernel/printk
# tb335_audio_fix 模块会导致整机复位，已禁用
true
# 声卡就绪后让 PulseAudio 重新探测（user@0/pulseaudio.service 常在声卡注册前就启动，
# 而本机 udev 很弱，PA 收不到 card 热插事件，会一直只有 auto_null）
if [ -n "$(ls /dev/snd/controlC0 2>/dev/null)" ]; then
    XDG_RUNTIME_DIR=/run/user/0 systemctl --user restart pulseaudio 2>/dev/null || true
    log "pulseaudio restarted"
fi

exit 0
