#!/bin/sh
# 本机 /dev 是 tmpfs 且内核不支持 ACL，udev 会把 /dev/snd 目录设成 0660（丢 x 位），
# 导致 desk 身份的 PipeWire/WirePlumber 打不开 ALSA mixer，声卡被丢弃。
# 这里持续把目录修回 0755，节点保持 0660 root:audio。
# pcmC0D26 (Hostless_Spk_Init) 例外：VAUD18 未上电时 open 会挂死 AXI -> 看门狗硬复位，
# 所以每轮强制 000，任何用户（包括 audio 组成员 desk）都打不开。
while :; do
    chmod 000 /dev/snd/pcmC0D26p /dev/snd/pcmC0D26c 2>/dev/null
    if [ -d /dev/snd ]; then
        m=$(stat -c %a /dev/snd 2>/dev/null)
        if [ "$m" != "755" ]; then
            chmod 0755 /dev/snd 2>/dev/null
            find /dev/snd -maxdepth 1 -type c ! -name 'pcmC0D26*' -exec chgrp audio {} \; -exec chmod 0660 {} \; 2>/dev/null
        fi
    fi
    if [ -d /dev/snd/by-path ]; then
        m=$(stat -c %a /dev/snd/by-path 2>/dev/null)
        [ "$m" = "755" ] || chmod 0755 /dev/snd/by-path 2>/dev/null
    fi
    sleep 0.05
done
