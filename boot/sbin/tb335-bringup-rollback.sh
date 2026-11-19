#!/bin/bash
# =============================================================================
# tb335-bringup-rollback.sh -- 自愈回滚守卫（状态机版）
#
# 由 tb335-bringup-rollback.service 在 tb335-modules.service 之前调用。
#
# 状态机（状态文件 /persist/a9b/.bringup-trial-state）：
#   NONE       -> 未启用补丁
#   ARMED      -> 补丁已安装，下一次启动将使用补丁模块（放行，不还原）
#   BOOTING    -> 本次启动已在使用补丁模块（守卫已放行过）
#   OK         -> 带补丁的启动已完整走完（由 trialok 单元写入）
#   ROLLEDBACK -> 已因上次启动不完整而自动还原
#   REVERTED   -> 人工回滚
#
# 逻辑：看到 BOOTING 就说明"上一次带补丁的启动没有走到 trial-ok" ⇒ 自动还原原始模块。
#       看到 ARMED 只放行一次并转为 BOOTING，因此不会误判首次启动。
# =============================================================================
set -eu

KREL="$(uname -r)"
MODDIR="/usr/lib/modules/${KREL}"
ORIGDIR="/persist/a9b/backup/gpufreq-bringup-orig"
STATE_FILE="/persist/a9b/.bringup-trial-state"
LOG=/persist/a9b/bringup-patch.log

log() { echo "$(date '+%F %T') rollback-guard: $*" >>"$LOG" 2>/dev/null || true; }

# ---- BOOT_ONCE_LOCK: 同一次启动只允许执行一次 ----
# 实测出现过同一启动内本脚本被执行两次，第二次看到 BOOTING 就误判为
# "上次带补丁的启动没走完" 并提前还原（模块还没加载就被换回原厂）。
BOOT_ID="$(cat /proc/sys/kernel/random/boot_id 2>/dev/null)"
ONCE_FILE=/persist/a9b/.bringup-guard-bootid
if [ -n "${BOOT_ID}" ] && [ "$(cat "${ONCE_FILE}" 2>/dev/null)" = "${BOOT_ID}" ]; then
	log "BOOT_ONCE_LOCK: already ran for boot ${BOOT_ID}, skip"
	exit 0
fi
[ -n "${BOOT_ID}" ] && echo "${BOOT_ID}" > "${ONCE_FILE}"

STATE="$(cat "${STATE_FILE}" 2>/dev/null || echo NONE)"
log "entered, state=${STATE}, uptime=$(cut -d' ' -f1 /proc/uptime)"

case "${STATE}" in
ARMED)
	# 首次带补丁启动：放行，标记为"正在启动"
	echo BOOTING >"${STATE_FILE}"
	sync
	log "ARMED -> BOOTING (first patched boot, proceed)"
	;;
BOOTING)
	# 上一次带补丁的启动没有走完 ⇒ 自动还原
	log "BOOTING detected -> previous patched boot did NOT complete, RESTORING originals"
	for m in mtk_gpufreq_mt6835 mtk_gpufreq_wrapper; do
		if [ -f "${ORIGDIR}/${m}.ko" ]; then
			install -m 0644 "${ORIGDIR}/${m}.ko" "${MODDIR}/${m}.ko" &&
				log "restored ${m}.ko"
		else
			log "WARN missing backup ${ORIGDIR}/${m}.ko"
		fi
	done
	sync
	echo ROLLEDBACK >"${STATE_FILE}"
	log "BOOTING -> ROLLEDBACK"
	;;
*)
	log "state=${STATE}, nothing to do"
	;;
esac
exit 0
