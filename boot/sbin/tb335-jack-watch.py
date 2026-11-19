#!/usr/bin/env python3
"""TB335 3.5mm 插孔自动切换（v5）。

插孔类型来源：ALSA jack -> input "mt6835-mt6377 Headset Jack" 的 EV_SW：
  bit2 SW_HEADPHONE_INSERT -> 3 段耳机；bit4 SW_MICROPHONE_INSERT -> 4 段耳麦

输出：两类都走 HP；拔出回扬声器。
      切换时序：ExtAmp off -> HPL/HPR Mux 0(Open) 强制 DAPM 下电 -> 目标 mux 上电。

采集：4 段 -> 耳机麦（PGA_L=Ain1、PGA_R=None、UL1_CH2 复制 ADDA_UL_CH1）
      3 段/拔出 -> 内置麦（PGA_L/R=Ain0、UL1_CH1/CH2 各取各的 ADDA_UL）

注意（实测）：DAPM 只有在采集流已经活跃时才会把 AIN1 这条边上电。
空闲时先把 mux 写成 AIN1，之后开流会采到恒零。因此守护会盯着
/proc/asound/card0/pcm10c/sub0/status，一旦发现流刚被打开就强制重挂一次。
"""

import os
import select
import struct
import subprocess
import sys
import time

JACK_NAME = "mt6835-mt6377 Headset Jack"
EVIOCGSW = 0x8008451B
SW_HP = 1 << 2
SW_MIC = 1 << 4
EV_SW = 5
CAP_STATUS = "/proc/asound/card0/pcm10c/sub0/status"

EVENT_FMT = "llHHi"  # 24 字节，linux input_event


def log(msg):
    print(time.strftime("%H:%M:%S"), msg, flush=True)


def amixer(name, val):
    subprocess.run(
        ["amixer", "-c", "0", "cset", "name=%s" % name, str(val)],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )


def cap_headset(force=False):
    """采集切到耳机麦（4 段）。force=True 时先把 mux 拉回 None 再写 AIN1。"""
    if force:
        amixer("PGA_L_Mux", "None")
        time.sleep(0.05)
    amixer("ADC_L_Mux", "Left Preamplifier")
    amixer("PGA_R_Mux", "None")
    amixer("UL1_CH1 ADDA_UL_CH1", 1)
    amixer("UL1_CH2 ADDA_UL_CH1", 1)
    amixer("UL1_CH2 ADDA_UL_CH2", 0)
    amixer("PGA_L_Mux", "AIN1")


def cap_builtin():
    """采集回到内置麦（3 段 / 拔出）。"""
    amixer("UL1_CH2 ADDA_UL_CH1", 0)
    amixer("UL1_CH2 ADDA_UL_CH2", 1)
    amixer("UL1_CH1 ADDA_UL_CH1", 1)
    amixer("ADC_L_Mux", "Left Preamplifier")
    amixer("ADC_R_Mux", "Right Preamplifier")
    amixer("PGA_L_Mux", "AIN0")
    amixer("PGA_R_Mux", "AIN0")


def out_headset():
    amixer("LOL Mux", 0)
    amixer("HPL Mux", 2)
    amixer("HPR Mux", 2)
    amixer("Headset Volume", 18)


def out_speaker():
    amixer("LOL Mux", 2)
    amixer("HPL Mux", 5)
    amixer("HPR Mux", 5)
    amixer("Headset Volume", 18)
    amixer("Lineout Volume", 18)
    amixer("Ext_Speaker_Amp Switch", 1)


def apply_route(kind, first=False):
    amixer("Ext_Speaker_Amp Switch", 0)
    if kind != "none":
        out_headset()
        # 3 段（TRS）没有麦克风针，采集必须留在内置麦；只有 4 段才切耳机麦
        if kind == "headset":
            cap_headset()
        else:
            cap_builtin()
        log("%s plugged -> HP out + headset mic" %
            ("4-pole headset" if kind == "headset" else "3-pole headphone"))
    else:
        out_speaker()
        cap_builtin()
        log("unplugged -> Speaker out + builtin mic")
    if first:
        log("initial route applied (%s)" % kind)


def switch_route(kind):
    """带 DAPM Open 过渡的输出切换；采集紧跟其后。"""
    amixer("Ext_Speaker_Amp Switch", 0)
    amixer("HPL Mux", 0)
    amixer("HPR Mux", 0)
    time.sleep(0.2)
    apply_route(kind)
    time.sleep(0.1)


def kind_of(sw):
    """EV_SW 位图 -> 插孔类型（bit2=3 段耳机，bit4=4 段耳麦）。"""
    if sw & SW_MIC:
        return "headset"
    if sw & SW_HP:
        return "headphone"
    return "none"


def find_event():
    """按设备名找 /dev/input/eventN（重启后节点号会变，不能写死）。"""
    import glob
    for syspath in sorted(glob.glob("/sys/class/input/event*")):
        try:
            with open(os.path.join(syspath, "device", "name")) as f:
                if f.read().strip() != JACK_NAME:
                    continue
        except OSError:
            continue
        return os.path.join("/dev/input", os.path.basename(syspath))
    return None


def read_sw(fd):
    import fcntl
    buf = struct.pack("i", 0) * 8
    raw = fcntl.ioctl(fd, EVIOCGSW, buf)
    return struct.unpack("I", raw[:4])[0]


def cap_stream_open():
    try:
        with open(CAP_STATUS) as f:
            body = f.read()
    except OSError:
        return False
    return "closed" not in body


def main():
    path = find_event()
    if not path:
        log("jack input device not found")
        return 1

    fd = os.open(path, os.O_RDONLY)
    state = read_sw(fd)
    kind = kind_of(state)
    apply_route(kind, first=True)
    last_open = cap_stream_open()
    if kind == "headset" and last_open:
        cap_headset(force=True)
        log("capture stream already open -> forced headset mic route")

    while True:
        r, _, _ = select.select([fd], [], [], 1.0)
        if r:
            for _ in range(16):
                data = os.read(fd, 24)
                if not data:
                    break
                _, _, ev_type, code, value = struct.unpack(EVENT_FMT, data)
                if ev_type != EV_SW or code not in (2, 4):
                    continue
                try:
                    sw = read_sw(fd)
                except OSError:
                    sw = 0
                newk = "headset" if (sw & SW_MIC) else ("headphone" if (sw & SW_HP) else "none")
                if newk != kind:
                    kind = newk
                    switch_route(kind)
                    last_open = cap_stream_open()
        now_open = cap_stream_open()
        if now_open and not last_open and kind == "headset":
            cap_headset(force=True)
            log("capture stream opened -> re-apply headset mic route")
        last_open = now_open


if __name__ == "__main__":
    sys.exit(main())
