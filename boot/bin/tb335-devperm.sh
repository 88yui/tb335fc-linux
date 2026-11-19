#!/bin/sh
for i in 1 2 3 4 5 6 7 8 9 10; do
    [ -e /dev/rfkill ] && [ -d /dev/snd ] && break
    sleep 1
done
[ -e /dev/rfkill ] && chmod 0666 /dev/rfkill 2>/dev/null
/usr/local/bin/tb335-snd-perm.sh
exit 0
