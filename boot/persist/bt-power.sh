#!/bin/sh
# 等 WMT(wmt_launcher+/dev/wmtWifi) 就绪 -> 等 bridge 建立 hciX -> 解除 rfkill 并 up。
# 2026-11-18：本脚本改为由 tb335-bt.service 的 ExecStartPost 在后台执行（nohup + timeout 120），
# 不再阻塞 multi-user.target。旧的 40x1s 空等会拖慢启动 40s。
for i in $(seq 1 60); do
    pgrep -f /vendor/bin/wmt_launcher >/dev/null 2>&1 && [ -e /dev/wmtWifi ] && break
    sleep 1
done
for i in $(seq 1 40); do
    hciconfig hci0 >/dev/null 2>&1 && break
    sleep 1
done
rfkill unblock bluetooth 2>/dev/null || true
hciconfig hci0 up || true
