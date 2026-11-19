#!/bin/sh
L=/persist/a9b/shut.log
echo "$(date '+%F %T') uptime=$(cut -d' ' -f1 /proc/uptime) $*" >> $L
sync
