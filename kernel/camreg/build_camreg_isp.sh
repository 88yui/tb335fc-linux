#!/bin/bash
# 用当前内核（5.15.185-android13-8-g365af54093bd）的构建目录编译 camreg.ko
set -e
W='/mnt/e/WorkBuddy工作空间/2026-10-12-19-41-01/临时'
KDIR=/home/qiji_/kernel_work/kernel-5.15
OBD=/home/qiji_/kb_out_vt
MDIR=/home/qiji_/camreg_isp

echo '=== 签名/校验相关配置 ==='
grep -E '^CONFIG_MODULE_SIG|^CONFIG_MODULE_FORCE_LOAD|^CONFIG_MODVERSIONS|^CONFIG_MODULE_UNLOAD' "$OBD/.config" || true

mkdir -p "$MDIR"
cp -f "$W/camreg/camreg_isp.c" "$MDIR/"
printf 'obj-m += camreg_isp.o\n' > "$MDIR/Makefile"

cd "$MDIR"
export BUILD_NUMBER=14024729
ARGS="ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- LLVM=1 LLVM_IAS=1"
make -C "$KDIR" O="$OBD" M="$MDIR" $ARGS clean >/dev/null 2>&1 || true
make -C "$KDIR" O="$OBD" M="$MDIR" $ARGS modules 2>&1 | tail -20

echo '=== 产物 ==='
ls -l "$MDIR/camreg.ko"
strings "$MDIR/camreg.ko" | grep -E '^vermagic='
cp -f "$MDIR/camreg.ko" "$W/camreg/camreg_isp.ko"
echo '=== 已复制到 临时/camreg/camreg_isp.ko ==='
