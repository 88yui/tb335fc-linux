#!/bin/busybox sh
# =====================================================================
# TB335FC 路线 A · stage A9b —— systemd 试验 + 串口救援包装器（救援通道前置版）
#
# ★ 本版核心修正（A7 系列失败的真因）：
#   systemd rootfs 的 /sbin/init 是【绝对软链接】-> /lib/systemd/systemd。
#   `[ -e /newroot/sbin/init ]` 会跟随软链、把目标拿到【当前根(initramfs)】
#   去解析，必然为假 ⇒ 误判"没有 init" ⇒ exec sh -i 掉进无人诊断 shell
#   ⇒ PID1 退出 ⇒ 内核 panic ⇒ 重启/Download Mode。
#   （sysv rootfs 的 /sbin/init 是【普通文件】，所以 A6 一直没事。）
#   ⇒ 一律改用 has_init()：自己解析软链，把绝对目标当 rootfs 内路径。
#
# 设计铁律：步骤 1~8 与 A6 逐字相同（挂载顺序、--install 时机、2>/dev/null
#           写法、三级回退全部照抄）。只在三处做纯追加/替换：
#   (1) 步骤 2 末尾：裸 sdc69 节点 /dev/tr + 极早期心跳（不依赖任何挂载）
#   (2) 步骤 7.5：三级回退里的 init 判定改为 has_init（软链安全）
#   (3) 步骤 8.5：新增"双 rootfs 选择"——/newroot/sdroot=systemd(bind) 优先，
#       /newroot=sysv 兜底；计数器 /boot_attempt 连续 2 次未成功即永久回退。
#       systemd 起来后由 mark-boot-ok.service 清零计数。
#   (4) 步骤 9.4：把「救援包装器」写进新根（$NEWROOT/a9init），switch_root 改为执行它。
#       包装器先架好 ttyGS0 救援 shell + kmsg 落盘 + journald 持久化，最后才 exec systemd
#       ⇒ 即使 systemd 卡死，COM11 上仍留有一个可交互的真 shell。
# =====================================================================
BB=/bin/busybox
export PATH=/bin:/sbin:/usr/bin:/usr/sbin
$BB --install -s /bin  2>/dev/null
$BB --install -s /sbin 2>/dev/null

LOG=/tmp/a9blog
: > "$LOG"
K() { echo "$*" >> "$LOG"; }
O() { echo "$*" >> "$LOG"; ( echo "$*" > /dev/ttyGS0 2>/dev/null ) & }

