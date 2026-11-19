#!/bin/sh
# 默认不启动；仅在存在 /etc/tb335-plasma-test 时才启动 Plasma Wayland
[ -f /etc/tb335-plasma-test ] || { echo "plasma-test marker absent, skip"; exit 0; }
LOG=/tmp/plasma-wl-run.log
exec >> "$LOG" 2>&1
echo "=== $(date) wrapper start ==="
export HOME=/home/desk
export XDG_RUNTIME_DIR=/run/user/1000
export XDG_SESSION_TYPE=wayland
export XDG_CURRENT_DESKTOP=KDE
export XDG_SESSION_DESKTOP=KDE
export DESKTOP_SESSION=plasmawayland
export QT_QPA_PLATFORM=wayland
export LIBSEAT_BACKEND=seatd
export KWIN_DRM_DEVICES=/dev/dri/card0
# KWin 自己不持久化输出旋转，等 wayland socket 就绪后用 kscreen-doctor 补上
(
  sleep 2
  i=0
  while [ $i -lt 60 ]; do
    XDG_RUNTIME_DIR=/run/user/1000 WAYLAND_DISPLAY=wayland-0 QT_QPA_PLATFORM=wayland \
      /usr/bin/kscreen-doctor output.DSI-1.rotation.right >/dev/null 2>&1 && break
    sleep 1
    i=$((i+1))
  done
  echo "=== rotation task done (tries=$i) ==="
) &
exec dbus-run-session -- /usr/bin/startplasma-wayland
