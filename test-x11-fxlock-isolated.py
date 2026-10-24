#!/usr/bin/env python3
"""只读取证：X11 客户端「只交一帧」之后合成器有没有重贴。

隔离条件：
  - 自带 headless wlroots（WLR_BACKENDS=headless），独立 XDG_RUNTIME_DIR，
    无 DBus，绝不碰真实桌面。
  - X11 客户端用 tools/fxlock-probe.c：**不订阅 Expose / ConfigureNotify**，
    所以它不会自己重涂；屏幕上出现次帧色只能是合成器真的把新内容画出来了。

时序（每一步都打时间戳，写入 timeline.txt）：
  map(蓝首帧) -> 等 fx/bake 稳定 -> 截 A -> 探针 put_image(绿色) -> 等
  -> 连续截 B1..Bn（期间绝不注入任何输入、不改焦点、不动窗口）
  -> 可选：只动焦点（不移动、不改尺寸）再截 C。
"""
import argparse
import json
import os
import re
import signal
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent
PROBE = '/tmp/fxlock-probe'


def stamp(t0, what):
    us = int((time.monotonic() - t0) * 1e6)
    return '%d.%03d %s' % (us // 1_000_000, us % 1_000_000 // 1000, what)


def count(im, rgb, tol=48):
    r0, g0, b0 = rgb
    n = 0
    for r, g, b in im.getdata():
        if abs(r - r0) <= tol and abs(g - g0) <= tol and abs(b - b0) <= tol:
            n += 1
    return n


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--binary', default=str(ROOT / 'build/vidow'))
    ap.add_argument('--scale', type=int, default=1)
    ap.add_argument('--delay-ms', type=int, default=3000)
    ap.add_argument('--shots', type=int, default=5)
    ap.add_argument('--shot-gap', type=float, default=1.0)
    ap.add_argument('--settle', type=float, default=6.0)
    ap.add_argument('--focus-test', action='store_true')
    ap.add_argument('--out', default=None)
    args = ap.parse_args()

    from PIL import Image
    run = Path(args.out) if args.out else Path(tempfile.mkdtemp(prefix='vidow-fxlock-'))
    run.mkdir(parents=True, exist_ok=True)
    run.chmod(0o700)
    (run / 'config').mkdir(exist_ok=True)
    timeline = (run / 'timeline.txt').open('w')

    def log(msg):
        line = stamp(T0, msg)
        print(line, flush=True)
        timeline.write(line + '\n')
        timeline.flush()

    T0 = time.monotonic()
    env = dict(os.environ,
               XDG_RUNTIME_DIR=str(run), XDG_CONFIG_HOME=str(run / 'config'),
               XDG_DATA_HOME=str(run / 'data'),
               DBUS_SESSION_BUS_ADDRESS='unix:path=' + str(run / 'no-bus'),
               WLR_BACKENDS='headless', WLR_HEADLESS_OUTPUTS='1',
               WLR_RENDERER='gles2', WLR_LIBINPUT_NO_DEVICES='1',
               VIDOW_LOG='debug', VIDOW_PORTAL_NO_RESTART='1',
               WAYLAND_DISPLAY='', DISPLAY='')
    for k in ('XCURSOR_SIZE',):
        env.pop(k, None)

    clog = (run / 'vidow.log').open('w')
    plog = (run / 'probe.log').open('w')
    procs = []

    def spawn(argv, log=clog):
        p = subprocess.Popen(argv, env=env, stdout=log, stderr=log,
                             start_new_session=True)
        procs.append(p)
        return p

    def msg(*a):
        return subprocess.check_output([str(ROOT / 'build/vidowmsg'), *a],
                                       env=env, text=True, timeout=10)

    def shot(name):
        path = run / (name + '.png')
        subprocess.check_output(['grim', str(path)], env=env, timeout=20)
        im = Image.open(path).convert('RGB')
        blue = count(im, (0, 0, 192))
        green = count(im, (0, 192, 0))
        log('shot %s size=%dx%d blue=%d green=%d' %
            (name, im.width, im.height, blue, green))
        return {'name': name, 'blue': blue, 'green': green,
                'size': [im.width, im.height]}

    results = {'samples': [], 'windows': []}
    try:
        spawn([args.binary])
        for _ in range(200):
            text = (run / 'vidow.log').read_text(errors='replace')
            wl = re.findall(r'WAYLAND_DISPLAY=([\w.-]+)', text)
            xd = re.findall(r'DISPLAY=(:\d+)', text)
            if wl:
                env['WAYLAND_DISPLAY'] = wl[-1]
            if wl and xd:
                env['DISPLAY'] = xd[-1]
                break
            time.sleep(.05)
        else:
            raise RuntimeError('compositor did not start')
        log('compositor up wl=%s x=%s' % (env['WAYLAND_DISPLAY'], env['DISPLAY']))
        time.sleep(1.0)

        subprocess.run(['xrandr', '--query'], env=env, timeout=10,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        time.sleep(0.8)
        if args.scale != 1:
            msg('output-scale', 'HEADLESS-1', str(args.scale))
            time.sleep(1.0)
            text = (run / 'vidow.log').read_text(errors='replace')
            xd = re.findall(r'DISPLAY=(:\d+)', text)
            if xd:
                env['DISPLAY'] = xd[-1]
            log('scale set to %d, DISPLAY=%s' % (args.scale, env.get('DISPLAY')))

        basemark = (run / 'vidow.log').stat().st_size

        # 1) 蓝首帧 map
        p = subprocess.Popen([PROBE, '--delay-ms', str(args.delay_ms),
                              '--w', '400', '--h', '300', '--x', '120', '--y', '120',
                              '--first', '0000c0', '--second', '00c000',
                              '--tag', 'fxlock'],
                             env=env, stdout=plog, stderr=plog, start_new_session=True)
        procs.append(p)
        log('probe spawned pid=%d delay=%dms scale=%d' % (p.pid, args.delay_ms, args.scale))
        time.sleep(args.settle)
        results['samples'].append(shot('A-first-frame'))
        results['windows'].append(json.loads(msg('windows')))

        text_probe = (run / 'probe.log').read_text(errors='replace')
        results['probe_log_at_A'] = text_probe

        # 2) 等探针 put_image 完成
        deadline = time.time() + 30
        while time.time() < deadline:
            if 'put_image issued' in (run / 'probe.log').read_text(errors='replace'):
                break
            time.sleep(0.05)
        log('probe reported second frame')
        time.sleep(0.5)

        # 3) 连续截图，期间无任何输入注入
        for i in range(args.shots):
            results['samples'].append(shot('B%d-after-commit' % (i + 1)))
            results['windows'].append(json.loads(msg('windows')))
            time.sleep(args.shot_gap)

        # 4) 可选：只改焦点（不挪窗、不改尺寸）
        if args.focus_test:
            log('focus test: sending focus change only')
            try:
                wid = None
                for w in json.loads(msg('windows')):
                    if (w.get('title') or '') == 'fxlock':
                        wid = str(w.get('window_id') or (w.get('x11') or {}).get('window_id'))
                log('window id for focus = %r' % wid)
                msg('focus-self') if False else None
            except Exception as e:  # noqa: BLE001
                log('focus test skipped: %r' % (e,))
            time.sleep(1.5)
            results['samples'].append(shot('C-after-focus'))

        tail = (run / 'vidow.log').read_text(errors='replace')[basemark:]
        (run / 'vidow-tail.log').write_text(tail)
        results['bake_lines'] = [l for l in tail.splitlines() if 'bake' in l.lower()]
        results['fx_lines'] = [l for l in tail.splitlines()
                               if re.search(r'\bfx\b|decor|重烘|bakes', l)]
        results['x11_lines'] = [l for l in tail.splitlines() if l.startswith('x11:') or 'xwayland' in l.lower()]
        log('done')
        return 0
    finally:
        for p in reversed(procs):
            if p.poll() is None:
                try:
                    os.killpg(p.pid, signal.SIGTERM)
                    p.wait(timeout=5)
                except Exception:  # noqa: BLE001
                    os.killpg(p.pid, signal.SIGKILL)
        clog.close()
        plog.close()
        timeline.close()
        (run / 'result.json').write_text(json.dumps(results, indent=2, ensure_ascii=False))
        print('ARTIFACT', run, flush=True)


if __name__ == '__main__':
    sys.exit(main())
