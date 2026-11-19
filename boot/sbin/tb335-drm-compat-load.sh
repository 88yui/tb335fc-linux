#!/bin/bash
# =============================================================================
# tb335-drm-compat-load.sh -- 在显示驱动加载前插入"兼容垫片模块"
#
# 重编的 mediatek-drm.ko 引用了 9 个设备上不存在的符号（见 tb335_drm_compat.c）。
# 本脚本在 tb335-modules.service 之前 insmod 垫片，使那些符号可用。
#
# 幂等：已加载 / 文件不存在 / 状态不是 BOOTING 时都直接退出。
# =============================================================================
set -u

KREL="$(uname -r)"
MODDIR="/lib/modules/${KREL}"
KO="${MODDIR}/tb335_drm_compat.ko"
STATE_FILE="/persist/a9b/.drm-trial-state"
LOG=/persist/a9b/drm-patch.log

log() { echo "$(date '+%F %T') drm-compat: $*" >>"$LOG" 2>/dev/null || true; }

if [ ! -f "${KO}" ]; then
	log "no ${KO}, skip"
	exit 0
fi

if [ -d /sys/module/tb335_drm_compat ]; then
	log "already loaded, skip"
	exit 0
fi

if insmod "${KO}" 2>>"$LOG"; then
	log "insmod ok: $(basename "${KO}")"
else
	log "!! insmod FAILED (see above)"
fi
exit 0
