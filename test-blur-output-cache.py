#!/usr/bin/env python3
"""Isolated dual-output regression: idle blur elision and output-local invalidation.
Never connects to the real compositor. Leaves its bounded logs in a temp directory.
"""
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parent

def main():
    run = Path(tempfile.mkdtemp(prefix='vidow-blur-cache-'))
    run.chmod(0o700)
    config = run / 'config.lua'
    config.write_text('decorations=true\nvidow.output{name="HEADLESS-1",scale=1,x=0,y=0}\nvidow.output{name="HEADLESS-2",scale=1,x=1280,y=0}\n')
    env = dict(os.environ, XDG_RUNTIME_DIR=str(run),
        XDG_CONFIG_HOME=str(run / 'config'), XDG_DATA_HOME=str(run / 'data'),
        DBUS_SESSION_BUS_ADDRESS='unix:path=' + str(run / 'no-bus'),
        WLR_BACKENDS='headless', WLR_HEADLESS_OUTPUTS='2',
        WLR_LIBINPUT_NO_DEVICES='1', WLR_RENDERER='gles2',
        VIDOW_PORTAL_NO_RESTART='1', VIDOW_CONFIG=str(config), VIDOW_LOG='debug',
        VIDOW_CAPTURE_FPS='1000',  # Exercise fallback before natural headless frames.
        WAYLAND_DISPLAY='', DISPLAY='')
    children = []
    log = run / 'vidow.log'
    f = log.open('w')
    def spawn(args):
        p = subprocess.Popen(args, env=env, stdout=f, stderr=f, start_new_session=True)
        children.append(p)
        return p
    def msg(*args):
        return subprocess.check_output([str(ROOT / 'build/vidowmsg'), *args], env=env, text=True, timeout=10)
    def counts(text):
        return {name: text.count('blur: shared rebuild ' + name + ' ') for name in ('HEADLESS-1', 'HEADLESS-2')}
    try:
        compositor = spawn([str(ROOT / 'build/vidow')])
        for _ in range(100):
            text = log.read_text(errors='replace')
            wl = re.findall(r'WAYLAND_DISPLAY=([\w.-]+)', text)
            if wl:
                env['WAYLAND_DISPLAY'] = wl[-1]
                break
            if compositor.poll() is not None:
                raise RuntimeError('isolated compositor exited: ' + str(log))
            time.sleep(.1)
        else:
            raise RuntimeError('isolated display startup timeout')
        # Force both output commits while there are no blur consumers.
        for name in ('HEADLESS-1', 'HEADLESS-2'):
            subprocess.run(['grim', '-o', name, str(run / (name + '.png'))], env=env, check=True, timeout=15)
        assert not any(counts(log.read_text(errors='replace')).values()), 'blur rebuilt without consumers'
        print('PASS no consumers: zero shared blur rebuilds', flush=True)
        # Record static output 1 and require a logged fallback invocation, so
        # this checks the pending-output filter rather than only natural frames.
        start = log.stat().st_size
        recorder = spawn(['wf-recorder', '-D', '-o', 'HEADLESS-1', '-f', str(run / 'still.mkv')])
        time.sleep(2)
        assert recorder.poll() is None, 'recorder exited before capture'
        os.killpg(recorder.pid, signal.SIGINT)
        recorder.wait(timeout=10)
        assert recorder.returncode == 0, 'recording failed'
        watchdog = log.read_text(errors='replace')[start:]
        assert 'capture watchdog: render HEADLESS-1' in watchdog, 'watchdog path not exercised'
        assert 'capture watchdog: render HEADLESS-2' not in watchdog, 'unrequested output rendered by watchdog'
        assert (run / 'still.mkv').stat().st_size > 0, 'empty static recording'
        print('PASS static recording: watchdog rendered only requested output', flush=True)
        for name in ('HEADLESS-1', 'HEADLESS-2'):
            p = spawn([str(ROOT / 'build/bgblur'), '--full'])
            for _ in range(50):
                wins = json.loads(msg('windows'))
                win = next((w for w in wins if w.get('pid') == p.pid), None)
                if win:
                    msg('move-window-to-output', '--id', str(win['id']), name)
                    break
                time.sleep(.1)
            else:
                raise RuntimeError('blur client did not map')
        time.sleep(1)
        # Repaint a background layer on output 1 without map/unmap/layout churn.
        wall = (ROOT / 'tools/wall.py').read_text()
        wall = wall.replace('Gtk.main()\n', 'from gi.repository import GLib\nGLib.timeout_add(100, lambda: ([w.queue_draw() for w in Gtk.Window.list_toplevels()], True)[1])\nGtk.main()\n')
        spawn(['python3', '-c', wall, '--output', 'HEADLESS-1'])
        time.sleep(2)
        before = counts(log.read_text(errors='replace'))
        # Repeatedly commit output 2 as well, to expose accidental global invalidation.
        for _ in range(4):
            subprocess.run(['grim', '-o', 'HEADLESS-2', str(run / 'check.png')], env=env, check=True, timeout=15)
            time.sleep(.2)
        after = counts(log.read_text(errors='replace'))
        assert after['HEADLESS-1'] > before['HEADLESS-1'], (before, after)
        assert after['HEADLESS-2'] == before['HEADLESS-2'], (before, after)
        assert before['HEADLESS-2'] > 0, 'output 2 never built its initial blur cache'
        print('PASS output-local background commits:', before, '->', after, flush=True)
        return 0
    finally:
        for p in reversed(children):
            if p.poll() is None:
                os.killpg(p.pid, signal.SIGTERM)
                try:
                    p.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    os.killpg(p.pid, signal.SIGKILL)
                    p.wait(timeout=5)
        f.close()
        print('isolated logs:', run, flush=True)

if __name__ == '__main__':
    raise SystemExit(main())
