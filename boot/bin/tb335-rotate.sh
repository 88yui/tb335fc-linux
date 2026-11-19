#!/bin/sh
# 手动循环切换 DSI-1 的旋转（这台设备没有可用的加速度计驱动，无法自动旋转）
STATE=/home/desk/.config/sway/.rot-state
mkdir -p /home/desk/.config/sway
[ -f "$STATE" ] || echo 90 > "$STATE"
cur=$(cat "$STATE")
case "$cur" in
  90) next=180 ;;
  180) next=270 ;;
  270) next=0 ;;
  *) next=90 ;;
esac
echo "$next" > "$STATE"
chown desk:desk "$STATE" 2>/dev/null
export XDG_RUNTIME_DIR=/run/user/1000
export SWAYSOCK=$(ls /run/user/1000/sway-ipc.*.sock 2>/dev/null | head -1)
[ -n "$SWAYSOCK" ] || exit 1
/usr/bin/swaymsg output DSI-1 transform "$next" >/dev/null 2>&1
