#!/bin/sh
# Debian -> root Android 三件套（直接块设备节点版）
# 背景：/dev/disk/by-partlabel/ 在本机 udev 缺失时不存在，
#       故改用固定节点编号（与 restore_debian_manual.sh 的 readlink 结果一致）：
#         boot_a=/dev/sdc29 init_boot_a=/dev/sdc31 misc=/dev/sdc1
set -e

BOOT=/dev/sdc29
IBOOT=/dev/sdc31
MISC=/dev/sdc1

echo "=== 0. 设备节点确认 ==="
ls -l $BOOT $IBOOT $MISC

echo "=== 1. 源镜像 md5 ==="
md5sum /persist/boot_a_orig.img /persist/init_boot_stock.img

echo "=== 2. 写前分区 md5 ==="
md5sum $BOOT $IBOOT

echo "=== 3. 写 boot_a (64MB) ==="
dd if=/persist/boot_a_orig.img of=$BOOT bs=1M conv=fsync

echo "=== 4. 写 init_boot_a (8MB) ==="
dd if=/persist/init_boot_stock.img of=$IBOOT bs=1M conv=fsync

echo "=== 5. 清 misc 前 32 字节 (BCB) ==="
dd if=/dev/zero of=$MISC bs=1 count=32 conv=fsync

echo "=== 6. 回读校验 ==="
md5sum $BOOT $IBOOT
sync; sync
echo "DONE 期望: 39eb843a7fb56832869a972e346700d6 / 4e93e047475d6d0473ba3a5077d409cf"
