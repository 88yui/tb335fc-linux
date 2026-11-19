#!/bin/bash
# tb335 WiFi bring-up —— MTK connsys (MT6835 + MT6631 A-die)
# 依据 P35 交接文档：loader(detect) -> launcher(STP/校准/patch) -> /dev/wmtWifi 写 '1'(WiFi func on)
# 依赖：/persist/libpropfake.so（bionic property 垫片）、/persist/logd-shim.py（logd 垫片）
LOG=/persist/wifi-up.log
exec >>"$LOG" 2>&1
echo "===== $(date '+%F %T') boot=$(cat /proc/sys/kernel/random/boot_id) ====="

st() { echo "[$(date +%T)] $*"; }

mknode() {
    name=$1; hard=$2
    [ -e "/dev/$name" ] && return 0
    dev=$(cat "/sys/devices/virtual/$name/$name/dev" 2>/dev/null)
    [ -z "$dev" ] && dev=$hard
    mknod "/dev/$name" c "${dev%%:*}" "${dev##*:}" 2>/dev/null && chmod 660 "/dev/$name"
}

# 1) 驱动链（boot 已加载则幂等返回）
modprobe wlan_drv_gen4m_6835 2>/dev/null || echo "modprobe wlan failed"
modprobe bt_drv_connac1x 2>/dev/null || echo "modprobe bt failed"
sleep 3

# 2) 字符设备节点（udev 弱，手工建）
mknode wmtdetect 154:0
mknode stpwmt    190:0
mknode wmtWifi   491:0
ls -l /dev/wmtdetect /dev/stpwmt /dev/wmtWifi

# 3) 假 property 表说明：loader 见到 vendor.connsys.driver.ready=yes 会误判“已初始化”
#    而跳过 detect（rc=3，STP major 不注册）。因此每次真正跑 loader 前必须清空该文件，
#    之后 chipid/ready 全部由 loader 自己写入（见第 5 步）。
PROPS=/persist/fake.props

# 4) logd 垫片（bionic liblog 的 logdw socket；缺了 launcher 会原地重试）
pgrep -f logd-shim.py >/dev/null || { nohup python3 /persist/logd-shim.py >/dev/null 2>&1 & sleep 1; }

# 5) wmt_loader：每次开机只跑一次（重复跑会刷 double register 告警）
MARK=/persist/.wmt-loader.bootid
BID=$(cat /proc/sys/kernel/random/boot_id)
pkill -f '/vendor/bin/wmt_launcher' 2>/dev/null
if [ "$(cat $MARK 2>/dev/null)" != "$BID" ]; then
    st "-- run wmt_loader"
    : > "$PROPS"
    LD_PRELOAD=/persist/libpropfake.so timeout 40 /vendor/bin/wmt_loader
    echo "loader rc=$?"
    if ! grep -q mtk_stp_wmt /proc/devices; then
        st "-- STP major missing, retry loader with clean props"
        : > "$PROPS"
        LD_PRELOAD=/persist/libpropfake.so timeout 40 /vendor/bin/wmt_loader
        echo "loader retry rc=$?"
    fi
    echo "$BID" > $MARK
fi
grep -iE 'mtk_stp_wmt|mtk_wmt_wifi' /proc/devices || echo "STP cdev missing!"

# 6) wmt_launcher 常驻（ROM patch/校准/STP；被杀后 STP tx 会异常）
if ! pgrep -f '/vendor/bin/wmt_launcher' >/dev/null; then
    st "-- start wmt_launcher"
    LD_PRELOAD=/persist/libpropfake.so nohup /vendor/bin/wmt_launcher -p /vendor/firmware/ -o 1 \
        >/persist/wmt-launcher.out 2>&1 &
fi
sleep 6

# 7) 打开 WiFi 功能：向 /dev/wmtWifi 写 '1'（内核 WIFI_write -> mtk_wcn_wmt_func_on(WIFI)）
for i in $(seq 1 20); do
    if ip link show wlan0 >/dev/null 2>&1; then break; fi
    st "-- wifi on, try $i"
    python3 - <<'PY'
import os
try:
    fd = os.open('/dev/wmtWifi', os.O_WRONLY | os.O_NOCTTY)
    os.write(fd, b'1')
    os.close(fd)
except OSError as e:
    print('write wmtWifi failed:', e)
PY
    sleep 3
done

# 8) 拉起接口 + 监管域
iw reg set CN 2>/dev/null
ip link set wlan0 up 2>/dev/null
ip -br link | grep -E 'wlan0|p2p' || echo "no wlan iface"

# 9) wpa_supplicant（二层）：只提供可控接口，不预置任何凭据
CONF=/etc/wpa_supplicant/wpa_supplicant-wlan0.conf
if [ ! -f "$CONF" ]; then
    mkdir -p /etc/wpa_supplicant
    cat > "$CONF" <<'EOF'
ctrl_interface=DIR=/run/wpa_supplicant GROUP=netdev
update_config=1
country=CN
EOF
fi
mkdir -p /run/wpa_supplicant
pgrep -f 'wpa_supplicant -B -i wlan0' >/dev/null || wpa_supplicant -B -i wlan0 -c "$CONF" 2>&1
sleep 1
wpa_cli -i wlan0 status 2>&1 | head -4
echo "===== done $(date '+%F %T') ====="
