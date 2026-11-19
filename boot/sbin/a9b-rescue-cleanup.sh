#!/bin/sh
# A9b: 清掉 initramfs 包装器留下的救援 shell / kmsg 落盘循环
# 原因：/bin/sh -i（交互 shell）忽略 SIGTERM，systemd-shutdown 会为它等满 90s 才 SIGKILL，
#       导致「关机后背光常亮约 100 秒」；这些循环只是启动期诊断，进系统后已无必要。
for i in 1 2 3 4 5; do
  n=0
  for p in /proc/[0-9]*; do
    pid=${p#/proc/}
    [ -r "$p/cmdline" ] || continue
    c=$(tr '\0' ' ' < "$p/cmdline" 2>/dev/null)
    case "$c" in
      "/bin/sh /a9init "|"/bin/sh /a9init")
        kids=$(cat "$p/task/$pid/children" 2>/dev/null)
        for k in $kids; do kill -9 "$k" 2>/dev/null; done
        kill -9 "$pid" 2>/dev/null; n=$((n+1)) ;;
      "/bin/sh -i "|"/bin/sh -i")
        if readlink "$p/fd/0" 2>/dev/null | grep -q ttyGS0; then
          kill -9 "$pid" 2>/dev/null; n=$((n+1))
        fi ;;
    esac
  done
  [ "$n" -eq 0 ] && break
  sleep 1
done
exit 0
