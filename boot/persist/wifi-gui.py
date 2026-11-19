#!/usr/bin/env python3
# tb335 网络与蓝牙面板（GTK3）
# 依赖：tb335-wifi.service（wlan0 + wpa_supplicant）、tb335-bt.service（hci0）
import re
import subprocess
import threading
import time

import gi

gi.require_version("Gtk", "3.0")
from gi.repository import GLib, Gtk, Pango  # noqa: E402

IFACE = "wlan0"


def log(msg):
    try:
        with open("/persist/wifi-gui.log", "a") as f:
            f.write(time.strftime("%F %T ") + msg + "\n")
    except OSError:
        pass


def sh(cmd, timeout=30):
    try:
        p = subprocess.run(cmd, shell=True, capture_output=True, text=True, timeout=timeout)
        return p.returncode, p.stdout, p.stderr
    except Exception as e:  # noqa: BLE001
        return 1, "", str(e)


# ---------------- WiFi ----------------

def wpa(cmd):
    return sh(f"wpa_cli -i {IFACE} {cmd}")


def wpa_ready():
    rc, out, err = sh(f"wpa_cli -i {IFACE} ping", timeout=5)
    return rc == 0 and "PONG" in out


def scan_aps():
    rc, out, err = sh(f"iw dev {IFACE} scan", timeout=30)
    if rc != 0 or not out:
        return [], err.strip() or out.strip()
    aps, cur = [], None
    for line in out.splitlines():
        s = line.strip()
        if s.startswith("BSS "):
            if cur:
                aps.append(cur)
            cur = {"bssid": s.split()[1].split("(")[0], "ssid": "", "signal": -100, "sec": ""}
        elif cur is not None:
            if s.startswith("SSID:"):
                cur["ssid"] = s[5:].strip()
            elif s.startswith("signal:"):
                m = re.search(r"(-?\d+)", s)
                if m:
                    cur["signal"] = int(m.group(1))
            elif s.startswith("RSN:"):
                cur["sec"] = cur["sec"] or "WPA2"
            elif s.startswith("WPA:"):
                cur["sec"] = cur["sec"] or "WPA"
            elif "Authentication suites:" in s and "SAE" in s:
                cur["sec"] = "WPA3"
    if cur:
        aps.append(cur)
    best = {}
    for ap in aps:
        if not ap["ssid"]:
            continue
        k = ap["ssid"]
        if k not in best or ap["signal"] > best[k]["signal"]:
            best[k] = ap
    return sorted(best.values(), key=lambda a: a["signal"], reverse=True), ""


def wifi_status():
    if not wpa_ready():
        return {"state": "服务未就绪", "ssid": "", "ip": "", "route": False}
    rc, out, _ = sh(f"wpa_cli -i {IFACE} status", timeout=5)
    d = {}
    for line in out.splitlines():
        if "=" in line:
            k, v = line.split("=", 1)
            d[k] = v
    rc2, ipout, _ = sh(f"ip -4 addr show {IFACE}", timeout=5)
    m = re.search(r"inet (\d+\.\d+\.\d+\.\d+)", ipout)
    rc3, rt, _ = sh("ip route show default", timeout=5)
    has_route = bool(re.search(r"default via \S+ dev wlan0", rt))
    return {"state": d.get("wpa_state", "?"), "ssid": d.get("ssid", ""),
            "ip": m.group(1) if m else "", "route": has_route}


def wifi_gateway():
    rc, out, _ = sh(f"ip -4 route show dev {IFACE}", timeout=5)
    m = re.search(r"default via (\d+\.\d+\.\d+\.\d+)", out)
    if m:
        return m.group(1)
    try:
        with open("/var/lib/dhcp/dhclient.leases") as f:
            txt = f.read()
    except OSError:
        return ""
    gws = re.findall(r'interface "wlan0"[\s\S]*?option routers ([0-9.]+);', txt)
    return gws[-1] if gws else ""


def ensure_wifi_route():
    gw = wifi_gateway()
    if not gw:
        return False
    sh("ip route del default dev wlan0 2>/dev/null", timeout=5)
    rc, _, _ = sh(f"ip route replace default via {gw} dev wlan0 metric 50", timeout=5)
    return rc == 0


