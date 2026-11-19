#!/bin/sh
# GPU /dev/mali0 node: kernel usually has no mali0 misc device (kbase not loaded by design).
# 2026-11-18 v2: if /sys/class/misc/mali0 does not exist at all, exit immediately (no wait).
if [ ! -d /sys/class/misc/mali0 ]; then exit 0; fi
echo "tb335-mali0-node: /sys/class/misc/mali0 present, waiting for dev attr" >> /persist/a9b/modload.log
for i in 1 2 3 4 5 6; do
    [ -e /sys/class/misc/mali0/dev ] && break
    sleep 0.5
done
D=$(cat /sys/class/misc/mali0/dev 2>/dev/null)
[ -n "$D" ] || exit 0
MAJ=$(echo "$D" | cut -d: -f1)
MIN=$(echo "$D" | cut -d: -f2)
[ -e /dev/mali0 ] || mknod /dev/mali0 c "$MAJ" "$MIN" 2>/dev/null
chmod 666 /dev/mali0 2>/dev/null
echo "tb335-mali0-node: /dev/mali0 ($D)" >> /persist/a9b/modload.log
