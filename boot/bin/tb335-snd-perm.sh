#!/bin/sh
[ -d /dev/snd ] || exit 0
chmod 0755 /dev/snd 2>/dev/null
[ -d /dev/snd/by-path ] && chmod 0755 /dev/snd/by-path 2>/dev/null
find /dev/snd -maxdepth 1 -type c ! -name 'pcmC0D26*' -exec chgrp audio {} \; -exec chmod 0660 {} \; 2>/dev/null
exit 0
