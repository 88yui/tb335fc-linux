#!/bin/sh
# TB335: 等 WiFi 关联成功后获取 IPv4 地址。开机时序不稳，因此多轮重试并记录日志。
LOG=/persist/wifi-dhcp.log
exec >>"$LOG" 2>&1
echo "===== $(date '+%F %T') wifi-dhcp start ====="

have_ip() {
    ip -4 addr show dev wlan0 2>/dev/null | grep -q 'inet '
}

# 1) 等 wpa_supplicant 关联完成（最多 3 分钟）
st=""
i=0
while [ "$i" -lt 90 ]; do
    i=$((i + 1))
    st=$(wpa_cli -i wlan0 status 2>/dev/null | sed -n 's/^wpa_state=//p')
    [ "$st" = "COMPLETED" ] && break
    have_ip && break
    sleep 2
done
echo "wpa_state=$st after ${i} polls"

# 2) 已有地址就收工
if have_ip; then
    echo "already has address: $(ip -4 -br addr show dev wlan0)"
    exit 0
fi

# 3) 多轮 DHCP，任一成功即退出 0
tries=0
while [ "$tries" -lt 6 ]; do
    tries=$((tries + 1))
    echo "dhclient attempt $tries"
    dhclient -1 -v wlan0
    if have_ip; then
        echo "got address: $(ip -4 -br addr show dev wlan0)"
        exit 0
    fi
    sleep 5
done

echo "FAILED to obtain address after $tries attempts"
exit 1
