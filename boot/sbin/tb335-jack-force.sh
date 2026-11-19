#!/bin/sh
# 手动强制输出路由（插孔检测失效时用）
# 用法: tb335-jack-force.sh headphone | speaker
c() { amixer -c 0 cset name="$1" "$2" >/dev/null 2>&1; }
case "$1" in
  headphone)
    c 'Ext_Speaker_Amp Switch' 0
    c 'LOL Mux' 0
    c 'HPL Mux' 2
    c 'HPR Mux' 2
    c 'Headset Volume' 18
    echo 'forced: headphone (HP out)'
    ;;
  speaker|'')
    c 'Ext_Speaker_Amp Switch' 0
    c 'LOL Mux' 2
    c 'HPL Mux' 5
    c 'HPR Mux' 5
    c 'Headset Volume' 18
    c 'Lineout Volume' 18
    c 'Ext_Speaker_Amp Switch' 1
    echo 'forced: speaker'
    ;;
  *) echo 'usage: /usr/bin/bash headphone|speaker'; exit 1;;
esac
