#!/bin/bash
# =============================================================================
# tb335-drm-trialok.sh -- 判定"带重编 mediatek-drm 的这次启动是否真的成功"
#
# 由 tb335-drm-trialok.service 在 tb335-modules.service 完成并等待稳定期后调用。
#
# 与 GPU 版 trialok 的关键区别：
#   GPU 的坏模块会直接把内核搞崩，所以"能走到这里"本身即成功。
#   显示驱动加载失败**不会**崩内核 —— 系统照常启动，只是没有屏幕。
#   因此这里必须**主动验证显示设备真的出现了**，否则视为失败。
#
# 失败时立刻还原原版模块并重启（sysrq，绕开可能卡住的 systemctl reboot）。
# =============================================================================
set -u

KREL="$(uname -r)"
MODDIR="/lib/modules/${KREL}"
ORIGDIR="/persist/a9b/backup/drm-bringup-orig"
STATE_FILE="/persist/a9b/.drm-trial-state"
LOG=/persist/a9b/drm-patch.log

log() { echo "$(date '+%F %T') drm-trial: $*" >>"$LOG" 2>/dev/null || true; }

STATE="$(cat "${STATE_FILE}" 2>/dev/null || echo NONE)"
log "entered, state=${STATE}, uptime=$(cut -d' ' -f1 /proc/uptime)"

if [ "${STATE}" != "BOOTING" ]; then
	log "not BOOTING; no state change"
	exit 0
fi

OK=1

# 1) 驱动模块是否在位
if [ ! -d /sys/module/mediatek_drm ]; then
	OK=0; log "FAIL: /sys/module/mediatek_drm 不存在"
fi

# 2) DRM 设备节点是否出现
if [ ! -e /sys/class/drm/card0 ]; then
	OK=0; log "FAIL: /sys/class/drm/card0 不存在"
fi

# 3) 是否有符号解析失败
if dmesg 2>/dev/null | grep -qiE 'mediatek.?drm.*(Unknown symbol|disagrees about version)'; then
	OK=0; log "FAIL: dmesg 里有 Unknown symbol / version 冲突"
fi

# 4) 面板链路是否起来了（DSI connector）
if [ ! -e /sys/class/drm/card0-DSI-1 ]; then
	OK=0; log "FAIL: card0-DSI-1 不存在"
fi

if [ "${OK}" -eq 1 ]; then
	echo OK >"${STATE_FILE}"
	sync
	log "BOOTING -> OK (重编 mediatek-drm 启动成功)"
	exit 0
fi

log "RESTORING originals and rebooting"
if [ -f "${ORIGDIR}/mediatek-drm.ko" ]; then
	install -m 0644 "${ORIGDIR}/mediatek-drm.ko" "${MODDIR}/mediatek-drm.ko" &&
		log "restored mediatek-drm.ko"
else
	log "WARN: 缺少备份 ${ORIGDIR}/mediatek-drm.ko"
fi
rm -f "${MODDIR}/tb335_drm_compat.ko" && log "removed tb335_drm_compat.ko"
echo ROLLEDBACK >"${STATE_FILE}"
sync
sleep 1
# 用 sysrq 立即重启：systemctl reboot 在本机可能卡在关机序列
echo b > /proc/sysrq-trigger
