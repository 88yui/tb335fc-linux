#!/bin/bash
# =============================================================================
# tb335-drm-rollback.sh -- 显示驱动（mediatek-drm）重编试验的自愈回滚守卫
#
# 由 tb335-drm-rollback.service 在 tb335-drm-compat / tb335-modules 之前调用。
#
# 状态机（/persist/a9b/.drm-trial-state）：
#   NONE / REVERTED -> 未启用
#   ARMED           -> 已安装重编模块，下一次启动使用它（放行一次）
#   BOOTING         -> 本次启动正在用重编模块
#   OK              -> 由 trialok 确认"显示真的起来了"
#   ROLLEDBACK      -> 已自动还原
#
# 判据：看到 BOOTING 说明上一次带重编模块的启动没走到 trial-ok
#       ⇒ 还原原版并移除兼容垫片。
# =============================================================================
set -eu

KREL="$(uname -r)"
MODDIR="/lib/modules/${KREL}"
ORIGDIR="/persist/a9b/backup/drm-bringup-orig"
STATE_FILE="/persist/a9b/.drm-trial-state"
LOG=/persist/a9b/drm-patch.log

log() { echo "$(date '+%F %T') drm-rollback: $*" >>"$LOG" 2>/dev/null || true; }

STATE="$(cat "${STATE_FILE}" 2>/dev/null || echo NONE)"
log "entered, state=${STATE}, uptime=$(cut -d' ' -f1 /proc/uptime)"

case "${STATE}" in
ARMED)
	echo BOOTING >"${STATE_FILE}"
	sync
	log "ARMED -> BOOTING (first boot with rebuilt mediatek-drm, proceed)"
	;;
BOOTING)
	log "BOOTING detected -> previous boot did NOT reach trial-ok, RESTORING originals"
	if [ -f "${ORIGDIR}/mediatek-drm.ko" ]; then
		install -m 0644 "${ORIGDIR}/mediatek-drm.ko" "${MODDIR}/mediatek-drm.ko" &&
			log "restored mediatek-drm.ko"
	fi
	rm -f "${MODDIR}/tb335_drm_compat.ko" && log "removed tb335_drm_compat.ko"
	sync
	echo ROLLEDBACK >"${STATE_FILE}"
	log "BOOTING -> ROLLEDBACK"
	;;
*)
	log "state=${STATE}, nothing to do"
	;;
esac
exit 0
