#!/usr/bin/env python3
"""MTK /dev/stpbt <-> HCI H4 pty bridge for BlueZ btattach.
打开 /dev/stpbt 会触发 WMT 给 connsys 蓝牙上电（dmesg: BT_open: WMT turn on BT OK!）。
用法: bt-bridge.py [stop]
"""
import os
import pty
import select
import signal
import subprocess
import sys
import time

STP = "/dev/stpbt"
LOG = "/persist/bt-bridge.log"
PIDF = "/run/bt-bridge.pid"
BTATTACH = "/usr/bin/btattach"
MAXLOG = 200

# MTK connsys 固件把 "Synchronization Train" 特性标为支持，但未实现
# Read Synchronization Train Parameters (0x0C77)，回 Unknown Command 会让
# 内核初始化中断（hciconfig up -> EBADRQC）。
# 主手段：改写 Read Local Extended Features(page 2) 的返回，清掉该特性位。
FEAT_PREFIX = bytes.fromhex("040e0e010410000202")   # CC of 0x1004, page 2
BAD_CMD = bytes.fromhex("01770c00")
BAD_RESP = bytes.fromhex("040e0b01770c" + "00" * 8)  # 兜底：伪造 CC（8 字节参数）


def log(msg):
    with open(LOG, "a") as f:
        f.write(time.strftime("%F %T ") + msg + "\n")


def running_pid():
    try:
        with open(PIDF) as f:
            pid = int(f.read().strip())
        with open(f"/proc/{pid}/cmdline", "rb") as f:
            if b"bt-bridge.py" not in f.read():
                return 0
        os.kill(pid, 0)
        return pid
    except Exception:
        return 0


def stop():
    pid = running_pid()
    if pid:
        try:
            os.kill(pid, signal.SIGTERM)
            log(f"stop pid={pid}")
        except Exception:
            pass
    try:
        os.unlink(PIDF)
    except OSError:
        pass


def main():
    if len(sys.argv) > 1 and sys.argv[1] == "stop":
        stop()
        return 0
    if running_pid():
        print("already running")
        return 0
    with open(PIDF, "w") as f:
        f.write(str(os.getpid()))

    for _ in range(60):
        if os.path.exists(STP):
            break
        time.sleep(1)
    if not os.path.exists(STP):
        log("stpbt missing, abort")
        return 1

    master, slave = pty.openpty()
    name = os.ttyname(slave)
    os.close(slave)
    log(f"pty {name}")
    proc = subprocess.Popen([BTATTACH, "-B", name, "-P", "h4"],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(0.5)

    fd = os.open(STP, os.O_RDWR)          # triggers WMT BT power on
    log("stpbt opened")
    os.set_blocking(master, False)
    os.set_blocking(fd, False)

    logged = 0

    def _term(_sig, _frm):
        try:
            proc.terminate()
        except Exception:
            pass
        try:
            os.unlink(PIDF)
        except OSError:
            pass
        sys.exit(0)

    signal.signal(signal.SIGTERM, _term)
    signal.signal(signal.SIGINT, _term)
    while True:
        if proc.poll() is not None:
            log(f"btattach exited rc={proc.returncode}, restarting")
            time.sleep(1)
            proc = subprocess.Popen([BTATTACH, "-B", name, "-P", "h4"],
                                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            ready, _, _ = select.select([master, fd], [], [], 2.0)
        except InterruptedError:
            continue
        for src in ready:
            try:
                data = os.read(src, 4096)
            except OSError:
                data = b""
            if not data:
                continue
            dst = fd if src == master else master
            if src == fd and FEAT_PREFIX in data:
                i = data.find(FEAT_PREFIX)
                j = i + len(FEAT_PREFIX)
                if j + 8 <= len(data) and data[j] & 0x04:
                    data = data[:j] + bytes([data[j] & ~0x04]) + data[j + 1:]
                    log("cleared sync-train feature bit")
            if src == master and BAD_CMD in data:
                data = data.replace(BAD_CMD, b"")
                log("filtered 0x0c77, injected fake CC")
                try:
                    os.write(master, BAD_RESP)
                except OSError as e:
                    log(f"inject fail {e}")
                if not data:
                    continue
            try:
                os.write(dst, data)
            except OSError as e:
                log(f"write fail {e}")
            if logged < MAXLOG:
                logged += 1
                log(("H->C " if src == master else "C->H ") + data[:16].hex())
    return 0


if __name__ == "__main__":
    sys.exit(main())