# ---------------- Bluetooth ----------------

def bt_show():
    rc, out, _ = sh("bluetoothctl show", timeout=8)
    if rc != 0 or "Controller" not in out:
        return {"ok": False, "addr": "", "powered": False}
    m = re.search(r"Controller (\S+)", out)
    return {"ok": True, "addr": m.group(1) if m else "", "powered": "Powered: yes" in out}


def bt_devices(limit=25):
    rc, out, _ = sh("bluetoothctl devices", timeout=10)
    if rc != 0 or not out:
        return []
    devs = []
    for line in out.splitlines():
        m = re.match(r"Device ([0-9A-Fa-f:]{17}) (.+)$", line.strip())
        if not m:
            continue
        mac, name = m.group(1), m.group(2)
        rc2, info, _ = sh(f"bluetoothctl info {mac}", timeout=8)
        devs.append({
            "mac": mac,
            "name": name,
            "paired": "Paired: yes" in info,
            "connected": "Connected: yes" in info,
        })
        if len(devs) >= limit:
            break
    devs.sort(key=lambda d: (not d["connected"], not d["paired"], d["name"].lower()))
    return devs


class NetWindow(Gtk.Window):
    def __init__(self):
        super().__init__(title="网络与蓝牙")
        self.set_default_size(820, 780)
        self.set_border_width(10)
        self.aps = []
        self.bt_devs = []
        self._bt_busy = False

        nb = Gtk.Notebook()
        self.add(nb)
        self.notebook = nb
        nb.append_page(self._build_wifi_tab(), Gtk.Label(label="无线网络"))
        nb.append_page(self._build_bt_tab(), Gtk.Label(label="蓝牙"))
        nb.connect("switch-page", self.on_page_switch)
        self._last_bt_scan = 0.0

        self.refresh_status()
        GLib.timeout_add(3000, self.refresh_status)
        GLib.timeout_add(300, self.on_refresh, None)
        GLib.timeout_add(800, self._bt_first)
        GLib.timeout_add(8000, self._bt_timer)
        GLib.timeout_add(45000, self._bt_auto_scan)
        self.connect("destroy", Gtk.main_quit)
        self.show_all()

    # ---------- WiFi 页 ----------
    def _build_wifi_tab(self):
        box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=8)
        box.set_border_width(8)

        self.status_label = Gtk.Label(xalign=0)
        self.status_label.set_ellipsize(Pango.EllipsizeMode.END)
        box.pack_start(self.status_label, False, False, 0)

        bar = Gtk.Box(spacing=6)
        box.pack_start(bar, False, False, 0)
        self.btn_refresh = Gtk.Button(label="扫描")
        self.btn_refresh.connect("clicked", self.on_refresh)
        bar.pack_start(self.btn_refresh, False, False, 0)
        self.btn_hidden = Gtk.Button(label="加入隐藏网络…")
        self.btn_hidden.connect("clicked", self.on_hidden)
        bar.pack_start(self.btn_hidden, False, False, 0)
        self.btn_service = Gtk.Button(label="重启 WiFi 服务")
        self.btn_service.connect("clicked", self.on_restart_service)
        bar.pack_start(self.btn_service, False, False, 0)
        self.btn_disc = Gtk.Button(label="断开")
        self.btn_disc.connect("clicked", self.on_disconnect)
        bar.pack_end(self.btn_disc, False, False, 0)

        sw = Gtk.ScrolledWindow()
        sw.set_vexpand(True)
        box.pack_start(sw, True, True, 0)
        self.store = Gtk.ListStore(str, str, str)
        self.tree = Gtk.TreeView(model=self.store)
        sw.add(self.tree)
        for i, (title, w) in enumerate((("SSID", 360), ("信号", 110), ("加密", 130))):
            r = Gtk.CellRendererText()
            if i == 1:
                r.set_property("xalign", 0.5)
            c = Gtk.TreeViewColumn(title, r, text=i)
            c.set_sizing(Gtk.TreeViewColumnSizing.FIXED)
            c.set_fixed_width(w)
            self.tree.append_column(c)
        self.tree.get_selection().connect("changed", self.on_sel)
        self.tree.connect("row-activated", self.on_activate)

        bottom = Gtk.Box(spacing=6)
        box.pack_start(bottom, False, False, 0)
        self.hint = Gtk.Label(xalign=0)
        self.hint.set_ellipsize(Pango.EllipsizeMode.END)
        bottom.pack_start(self.hint, True, True, 0)
        self.btn_connect = Gtk.Button(label="连接")
        self.btn_connect.set_sensitive(False)
        self.btn_connect.connect("clicked", self.on_activate)
        bottom.pack_end(self.btn_connect, False, False, 0)
        return box

    def refresh_status(self):
        st = wifi_status()
        txt = f"WiFi：{st['state']}"
        if st["ssid"]:
            txt += f"    已连接：{st['ssid']}"
        if st["ip"]:
            txt += f"    IP：{st['ip']}"
        if st["ip"]:
            txt += "    默认路由：" + ("wlan0 ✓" if st["route"] else "未走 wlan0 ✗")
        self.status_label.set_text(txt)
        return True

    def on_refresh(self, _btn):
        self.hint.set_text("正在扫描…")
        self.btn_refresh.set_sensitive(False)
        threading.Thread(target=self._scan_thread, daemon=True).start()
        return False

    def _scan_thread(self):
        aps, err = scan_aps()
        GLib.idle_add(self._scan_done, aps, err)

    def _scan_done(self, aps, err):
        self.aps = aps
        self.store.clear()
        for ap in aps:
            self.store.append([ap["ssid"], f"{ap['signal']} dBm", ap["sec"] or "开放"])
        self.btn_refresh.set_sensitive(True)
        self.hint.set_text(f"找到 {len(aps)} 个网络" if not err else f"扫描失败：{err[:80]}")
        return False

    def _selected(self):
        m, it = self.tree.get_selection().get_selected()
        if it is None:
            return None
        name = m[it][0]
        for ap in self.aps:
            if ap["ssid"] == name:
                return ap
        return None

    def on_sel(self, _sel):
        self.btn_connect.set_sensitive(self._selected() is not None)

    def on_activate(self, *_):
        ap = self._selected()
        if not ap:
            return
        if ap["sec"]:
            self._ask_password(ap)
        else:
            self._connect(ap["ssid"], "", False)

    def _ask_password(self, ap):
        d = Gtk.Dialog(title=f"连接到 {ap['ssid']}", transient_for=self, modal=True)
        d.add_buttons("取消", Gtk.ResponseType.CANCEL, "连接", Gtk.ResponseType.OK)
        area = d.get_content_area()
        area.set_spacing(8)
        area.set_border_width(12)
        area.pack_start(Gtk.Label(label=f"请输入「{ap['ssid']}」的密码（{ap['sec']}）：", xalign=0),
                        False, False, 0)
        entry = Gtk.Entry()
        entry.set_visibility(False)
        entry.set_activates_default(True)
        area.pack_start(entry, False, False, 0)
        show = Gtk.CheckButton(label="显示密码")
        show.connect("toggled", lambda b: entry.set_visibility(b.get_active()))
        area.pack_start(show, False, False, 0)
        d.set_default_response(Gtk.ResponseType.OK)
        d.show_all()
        resp = d.run()
        pwd = entry.get_text()
        d.destroy()
        if resp == Gtk.ResponseType.OK:
            self._connect(ap["ssid"], pwd, True)

    def on_hidden(self, _b):
        d = Gtk.Dialog(title="加入隐藏网络", transient_for=self, modal=True)
        d.add_buttons("取消", Gtk.ResponseType.CANCEL, "连接", Gtk.ResponseType.OK)
        area = d.get_content_area()
        area.set_spacing(8)
        area.set_border_width(12)
        area.pack_start(Gtk.Label(label="SSID：", xalign=0), False, False, 0)
        e1 = Gtk.Entry()
        area.pack_start(e1, False, False, 0)
        area.pack_start(Gtk.Label(label="密码（开放网络留空）：", xalign=0), False, False, 0)
        e2 = Gtk.Entry()
        e2.set_visibility(False)
        area.pack_start(e2, False, False, 0)
        d.set_default_response(Gtk.ResponseType.OK)
        d.show_all()
        resp = d.run()
        ssid, pwd = e1.get_text().strip(), e2.get_text()
        d.destroy()
        if resp == Gtk.ResponseType.OK and ssid:
            self._connect(ssid, pwd, bool(pwd))

    def _connect(self, ssid, pwd, secured):
        self.hint.set_text(f"正在连接 {ssid} …")
        self.btn_connect.set_sensitive(False)
        threading.Thread(target=self._connect_thread, args=(ssid, pwd, secured), daemon=True).start()

    def _connect_thread(self, ssid, pwd, secured):
        if not wpa_ready():
            GLib.idle_add(self.hint.set_text, "wpa_supplicant 未就绪：请先点「重启 WiFi 服务」")
            GLib.idle_add(self.btn_connect.set_sensitive, True)
            return
        safe = ssid.replace("\\", "\\\\").replace('"', '\\"')
        rc, out, _ = wpa("add_network")
        nid = out.strip().splitlines()[-1].strip() if out.strip() else ""
        if not nid.isdigit():
            GLib.idle_add(self.hint.set_text, f"add_network 失败：{out.strip()[:80]}")
            GLib.idle_add(self.btn_connect.set_sensitive, True)
            return
        wpa(f'set_network {nid} ssid \'"{safe}"\'')
        if secured:
            wpa(f'set_network {nid} psk \'"{pwd}"\'')
        else:
            wpa(f"set_network {nid} key_mgmt NONE")
        wpa(f"enable_network {nid}")
        wpa(f"select_network {nid}")
        wpa("save_config")

        state = ""
        for _ in range(20):
            st = wifi_status()
            state = st["state"]
            if state == "COMPLETED":
                break
            time.sleep(1)
        if state != "COMPLETED":
            GLib.idle_add(self.hint.set_text, f"连接未完成（{state}），请检查密码或信号")
            GLib.idle_add(self.btn_connect.set_sensitive, True)
            return

        GLib.idle_add(self.hint.set_text, "已关联，正在获取 IP…")
        for attempt in range(2):
            sh(f"dhclient -1 {IFACE}", timeout=60)
            st = wifi_status()
            if st["ip"]:
                break
            time.sleep(2)
        st = wifi_status()
        if not st["ip"]:
            GLib.idle_add(self.hint.set_text, "已关联但未获得 IPv4：路由器的 DHCP 可能拒绝旧租约，点「连接」重试一次")
        else:
            ensure_wifi_route()
            st = wifi_status()
            msg = f"已连接 {ssid}，IP {st['ip']}"
            if not st["route"]:
                msg += "（默认路由未就绪）"
            GLib.idle_add(self.hint.set_text, msg)
        GLib.idle_add(self.refresh_status)
        GLib.idle_add(self.btn_connect.set_sensitive, True)

    def on_disconnect(self, _b):
        wpa("disconnect")
        sh(f"dhclient -r {IFACE}", timeout=10)
        sh("ip route del default dev wlan0 2>/dev/null", timeout=5)
        self.refresh_status()
        self.hint.set_text("已断开")

    def on_restart_service(self, _b):
        self.hint.set_text("正在重启 tb335-wifi.service …")
        self.btn_service.set_sensitive(False)

        def work():
            rc, out, err = sh("systemctl restart tb335-wifi.service", timeout=180)
            GLib.idle_add(self.hint.set_text, "服务已重启" if rc == 0 else f"重启失败：{(err or out)[:80]}")
            GLib.idle_add(self.btn_service.set_sensitive, True)
            GLib.idle_add(self.on_refresh, None)
            GLib.idle_add(self.refresh_status)

        threading.Thread(target=work, daemon=True).start()

    # ---------- 蓝牙页 ----------
    def _build_bt_tab(self):
        box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=8)
        box.set_border_width(8)

        self.bt_status = Gtk.Label(xalign=0)
        self.bt_status.set_ellipsize(Pango.EllipsizeMode.END)
        box.pack_start(self.bt_status, False, False, 0)

        bar = Gtk.Box(spacing=6)
        box.pack_start(bar, False, False, 0)
        self.btn_bt_scan = Gtk.Button(label="扫描蓝牙")
        self.btn_bt_scan.connect("clicked", self.on_bt_scan)
        bar.pack_start(self.btn_bt_scan, False, False, 0)
        self.btn_bt_pair = Gtk.Button(label="配对/连接")
        self.btn_bt_pair.set_sensitive(False)
        self.btn_bt_pair.connect("clicked", self.on_bt_pair)
        bar.pack_start(self.btn_bt_pair, False, False, 0)
        self.btn_bt_disc = Gtk.Button(label="断开")
        self.btn_bt_disc.set_sensitive(False)
        self.btn_bt_disc.connect("clicked", self.on_bt_disconnect)
        bar.pack_start(self.btn_bt_disc, False, False, 0)
        self.btn_bt_remove = Gtk.Button(label="移除")
        self.btn_bt_remove.set_sensitive(False)
        self.btn_bt_remove.connect("clicked", self.on_bt_remove)
        bar.pack_start(self.btn_bt_remove, False, False, 0)
        self.btn_bt_service = Gtk.Button(label="重启蓝牙服务")
        self.btn_bt_service.connect("clicked", self.on_bt_restart)
        bar.pack_end(self.btn_bt_service, False, False, 0)

        sw = Gtk.ScrolledWindow()
        sw.set_vexpand(True)
        box.pack_start(sw, True, True, 0)
        self.bt_store = Gtk.ListStore(str, str, str)
        self.bt_tree = Gtk.TreeView(model=self.bt_store)
        sw.add(self.bt_tree)
        for i, (title, w) in enumerate((("名称", 380), ("地址", 220), ("状态", 160))):
            r = Gtk.CellRendererText()
            c = Gtk.TreeViewColumn(title, r, text=i)
            c.set_sizing(Gtk.TreeViewColumnSizing.FIXED)
            c.set_fixed_width(w)
            self.bt_tree.append_column(c)
        self.bt_tree.get_selection().connect("changed", self.on_bt_sel)
        self.bt_tree.connect("row-activated", self.on_bt_pair)

        self.bt_hint = Gtk.Label(xalign=0)
        self.bt_hint.set_ellipsize(Pango.EllipsizeMode.END)
        box.pack_start(self.bt_hint, False, False, 0)
        return box

    def _bt_first(self):
        self._bt_refresh(None)
        return False

    def _bt_timer(self):
        self._bt_refresh(None)
        return True

    def _bt_once(self):
        self._bt_refresh(None)
        return False

    def _bt_auto_scan(self):
        if (self.notebook.get_current_page() == 1 and not self._bt_busy
                and time.time() - self._last_bt_scan > 40):
            self.on_bt_scan(None)
        return True

    def _bt_refresh(self, *_):
        if self._bt_busy:
            return True
        self._bt_busy = True
        threading.Thread(target=self._bt_worker, daemon=True).start()
        return True

    def _bt_worker(self):
        try:
            adapter = bt_show()
            devs = bt_devices() if adapter["ok"] else []
            log(f"bt refresh ok={adapter['ok']} power={adapter['powered']} devs={len(devs)}")
        except Exception as e:  # noqa: BLE001
            log(f"bt worker error: {e}")
            adapter = {"ok": False, "addr": "", "powered": False}
            devs = []
        GLib.idle_add(self._bt_done, adapter, devs)

    def _bt_done(self, adapter, devs):
        self._bt_busy = False
        if not adapter["ok"]:
            self.bt_status.set_text("蓝牙：hci0 未就绪（可点「重启蓝牙服务」）")
        else:
            self.bt_status.set_text(
                f"蓝牙：{adapter['addr']}    " + ("已上电" if adapter["powered"] else "未上电（点「重启蓝牙服务」）"))
        cur = self._bt_selected()
        self.bt_devs = devs
        self.bt_store.clear()
        for d in devs:
            if d["connected"]:
                s = "已连接"
            elif d["paired"]:
                s = "已配对"
            else:
                s = "未配对"
            self.bt_store.append([d["name"], d["mac"], s])
        if cur:
            for i, d in enumerate(devs):
                if d["mac"] == cur:
                    self.bt_tree.get_selection().select_path(Gtk.TreePath.new_from_indicator(i, -1))
                    break
        if not self.bt_hint.get_text():
            self.bt_hint.set_text(f"共 {len(devs)} 个已知设备" if devs else "点「扫描蓝牙」查找设备")
        return False

    def _bt_selected(self):
        m, it = self.bt_tree.get_selection().get_selected()
        if it is None:
            return None
        return m[it][1]

    def _bt_selected_dev(self):
        mac = self._bt_selected()
        for d in self.bt_devs:
            if d["mac"] == mac:
                return d
        return None

    def on_bt_sel(self, _sel):
        mac = self._bt_selected()
        self.btn_bt_pair.set_sensitive(bool(mac))
        self.btn_bt_remove.set_sensitive(bool(mac))
        self.btn_bt_disc.set_sensitive(bool(mac))

    def on_bt_scan(self, _b):
        log("bt scan clicked")
        self.bt_hint.set_text("正在扫描蓝牙（约 12 秒）…")
        self.btn_bt_scan.set_sensitive(False)
        self._last_bt_scan = time.time()

        def work():
            rc, out, err = sh("bluetoothctl --timeout 12 scan on", timeout=40)
            log(f"bt scan done rc={rc} new={out.count('NEW')} err={err.strip()[:60]}")
            GLib.idle_add(self.btn_bt_scan.set_sensitive, True)
            GLib.idle_add(self._bt_once)

        threading.Thread(target=work, daemon=True).start()

    def on_page_switch(self, _nb, _page, idx):
        # 未配对设备会被 BlueZ 很快清掉，进入蓝牙页时自动扫一次
        log(f"page switch idx={idx} busy={self._bt_busy} last={self._last_bt_scan}")
        if idx == 1 and not self._bt_busy and time.time() - self._last_bt_scan > 30:
            self.on_bt_scan(None)

    def _bt_action(self, mac, label, cmds):
        self.bt_hint.set_text(f"正在{label}…")

        def work():
            out = ""
            ok = True
            for c in cmds:
                rc, o, e = sh(c, timeout=40)
                out += o + e
                if rc != 0:
                    ok = False
            tail = " ".join(out.split())[-90:]
            GLib.idle_add(self.bt_hint.set_text, f"{label}{'成功' if ok else '失败'}：{tail}")
            GLib.idle_add(self._bt_once)

        threading.Thread(target=work, daemon=True).start()

    def on_bt_pair(self, *_):
        mac = self._bt_selected()
        if not mac:
            return
        dev = self._bt_selected_dev()
        if dev and dev["paired"] and not dev["connected"]:
            self._bt_action(mac, "连接", [f"bluetoothctl connect {mac}"])
            return
        self._bt_action(mac, "配对", [
            f"bluetoothctl pair {mac}",
            f"bluetoothctl trust {mac}",
            f"bluetoothctl connect {mac}",
        ])

    def on_bt_disconnect(self, _b):
        mac = self._bt_selected()
        if mac:
            self._bt_action(mac, "断开", [f"bluetoothctl disconnect {mac}"])

    def on_bt_remove(self, _b):
        mac = self._bt_selected()
        if mac:
            self._bt_action(mac, "移除", [f"bluetoothctl remove {mac}"])

    def on_bt_restart(self, _b):
        self.bt_hint.set_text("正在重启 tb335-bt.service …")
        self.btn_bt_service.set_sensitive(False)

        def work():
            rc, out, err = sh("systemctl restart tb335-bt.service", timeout=120)
            GLib.idle_add(self.bt_hint.set_text, "蓝牙服务已重启" if rc == 0 else f"重启失败：{(err or out)[:80]}")
            GLib.idle_add(self.btn_bt_service.set_sensitive, True)
            GLib.idle_add(self._bt_once)

        threading.Thread(target=work, daemon=True).start()


def main():
    if sh("ip link show wlan0")[0] != 0:
        d = Gtk.MessageDialog(message_type=Gtk.MessageType.ERROR, buttons=Gtk.ButtonsType.CLOSE,
                              text="找不到 wlan0")
        d.format_secondary_text("WiFi 服务未就绪。请执行：systemctl restart tb335-wifi.service")
        d.run()
        d.destroy()
        return
    NetWindow()
    Gtk.main()


if __name__ == "__main__":
    main()
