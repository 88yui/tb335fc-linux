#!/bin/sh
# Wait for the vendor DRM device node, then hide the phantom writeback connector.
# 2026-11-18 开机优化：模块加载已提速，DRM 节点出现后 0.3s 粒度即可；
# 原为 150x2s（最多 300s），现为 400x0.3s（最多 120s，超时放弃 insmod）。
for i in $(seq 1 400); do
    [ -e /dev/dri/card0 ] && break
    sleep 0.3
done
if lsmod | grep -q hide_dp; then
    exit 0
fi
if [ -c /dev/dri/card0 ]; then
    insmod /persist/a9b/hide_dp.ko
fi
