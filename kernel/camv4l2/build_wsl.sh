#!/bin/sh
set -e
BASE="/mnt/e/WorkBuddy工作空间/2026-10-12-19-41-01/kmod/camv4l2"
DST=/home/qiji_/kmod_camv4l
mkdir -p "$DST"
cp "$BASE/camv4l2.c" "$DST/camv4l2.c"
printf 'obj-m := camv4l2.o\nKDIR ?= /home/qiji_/kernel_work/kb_out_cfi\nall:\n\t$(MAKE) -C $(KDIR) M=$(CURDIR) ARCH=arm64 LLVM=1 modules\nclean:\n\t$(MAKE) -C $(KDIR) M=$(CURDIR) ARCH=arm64 LLVM=1 clean\n' > "$DST/Makefile"
cd "$DST"
rm -f camv4l2.ko camv4l2.o
make 2>&1 | tail -40
echo BUILD_RC=$?
ls -l camv4l2.ko
md5sum camv4l2.ko
modinfo camv4l2.ko | head -6
cp -f camv4l2.ko "$BASE/camv4l2.ko"
echo COPIED