#!/bin/bash
# 构建 camreg.ko，并按板子实际内核修正 vermagic / 符号 CRC
set -e
W='/mnt/e/WorkBuddy工作空间/2026-10-12-19-41-01/临时'
KDIR=/home/qiji_/kernel_work/kernel-5.15
OBD=/home/qiji_/kb_out
MDIR=/home/qiji_/camreg
SCM=$KDIR/.scmversion
REL='5.15.185-android13-8-00044-g051a97cf151a-ab14024729'

mkdir -p "$MDIR"
cp -f "$W/camreg/camreg.c" "$W/camreg/Makefile" "$MDIR/"

restore() {
    [ -f "$SCM.orig" ] && mv -f "$SCM.orig" "$SCM"
    [ -f "$OBD/Module.symvers.orig" ] && mv -f "$OBD/Module.symvers.orig" "$OBD/Module.symvers"
    printf '#define UTS_RELEASE "%s"\n' "$REL" > "$OBD/include/generated/utsrelease.h"
    printf '%s' "$REL" > "$OBD/include/config/kernel.release"
}
trap restore EXIT

[ -f "$SCM" ] && cp -f "$SCM" "$SCM.orig"
printf '%s' '-android13-8-00044-g051a97cf151a' > "$SCM"
printf '#define UTS_RELEASE "%s"\n' "$REL" > "$OBD/include/generated/utsrelease.h"
printf '%s' "$REL" > "$OBD/include/config/kernel.release"

python3 - "$W/crc_map.txt" <<'PY'
import shutil, sys
crcmap = {}
for line in open(sys.argv[1]):
    f = line.split()
    if len(f) >= 2:
        crcmap[f[1]] = f[0]
p = '/home/qiji_/kb_out/Module.symvers'
shutil.copy2(p, p + '.orig')
out, hit = [], set()
for line in open(p):
    f = line.rstrip('\n').split('\t')
    if len(f) >= 2 and f[1] in crcmap:
        f[0] = crcmap[f[1]]
        hit.add(f[1])
        line = '\t'.join(f) + '\n'
    out.append(line)
open(p, 'w').writelines(out)
print('patched %d symbols' % len(hit))
PY

cd "$MDIR"
export BUILD_NUMBER=14024729
ARGS="ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- LLVM=1"
make -C "$KDIR" O="$OBD" M="$MDIR" $ARGS clean >/dev/null 2>&1 || true
make -C "$KDIR" O="$OBD" M="$MDIR" $ARGS modules 2>&1 | tail -25

echo "=== vermagic ==="
strings "$MDIR/camreg.ko" | grep -m1 'vermagic='
