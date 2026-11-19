#!/bin/bash
# TB335: 若 /persist 恢复流程把「原版 A9b initramfs（无主机时 ttyGS0 写阻塞）」写回 init_boot_a，
# 则在 Debian 启动时自动重新刷入 v4 修复镜像，避免冷启动必须插数据线的问题复发。
EXP_V4=bc8e7e9995caedd3040d3c8e96f0896f
BAD_ORIG=9ef20ef76dc8deb877247e6a2bf6dd83
DEV=/dev/sdc31
SRC=/persist/ib_current.img
LOG=/persist/initboot_guard.log

[ -b "$DEV" ] || exit 0
if [ ! -f "$SRC" ]; then
    grep -qE ' /persist ' /proc/mounts || mount /dev/sdc69 /persist 2>/dev/null
fi
if [ ! -f "$SRC" ]; then
    echo "$(date '+%F %T') src $SRC missing, skip" >> $LOG 2>/dev/null
    exit 0
fi
S=$(md5sum "$SRC" 2>/dev/null | awk '{print $1}')
if [ "$S" != "$EXP_V4" ]; then
    echo "$(date '+%F %T') src md5=$S not v4, skip" >> $LOG 2>/dev/null
    exit 0
fi
C=$(dd if="$DEV" bs=1M count=8 2>/dev/null | md5sum | awk '{print $1}')
if [ "$C" = "$BAD_ORIG" ]; then
    echo "$(date '+%F %T') REPAIR init_boot_a orig($C) -> v4" >> $LOG 2>/dev/null
    dd if="$SRC" of="$DEV" bs=1M conv=fsync 2>/dev/null
    sync
    N=$(dd if="$DEV" bs=1M count=8 2>/dev/null | md5sum | awk '{print $1}')
    echo "$(date '+%F %T') after=$N" >> $LOG 2>/dev/null
fi
exit 0