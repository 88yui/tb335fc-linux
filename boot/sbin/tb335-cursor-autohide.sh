#!/bin/sh
# TB335: 有指针设备(带 REL_X/REL_Y)时显示光标(Adwaita)，否则隐藏(blank)。
# 判定依据是与内核直接对应的 /proc/bus/input/devices，不再依赖 udev 的
# ID_INPUT_MOUSE 标签（本机 /run/udev/data 缺条目，旧判定永远认为"无鼠标"）。
LOG=/persist/a9b/cursor-autohide.log
mkdir -p /persist/a9b 2>/dev/null
prev=""
while true; do
    has=0
    while IFS= read -r line; do
        case "$line" in
            "B: REL="*)
                rel=${line#B: REL=}
                # REL_X=bit0, REL_Y=bit1：两位都置位即为指针设备（鼠标/触摸板）
                if [ $(( 0x$rel & 3 )) -eq 3 ]; then
                    has=1
                fi
                ;;
        esac
    done < /proc/bus/input/devices
    if [ "$has" = "1" ]; then want="Adwaita"; else want="blank"; fi
    cur=$(gsettings get org.gnome.desktop.interface cursor-theme 2>/dev/null | tr -d "'")
    if [ "$cur" != "$want" ]; then
        gsettings set org.gnome.desktop.interface cursor-theme "$want" 2>/dev/null
        if [ "$want" != "$prev" ]; then
            echo "$(date '+%F %T') cursor-theme '$cur' -> '$want'" >> "$LOG"
            prev="$want"
        fi
    fi
    sleep 1
done
