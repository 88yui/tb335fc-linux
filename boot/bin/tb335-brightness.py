#!/usr/bin/env python3
"""TB335 屏幕亮度调节（GTK3）

本机没有 /sys/class/backlight（GNOME 因此不显示亮度滑块），
背光实际由 /sys/class/leds/lcd-backlight 控制，这里直接写该节点。

依赖：python3-gi（已装）；需要 udev 规则把节点放开给 desk：
    /etc/udev/rules.d/99-tb335-backlight.rules
"""

import gi

gi.require_version("Gtk", "3.0")
from gi.repository import Gtk  # noqa: E402

MIN_PCT = 15          # 最低亮度保底百分比

NODE = "/sys/class/leds/lcd-backlight/brightness"
MAX_NODE = "/sys/class/leds/lcd-backlight/max_brightness"


def read_int(path, default=None):
    try:
        with open(path) as fh:
            return int(fh.read().strip())
    except (OSError, ValueError):
        return default


class BrightnessWindow(Gtk.Window):
    def __init__(self):
        super().__init__(title="屏幕亮度")
        self.set_border_width(18)
        self.set_default_size(380, 130)
        self.set_keep_above(True)

        self.max = read_int(MAX_NODE, 255) or 255
        cur = read_int(NODE, self.max) or 0

        box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=10)
        self.add(box)

        self.value_label = Gtk.Label()
        self.value_label.set_markup("<span size='x-large'>%d%%</span>" % round(cur * 100 / self.max))
        box.pack_start(self.value_label, False, False, 0)

        self.scale = Gtk.Scale.new_with_range(Gtk.Orientation.HORIZONTAL, MIN_PCT, 100, 1)
        self.scale.set_value(max(MIN_PCT, round(cur * 100 / self.max)))
        self.scale.set_draw_value(False)
        self.scale.connect("value-changed", self.on_value_changed)
        box.pack_start(self.scale, True, True, 0)

        self.hint = Gtk.Label()
        box.pack_start(self.hint, False, False, 0)

    def on_value_changed(self, widget):
        pct = int(widget.get_value())
        level = int(round(self.max * pct / 100.0))
        self.value_label.set_markup("<span size='x-large'>%d%%</span>" % pct)
        try:
            with open(NODE, "w") as fh:
                fh.write(str(level))
            self.hint.set_text("")
        except PermissionError:
            self.hint.set_text("没有写权限（检查 udev 规则 99-tb335-backlight.rules）")
        except OSError as err:
            self.hint.set_text(str(err))


def main():
    win = BrightnessWindow()
    win.connect("destroy", Gtk.main_quit)
    win.show_all()
    Gtk.main()


if __name__ == "__main__":
    main()
