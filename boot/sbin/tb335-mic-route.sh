#!/bin/sh
# TB335 内置麦克风采集路由（等价于 UCM 的 SectionDevice."Mic" EnableSequence）。
# 开机在声卡注册之后执行；UCM 因为 udev 枚举缺失不可用，所以这里直接写 mixer。
set -u

CARD=${1:-0}

apply() {
    n="$1"; v="$2"
    amixer -c "$CARD" cset name="$n" "$v" >/dev/null 2>&1
}

# 等声卡出现（最多 30 秒）
i=0
while [ $i -lt 30 ]; do
    grep -q mt6835 /proc/asound/cards 2>/dev/null && break
    sleep 1; i=$((i+1))
done

apply "ADDA_UL_Mux" 0
apply "PGA_L_Mux" 1
apply "PGA_R_Mux" 1
apply "ADC_L_Mux" 2
apply "ADC_R_Mux" 2
apply "PGA1 Volume" 4
apply "PGA2 Volume" 4
apply "UL1_CH1 ADDA_UL_CH1" 1
apply "UL1_CH2 ADDA_UL_CH2" 1

logger -t tb335-mic-route "mic route applied (card $CARD)"
# 顺带把当前整卡状态存盘，下次开机 alsactl restore 直接带回
alsactl -f /var/lib/alsa/asound.state store "$CARD" 2>/dev/null || true
exit 0