# ---------- 0. 软链安全的 init 判定 ----------
# 用法: has_init <rootfs 目录>
has_init() {
    d=$1; p="$d/sbin/init"; n=0
    while [ $n -lt 6 ]; do
        [ -L "$p" ] || break
        t=$($BB readlink "$p" 2>/dev/null)
        [ -n "$t" ] || break
        case "$t" in
            /*) p="$d$t" ;;
            *)  p="$d/sbin/$t" ;;
        esac
        n=$((n+1))
    done
    [ -e "$p" ]
}

# ---------- 1. 基础挂载 ----------
$BB mount -t proc  proc  /proc 2>/dev/null
$BB mount -t sysfs sysfs /sys  2>/dev/null
$BB mount -t tmpfs tmpfs /dev  2>/dev/null
$BB mount -t tmpfs tmpfs /tmp  2>/dev/null
$BB mount -t tmpfs tmpfs /run  2>/dev/null

# ---------- 2. 静态设备节点 ----------
$BB mknod -m 666 /dev/null c 1 3 2>&1
for spec in "console 622 5 1" "zero 666 1 5" "full 666 1 7" "tty 666 5 0" "ptmx 666 5 2" "random 444 1 8" "urandom 444 1 9" "kmsg 600 1 11"; do
    set -- $spec
    $BB mknod -m $2 /dev/$1 c $3 $4 2>/dev/null
done
for i in 0 1 2 3; do
    $BB mknod /dev/loop$i b 7 $((i*8)) 2>/dev/null
done
$BB mkdir -p /dev/pts /dev/block /dev/input /dev/shm /dev/mqueue
$BB mount -t devpts devpts /dev/pts 2>/dev/null
# ★ A8 追加：裸 sdc69 节点 + 极早期心跳（偏移 92.16 GB，debugfs 已验为空闲块）
$BB mknod -m 600 /dev/tr b 259 53 2>/dev/null
TT() { echo "A9bT$1 $2" | $BB dd of=/dev/tr bs=512 seek=$((180000000+$1)) conv=notrunc 2>/dev/null; }
TT 1 "step2-nodes-ok"

# ---------- 2.5 A9bM 步进心跳（kmsg 每 5s；/newroot 可写后同时落文件） ----------
STEPF=/tmp/a9bstep
: > "$STEPF"; echo "boot-start" > "$STEPF"
echo "A9bM st boot-start" > /dev/kmsg 2>/dev/null
ST() { echo "$1" > "$STEPF" 2>/dev/null; echo "A9bM st $1" > /dev/kmsg 2>/dev/null; }
(
  while :; do
    s=$($BB cat "$STEPF" 2>/dev/null)
    u=$($BB cut -d' ' -f1 /proc/uptime 2>/dev/null)
    echo "A9bM hb t=$u step=$s" > /dev/kmsg 2>/dev/null
    if [ -f /newroot/.a9bw ]; then echo "A9bM hb t=$u step=$s" >> /newroot/a9bmit.log 2>/dev/null; fi
    $BB sleep 5
  done
) &
HB=$!


# ---------- 3. 加载模块 ----------
ST "step3-modules"
is_loaded() {
    n=${1%.ko}; n=$($BB echo "$n" | $BB tr '-' '_')
    $BB grep -q "^${n}[[:space:]]" /proc/modules 2>/dev/null
}
ins() {
    ST "mod:$1"
    f=/lib/modules/$1
    [ -f "$f" ] || { echo "$1 :: MISS" >> /tmp/failed_round; return 1; }
    if $BB insmod "$f" 2>/tmp/e; then return 0
    else echo "$1 :: $($BB head -c 120 /tmp/e 2>/dev/null | $BB tr '\n' ' ')" >> /tmp/failed_round; return 1; fi
}
: > /tmp/failed_round
: > /tmp/failed_final
PASS=0
while [ $PASS -lt 8 ]; do
    PASS=$((PASS+1)); NEW=0; FAIL=0; SKIP=0
    : > /tmp/failed_round
    while read -r m; do
        [ -n "$m" ] || continue
        is_loaded "$m" && { SKIP=$((SKIP+1)); continue; }
        if ins "$m"; then NEW=$((NEW+1)); else FAIL=$((FAIL+1)); fi
    done < /etc/modorder
    $BB cp /tmp/failed_round /tmp/failed_final 2>/dev/null
    K "modround $PASS: new=$NEW fail=$FAIL skip=$SKIP"
    [ "$NEW" = "0" ] && break
done
$BB mdev -s 2>/dev/null
ST "step3-done"
TT 2 "step3-modules-done"

# ---------- 4. 等 UFS + 建块节点 ----------
W=0
while [ $W -lt 60 ]; do
    $BB ls /sys/class/block 2>/dev/null | $BB grep -q '^sdc' && break
    $BB sleep 1; W=$((W+1))
done
ST "step4-scan-blocks"
for d in /sys/class/block/*; do
    [ -e "$d/dev" ] || continue
    nm=${d##*/}; [ -b "/dev/$nm" ] && continue
    dv=$($BB cat "$d/dev" 2>/dev/null); [ -n "$dv" ] || continue
    $BB mknod "/dev/$nm" b "${dv%%:*}" "${dv##*:}" 2>/dev/null
done
K "UFS block ready (${W}s)"
ST "step4-done-${W}s"
TT 3 "step4-ufs-ready-${W}s"

