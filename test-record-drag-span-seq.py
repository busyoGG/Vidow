#!/usr/bin/env python3
"""record-drag-span.py 帧序号同步 / 防覆盖的无合成器回归测试。

不启动合成器、不跑录制入口、不碰任何 Vidow 进程：只 import 模块里的
FrameSeq，并直接执行该脚本写出的 frame.sh（grim 换成模拟的）。
所有临时文件落在 mkdtemp 目录，结束即删。

用法：test-record-drag-span-seq.py        退出 0 = 全过，1 = 有失败。
"""
import importlib.util
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent

# 模拟 grim：和真 grim 一样，文件名是**最后一个参数**（调用方式 `grim -s 1.0 <png>`）。
# 成功时写一个占位 png；FAKE_GRIM_FAIL 置位时非 0 退出并写 stderr。
# 唯一的差别是它**不认识选项**：`-s 1.0` 那两个 `-` 开头的参数必须跳过 ——
# 否则文件名会变成 `-s`，在本目录留下一个叫 `-s` 的假 PNG（曾经真留下过）。
# 用 bash 而不是 sh：dash 的 printf 不认 \n（失败但退出码仍 0，就不像真 grim 了）。
FAKE_GRIM = r'''#!/bin/bash
out=""
for a in "$@"; do
	case "$a" in -*) continue ;; esac
	out="$a"
done
if [ -z "$out" ]; then echo "fake grim: no output file" >&2; exit 2; fi
[ -n "$FAKE_GRIM_LOG" ] && echo "$out" >> "$FAKE_GRIM_LOG"
if [ -n "$FAKE_GRIM_FAIL" ]; then echo "fake grim: boom" >&2; exit 3; fi
printf 'FAKEPNG\n' > "$out" || exit 4
'''

# 与 record-drag-span.py 中 main() 写的 frame.sh 保持同源（改那边要同步改这里）
FRAME_SH_TMPL = '''#!/bin/sh
seqf={run}/seq
if [ -f "$seqf" ]; then
	n=$(cat "$seqf") || exit 1
else
	n=0
fi
case $n in
	''|*[!0-9]*) echo "frame.sh: 帧序号文件损坏: $n" >&2; exit 1 ;;
esac
n=$((n + 1))
png={frames}/$(printf %05d $n).png
if [ -e "$png" ]; then echo "frame.sh: 帧 $png 已存在，拒绝覆盖" >&2; exit 1; fi
if ! echo $n > "$seqf"; then echo "frame.sh: 写帧序号失败 $seqf" >&2; exit 1; fi
if ! grim -s 1.0 "$png"; then echo "frame.sh: grim 失败 $png" >&2; exit 1; fi
'''

fails = []


def check(name, cond, detail=''):
    print(('PASS ' if cond else 'FAIL ') + name + ('' if cond else f'  [{detail}]'))
    if not cond:
        fails.append(name)


def load_mod():
    spec = importlib.util.spec_from_file_location('rds', ROOT / 'record-drag-span.py')
    mod = importlib.util.module_from_spec(spec)
    sys.argv = ['record-drag-span.py', 'PLACEHOLDER.mp4']  # 只读 argv，不启动合成器
    spec.loader.exec_module(mod)
    return mod


def setup(tmp, name):
    d = tmp / name
    (d / 'frames').mkdir(parents=True)
    return d, d / 'frames'


def write_frame_sh(run, frames):
    p = pathlib.Path(run) / 'frame.sh'
    p.write_text(FRAME_SH_TMPL.format(run=run, frames=frames))
    p.chmod(0o755)
    return p


