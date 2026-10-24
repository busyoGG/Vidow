#!/usr/bin/env python3
"""Isolated proof that an X11 window's FX bake samples its buffer correctly on a
2x output (the "Steam: shape but black content" bug).

Runs its own compositor (headless, scale 2), its own XDG_RUNTIME_DIR, no DBus,
so it never touches the real desktop. Builds the current tree's xpopup, maps a
solid blue X11 window, screenshots with grim and counts blue pixels.

Usage:  python3 test-x11-fx-color.py [--binary build/vidow]
"""
import argparse
import os
from pathlib import Path
import re
import signal
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parent
BLUE = (0, 0, 192)  # xpopup owner paints 0x0000c0


def count_blue(path):
    from PIL import Image
    im = Image.open(path).convert('RGB')
    px = im.getdata()
    blue = sum(1 for r, g, b in px if b >= 150 and r < 60 and g < 60)
    green = sum(1 for r, g, b in px if g >= 150 and r < 60 and b < 60)
    return im.size, blue, len(px), green


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--binary', default=str(ROOT / 'build/vidow'))
    ap.add_argument('--keep', action='store_true')
    args = ap.parse_args()

    run = Path(tempfile.mkdtemp(prefix='vidow-x11fx-'))
    run.chmod(0o700)
    (run / 'config').mkdir()
    env = dict(os.environ,
               XDG_RUNTIME_DIR=str(run),
               XDG_CONFIG_HOME=str(run / 'config'),
               XDG_DATA_HOME=str(run / 'data'),
               DBUS_SESSION_BUS_ADDRESS='unix:path=' + str(run / 'no-bus'),
               WLR_BACKENDS='headless', WLR_HEADLESS_OUTPUTS='1',
               WLR_RENDERER='gles2', WLR_LIBINPUT_NO_DEVICES='1',
               VIDOW_PORTAL_NO_RESTART='1')
    env.pop('WAYLAND_DISPLAY', None)
    env.pop('DISPLAY', None)
    env.pop('XCURSOR_SIZE', None)

    log = (run / 'vidow.log').open('w')
    procs = []

    def spawn(argv, **kw):
        p = subprocess.Popen(argv, env=env, stdout=kw.get('stdout', log),
                             stderr=kw.get('stderr', log), start_new_session=True)
        procs.append(p)
        return p

    try:
        spawn([args.binary])
        for _ in range(120):
            text = (run / 'vidow.log').read_text(errors='replace')
            wl = re.findall(r'WAYLAND_DISPLAY=([\w.-]+)', text)
            if wl:
                env['WAYLAND_DISPLAY'] = wl[-1]
                break
            time.sleep(.1)
        else:
            raise RuntimeError('compositor did not start')
        time.sleep(1.2)

        # set output scale 2 (the real DP-2 setup that broke)
        subprocess.check_output(
            [str(ROOT / 'build/vidowmsg'), 'output-scale', 'HEADLESS-1', '2'],
            env=env, timeout=10)
        time.sleep(.6)

        # Xwayland starts lazily and exports DISPLAY only when it boots; the
        # scale change above resets it, so resolve DISPLAY from the live log
        # right before launching the X11 client.
        for _ in range(150):
            text = (run / 'vidow.log').read_text(errors='replace')
            disp = re.findall(r'xwayland: DISPLAY=(:\d+)', text)
            if disp:
                env['DISPLAY'] = disp[-1]
            if (run / 'X1-lock').exists() or (run / '.X11-unix').exists() or disp:
                break
            time.sleep(.1)
        subprocess.run(['xrandr', '--query'], env=env, timeout=10,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        time.sleep(.8)
        # scale=2 在窗口上写 _XWAYLAND_GLOBAL_OUTPUT_SCALE=2：Xwayland 会按 2 倍
        # 提交缓冲（缓冲 500x300 而逻辑 250x150）—— 正是 Steam 那种组合，
        # 也是「缓冲尺寸 != 逻辑尺寸」时 X11 采样换算出错的前提。
        spawn([str(ROOT / 'build/xpopup'), 'owner', '0', '0', '500', '300',
               'scale=2'])
        time.sleep(6.0)  # let startup animations settle: FX re-bakes on commit, then freezes

        # 等启动动画彻底结束，装饰缓存这之后才“冻结”（再没有摆放/动画推版本号）。
        time.sleep(4.0)

        shot = run / 'shot.png'
        subprocess.check_output(['grim', '-t', 'png', str(shot)], env=env, timeout=20)
        size, blue, total, green = count_blue(shot)
        pct = blue * 100.0 / total
        print('screenshot %dx%d, blue %d (%.2f%%), green %d'
              % (size[0], size[1], blue, pct, green))

        info = subprocess.check_output(
            [str(ROOT / 'build/vidowmsg'), 'windows'], env=env, timeout=10, text=True)
        import json
        rows = []
        for w in json.loads(info):
            if w.get('kind') == 'x11':
                surf = w.get('surface') or {}
                rows.append(w)
                print('x11 window: %r geom=%s surface=%sx%s buffer=%sx%s '
                      'dst=%sx%s fx=%s uv=(%s,%s)' % (
                          (w.get('title') or '')[:16], w.get('geometry'),
                          surf.get('width'), surf.get('height'),
                          surf.get('buffer_width'), surf.get('buffer_height'),
                          surf.get('dst_width'), surf.get('dst_height'),
                          surf.get('fx_active'), surf.get('tex_du'), surf.get('tex_dv')))
        # 几何回归：X11 窗口声明 500x300（scale=2 缓冲 622x696 是窗口管理器加框后的），
        # 逻辑尺寸必须落在 250x150 附近 —— 不是 125x75（被多除一次）。
        if rows:
            g = rows[0].get('geometry') or {}
            surf = rows[0].get('surface') or {}
            bw = surf.get('buffer_width') or 0
            gw = g.get('width') or 0
            # geometry is the layout slot, not the FX sampling geometry. Check
            # the actual frame too: the old assertion missed the extra /2 bug.
            fw = surf.get('frame_width') or 0
            fh = surf.get('frame_height') or 0
            dw = surf.get('dst_width') or 0
            dh = surf.get('dst_height') or 0
            du = surf.get('tex_du') or 0
            dv = surf.get('tex_dv') or 0
            # Zero UV selects fx.c's legacy whole-surface ratio (w/src_w=1).
            whole_uv = (du == 0 and dv == 0) or (abs(du - 1) < .001 and abs(dv - 1) < .001)
            ok_geom = (abs(gw - bw / 2) <= 2 and abs(fw - dw) <= 1
                       and abs(fh - dh) <= 1 and whole_uv)
            print('actual FX frame %dx%d, scene destination %dx%d, whole-source=%s'
                  % (fw, fh, dw, dh, whole_uv))
            print('geometry check: buffer %d / 2 = %d, geom width = %d -> %s'
                  % (bw, bw // 2, gw, 'OK' if ok_geom else 'MISMATCH'))
            geom_ok = ok_geom
        win = (run / 'vidow.log').read_text(errors='replace')
        hit = re.search(r'x11: .*缓冲 (\d+)x(\d+) 按 \d+ 倍声明 -> 场景 (\d+)x(\d+)', win)
        if hit:
            print('x11 buffer %sx%s -> scene %sx%s' % hit.groups())

        # 窗口逻辑 250x150=37500 逻辑像素，缩到 1280x720 截图里约 0.28% 起步；
        # 修好前是 0 个蓝像素（全黑），修好后应当有整块窗口那么多。
        ok = blue > 1000 and geom_ok
        print('PASS X11 content visible through FX' if ok
              else 'FAIL X11 window is black through FX')
        return 0 if ok else 1
    finally:
        for p in reversed(procs):
            if p.poll() is None:
                os.killpg(p.pid, signal.SIGTERM)
                try:
                    p.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    os.killpg(p.pid, signal.SIGKILL)
        log.close()
        if args.keep:
            print('artifacts:', run)


if __name__ == '__main__':
    raise SystemExit(main())
