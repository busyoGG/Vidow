#!/bin/bash
# 用法：EASE="spring 1.0 800" RATE=0.03 N=8 ./tools/viewport-ab.sh
set -u
cd "$(dirname "$0")"
EASE="${EASE:-spring 0.6 800}"
RATE="${RATE:-0.01}"
N="${N:-5}"
RUNDIR=/tmp/vidow-vpab
CFGDIR="$RUNDIR/home"
LOG="$RUNDIR/log"
rm -rf "$RUNDIR"; mkdir -p "$CFGDIR/.config/vidow" "$RUNDIR/run"; chmod 700 "$RUNDIR/run"
cat > "$CFGDIR/.config/vidow/config.lua" <<CFG
vidow.output{ name = "HEADLESS-1", scale = 1, mode = "1280x720" }
window = { ms = 480, easing = "out_back 0.18" }
viewport_easing = "$EASE"
CFG
export XDG_RUNTIME_DIR="$RUNDIR/run" HOME="$CFGDIR" VIDOW_LOG=info
export DISPLAY= WAYLAND_DISPLAY=
export WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 WLR_LIBINPUT_NO_DEVICES=1
export VIDOW_CONFIG_LUA="$CFGDIR/.config/vidow/config.lua"
setsid ./build/vidow > "$LOG" 2>&1 &
VP=$!
trap 'kill -TERM -- "-$VP" 2>/dev/null' EXIT
for _ in $(seq 1 80); do grep -aq "vidow ready" "$LOG" && break; sleep 0.2; done
for _ in $(seq 1 80); do
	WAYLAND_DISPLAY=$(ls "$RUNDIR/run" 2>/dev/null | grep -m1 '^wayland-[0-9]*$')
	[ -n "$WAYLAND_DISPLAY" ] && break; sleep 0.1
done
export WAYLAND_DISPLAY
sleep 0.5
for i in 1 2 3 4 5; do ./build/frames "w$i" 20 > /dev/null 2>&1 & sleep 0.35; done
sleep 1.5
for _ in 1 2 3 4 5; do ./build/vidowmsg focus-column -1 >/dev/null 2>&1; sleep 0.06; done
sleep 2
( while :; do
	printf '%s %s\n' "$(date +%s%N)" "$(./build/vidowmsg state 2>/dev/null | python3 -c '
import json,sys
try: st=json.load(sys.stdin)
except Exception: print(-1); raise SystemExit
for o in st.get("outputs") or []:
    if str(o.get("name","")).startswith("HEADLESS"):
        print(o.get("viewport_x", -1)); raise SystemExit
print(-1)')"
	sleep "$RATE"
done ) > "$RUNDIR/vp.txt" 2>/dev/null &
SPID=$!
sleep 0.3
for _ in $(seq 1 "$N"); do date +%s%N >> "$RUNDIR/cmds.txt"; ./build/vidowmsg focus-column 1 >/dev/null 2>&1; sleep "$RATE"; done
sleep 2.5
kill $SPID 2>/dev/null
python3 - "$RUNDIR/vp.txt" "$EASE" "$RUNDIR/cmds.txt" <<'PY'
import sys
rows=[l.split() for l in open(sys.argv[1]) if l.strip()]
vals=[float(v) for _,v in rows if float(v)>=0]
if not vals: print("没采到"); raise SystemExit
final=vals[-1]
d=[b-a for a,b in zip(vals,vals[1:])]
rev=sum(1 for a,b in zip(d,d[1:]) if a*b<0 and abs(a)>2 and abs(b)>2)
ts=[int(a) for a,b in rows if float(b)>=0]
lastcmd=int(open(sys.argv[3]).read().split()[-1])
last=0
for i in range(1,len(vals)):
    if abs(vals[i]-vals[i-1])>0.5:
        last=i
settle=(ts[last]-lastcmd)/1e6 if last else 0
print("曲线 %-14s 终点 %.0f  过冲 %3.0f px  反转 %d 次  尾段静止 %4.0f ms"%(sys.argv[2],final,max(vals)-final,rev,settle))
PY
