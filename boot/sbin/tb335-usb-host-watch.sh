#!/bin/sh
# TB335: 智能切换 USB 数据角色。
#   接电脑（充电器报 SDP/CDP，即可供数据的下行口）  -> device（USB 网络 192.168.55.2 可用）
#   接下挂设备（hub/键鼠/U 盘已枚举）              -> host
#   其它（充电器 PD/DCP/无）                      -> host（无害，且坞才能工作）
# 手动强制 device：touch /persist/a9b/allow-usb-device
R=/sys/class/usb_role/11201000.usb0-role-switch/role
UT=/sys/class/power_supply/charger/usb_type
FLAG=/persist/a9b/allow-usb-device
LOG=/persist/a9b/usb-role.log
mkdir -p /persist/a9b 2>/dev/null
logger -t tb335-usbhost "smart role watcher started"
cur_want=""; stable=0

wanted() {
    if [ -e "$FLAG" ]; then echo device; return; fi
    # 已有下挂设备 -> host
    for d in /sys/bus/usb/devices/*/; do
        b=${d%/}; b=${b##*/}
        case "$b" in usb*|*:*) continue;; esac
        [ -e "$d/idVendor" ] && { echo host; return; }
    done
    # 充电器类型：SDP / CDP 说明对端是可提供数据的下行口（电脑）
    t=$(sed -n 's/.*\[\([^]]*\)\].*/\1/p' "$UT" 2>/dev/null)
    case "$t" in
        SDP|CDP) echo device ;;
        *)       echo host ;;
    esac
}

while true; do
    w=$(wanted)
    if [ "$w" = "$cur_want" ]; then
        stable=$((stable+1))
    else
        cur_want="$w"; stable=0
    fi
    if [ "$stable" -ge 2 ]; then
        cur=$(cat "$R" 2>/dev/null)
        if [ "$cur" != "$cur_want" ]; then
            echo "$cur_want" > "$R" 2>/dev/null && \
                echo "$(date '+%F %T') role '$cur' -> '$cur_want' (charger=$(sed -n 's/.*\[\([^]]*\)\].*/\1/p' $UT 2>/dev/null))" >> "$LOG"
        fi
    fi
    sleep 1
done