# ---------- 5. USB gadget：ACM + NCM ----------
ST "step5-gadget"
$BB mkdir -p /sys/kernel/config
$BB mount -t configfs none /sys/kernel/config 2>/dev/null
G=/sys/kernel/config/usb_gadget/tb335fc
$BB mkdir -p $G 2>/dev/null
echo 0x1d6b > $G/idVendor    2>/dev/null
echo 0x0104 > $G/idProduct   2>/dev/null
echo 0x0200 > $G/bcdUSB      2>/dev/null
$BB mkdir -p $G/strings/0x409
echo "TB335FCA2"             > $G/strings/0x409/serialnumber 2>/dev/null
echo "Lenovo"                > $G/strings/0x409/manufacturer 2>/dev/null
echo "TB335FC Halium A9b"    > $G/strings/0x409/product      2>/dev/null
$BB mkdir -p $G/configs/c.1/strings/0x409
echo "ACM+NCM"               > $G/configs/c.1/strings/0x409/configuration 2>/dev/null
echo 250                    > $G/configs/c.1/MaxPower 2>/dev/null
$BB mkdir -p $G/functions/acm.usb0
$BB mkdir -p $G/functions/ncm.usb0
echo "02:00:00:00:00:02" > $G/functions/ncm.usb0/dev_addr  2>/dev/null
echo "02:00:00:00:00:01" > $G/functions/ncm.usb0/host_addr 2>/dev/null
$BB ln -sf $G/functions/acm.usb0 $G/configs/c.1/ 2>/dev/null
$BB ln -sf $G/functions/ncm.usb0 $G/configs/c.1/ 2>/dev/null

UDC=$($BB ls /sys/class/udc 2>/dev/null | $BB head -1)
if [ -n "$UDC" ]; then
    ST "step5-udc-write"
    ( echo "$UDC" > $G/UDC 2>/dev/null ) &
    $BB sleep 4
    SC=/sys/class/udc/$UDC/soft_connect
    ST "step5-softconnect"
    [ -e "$SC" ] && { echo connect > "$SC" 2>/dev/null; $BB sleep 3; }
    ST "step5-softconnect-done"
fi
K "gadget acm+ncm UDC=$UDC state=$($BB cat /sys/class/udc/$UDC/state 2>/dev/null)"
ST "step5-done"
TT 4 "step5-gadget-done"

# ---------- 6. 等 ttyGS0 + usb0 ----------
ST "step6-wait-tty"
i=0
while [ $i -lt 40 ]; do
    [ -c /dev/ttyGS0 ] && [ -e /sys/class/net/usb0 ] && break
    $BB mdev -s 2>/dev/null
    $BB sleep 1; i=$((i+1))
done
K "ttyGS0=$([ -c /dev/ttyGS0 ] && echo ok || echo MISS) usb0=$([ -e /sys/class/net/usb0 ] && echo ok || echo MISS) (${i}s)"
ST "step6-done-${i}s"

# ---------- 7. 网络配置 ----------
ST "step7-net"
if [ -e /sys/class/net/usb0 ]; then
    $BB ifconfig usb0 192.168.55.2 netmask 255.255.255.0 up 2>/dev/null
    K "usb0 = 192.168.55.2"
fi
ST "step7-done"
TT 5 "step7-net-done"

# ---------- 7.5 ★ 持久化 rootfs
ST "step75-mount"（A6 三级回退；判定改软链安全版）----------
$BB mkdir -p /newroot
PERSIST=0
ROOTDEV=""
for PD in /dev/sdc69 /dev/sdc67; do
    [ -b "$PD" ] || { K "$PD not present"; continue; }
    W2=0
    while [ $W2 -lt 20 ]; do
        $BB mount -t ext4 "$PD" /newroot 2>/dev/null
        has_init /newroot && break
        $BB umount /newroot 2>/dev/null
        $BB sleep 1; W2=$((W2+1))
    done
    if has_init /newroot; then
        PERSIST=1; ROOTDEV=$PD
        K "persist rootfs $PD OK (wait ${W2}s)"
        O "A9b: persist rootfs $PD OK"
        break
    else
        $BB umount /newroot 2>/dev/null
        K "$PD no init"
        O "A9b: $PD no init"
    fi
