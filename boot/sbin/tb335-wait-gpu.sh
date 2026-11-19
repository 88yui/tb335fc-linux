#!/bin/sh
# TB335: wait until the vendor DRM node AND the Mali render node are usable, then
# let the late DRM/udev churn settle before the display manager starts.
# If gnome-shell starts too early its EGL surface dies with EGL_BAD_SURFACE
# (mutter then logs "Failed to lock front buffer" forever and the panel freezes).
# 2026-10-30 修正：原上限 240s，GPU 探测卡死时会把 gdm 一起拖 7 分钟；
# 现在最多等 90s，超时也放行（宁可先降级到软件合成，也不能没有桌面）。
# 2026-11-16 修正：厂商 mali 驱动有时会抢占 13000000.mali，导致 panfrost 绑不上、
# /dev/dri/renderD128 不出现（桌面退回软件渲染）。等 12 秒仍无 render 节点时，
# 自动 unbind mali → bind panfrost 再继续等。
i=0
tried_swap=0
while [ "$i" -lt 90 ]; do
    i=$((i + 1))
    if [ -c /dev/dri/card0 ] && [ -c /dev/dri/renderD128 ]; then
        if python3 -c "import os; fd=os.open('/dev/dri/renderD128', os.O_RDWR); os.close(fd)" >/dev/null 2>&1; then
            chmod 666 /dev/dri/renderD128 2>/dev/null
            logger -t tb335-wait-gpu "render node usable after ${i}s"
            sleep 1
            logger -t tb335-wait-gpu "settle done"
            exit 0
        fi
    fi

    if [ "$tried_swap" -eq 0 ] && [ "$i" -ge 12 ]; then
        tried_swap=1
        dev=$(ls /sys/bus/platform/drivers/mali/ 2>/dev/null | grep -E '^[0-9a-f]+\.mali$' | head -1)
        if [ -n "$dev" ]; then
            logger -t tb335-wait-gpu "render node missing, swapping ${dev}: mali -> panfrost"
            echo "$dev" > /sys/bus/platform/drivers/mali/unbind 2>/dev/null
            echo "$dev" > /sys/bus/platform/drivers/panfrost/bind 2>/dev/null
        fi
    fi

    sleep 1
done
logger -t tb335-wait-gpu "timeout waiting for usable render node, continue without GPU"
exit 0