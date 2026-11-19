#!/usr/bin/env python3
"""tb335-kmsg-persist.py -- 逐批 fsync 的内核日志落盘器（为硬复位取证设计）

为什么需要它
------------
原 `tb335-kmsg-capture.sh` 用 `cat /dev/kmsg >> log`。数据先落在**页缓存**，
受 `vm.dirty_expire_centisecs`（本机 500 = 5 秒）控制才回写。
而 Xorg 一提交就**约 2 秒**内硬复位 —— 关键的最后几秒正好在缓存里，随复位一起蒸发。

本脚本的做法
------------
* 读 /dev/kmsg（lseek 到 END，只看新记录，不清空环形缓冲、不影响其它工具）
* 累积到 64 KiB 或 100 ms 就 write + **os.fsync()**，保证复位前已落盘
* 每次启动把上次的日志轮转为 kmsg-last.log（只保留尾部 8 KiB 作为"上一轮死因"）
* 单文件上限 32 MiB

用法
----
    python3 tb335-kmsg-persist.py            # 前台常驻（systemd Type=simple）
    python3 tb335-kmsg-persist.py --once     # 只跑 3 秒，用于自检
"""
import errno
import os
import select
import sys
import time

LOG = "/persist/a9b/kmsg-trial.log"
PREV_LOG = "/persist/a9b/kmsg-prev.log"
MAXSZ = 32 * 1024 * 1024
BATCH = 64 * 1024
SYNC_EVERY = 0.10          # 秒
PREV_KEEP = 4 * 1024 * 1024  # 上一轮保留的字节数（从尾部截取）


def log(msg):
    try:
        with open("/dev/kmsg", "wb", buffering=0) as k:
            k.write(b"tb335-kmsg: " + msg.encode() + b"\n")
    except Exception:
        pass


def rotate():
    """上一轮的日志 -> 只留尾部，避免无限增长；保留死因现场。"""
    try:
        if not os.path.exists(LOG):
            return
        sz = os.path.getsize(LOG)
        with open(LOG, "rb") as f:
            if sz > PREV_KEEP:
                f.seek(sz - PREV_KEEP)
            tail = f.read()
        with open(PREV_LOG, "wb") as f:
            f.write(tail)
            f.flush()
            os.fsync(f.fileno())
        os.unlink(LOG)
    except Exception as e:
        log("rotate failed: %r" % (e,))


def main():
    once = "--once" in sys.argv
    rotate()

    fd = os.open("/dev/kmsg", os.O_RDONLY | os.O_NONBLOCK)
    try:
        os.lseek(fd, 0, os.SEEK_END)      # 只看新记录
    except OSError:
        pass

    out = open(LOG, "ab", buffering=0)
    out.write(("===== persist start %s uptime=%.2f =====\n"
               % (time.strftime("%F %T"),
                  float(open("/proc/uptime").read().split()[0]))).encode())
    os.fsync(out.fileno())

    buf = bytearray()
    last_sync = time.time()
    deadline = time.time() + 3 if once else None

    while True:
        if deadline and time.time() > deadline:
            break
        r, _, _ = select.select([fd], [], [], 0.05)
        if r:
            while True:
                try:
                    rec = os.read(fd, 8192)
                except OSError as e:
                    if e.errno == errno.EPIPE:      # 环形缓冲被覆盖，重开
                        os.close(fd)
                        fd = os.open("/dev/kmsg", os.O_RDONLY | os.O_NONBLOCK)
                        continue
                    if e.errno in (errno.EAGAIN,):
                        break
                    raise
                if not rec:
                    break
                buf += rec
                if len(buf) >= BATCH:
                    break

        now = time.time()
        if buf and (len(buf) >= BATCH or now - last_sync >= SYNC_EVERY):
            try:
                if os.path.getsize(LOG) > MAXSZ:
                    out.close()
                    rotate()
                    out = open(LOG, "ab", buffering=0)
                out.write(buf)
                os.fsync(out.fileno())          # ★ 关键：每批都落盘
            except Exception as e:
                log("write failed: %r" % (e,))
            buf.clear()
            last_sync = now

    out.close()
    os.close(fd)
    return 0


if __name__ == "__main__":
    sys.exit(main())