done
[ "$PERSIST" = "1" ] || O "A9b: no persist rootfs -> net fallback"
if [ "$PERSIST" = "1" ] && [ -w /newroot ]; then
    : > /newroot/.a9bw
    echo "A9bM st filelog-on t=$($BB cut -d' ' -f1 /proc/uptime) dev=$ROOTDEV" >> /newroot/a9bmit.log 2>/dev/null
fi
ST "step75-done-persist=$PERSIST"
TT 6 "step75-persist=$PERSIST-dev=$ROOTDEV"

# ---------- 8. 下载 rootfs
ST "step8-download"（仅当无持久化 rootfs 时回退）----------
if [ "$PERSIST" != "1" ]; then
    $BB mkdir -p /newroot_img
    $BB mount -t tmpfs -o size=2G tmpfs /newroot_img 2>/dev/null
    O "A9b: waiting for host http (192.168.55.1:8000) ..."
    DL=0; n=0
    while [ $n -lt 30 ]; do
        n=$((n+1))
        if $BB wget -q http://192.168.55.1:8000/rootfs.img -O /newroot_img/rootfs.img 2>/dev/null; then
            DL=1; break
        fi
        $BB sleep 3
    done
    SZ=$($BB wc -c < /newroot_img/rootfs.img 2>/dev/null)
    K "download=$DL size=$SZ (${n} try)"
    O "A9b: download=$DL size=$SZ (${n} try)"
    if [ "$DL" != "1" ]; then
        K "download FAIL -> fallback shell"
        O "download FAIL -> 诊断 shell"
        exec $BB sh -i < /dev/console > /dev/console 2>&1
    fi
    $BB losetup /dev/loop0 /newroot_img/rootfs.img 2>/dev/null
    $BB mount -t ext4 /dev/loop0 /newroot 2>/dev/null
fi

# ---------- 8.5 ★ systemd / sysv 选择 ----------
ST "step85-select"
NEWROOT=/newroot
MODE=sysv
if [ "$PERSIST" = "1" ] && [ "$ROOTDEV" = "/dev/sdc69" ]; then
    CNT=$($BB cat /newroot/boot_attempt 2>/dev/null)
    [ -n "$CNT" ] || CNT=0
    [ -f /newroot/no_systemd ] && CNT=99
    CNT=$((CNT+1))
    echo "$CNT" > /newroot/boot_attempt 2>/dev/null
    echo "A9b: select cnt=$CNT sdroot_init=$(has_init /newroot/sdroot && echo yes || echo no)" >> /newroot/a9btrace.log 2>/dev/null
    $BB sync 2>/dev/null
    if [ "$CNT" -le 2 ] && has_init /newroot/sdroot; then
        $BB mount --bind /newroot/sdroot /newroot/sdroot 2>/dev/null
        if $BB grep -q "[[:space:]]/newroot/sdroot[[:space:]]" /proc/mounts 2>/dev/null; then
            NEWROOT=/newroot/sdroot
            MODE=systemd
        fi
    fi
    if [ "$MODE" = "systemd" ]; then
        echo "A9b: -> systemd cnt=$CNT NEWROOT=$NEWROOT" >> /newroot/a9btrace.log 2>/dev/null
    else
        [ "$CNT" -gt 2 ] && : > /newroot/no_systemd 2>/dev/null
        echo "A9b: -> sysv cnt=$CNT" >> /newroot/a9btrace.log 2>/dev/null
    fi
    $BB sync 2>/dev/null
fi
K "mode=$MODE NEWROOT=$NEWROOT"
ST "step85-done-mode=$MODE"
TT 7 "step85-mode=$MODE"

