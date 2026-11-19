#!/usr/bin/env python3
"""TB335 电源键 + 背光守护

短按：屏幕熄灭 / 点亮（点亮只恢复显示，仍停在锁屏时钟界面）
长按 3 秒：弹 GNOME 关机菜单（电源/重启/取消，自己选）
继续按住到 FORCE_OFF_MS（默认 8 秒）：绕过菜单直接强制断电（GNOME 卡死时的保底）

背光策略（关键）：
    面板是否在扫描由 mutter 的 PowerSaveMode 决定，并直接反映在 DRM 的
    /sys/class/drm/card0-DSI-1/dpms 上（Off/On）。但这台机器的背光走的是
    /sys/class/leds/lcd-backlight，不归合成器管（/sys/class/backlight 是空的、
    gsd 的 Screen.Brightness 恒为 -1），所以「面板停了、背光还亮着」时就会看到
    一片亮着的黑屏。
    本守护盯着 dpms：变成 Off 就把背光淡出到 0，变回 On 就淡入原值。
    这样无论是电源键锁屏、还是锁屏界面自己闲置熄屏，背光都会跟着一起灭。

为什么按键状态要在「按下」时锁存：
    按电源键这个动作本身会被 mutter 当成用户活动，按下瞬间它就把面板唤醒了，
    到松开时 dpms 已经是 On。若用松开时的状态判断，会永远走进「锁屏」分支。

为什么不用 logind 的 HandlePowerKey / HandlePowerKeyLongPress：
    GNOME 的 gsd-media-keys 无条件持有 handle-power-key 的 block 抑制剂，
    把 power-button-action 设成 nothing 也不释放，logind 收不到这个键。

---- 2026-11-16 v2 修订（按实测时序改）----
 1. 熄屏不再「等 dpms 变 Off 才淡出」，改为发出熄屏命令后**立即同步淡出**
    （FADE_OUT_MS 默认 650ms / 90 步），与锁屏动画同时结束，消除
    「面板已黑、背光还亮着」的空档，也不会再出现背光突然消失。
 2. 新增 BLANK_GRACE_MS（默认 4000）：熄屏动作发出后的宽限期内，即使 dpms
    短暂回到 On 也不回补背光；宽限期过后面板仍亮着才恢复亮度（真失败兜底）。
 3. 熄屏前把**当前真实亮度**记入 desired，亮屏回到用户调过的值，不再固定 102。
 4. os.read() 异常时退避 100ms 并重新解析输入设备，避免节点消失后 100% CPU 空转。
 5. 每次按下/松手写一行日志（含按住时长），便于对齐「按了几次 / 执行了几次」。
"""

import glob
import os
import select
import struct
import subprocess
import sys
import time

CFG = os.environ.get("TB335_POWERKEY_CONF", "/etc/tb335-powerkey.conf")

DEFAULTS = {
    "DEVICE_NAME": "mtk-pmic-keys",
    "KEY_CODE": "116",              # KEY_POWER
    "LONG_PRESS_MS": "3000",
    "COOLDOWN_MS": "400",
    "BACKLIGHT": "/sys/class/leds/lcd-backlight/brightness",
    "FALLBACK_BRIGHTNESS": "102",
    "MIN_BRIGHTNESS": "38",   # 熄屏/亮屏恢复时的亮度下限（38/255 ≈ 15%）
    "DPMS": "/sys/class/drm/card0-DSI-1/dpms",
    "POLL_MS": "100",
    "FADE_MS": "450",
    "FADE_STEPS": "60",
    "EASE_OUT": "0.4,0,0.2,1",
    "EASE_IN": "0,0,0.2,1",
    "FADE_OUT_MS": "650",
    "FADE_OUT_STEPS": "90",
    "BLANK_GRACE_MS": "4000",
    "LOCK": "1",
    "LOCK_CMD": "loginctl lock-sessions",
    "BLANK_CMD": "runuser -u desk -- env XDG_RUNTIME_DIR=/run/user/1000 "
                 "DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus "
                 "busctl --user set-property org.gnome.Mutter.DisplayConfig "
                 "/org/gnome/Mutter/DisplayConfig org.gnome.Mutter.DisplayConfig "
                 "PowerSaveMode i 3",
    "WAKE_CMD": "runuser -u desk -- env XDG_RUNTIME_DIR=/run/user/1000 "
                "DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus "
                "busctl --user set-property org.gnome.Mutter.DisplayConfig "
                "/org/gnome/Mutter/DisplayConfig org.gnome.Mutter.DisplayConfig "
                "PowerSaveMode i 0",
    # 长按 3s 的动作：弹 GNOME 的关机菜单（--no-prompt 才是直接关机，这里故意不带）
    "POWEROFF_CMD": "gnome-session-quit --power-off",
    # 继续按住到这个时长（毫秒）直接强制断电；0 = 关闭该保底动作
    "FORCE_OFF_MS": "8000",
    "FORCE_OFF_CMD": "/usr/local/sbin/tb335-poweroff.sh",
}

