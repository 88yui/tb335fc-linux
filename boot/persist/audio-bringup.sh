#!/bin/sh
# TB335 音频一键启卡脚本
# 用法: sh /persist/audio-bringup.sh
# 注意: 绝对不能 rmmod mt6835-mt6377（会导致设备复位！）
#
# 开机时有音频驱动的硬拦截（为了加快启动：音频驱动会 EPROBE_DEFER 堵 80 秒）
# 本脚本会自动暂时解除拦截；用完按末尾提示恢复

echo "========== TB335 音频启卡 =========="

BLOCK1=/etc/modprobe.d/blacklist-mtk-audio.conf
BLOCK2=/etc/modprobe.d/zz-block-mtk-audio.conf

echo
echo "=== 0) 检查音频驱动拦截 ==="
if [ -e "$BLOCK1" ] || [ -e "$BLOCK2" ]; then
    echo "  发现拦截文件，暂时移走"
    [ -e "$BLOCK1" ] && mv "$BLOCK1" "${BLOCK1}.disabled" && echo "    $BLOCK1 -> .disabled"
    [ -e "$BLOCK2" ] && mv "$BLOCK2" "${BLOCK2}.disabled" && echo "    $BLOCK2 -> .disabled"
else
    echo "  无拦截"
fi

echo
echo "=== 1) 加载 machine driver ==="
if grep -q '^mt6835_mt6377 ' /proc/modules 2>/dev/null; then
    echo "  已加载"
else
    modprobe mt6835-mt6377 2>&1 | head -2
    sleep 3
fi

echo
echo "=== 2) 声卡状态 ==="
cat /proc/asound/cards 2>/dev/null

echo
echo "=== 3) 创建 /dev/snd 设备节点（按 sysfs 真实 major:minor）==="
mkdir -p /dev/snd
N=0
for c in /sys/class/sound/*/; do
    name=$(basename "$c")
    case "$name" in
        by-path|timer|seq|version) continue;;
    esac
    dev=$(cat "$c/dev" 2>/dev/null)
    [ -z "$dev" ] && continue
    major=${dev%%:*}
    minor=${dev##*:}
    rm -f "/dev/snd/$name" 2>/dev/null
    mknod "/dev/snd/$name" c "$major" "$minor" 2>/dev/null && N=$((N+1))
done
chmod 660 /dev/snd/* 2>/dev/null
chgrp audio /dev/snd/* 2>/dev/null
echo "  创建了 $N 个节点"

echo
echo "=== 4) SCP / DSP 状态（关键）==="
echo "  remoteproc: $(ls /sys/class/remoteproc/ 2>/dev/null | tr '\n' ' ')"
echo "  scp debugfs: $(ls /sys/kernel/debug/scp/ 2>/dev/null | tr '\n' ' ')"
ls -la /vendor/firmware/*scp* 2>/dev/null | head -2

echo
echo "=== 5) efuse 失败次数 ==="
dmesg 2>/dev/null | grep -c 'Failed to get nvmem cell mt6377_e_data'

echo
echo "=== 6) 逐个设备播放测试（用退出码判断）==="
python3 -c "
import wave, struct, math
w = wave.open('/tmp/tone.wav','w')
w.setnchannels(2); w.setsampwidth(2); w.setframerate(48000)
for i in range(48000):
    v = int(20000*math.sin(2*math.pi*440*i/48000))
    w.writeframes(struct.pack('<hh', v, v))
w.close()
" 2>/dev/null

CARDS=$(grep -c '\[' /proc/asound/cards 2>/dev/null)
if [ "${CARDS:-0}" = "0" ]; then
    echo "  没有声卡，跳过"
else
    for d in 0 1 2 3 4 5 6 7 8 9 10 11 58 59 60 61 62; do
        timeout 5 aplay -D hw:0,$d -q /tmp/tone.wav >/dev/null 2>&1
        rc=$?
        if [ $rc -eq 0 ]; then
            echo "  dev $d: OK (播放成功)"
        else
            err=$(timeout 5 aplay -D hw:0,$d -q /tmp/tone.wav 2>&1 | grep -m1 -oE 'set_params|audio open error|pcm_write' | head -1)
            case "$err" in
                set_params) echo "  dev $d: set_params 失败（afe 依赖 DSP）";;
                pcm_write)  echo "  dev $d: 能打开！写失败（等 DSP 就绪）";;
                *)          echo "  dev $d: 打不开";;
            esac
        fi
    done
fi

echo
echo "========== 完成 =========="
echo ""
echo "  判读:"
echo "    'set_params 失败' -> 这个 PCM 通路需要 DSP"
echo "    '能打开！写失败'  -> 只差 SCP 固件"
echo ""
echo "  恢复启动优化（重新加回音频拦截）："
echo "    mv $BLOCK1.disabled $BLOCK1 2>/dev/null"
echo "    mv $BLOCK2.disabled $BLOCK2 2>/dev/null"
echo ""
echo "  参考文档: 交接文档/P32-音频线交接-20261022.md"
