#!/system/bin/sh
# Android -> Debian: 写三件套; init_boot 强制使用 v4 修复版
SRC=/data/local/tmp
BOOT=$SRC/deb_boot.img
IB=$SRC/deb_ib_v4.img
MISC=$SRC/deb_misc.img
echo "=== source md5 ==="
md5sum "$BOOT" "$IB" "$MISC"
IBH=$(md5sum "$IB" | awk '{print $1}')
if [ "$IBH" != "bc8e7e9995caedd3040d3c8e96f0896f" ]; then
    echo "ABORT: $IB md5=$IBH != v4(bc8e7e99...), refuse"
    exit 1
fi
echo "=== before ==="
md5sum /dev/block/by-name/boot_a /dev/block/by-name/init_boot_a /dev/block/by-name/misc
dd if="$BOOT" of=/dev/block/by-name/boot_a bs=1M conv=fsync
dd if="$IB"   of=/dev/block/by-name/init_boot_a bs=1M conv=fsync
dd if="$MISC" of=/dev/block/by-name/misc bs=1M conv=fsync
sync
echo "=== verify ==="
md5sum /dev/block/by-name/boot_a /dev/block/by-name/init_boot_a /dev/block/by-name/misc
echo DONE
