#!/usr/bin/env python3
"""Isolated proof that cursor_size from config reload rebuilds the cursor at once."""
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parent
run = Path(tempfile.mkdtemp(prefix='vidow-cursor-reload-'))
run.chmod(0o700)
conf = run / 'config.lua'
conf.write_text('cursor_size = 32\n')
env = dict(os.environ, XDG_RUNTIME_DIR=str(run), XDG_CONFIG_HOME=str(run / 'config'),
           XDG_DATA_HOME=str(run / 'data'),
           DBUS_SESSION_BUS_ADDRESS='unix:path=' + str(run / 'no-bus'),
           WLR_BACKENDS='headless', WLR_HEADLESS_OUTPUTS='1', WLR_RENDERER='gles2',
           WLR_LIBINPUT_NO_DEVICES='1', VIDOW_CONFIG=str(conf), VIDOW_LOG='info',
           VIDOW_PORTAL_NO_RESTART='1')
env.pop('XCURSOR_SIZE', None)
children = []
log = (run / 'vidow.log').open('w')


def spawn(args):
    p = subprocess.Popen(args, env=env, stdout=log, stderr=log, start_new_session=True)
    children.append(p)
    return p


def msg(*args):
    return subprocess.check_output([str(ROOT / 'build/vidowmsg'), *args], env=env,
                                   text=True, timeout=10)


def image_size():
    return json.loads(msg('cursor'))['image']


try:
    spawn([os.environ.get('VIDOW_TEST_BINARY', str(ROOT / 'build/vidow'))])
    for _ in range(100):
        text = (run / 'vidow.log').read_text(errors='replace')
        wl = re.findall(r'WAYLAND_DISPLAY=([\w.-]+)', text)
        if wl:
            env['WAYLAND_DISPLAY'] = wl[-1]
            break
        time.sleep(.1)
    else:
        raise RuntimeError('isolated compositor startup timeout')
    time.sleep(1.0)
    first = image_size()
    print('startup cursor_size=32 ->', first, flush=True)
    conf.write_text('cursor_size = 64\n')
    msg('reload-config')
    time.sleep(1.0)
    second = image_size()
    print('reload config cursor_size=64 ->', second, flush=True)
    text = (run / 'vidow.log').read_text(errors='replace')
    starts = re.findall(r'cursor: 主题 \S+、尺寸 (\d+)', text)
    sizes = [int(first.split('x')[0]), int(second.split('x')[0])]
    print('cursor_apply log sizes:', starts, flush=True)
    # Xcursor 主题按缩放进位到最接近的一张图（Adwaita 30/72，Bibata 32/64），所以
    # 比的是趋势：reload 之后必须变大，并且 manager 确实被重建过。
    ok = len(starts) >= 2 and sizes[1] > sizes[0]
    print('PASS config reload rebuilt the cursor' if ok else 'FAIL cursor did not follow config reload', flush=True)
    raise SystemExit(0 if ok else 1)
finally:
    for p in reversed(children):
        if p.poll() is None:
            os.killpg(p.pid, signal.SIGTERM)
            try:
                p.wait(timeout=5)
            except subprocess.TimeoutExpired:
                os.killpg(p.pid, signal.SIGKILL)
    log.close()
    print('isolated artifacts:', run, flush=True)
