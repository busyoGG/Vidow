#!/bin/bash
# 只杀「测试用的」vidow 实例，**绝不动用户正在用的那个**。
#
# 血的教训：以前写 `pkill -x vidow`，按进程名匹配 —— 用户桌面那个也叫 vidow，
# 一起被杀，桌面当场黑屏（发生过两次）。这里改成按 **XDG_RUNTIME_DIR** 匹配。
#
# 关键细节：用户的实例跑在 `/tmp/vshell/run`（VShell 起的），它**也**以 /tmp
# 开头，所以「/tmp 下的都算测试」是错的判据。真正的判据是：测试实例的
# XDG_RUNTIME_DIR 一定是**我们自己的那几个固定目录**之一（见下面的白名单）。
# 白名单之外的，一个都不碰。
#
# 用法：
#   tools/kill-test-vidow.sh              # 杀白名单里所有目录下的 vidow
#   tools/kill-test-vidow.sh /tmp/dztest  # 只杀这一个运行目录下的
set -u

# 测试专用的运行目录白名单。**不要**往里加 /tmp/vshell（那是用户的）。
ALLOWED="/tmp/nesttest /tmp/dztest /tmp/xdotest /tmp/vidow-test /tmp/vidow-xhidpi-test
/tmp/vidow-portal-test /tmp/vidow-drag-test /tmp/vidow-deadzone-test
/tmp/vidow-popup-test /tmp/pp1 /tmp/pp2 /tmp/pp3 /tmp/pp"

want="${1:-}"

for p in /proc/[0-9]*; do
	pid=${p#/proc/}
	[ "$pid" = "$$" ] && continue
	[ -r "$p/comm" ] || continue
	[ "$(cat "$p/comm" 2>/dev/null)" = "vidow" ] || continue
	[ -r "$p/environ" ] || continue

	rd=$(tr '\0' '\n' < "$p/environ" 2>/dev/null |
		sed -n 's/^XDG_RUNTIME_DIR=//p' | head -1)

	# 不在白名单里 —— 一律跳过（用户桌面实例就在这里被保护）
	ok=0
	for a in $ALLOWED; do
		if [ "$rd" = "$a" ]; then
			ok=1
			break
		fi
	done
	[ "$ok" = "1" ] || continue

	if [ -n "$want" ] && [ "$rd" != "$want" ]; then
		continue
	fi

	echo "kill 测试实例 pid=$pid XDG_RUNTIME_DIR=$rd"
	kill "$pid" 2>/dev/null
done

exit 0