def main() -> int:
    tmp = pathlib.Path(tempfile.mkdtemp(prefix='rds-seq-test-'))
    try:
        binp = tmp / 'bin'
        binp.mkdir()
        (binp / 'grim').write_text(FAKE_GRIM)
        (binp / 'grim').chmod(0o755)
        env = dict(os.environ, PATH=f'{binp}:{os.environ["PATH"]}')
        os.environ['PATH'] = env['PATH']  # os.system 也走模拟 grim

        mod = load_mod()
        check('FrameSeq 已定义', hasattr(mod, 'FrameSeq'))
        if not hasattr(mod, 'FrameSeq'):
            return 1
        FS = mod.FrameSeq

        # ---- 用例 1（主场景）：python 2 帧 → shell 3 帧 → python 补 2 帧
        run, frames = setup(tmp, 'mixed')
        seq = FS(run, frames)
        check('空目录 read()=0', seq.read() == 0)
        for tag in ('py1', 'py2'):
            n = seq.next('grab_frame')
            (frames / f'{n:05d}.png').write_text(tag)
        sh = write_frame_sh(run, frames)
        rcs = [subprocess.run([str(sh)], capture_output=True).returncode
               for _ in range(3)]
        for tag in ('py3', 'py4'):
            n = seq.next('grab_frame')
            (frames / f'{n:05d}.png').write_text(tag)
        nums = sorted(int(p.stem) for p in frames.glob('*.png'))
        check('混跑帧号连续 1..7', nums == list(range(1, 8)), str(nums))
        check('frame.sh 三次都成功退出', rcs == [0, 0, 0], str(rcs))
        check('run/seq == 7', seq.read() == 7, str(seq.read()))
        check('python 第 1 帧未被 shell 覆盖', (frames / '00001.png').read_text() == 'py1')
        check('python 补的第 6 帧是 py3', (frames / '00006.png').read_text() == 'py3')
        check('shell 写的第 3 帧存在', (frames / '00003.png').exists())

        # ---- 用例 2：预置帧不可覆盖（shell 路径）
        run2, frames2 = setup(tmp, 'pre_shell')
        (frames2 / '00001.png').write_text('KEEP')
        sh2 = write_frame_sh(run2, frames2)
        p = subprocess.run([str(sh2)], capture_output=True, text=True)
        check('shell: 目标帧已存在 → 非零退出', p.returncode != 0, str(p.returncode))
        check('shell: 报错含「拒绝覆盖」', '拒绝覆盖' in p.stderr, p.stderr)
        check('shell: 预置帧内容不变', (frames2 / '00001.png').read_text() == 'KEEP')
        check('shell: 已存在时不推进 seq', FS(run2, frames2).read() == 0,
              str(FS(run2, frames2).read()))

        # ---- 用例 3：预置帧不可覆盖（python 路径）
        run3, frames3 = setup(tmp, 'pre_py')
        (frames3 / '00001.png').write_text('KEEP_PY')
        try:
            FS(run3, frames3).next('grab_frame')
            check('python: 目标帧已存在 → 报错', False, '没抛')
        except SystemExit as e:
            check('python: 目标帧已存在 → 报错', '拒绝覆盖' in str(e), str(e))
        check('python: 预置帧内容不变', (frames3 / '00001.png').read_text() == 'KEEP_PY')

        # ---- 用例 4：序号文件损坏 → 非 0，不当 0 处理
        run4, frames4 = setup(tmp, 'corrupt')
        (run4 / 'seq').write_text('garbage')
        sh4 = write_frame_sh(run4, frames4)
        p = subprocess.run([str(sh4)], capture_output=True, text=True)
        check('shell: 序号文件损坏 → 非零退出', p.returncode != 0, str(p.returncode))
        check('shell: 损坏时不截图', not list(frames4.glob('*.png')))
        try:
            FS(run4, frames4).read()
            check('python: 序号文件损坏 → 报错', False, '没抛')
        except SystemExit as e:
            check('python: 序号文件损坏 → 报错', '损坏' in str(e), str(e))

        # ---- 用例 5：grim 失败必须被报告（shell 路径）
        run5, frames5 = setup(tmp, 'grimfail')
        sh5 = write_frame_sh(run5, frames5)
        penv = dict(env, FAKE_GRIM_FAIL='1')
        p = subprocess.run([str(sh5)], capture_output=True, text=True, env=penv)
        check('shell: grim 非 0 → frame.sh 非 0', p.returncode != 0, str(p.returncode))
        check('shell: stderr 报出 grim 失败', 'grim 失败' in p.stderr, p.stderr)

        # ---- 用例 6：grim 失败必须被报告（python 路径用的同一条命令）
        frames6 = setup(tmp, 'grimfail_py')[1]
        target = frames6 / '00001.png'
        r = subprocess.run(['grim', '-s', '1.0', str(target)],
                           capture_output=True, text=True, env=penv)
        check('python: grim 非 0 被捕获', r.returncode != 0 and not target.exists(),
              str(r.returncode))
        r = subprocess.run(['grim', '-s', '1.0', str(target)],
                           capture_output=True, text=True, env=env)
        check('模拟 grim 成功时确实落盘', r.returncode == 0 and target.exists())

        check('没有 x.png 之类的中间文件',
              not list(frames.glob('x*')) and not list(frames6.glob('x*')))
        # 模拟 grim 曾经把 `-s` 当成文件名，在本目录（不是临时目录）留下一个叫
        # `-s` 的假 PNG。这里守住：跑完测试，cwd 里不该多出任何文件。
        check('cwd 里没有留下 -s 之类的假帧', not (ROOT / '-s').exists(),
              str(ROOT / '-s'))
        return 0 if not fails else 1
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
        print(f"\n{'全部通过' if not fails else str(len(fails)) + ' 项失败: ' + str(fails)}")


if __name__ == '__main__':
    sys.exit(main())
