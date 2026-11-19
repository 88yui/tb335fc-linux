#!/bin/sh
# TB335 长按关机：强制断电
#
# 踩过的坑（2026-11-16）：
#   systemctl poweroff / poweroff -f  → 偶尔卡在停机中途（sshd 退了、背光照亮、机器不死）
#   ctypes 直接调 reboot(2)          → 返回 EINVAL
#   /sbin/reboot -f -p               → 多数 1 秒内真断电；但 21:47 实测有一次打印
#                                      "Rebooting." 后整机没断电、又继续跑了 7 分钟
#
# 现在的做法：先 sync，再直接写内核 sysrq 的 poweroff(o)——它是 emergency_sync 后
# 直接 kernel_power_off()（PSCI SYSTEM_OFF），不走 systemd 的停机/停服务流程，
# 所以不会被某个卡住的单元拖住。sysrq 万一被挡（写完之后脚本还活着）再退回 reboot -f -p。
#
# 核对过：/proc/sys/kernel/sysrq=438（含 reboot/poweroff 位），
#         内核 HELP 里明确有 "poweroff(o)"，/proc/sysrq-trigger 为 root 可写。
timeout 8 sync 2>/dev/null || true
echo o > /proc/sysrq-trigger 2>/dev/null || true
sleep 1
exec /sbin/reboot -f -p
