#!/usr/bin/env python3
# 用 uinput 造一个虚拟 USB 鼠标，验证 udev 建节点 + Xorg 热添加是否恢复
import ctypes, os, struct, time, sys

UI_DEV_CREATE = 0x5501
UI_DEV_DESTROY = 0x5502
UI_DEV_SETUP = 0x405C5503
UI_SET_EVBIT = 0x40045564
UI_SET_KEYBIT = 0x40045565
UI_SET_RELBIT = 0x40045566

EV_SYN, EV_KEY, EV_REL = 0, 1, 2
REL_X, REL_Y = 0, 1
BTN_LEFT = 0x110
SYN_REPORT = 0

class InputId(ctypes.Structure):
    _fields_ = [("bustype", ctypes.c_uint16), ("vendor", ctypes.c_uint16),
                ("product", ctypes.c_uint16), ("version", ctypes.c_uint16)]

class UInputSetup(ctypes.Structure):
    _fields_ = [("id", InputId), ("name", ctypes.c_char * 80), ("ff_effects_max", ctypes.c_uint32)]

fd = os.open("/dev/uinput", os.O_WRONLY | os.O_NONBLOCK)
for code in (EV_KEY, EV_REL, EV_SYN):
    ctypes.CDLL('libc.so.6', use_errno=True).ioctl(fd, UI_SET_EVBIT, ctypes.c_int(code))
lc = ctypes.CDLL('libc.so.6', use_errno=True)
lc.ioctl(fd, UI_SET_KEYBIT, ctypes.c_int(BTN_LEFT))
lc.ioctl(fd, UI_SET_RELBIT, ctypes.c_int(REL_X))
lc.ioctl(fd, UI_SET_RELBIT, ctypes.c_int(REL_Y))

setup = UInputSetup()
setup.id = InputId(3, 0x1234, 0x5678, 1)   # BUS_USB
setup.name = b"tb335 virtual mouse"
setup.ff_effects_max = 0
r = lc.ioctl(fd, UI_DEV_SETUP, ctypes.byref(setup))
print("UI_DEV_SETUP ->", r, flush=True)
r = lc.ioctl(fd, UI_DEV_CREATE)
print("UI_DEV_CREATE ->", r, flush=True)
time.sleep(1.5)

def emit(etype, code, value):
    t = time.time()
    sec, usec = int(t), int((t - int(t)) * 1e6)
    os.write(fd, struct.pack("<qqHHi", sec, usec, etype, code, value))

# 移动光标：右 40 像素 + 下 40 像素
for _ in range(8):
    emit(EV_REL, REL_X, 5)
    emit(EV_REL, REL_Y, 5)
    emit(EV_SYN, SYN_REPORT, 0)
    time.sleep(0.05)
print("moved cursor", flush=True)
time.sleep(20)
lc.ioctl(fd, UI_DEV_DESTROY)
os.close(fd)
print("destroyed", flush=True)
