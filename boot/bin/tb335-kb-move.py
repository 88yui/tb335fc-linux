#!/usr/bin/env python3
# 屏幕键盘拖动条：贴在 onboard 键盘上方，按住即可拖动键盘
# 位置保存在 /persist/kb-pos.conf（KEY=X/Y），下次启动沿用
import os
import re
import subprocess
import time

import gi

gi.require_version("Gtk", "3.0")
gi.require_version("Gdk", "3.0")
from gi.repository import Gdk, Gtk  # noqa: E402

POS_FILE = "/persist/kb-pos.conf"
BAR_H = 30
GAP = 2
DEFAULT_X, DEFAULT_Y = 780, 1290  # 键盘左上角（屏幕 2560x1600）


def sh(args):
    try:
        return subprocess.run(args, capture_output=True, text=True, timeout=5).stdout
    except Exception:  # noqa: BLE001
        return ""


def kb_window():
    """返回面积最大的 onboard 窗口 id"""
    best = None
    for wid in sh(["xdotool", "search", "--class", "Onboard"]).split():
        geo = {}
        for k, v in re.findall(r"^(\w+)=(-?\d+)$", sh(["xdotool", "getwindowgeometry", "--shell", wid]), re.M):
            geo[k] = int(v)
        w, h = geo.get("WIDTH", 0), geo.get("HEIGHT", 0)
        if w > 200 and h > 100 and (best is None or w * h > best[0]):
            best = (w * h, wid, geo)
    return (best[1], best[2]) if best else (None, None)


def load_pos():
    try:
        with open(POS_FILE) as f:
            d = dict(re.findall(r"(\w+)=(-?\d+)", f.read()))
        return int(d["X"]), int(d["Y"])
    except Exception:  # noqa: BLE001
        return DEFAULT_X, DEFAULT_Y


def save_pos(x, y):
    try:
        with open(POS_FILE, "w") as f:
            f.write(f"X={x}\nY={y}\n")
    except OSError:
        pass


class Bar(Gtk.Window):
    def __init__(self, x, y):
        super().__init__(Gtk.WindowType.TOPLEVEL)
        self.set_decorated(False)
        self.set_keep_above(True)
        self.set_skip_taskbar_hint(True)
        self.set_skip_pager_hint(True)
        self.set_accept_focus(False)
        self.set_resizable(False)
        self.set_default_size(0, BAR_H)
        self._dragging = False
        self._last_move = 0.0

        screen = Gdk.Screen.get_default()
        self.sw = screen.get_width()
        self.sh = screen.get_height()

        eb = Gtk.EventBox()
        eb.set_events(Gdk.EventMask.BUTTON_PRESS_MASK | Gdk.EventMask.BUTTON_RELEASE_MASK
                      | Gdk.EventMask.POINTER_MOTION_MASK)
        rgba = Gdk.RGBA()
        rgba.parse("#1f3a5f")
        eb.override_background_color(Gtk.StateType.NORMAL, rgba)
        lbl = Gtk.Label()
        lbl.set_markup('<span foreground="#dfe8f3" size="medium">≡  按住此处拖动键盘</span>')
        eb.add(lbl)
        eb.connect("button-press-event", self.on_press)
        eb.connect("motion-notify-event", self.on_motion)
        eb.connect("button-release-event", self.on_release)
        self.add(eb)
        self.move(x, y - BAR_H - GAP)
        self.show_all()

    def on_press(self, _w, e):
        if e.button != 1:
            return False
        wid, geo = kb_window()
        if not wid:
            return False
        self._win = wid
        self._px, self._py = int(e.x_root), int(e.y_root)
        self._wx, self._wy = geo["X"], geo["Y"]
        self._ww, self._wh = geo["WIDTH"], geo["HEIGHT"]
        self._dragging = True
        return True

    def on_motion(self, _w, e):
        if not self._dragging:
            return False
        now = time.time()
        if now - self._last_move < 0.04:
            return True
        self._last_move = now
        nx = self._wx + int(e.x_root) - self._px
        ny = self._wy + int(e.y_root) - self._py
        nx = max(0, min(self.sw - self._ww, nx))
        ny = max(0, min(self.sh - self._wh, ny))
        self._apply(nx, ny)
        return True

    def on_release(self, _w, _e):
        if self._dragging:
            self._dragging = False
            wid, geo = kb_window()
            if geo:
                save_pos(geo["X"], geo["Y"])
                self.move(geo["X"], geo["Y"] - BAR_H - GAP)
        return False

    def _apply(self, nx, ny):
        subprocess.run(["xdotool", "windowmove", self._win, str(nx), str(ny)],
                       capture_output=True, timeout=5)
        self.move(nx, ny - BAR_H - GAP)


def main():
    x, y = load_pos()
    wid, geo = kb_window()
    if not wid:
        # 键盘没起就先拉起（脚本内部会等窗口出现）
        subprocess.run(["/usr/local/bin/tb335-keyboard"], timeout=30)
        for _ in range(20):
            time.sleep(0.5)
            wid, geo = kb_window()
            if wid:
                break
    if wid and geo:
        x, y = geo["X"], geo["Y"]
    bar = Bar(x, y)  # noqa: F841
    Gtk.main()


if __name__ == "__main__":
    main()
