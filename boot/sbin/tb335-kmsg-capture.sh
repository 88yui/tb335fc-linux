#!/bin/bash
# =============================================================================
# tb335-kmsg-capture.sh -- 把内核日志持续落盘（供"试验启动"事后取证）
#
# 背景：本机 CONFIG_DEVMEM 未开、pstore 在硬复位下不落盘，导致崩溃现场丢失。
#       本脚本以行缓冲方式把 /dev/kmsg 追加写入 /persist，最多滞后数秒，
#       足以定位"挂死在哪个子系统/哪一行之后"。
# 注意：读 /dev/kmsg 不会清空环形缓冲，不影响其它日志工具。
# =============================================================================
set -eu

LOG=/persist/a9b/kmsg-trial.log
SIZE_LIMIT=$((32 * 1024 * 1024))   # 32 MiB 上限，防止长期运行写爆

# 超过上限则轮转一次
if [ -f "$LOG" ] && [ "$(stat -c %s "$LOG" 2>/dev/null || echo 0)" -gt "$SIZE_LIMIT" ]; then
	mv -f "$LOG" "${LOG}.1" 2>/dev/null || true
fi

{
	echo "===== capture start $(date '+%F %T') uptime=$(cut -d' ' -f1 /proc/uptime 2>/dev/null) ====="
} >>"$LOG" 2>/dev/null || true

# -o0 保证逐行写出；-k 让 SIGTERM 之后立即结束（无需重启自愈）
exec python3 /usr/local/sbin/tb335-kmsg-persist.py
