#!/usr/bin/env python3
"""Isolated X11 fullscreen geometry stability during workspace transitions."""
import os, subprocess, tempfile, pathlib, time, json, re, signal
ROOT = pathlib.Path(__file__).resolve().parent
run = pathlib.Path(tempfile.mkdtemp(prefix='vidow-fullscreen-ws-'))
run.chmod(0o700)
conf = run / 'config.lua'
conf.write_text('xwayland=true\nxwayland_scale="auto"\nvidow.output{name="HEADLESS-1",scale=1,x=0,y=0}\n')
env = dict(os.environ, VIDOW_PORTAL_NO_RESTART='1', XDG_CONFIG_HOME=str(run/'config'), XDG_DATA_HOME=str(run/'data'), DBUS_SESSION_BUS_ADDRESS='unix:path='+str(run/'no-bus'), XDG_RUNTIME_DIR=str(run), WLR_BACKENDS='headless', WLR_HEADLESS_OUTPUTS='1', WLR_RENDERER='gles2', WLR_LIBINPUT_NO_DEVICES='1', VIDOW_CONFIG=str(conf), VIDOW_LOG='debug', WAYLAND_DISPLAY='', DISPLAY='')
children = []
f = (run/'vidow.log').open('w')
def spawn(args, out=f):
    p = subprocess.Popen(args, env=env, stdout=out, stderr=out, start_new_session=True)
    children.append(p)
    return p
def call(args):
    return subprocess.check_output(args, env=env, text=True, timeout=10)
def msg(*args):
    return call([str(ROOT/'build/vidowmsg'), *args])
try:
    spawn([str(ROOT/'build/vidow')])
    for _ in range(100):
        text = (run/'vidow.log').read_text(errors='replace')
        wl = re.findall(r'WAYLAND_DISPLAY=([\w.-]+)', text)
        xd = re.findall(r'DISPLAY=(:\d+)', text)
        if wl and xd:
            env.update(WAYLAND_DISPLAY=wl[-1], DISPLAY=xd[-1])
            break
        time.sleep(.1)
    else:
        raise RuntimeError('isolated startup timeout')
    evlog = (run/'xev.log').open('w')
    spawn(['stdbuf','-oL','xev','-name','fullscreen-ws-probe','-geometry','500x300'], evlog)
    time.sleep(1)
    view = next(w for w in json.loads(msg('windows')) if w.get('title') == 'fullscreen-ws-probe')
    wid = str(view['id'])
    msg('set-fullscreen', '--id', wid, 'true')
    time.sleep(.7)
    start = len((run/'xev.log').read_text(errors='replace'))
    initial = next(w for w in json.loads(msg('windows')) if str(w['id']) == wid)
    for _ in range(3):
        msg('focus-workspace','next')
        time.sleep(.35)
        msg('focus-workspace','previous')
        time.sleep(.35)
        current = next(w for w in json.loads(msg('windows')) if str(w['id']) == wid)
        assert current['fullscreen'], current
        assert current['geometry']['width'] == initial['geometry']['width'], current
        assert current['geometry']['height'] == initial['geometry']['height'], current
    events = (run/'xev.log').read_text(errors='replace')[start:]
    assert 'ConfigureNotify event' not in events, events
    print('PASS 3 workspace round trips: fullscreen retained; zero X ConfigureNotify')
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
    print('isolated artifacts:', run)