EV_KEY = 1
KEY_RELEASE = 0
KEY_PRESS = 1
EVENT_SIZE = 24                     # arm64: timeval(16) + u16 type + u16 code + s32 value


def log(msg):
    print(msg, flush=True)


def load_cfg(path):
    cfg = dict(DEFAULTS)
    try:
        with open(path) as fh:
            for raw in fh:
                line = raw.strip()
                if not line or line.startswith("#") or "=" not in line:
                    continue
                key, val = line.split("=", 1)
                cfg[key.strip()] = val.strip()
    except OSError:
        pass
    return cfg


def find_device(name):
    for path in sorted(glob.glob("/sys/class/input/event*/device/name")):
        try:
            with open(path) as fh:
                if fh.read().strip() != name:
                    continue
        except OSError:
            continue
        return "/dev/input/" + path.split("/")[4]
    return None


def open_dev(name):
    dev = find_device(name)
    if dev is None:
        return None, None
    try:
        return dev, os.open(dev, os.O_RDONLY)
    except OSError as err:
        log("打开 %s 失败: %s" % (dev, err))
        return None, None


def read_int(path):
    try:
        with open(path) as fh:
            return int(fh.read().strip())
    except (OSError, ValueError):
        return None


def read_text(path):
    try:
        with open(path) as fh:
            return fh.read().strip()
    except OSError:
        return None


def write_int(path, value):
    try:
        with open(path, "w") as fh:
            fh.write(str(value))
        return True
    except OSError as err:
        log("写 %s 失败: %s" % (path, err))
        return False


def _bezier(p1x, p1y, p2x, p2y):
    """返回 cubic-bezier(p1x,p1y,p2x,p2y) 的 y=f(x) 求值函数（二分求解 x→t）。"""
    def _c(t, a1, a2):
        mt = 1.0 - t
        return 3.0 * mt * mt * t * a1 + 3.0 * mt * t * t * a2 + t * t * t

    def f(x, _c=_c, p1x=p1x, p2x=p2x, p1y=p1y, p2y=p2y):
        if x <= 0.0:
            return 0.0
        if x >= 1.0:
            return 1.0
        lo, hi = 0.0, 1.0
        for _ in range(26):
            mid = (lo + hi) * 0.5
            if _c(mid, p1x, p2x) < x:
                lo = mid
            else:
                hi = mid
        return _c((lo + hi) * 0.5, p1y, p2y)
    return f


def parse_curve(text, fallback=None):
    """解析 "x1,y1,x2,y2"；失败时退回 smoothstep。"""
    try:
        parts = [float(v) for v in str(text).split(",")]
        if len(parts) == 4:
            return _bezier(parts[0], parts[1], parts[2], parts[3])
    except (TypeError, ValueError):
        pass
    return fallback or (lambda u: u * u * (3 - 2 * u))


def ramp(path, target, fade_ms, steps, curve=None):
    """按 curve(u) 做渐变。curve 缺省为 smoothstep；fade_ms<=0 时直接跳变。"""
    cur = read_int(path)
    if cur is None or cur == target or fade_ms <= 0:
        return write_int(path, target)

    steps = max(1, int(steps))
    interval = (fade_ms / 1000.0) / steps
    base = cur
    start = time.monotonic()
    for i in range(1, steps + 1):
        u = i / steps
        ease = curve(u) if curve else u * u * (3 - 2 * u)
        write_int(path, int(round(base + (target - base) * ease)))
        gap = start + interval * i - time.monotonic()
        if gap > 0:
            time.sleep(gap)
    return write_int(path, target)


def run_cmd(cmd):
    log("执行: %s" % cmd)
    try:
        subprocess.run(cmd, shell=True, timeout=60, check=False)
    except Exception as err:
        log("命令失败: %s" % err)


def run_cmd_bg(cmd):
    """后台拉起命令，不阻塞按键处理。

    2026-11-16 修复：关机脚本实测可能卡住（记录里有一次执行 60 秒没返回），
    以前用阻塞式 run_cmd 会让整个守护在长按后僵死一分钟，期间的按键全被当成
    后续事件处理（出现"关机没关掉、反而亮灭闪几下"）。改成 Popen 分离进程。
    """
    log("后台执行: %s" % cmd)
    try:
        subprocess.Popen(cmd, shell=True, stdin=subprocess.DEVNULL,
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                         start_new_session=True, close_fds=True)
    except Exception as err:
        log("后台命令失败: %s" % err)


