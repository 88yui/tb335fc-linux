#!/bin/sh
# TB335 冷重启：设 RTC 闹钟 → 优雅关机 → 到点自动开机
# 用途：热重启会丢 panfrost，改走"真断电 + 自动上电"的冷启动
# 用法：tb335-coldreboot.sh [延迟秒数，默认 30]

DELAY=${1:-30}

sync

if /usr/sbin/rtcwake -m no -s "$DELAY"; then
    # 万一优雅关不掉：15 秒后强制断电（闹钟仍在，会自动开机）
    systemd-run --on-active=15 --unit=tb335-forceoff \
        /bin/systemctl poweroff -f --no-block 2>/dev/null
    systemctl poweroff --no-block
else
    echo "rtcwake 设置闹钟失败，退化为普通关机" >&2
    systemctl poweroff --no-block
fi