# ---------- 9.4 * 把救援包装器写进新根
ST "step94-wrapper"（switch_root 后由它拉起 systemd）----------
TARGET=/sbin/init
if [ "$MODE" = "systemd" ]; then
    W=$NEWROOT/a9init
    $BB cat > $W <<'A9EOF'
#!/bin/sh
# ===== A9b 救援包装器 =====
# 由 initramfs 在 switch_root 之前写入新根；switch_root 之后以「新根」为 / 执行。
# 职责：先铺好诊断/救援通道，最后 exec systemd（systemd 必须是 PID1，故只能 exec）。
# ★ 排序铁律：救援 shell 与 kmsg 落盘必须排在最前 —— 它们之后任何一步卡住都不影响取证。
export PATH=/usr/sbin:/usr/bin:/sbin:/bin
TT=/dev/ttyGS0
say() { ( echo "$*" > $TT 2>/dev/null ) & }

say ""
say "================ A9b WRAPPER ================"
say "PID1 comm  = $(cat /proc/1/comm 2>/dev/null)"
say "PID1 cmd   = $(tr '\0' ' ' < /proc/1/cmdline 2>/dev/null)"
say "root mount = $(grep ' / ' /proc/mounts 2>/dev/null | head -1)"
say "dev/null   = $(ls -l /dev/null 2>&1)"
say "ttyGS0     = $(ls -l /dev/ttyGS0 2>&1)"
say "dev 条目数 = $(ls /dev 2>/dev/null | wc -l)"
say "systemd 二进制 = $(ls -l /lib/systemd/systemd 2>&1)"
say "sbin/init  = $(ls -l /sbin/init 2>&1)"
say "machine-id = $(cat /etc/machine-id 2>&1)"
say "fstab      = $(cat /etc/fstab 2>&1 | tr '\n' '|')"

# --- ★ 通道 1（最先）：串口救援 shell，自动重生 ---
( while :; do
      say "rescue shell up (pid $$)"
      setsid /bin/sh -i < $TT > $TT 2>&1
      say "rescue shell exited rc=$? -- respawn in 3s"
      sleep 3
  done ) &

# --- ★ 通道 2（其次）：kmsg 增量落盘（先落 /tmp，/persist 就绪后镜像过去）---
( PREV=0
  while :; do
      dmesg > /tmp/.dk 2>/dev/null
      N=$(wc -l < /tmp/.dk 2>/dev/null)
      if [ "${N:-0}" -gt "$PREV" ]; then
          tail -n +$((PREV+1)) /tmp/.dk >> /tmp/kmsg.log 2>/dev/null
          if [ -d /persist/a9b ]; then
              cat /tmp/kmsg.log > /persist/a9b/kmsg.log 2>/dev/null
          fi
          PREV=$N
          sync 2>/dev/null
      fi
      sleep 1
  done ) &

# --- 通道 3：sdc69 挂到 /persist，日志落盘 ---
mkdir -p /persist /persist/a9b 2>/dev/null
if mount -t ext4 /dev/sdc69 /persist 2>/dev/null; then
    say "persist    = mounted"
    [ -f /tmp/kmsg.log ] && cat /tmp/kmsg.log > /persist/a9b/kmsg.log 2>/dev/null
else
    say "persist    = MOUNT FAIL"
fi

# --- 通道 4：journald 持久化 + systemd debug 级日志 ---
mkdir -p /var/log/journal 2>/dev/null
printf '[Journal]\nStorage=persistent\nSystemMaxUse=200M\n' > /etc/systemd/journald.conf 2>/dev/null
printf '\n[Manager]\nLogLevel=debug\nLogTarget=journal+console\n' >> /etc/systemd/system.conf 2>/dev/null

