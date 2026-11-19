#!/bin/sh
# 耳机/扬声器 手动切换（插孔检测失效时使用）
c() { amixer -c 0 cset name="$1" "$2" >/dev/null 2>&1; }
cur=$(amixer -c 0 cget name='HPL Mux' 2>/dev/null | sed -n 's/.*: values=\([0-9]*\).*/\1/p' | head -1)
if [ "$cur" = "5" ]; then
    c 'Ext_Speaker_Amp Switch' 0
    c 'LOL Mux' 0
    c 'HPL Mux' 2
    c 'HPR Mux' 2
    c 'Headset Volume' 18
    msg='已切到：耳机'
else
    c 'Ext_Speaker_Amp Switch' 0
    c 'LOL Mux' 2
    c 'HPL Mux' 5
    c 'HPR Mux' 5
    c 'Headset Volume' 18
    c 'Lineout Volume' 18
    c 'Ext_Speaker_Amp Switch' 1
    msg='已切到：扬声器'
fi
notify-send -i audio-headphones "$msg" 2>/dev/null || echo "$msg"