def main():
    cfg = load_cfg(CFG)
    key_code = int(cfg["KEY_CODE"])
    long_s = int(cfg["LONG_PRESS_MS"]) / 1000.0
    force_s = int(cfg["FORCE_OFF_MS"]) / 1000.0
    cooldown_s = int(cfg["COOLDOWN_MS"]) / 1000.0
    poll_s = max(0.02, int(cfg["POLL_MS"]) / 1000.0)
    fade_ms = int(cfg["FADE_MS"])
    fade_steps = int(cfg["FADE_STEPS"])
    fade_out_ms = int(cfg["FADE_OUT_MS"])
    fade_out_steps = int(cfg["FADE_OUT_STEPS"])
    grace_s = max(0.0, int(cfg["BLANK_GRACE_MS"]) / 1000.0)
    ease_out = parse_curve(cfg["EASE_OUT"])
    ease_in = parse_curve(cfg["EASE_IN"])
    bl = cfg["BACKLIGHT"]
    dpms_path = cfg["DPMS"]
    fallback = int(cfg["FALLBACK_BRIGHTNESS"])
    min_br = max(0, int(cfg["MIN_BRIGHTNESS"]))

    dev, fd = open_dev(cfg["DEVICE_NAME"])
    if fd is None:
        log("找不到输入设备 %r，退出（systemd 会重试）" % cfg["DEVICE_NAME"])
        return 1

    if force_s > 0:
        log("监听 %s：短按=锁屏/点亮，长按 %.1fs=关机菜单，按住 %.1fs=强制断电；背光跟随 %s"
            % (dev, long_s, force_s, dpms_path))
    else:
        log("监听 %s：短按=锁屏/点亮，长按 %.1fs=关机菜单；背光跟随 %s"
            % (dev, long_s, dpms_path))

    cur = read_int(bl)
    state = {
        "desired": max(min_br, cur if cur and cur > 0 else fallback),
        "off": False,
        "grace_until": 0.0,
    }

    def sync_backlight():
        """面板停扫 → 背光淡出；面板恢复 → 淡入目标亮度（宽限期内不回补）。"""
        dpms = read_text(dpms_path)
        if dpms is None:
            return
        if dpms == "Off":
            if state["off"]:
                return
            if ramp(bl, 0, fade_ms, fade_steps, ease_out):
                state["off"] = True
                log("面板已停扫 → 淡出背光（目标亮度 %s）" % state["desired"])
        else:
            if not state["off"]:
                return
            if time.monotonic() < state["grace_until"]:
                return
            if ramp(bl, state["desired"], fade_ms, fade_steps, ease_in):
                state["off"] = False
                log("面板已恢复 → 淡入背光（%s）" % state["desired"])

    def wait_dpms(want_off, timeout=0.6):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if (read_text(dpms_path) == "Off") == want_off:
                return True
            time.sleep(0.03)
        return False

    def bus_path_of(cmd):
        """从 WAKE_CMD 里抽出 unix:path=... 的总线路径，用于启动早期判断总线是否就绪。"""
        marker = "unix:path="
        idx = (cmd or "").find(marker)
        if idx < 0:
            return None
        rest = cmd[idx + len(marker):]
        for sep in (" ", "\t", "\n", '"', "'"):
            cut = rest.find(sep)
            if cut >= 0:
                rest = rest[:cut]
        return rest or None

    def short_press(was_off):
        if was_off:
            # 屏幕原本是黑的：只恢复显示，会话仍锁着，露出的是锁屏时钟
            state["grace_until"] = 0.0
            # 2026-11-16 修复：早期启动（GDM/用户会话还没起来）时 /run/user/1000/bus
            # 可能还不存在，busctl 直接失败、面板不会被唤醒。先等总线再发命令。
            bus = bus_path_of(cfg["WAKE_CMD"])
            waited = 0.0
            while bus and not os.path.exists(bus) and waited < 8.0:
                time.sleep(0.2)
                waited += 0.2
            if bus and not os.path.exists(bus):
                log("唤醒跳过：用户总线 %s 未就绪（早期启动/未登录）" % bus)
            if cfg["WAKE_CMD"].strip():
                run_cmd(cfg["WAKE_CMD"])
            # 只有 dpms 确认面板恢复后才回补背光；否则保持 0，避免出现「亮着的黑屏」
            if wait_dpms(False, 2.0):
                if (read_int(bl) or 0) == 0:
                    ramp(bl, state["desired"], fade_ms, fade_steps, ease_in)
                    state["off"] = False
                    log("面板已恢复但背光为 0 → 回补到 %s" % state["desired"])
            else:
                log("唤醒失败：面板仍未恢复，保持背光 0（避免亮黑屏）")
            sync_backlight()
            return

        # 屏幕原本亮着：先把画面淡出到黑，再锁屏、关面板。
        # 2026-11-18 v3：旧顺序是"锁屏 -> 淡出"，实测桌面上按电源键时
        # GNOME 上锁/切锁屏画面会让淡出几乎看不见（表现为瞬间黑、无过渡）；
        # 锁屏界面下因为已经在锁屏页，淡出才看得见。改成先淡出后锁屏，
        # 两种情况都能看到过渡。锁屏动作放后台，不再阻塞按键处理。
        cur_bl = read_int(bl)
        if cur_bl and cur_bl > 0:
            state["desired"] = max(min_br, cur_bl)
        t0 = time.monotonic()
        if ramp(bl, 0, fade_out_ms, fade_out_steps, ease_out):
            state["off"] = True
            state["grace_until"] = time.monotonic() + grace_s
            log("短按熄屏 → 背光淡出 %.0fms（亮度 %s → 0，目标亮度 %s）"
                % ((time.monotonic() - t0) * 1000.0, cur_bl, state["desired"]))
        if cfg["LOCK"] == "1":
            run_cmd_bg(cfg["LOCK_CMD"])
        if cfg["BLANK_CMD"].strip():
            run_cmd(cfg["BLANK_CMD"])
        wait_dpms(True)
        # 面板停扫后再补一次 0（有些驱动在关屏瞬间会把亮度恢复成默认值）
        write_int(bl, 0)
        sync_backlight()

    pressed_at = None
    press_was_off = False
    long_fired = False
    force_fired = False
    last_action = 0.0

    sync_backlight()                    # 启动时先对齐一次（可能正处在一个坏状态里）

    while True:
        timeout = poll_s
        if pressed_at is not None:
            held = time.monotonic() - pressed_at
            if not long_fired:
                timeout = min(timeout, max(0.0, long_s - held))
            elif force_s > 0 and not force_fired:
                timeout = min(timeout, max(0.0, force_s - held))
        try:
            ready, _, _ = select.select([fd], [], [], timeout)
        except InterruptedError:
            continue

        if not ready:
            if pressed_at is not None:
                held = time.monotonic() - pressed_at
                if not long_fired and held >= long_s - 0.02:
                    long_fired = True
                    log("长按达 %.1fs → 弹关机菜单" % long_s)
                    run_cmd_bg(cfg["POWEROFF_CMD"])
                    continue
                if long_fired and force_s > 0 and not force_fired and held >= force_s - 0.02:
                    force_fired = True
                    log("按住达 %.1fs → 强制断电（菜单未处理时的保底）" % force_s)
                    run_cmd_bg(cfg["FORCE_OFF_CMD"])
                    continue
            sync_backlight()
            continue

        try:
            data = os.read(fd, EVENT_SIZE)
        except OSError as err:
            log("读 %s 失败: %s；重开设备" % (dev, err))
            try:
                os.close(fd)
            except OSError:
                pass
            time.sleep(0.1)
            dev, fd = open_dev(cfg["DEVICE_NAME"])
            if fd is None:
                time.sleep(1.0)
            continue
        if len(data) < EVENT_SIZE:
            continue

        _, _, etype, code, value = struct.unpack("llHHi", data)
        if etype != EV_KEY or code != key_code or value == 2:
            continue

        if value == KEY_PRESS:
            pressed_at = time.monotonic()
            long_fired = False
            force_fired = False
            # 关键：在按下的瞬间锁存状态，按下本身会让 mutter 立刻唤醒面板
            press_was_off = state["off"] or read_text(dpms_path) == "Off"
            log("按下（按前 %s）" % ("灭" if press_was_off else "亮"))
            continue
        if value != KEY_RELEASE:
            continue

        dur = 0.0 if pressed_at is None else time.monotonic() - pressed_at
        pressed_at = None
        log("松手（按住 %.2fs）" % dur)
        if long_fired:
            continue
        if dur >= long_s:
            long_fired = True
            log("长按 %.2fs → 弹关机菜单" % dur)
            run_cmd_bg(cfg["POWEROFF_CMD"])
            continue

        now = time.monotonic()
        if now - last_action < cooldown_s:
            log("忽略连击（间隔 %.2fs）" % (now - last_action))
            continue
        last_action = now
        short_press(press_was_off)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(0)