# --- 通道 5：★ 关键静态证据一次性落盘（systemd 起来前后都能取）---
{
  echo "===== fstab =====";       cat /etc/fstab 2>&1
  echo "===== machine-id =====";  cat /etc/machine-id 2>&1
  echo "===== os-release =====";  cat /etc/os-release 2>&1
  echo "===== sbin/init =====";   ls -l /sbin/init 2>&1
  echo "===== systemd ver ====="; /lib/systemd/systemd --version 2>&1 | head -3
  echo "===== mounts =====";      cat /proc/mounts 2>&1
  echo "===== wants dirs =====";  ls -l /etc/systemd/system/*.wants/ 2>&1
  echo "===== dev =====";         ls -l /dev 2>&1
  echo "===== cmdline =====";     cat /proc/cmdline 2>&1
} > /persist/a9b/probe.txt 2>&1
[ -s /persist/a9b/probe.txt ] || {
    mkdir -p /tmp/a9b 2>/dev/null
    echo "===== fstab =====";  cat /etc/fstab 2>&1
    echo "===== mounts ====="; cat /proc/mounts 2>&1
} > /tmp/a9b/probe.txt 2>&1
dmesg > /persist/a9b/dmesg-pre-systemd.txt 2>/dev/null
cat /proc/1/status > /persist/a9b/pid1status-pre.txt 2>/dev/null
sync 2>/dev/null

say "================ exec systemd ================"
exec /lib/systemd/systemd
RC=$?
say "!!!!!! systemd exec 返回 rc=$RC -- 停在诊断 shell !!!!!!"
while :; do sleep 30; done
A9EOF
    $BB chmod 755 $W 2>/dev/null
    if [ -f "$W" ] && [ -s "$W" ]; then
        TARGET=/a9init
        K "wrapper OK ($($BB wc -c < $W 2>/dev/null) B)"
        O "A9b: wrapper written -> /a9init"
    else
        TARGET=/sbin/init
        K "wrapper write FAIL -> 直接 /sbin/init"
        O "A9b: wrapper write FAIL"
    fi
fi
K "TARGET=$TARGET"

# ---------- 9. 校验 + switch_root ----------
OK=$([ "$MODE" = "systemd" ] && { $BB test -x "$NEWROOT/usr/lib/systemd/systemd" && echo ok || echo FAIL; } || { has_init "$NEWROOT" && echo ok || echo FAIL; })
K "rootfs init=$OK dev=$ROOTDEV NEWROOT=$NEWROOT"
O "A9b: mode=$MODE init=$OK dev=$ROOTDEV"

if [ "$OK" != "ok" ]; then
    K "init check FAIL -> fallback shell"
    O "init check FAIL -> 诊断 shell"
    exec $BB sh -i < /dev/console > /dev/console 2>&1
fi

INITINFO=$($BB ls -la $NEWROOT/sbin/init 2>/dev/null)
K "sbin/init = $INITINFO  ($([ -L $NEWROOT/sbin/init ] && echo SYMLINK || echo REGULAR))"

$BB mount --move /dev  $NEWROOT/dev  2>/dev/null
$BB mount --move /proc $NEWROOT/proc 2>/dev/null
$BB mount --move /sys  $NEWROOT/sys  2>/dev/null
$BB mount --move /run  $NEWROOT/run  2>/dev/null

( echo "A9b: switch_root -> $NEWROOT $TARGET ($MODE) ..." > $NEWROOT/dev/ttyGS0 2>/dev/null ) &
K "switch_root -> $NEWROOT $TARGET ($MODE)"
ST "step9-switchroot"
echo "A9bM st pre-switch-root t=$($BB cut -d' ' -f1 /proc/uptime) newroot=$NEWROOT mode=$MODE" >> $NEWROOT/a9bmit.log 2>/dev/null
$BB sync 2>/dev/null
kill $HB 2>/dev/null
exec $BB switch_root $NEWROOT $TARGET < /dev/null > /dev/null 2>&1

K "switch_root FAILED -> fallback shell"
( echo "switch_root FAILED -> 诊断 shell" > $NEWROOT/dev/ttyGS0 2>/dev/null ) &
exec $BB sh -i < $NEWROOT/dev/console > $NEWROOT/dev/console 2>&1
