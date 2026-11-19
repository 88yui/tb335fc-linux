#!/usr/bin/env python3
# 直接 ioctl 读 ALSA 控件表，绕开 amixer 的 mixer load 失败
import ctypes, os, sys

class ElemId(ctypes.Structure):
    _fields_ = [("numid", ctypes.c_uint), ("iface", ctypes.c_int),
                ("device", ctypes.c_uint), ("subdevice", ctypes.c_uint),
                ("name", ctypes.c_char * 44), ("index", ctypes.c_uint)]

class ElemList(ctypes.Structure):
    _fields_ = [("offset", ctypes.c_uint), ("space", ctypes.c_uint),
                ("used", ctypes.c_uint), ("count", ctypes.c_uint),
                ("pids", ctypes.POINTER(ElemId)), ("reserved", ctypes.c_ubyte * 50)]

SNDRV_CTL_IOCTL_ELEM_LIST = 0xC0505510
libc = ctypes.CDLL("libc.so.6", use_errno=True)

fd = os.open("/dev/snd/controlC0", os.O_RDWR)
lst = ElemList()
lst.space = 0
lst.pids = None
r = libc.ioctl(fd, ctypes.c_ulong(SNDRV_CTL_IOCTL_ELEM_LIST), ctypes.byref(lst))
if r < 0:
    print("ioctl list failed", ctypes.get_errno()); sys.exit(1)
total = lst.count
print("total controls:", total, flush=True)

arr = (ElemId * total)()
lst.pids = ctypes.cast(arr, ctypes.POINTER(ElemId))
lst.space = total
lst.offset = 0
libc.ioctl(fd, ctypes.c_ulong(SNDRV_CTL_IOCTL_ELEM_LIST), ctypes.byref(lst))
print("used:", lst.used, flush=True)

names = []
for i in range(lst.used):
    names.append((arr[i].iface, arr[i].numid, arr[i].name.decode("utf-8", "replace")))
for iface, numid, n in names:
    print(f"{numid}\t{iface}\t{n}")
