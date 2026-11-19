#!/bin/bash
# =============================================================================
# tb335-bringup-trialok.sh -- 带补丁启动的"成功标记"
#
# 由 tb335-bringup-trialok.service 在 tb335-modules.service 完成、并再等一段
# 稳定期（见 .service 的 ExecStartPre=/bin/sleep）之后调用。
# 只有把状态从 BOOTING 推进到 OK，才表示"本次带补丁的启动是成功的"，
# 从而不会再被 rollback-guard 自动回滚。
# =============================================================================
set -eu

STATE_FILE="/persist/a9b/.bringup-trial-state"
LOG=/persist/a9b/bringup-patch.log

log() { echo "$(date '+%F %T') trial-ok: $*" >>"$LOG" 2>/dev/null || true; }

STATE="$(cat "${STATE_FILE}" 2>/dev/null || echo NONE)"

# 记录本次启动的关键结果，便于事后判读
{
	log "uptime=$(cut -d' ' -f1 /proc/uptime)"
	log "devices_deferred(gpu lines): $(grep -cE 'gpufreq|ged|13000000.mali' /sys/kernel/debug/devices_deferred 2>/dev/null || echo '?')"
	log "/dev/mali0: $(ls -l /dev/mali0 2>&1)"
	log "gpufreq driver: $(readlink /sys/bus/platform/devices/13fbf000.gpufreq/driver 2>&1)"
} 2>/dev/null || true

if [ "${STATE}" = "BOOTING" ]; then
	echo OK >"${STATE_FILE}"
	log "BOOTING -> OK (patched boot completed)"
else
	log "state=${STATE}, not BOOTING; no state change"
fi
exit 0
