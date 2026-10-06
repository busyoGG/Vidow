#!/bin/bash
# 客户端新画面必须真的上屏 —— **dmabuf 客户端**（Qt/KDE 那类）那条路。
#
# 用户报的：「KDE 系的软件不渲染新画面（kate 停在最后一帧、dolphin 内容不出来），
# 其他软件正常，包括普通 Qt」。根因在装饰管线取客户端贴图那一步：为了「拿到的贴图
# 能 destroy」而绕开 wlr_client_buffer 自己那张、对 `client->source` 另建一张 ——
# 对 **dmabuf 客户端**（每帧重画同一个 dmabuf、只 commit 不换缓冲）那张一直是第一次
# 导入的那版内容：客户端在画、装饰缓冲也在重烘，里面永远是旧画面，屏幕上就是「停在
# 最后一帧」；拖到另一块屏会重新烘一次背衬，画面才跟上。GTK/nautilus 在我这儿走 shm，
# 所以看不出问题。
#
# 判据（headless 一块屏 + kate 当 dmabuf 客户端，没有 kate 就跳过）：
#   1) 打字前 / 打字后各截一张：屏幕像素必须变（修之前实测 0 像素）
#   2) 同时看装饰缓冲（VIDOW_FX_DUMP）：打字后的那张必须和打字前的不同 —— 这一条
#      把「谁没更新」钉在装饰管线那一层，屏幕那条是用户看得见的判据
#   3) vidow 全程活着、收尾一个进程不剩
#
# 用法：./test-dmabuf-update.sh   （VIDOW_TEST_KEEP=1 保留运行目录）
set -u
cd "$(dirname "$0")"

RUNDIR=/tmp/vidow-dmabuf-test
CFGDIR="$RUNDIR/home"
KHOME="$RUNDIR/khome"
LOG="$RUNDIR/log"
FAILED=0
VIDOW_PGID=""

pass() { echo "  PASS  $1"; }
fail() { echo "  FAIL  $1"; FAILED=1; }
skip() { echo "  SKIP  $1"; }
mark() { echo; echo "== $1 =="; }

cleanup() {
	if [ -n "$VIDOW_PGID" ]; then
		kill -TERM -- "-$VIDOW_PGID" 2>/dev/null
		sleep 0.4
		kill -KILL -- "-$VIDOW_PGID" 2>/dev/null
	fi
	for p in $(pgrep -x kate 2>/dev/null); do kill -TERM "$p" 2>/dev/null; done
	sleep 0.3
	for p in $(pgrep -x kate 2>/dev/null); do kill -KILL "$p" 2>/dev/null; done
	for p in $(pgrep -x vidow 2>/dev/null); do kill -TERM "$p" 2>/dev/null; done
	sleep 0.3
	for p in $(pgrep -x vidow 2>/dev/null); do kill -KILL "$p" 2>/dev/null; done
	if [ -z "${VIDOW_TEST_KEEP:-}" ]; then
		rm -rf "$RUNDIR"
		rm -f /tmp/vidow-fx-*.raw /tmp/vidow-fx-*.txt
	fi
}
trap cleanup EXIT

command -v kate >/dev/null || { echo "test-dmabuf-update.sh: 没装 kate，跳过"; exit 0; }
command -v grim >/dev/null || { echo "test-dmabuf-update.sh: 需要 grim"; exit 1; }
python3 -c 'import PIL' 2>/dev/null || { echo "test-dmabuf-update.sh: 需要 python3-PIL"; exit 1; }

mkdir -p "$CFGDIR/.config/vidow" "$RUNDIR/run" "$KHOME"
chmod 700 "$RUNDIR/run"
rm -f /tmp/vidow-fx-*.raw /tmp/vidow-fx-*.txt

export XDG_RUNTIME_DIR="$RUNDIR/run"
export HOME="$CFGDIR" VIDOW_LOG=info
export DISPLAY= WAYLAND_DISPLAY=

cat > "$CFGDIR/.config/vidow/config.lua" <<'CFG'
scale = "1"
xwayland = false
CFG
export VIDOW_CONFIG_LUA="$CFGDIR/.config/vidow/config.lua"

pixdiff() { # <a> <b>
	python3 - "$1" "$2" <<'PY'
import sys, warnings
warnings.filterwarnings("ignore")
from PIL import Image
a = Image.open(sys.argv[1]).convert("RGB")
b = Image.open(sys.argv[2]).convert("RGB")
if a.size != b.size:
    print(-1)
else:
    print(sum(1 for p, q in zip(a.getdata(), b.getdata()) if p != q))
PY
}

dump_diff() { # <raw1> <raw2>  -> 不同像素数
	python3 - "$1" "$2" <<'PY'
import sys, warnings
warnings.filterwarnings("ignore")
from PIL import Image
def load(p):
    w, h, fmt = open(p[:-4] + ".txt").read().split()
    return Image.frombytes("RGBA", (int(w), int(h)), open(p, "rb").read()).convert("RGB")
a, b = load(sys.argv[1]), load(sys.argv[2])
if a.size != b.size:
    print(-1)
else:
    print(sum(1 for p, q in zip(a.getdata(), b.getdata()) if p != q))
PY
}

