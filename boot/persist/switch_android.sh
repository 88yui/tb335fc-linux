#!/bin/sh
# Debian → root Android。三件套：boot_a / init_boot_a / misc
# 来源：交接文档/SOP-切安卓再切回Linux-20261101.md §3.1（原文照抄）
set -e
echo "=== 源镜像 md5 ==="
md5sum /persist/boot_a_orig.img /persist/init_boot_stock.img

echo "=== 写前分区 md5 ==="
md5sum /dev/disk/by-partlabel/boot_a /dev/disk/by-partlabel/init_boot_a

echo "=== 写 boot_a（原厂 64MB）==="
dd if=/persist/boot_a_orig.img of=/dev/disk/by-partlabel/boot_a bs=1M conv=fsync

echo "=== 写 init_boot_a（8MB）==="
dd if=/persist/init_boot_stock.img of=/dev/disk/by-partlabel/init_boot_a bs=1M conv=fsync

echo "=== 清 misc 前 32 字节（BCB）==="
dd if=/dev/zero of=/dev/disk/by-partlabel/misc bs=1 count=32 conv=fsync

echo "=== 回读校验 ==="
md5sum /dev/disk/by-partlabel/boot_a /dev/disk/by-partlabel/init_boot_a
sync; sync
echo "DONE（确认回读 md5 后再 reboot）"
