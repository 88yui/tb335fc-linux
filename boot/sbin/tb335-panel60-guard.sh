#!/bin/sh
# 60Hz 面板模块守卫：开机后 300 秒内若看不到 OK 标记，就回滚原厂模块并重启。
# 若已写 OK 标记，则清除 armed，后续开机不再检查。窗口放在后台，不阻塞启动。
[ -f /persist/a9b/.panel60-armed ] || exit 0

/usr/bin/logger -t panel60-guard "armed, 300 秒后校验"

(
	sleep 300
	if [ -f /persist/a9b/.panel60-ok ]; then
		rm -f /persist/a9b/.panel60-armed
		/usr/bin/logger -t panel60-guard "OK 标记存在，保留 60Hz 模块"
		exit 0
	fi
	/usr/bin/logger -t panel60-guard "超时未确认，回滚原厂面板模块"
	K=$(uname -r)
	cp -f /persist/a9b/backup/panel60-orig/panel-*.ko "/lib/modules/$K/" 2>/dev/null
	echo ROLLEDBACK > /persist/a9b/.panel60-state
	rm -f /persist/a9b/.panel60-armed
	sync
	/sbin/reboot
) >/dev/null 2>&1 &

exit 0