mark "dmabuf 客户端（kate）的新画面必须上屏"

export WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 WLR_LIBINPUT_NO_DEVICES=1
# VIDOW_FX_DUMP：把装饰缓冲 dump 到 /tmp/vidow-fx-N.raw，好断言「重烘的那张也变了」
env VIDOW_FX_DUMP=400 setsid ./build/vidow > "$LOG" 2>&1 &
VIDOW_PGID=$!
for _ in $(seq 1 60); do
	grep -aq "vidow ready" "$LOG" && break
	sleep 0.25
done
if ! grep -aq "vidow ready" "$LOG"; then
	echo "vidow 没起来"; tail -5 "$LOG"; exit 1
fi
export WAYLAND_DISPLAY=$(grep -ao "WAYLAND_DISPLAY=[a-z0-9-]*" "$LOG" |
	tail -1 | cut -d= -f2)
sleep 0.5

# kate：新鲜的 HOME（不带会话状态），打开一个短文件，光标落在第一行
printf 'line one\nline two\nline three\n' > "$KHOME/sample.txt"
env HOME="$KHOME" DBUS_SESSION_BUS_ADDRESS="unix:path=$RUNDIR/no-bus" \
	kate --new "$KHOME/sample.txt" > "$RUNDIR/kate.log" 2>&1 &
sleep 6

if ./build/vidowmsg windows 2>/dev/null | grep -q "Kate"; then
	pass "kate 的窗口进列表了"
else
	fail "kate 没映射上来（$(tail -2 "$RUNDIR/kate.log" | tr '\n' '|')）"
fi

BEFORE_DUMPS=$(ls /tmp/vidow-fx-*.raw 2>/dev/null | wc -l)
grim "$RUNDIR/before.png" >/dev/null 2>&1
./build/vidowmsg type-text "ZZZZQ" >/dev/null 2>&1
sleep 2
grim "$RUNDIR/after.png" >/dev/null 2>&1

DIFF=$(pixdiff "$RUNDIR/before.png" "$RUNDIR/after.png")
if [ "$DIFF" -gt 200 ] 2>/dev/null; then
	pass "打字后屏幕上真的变了（$DIFF 像素；修之前是 0）"
else
	fail "打字后屏幕没变（$DIFF 像素）—— 客户端的新画面没上屏"
fi

# 装饰缓冲那一层：挑一张打字前的、一张打字后的比
LAST_BEFORE=$(python3 - "$BEFORE_DUMPS" <<'PY'
import sys, glob
files = sorted(glob.glob("/tmp/vidow-fx-*.raw"), key=lambda p: int(p.split("-")[-1][:-4]))
n = int(sys.argv[1])
print(files[n - 1] if n > 0 and n <= len(files) else "")
PY
)
LAST_AFTER=$(python3 - <<'PY'
import glob
files = sorted(glob.glob("/tmp/vidow-fx-*.raw"), key=lambda p: int(p.split("-")[-1][:-4]))
print(files[-1] if files else "")
PY
)
if [ -n "$LAST_BEFORE" ] && [ -n "$LAST_AFTER" ] && [ "$LAST_BEFORE" != "$LAST_AFTER" ]; then
	DDIFF=$(dump_diff "$LAST_BEFORE" "$LAST_AFTER")
	if [ "$DDIFF" -gt 100 ] 2>/dev/null; then
		pass "装饰缓冲重烘出来的也是新画面（$DDIFF 像素不同）"
	else
		fail "装饰缓冲还是旧画面（$DDIFF 像素不同）—— 取客户端贴图那一步拿的是旧版本"
	fi
else
	fail "没拿到「打字前 / 打字后」两张装饰缓冲（$LAST_BEFORE / $LAST_AFTER）"
fi

pgrep -x vidow >/dev/null && pass "vidow 还活着" || fail "vidow 挂了"

mark "收尾"
kill -TERM -- "-$VIDOW_PGID" 2>/dev/null
sleep 0.5
kill -KILL -- "-$VIDOW_PGID" 2>/dev/null
VIDOW_PGID=""
for p in $(pgrep -x kate 2>/dev/null); do kill -KILL "$p" 2>/dev/null; done
sleep 0.5
LEFT=$(pgrep -x vidow 2>/dev/null | wc -l)
if [ "$LEFT" = "0" ]; then
	pass "收尾干净：一个 vidow 都不剩"
else
	fail "收尾还剩 $LEFT 个 vidow"
fi

echo
if [ "$FAILED" = 0 ]; then
	echo "test-dmabuf-update.sh: 全部通过"
else
	echo "test-dmabuf-update.sh: 有失败"
fi
exit "$FAILED"
