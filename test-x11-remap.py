#!/usr/bin/env python3
"""Isolated X11 withdraw/remap pixel regression. Never uses the desktop display."""
import ctypes
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import tempfile
import time
from PIL import Image

ROOT = Path(__file__).resolve().parent
run = Path(tempfile.mkdtemp(prefix='vidow-x11-remap-'))
run.chmod(0o700)
conf = run/'config.lua'
conf.write_text('xwayland=true\nxwayland_scale="auto"\nvidow.output{name="HEADLESS-1",scale=2,x=0,y=0}\n')
env = dict(os.environ, XDG_RUNTIME_DIR=str(run), XDG_CONFIG_HOME=str(run/'config'), XDG_DATA_HOME=str(run/'data'), DBUS_SESSION_BUS_ADDRESS='unix:path='+str(run/'no-bus'), WLR_BACKENDS='headless', WLR_HEADLESS_OUTPUTS='1', WLR_RENDERER='gles2', WLR_LIBINPUT_NO_DEVICES='1', VIDOW_CONFIG=str(conf), VIDOW_LOG='debug', VIDOW_PORTAL_NO_RESTART='1', WAYLAND_DISPLAY='', DISPLAY='')
children=[]
f=(run/'vidow.log').open('w')
def spawn(args):
    p=subprocess.Popen(args,env=env,stdout=f,stderr=f,start_new_session=True)
    children.append(p)
    return p
def msg(*args):
    return subprocess.check_output([str(ROOT/'build/vidowmsg'),*args],env=env,text=True,timeout=10)
def pixels(label):
    shot=run/(label+'.png')
    subprocess.run(['grim',str(shot)],env=env,check=True,timeout=10)
    image=Image.open(shot).convert('RGB')
    blue=sum(b>80 and b>r*2+20 and b>g*2+20 for r,g,b in image.getdata())
    assert blue>image.width*image.height*.15,(label,blue)
    print('PASS',label,'blue content pixels:',blue,flush=True)
try:
    spawn([os.environ.get('VIDOW_TEST_BINARY', str(ROOT/'build/vidow'))])
    for _ in range(100):
        text=(run/'vidow.log').read_text(errors='replace')
        wl=re.findall(r'WAYLAND_DISPLAY=([\w.-]+)',text)
        xd=re.findall(r'DISPLAY=(:\d+)',text)
        if wl and xd:
            env.update(WAYLAND_DISPLAY=wl[-1],DISPLAY=xd[-1]);break
        time.sleep(.1)
    else: raise RuntimeError('isolated startup timeout')
    subprocess.run(['xrandr','--query'],env=env,check=True,stdout=subprocess.DEVNULL,timeout=10)
    time.sleep(.5)  # Let the lazily started XWM finish claiming its selection.
    spawn([str(ROOT/'build/xpopup'),'owner','0','0','500','300'])
    for _ in range(50):
        view=next((w for w in json.loads(msg('windows')) if w.get('title')=='xp-owner'),None)
        if view is not None: break
        time.sleep(.1)
    else: raise RuntimeError('X11 test window did not map')
    time.sleep(.7)
    xid=view['x11']['window_id']
    lib=ctypes.CDLL('libX11.so.6')
    lib.XOpenDisplay.argtypes=[ctypes.c_char_p];lib.XOpenDisplay.restype=ctypes.c_void_p
    for name in ('XUnmapWindow','XMapWindow'):
        getattr(lib,name).argtypes=[ctypes.c_void_p,ctypes.c_ulong]
    lib.XFlush.argtypes=[ctypes.c_void_p];lib.XCloseDisplay.argtypes=[ctypes.c_void_p]
    display=lib.XOpenDisplay(env['DISPLAY'].encode())
    assert display
    try:
        pixels('first-map')
        for i in range(3):
            lib.XUnmapWindow(display,xid);lib.XFlush(display)
            time.sleep(.5)
            lib.XMapWindow(display,xid);lib.XFlush(display)
            time.sleep(.8)
            pixels('remap-'+str(i+1))
        # Install a solid 64px X cursor and inspect its 2x output mapping.
        class XColor(ctypes.Structure):
            _fields_=[('pixel',ctypes.c_ulong),('red',ctypes.c_ushort),('green',ctypes.c_ushort),('blue',ctypes.c_ushort),('flags',ctypes.c_byte),('pad',ctypes.c_byte)]
        lib.XCreateBitmapFromData.argtypes=[ctypes.c_void_p,ctypes.c_ulong,ctypes.c_char_p,ctypes.c_uint,ctypes.c_uint]
        lib.XCreateBitmapFromData.restype=ctypes.c_ulong
        lib.XCreatePixmapCursor.argtypes=[ctypes.c_void_p,ctypes.c_ulong,ctypes.c_ulong,ctypes.POINTER(XColor),ctypes.POINTER(XColor),ctypes.c_uint,ctypes.c_uint]
        lib.XCreatePixmapCursor.restype=ctypes.c_ulong
        lib.XDefineCursor.argtypes=[ctypes.c_void_p,ctypes.c_ulong,ctypes.c_ulong]
        bitmap=lib.XCreateBitmapFromData(display,xid,b'\xff'*512,64,64)
        color=XColor(0,0,65535,0,7,0)
        cursor=lib.XCreatePixmapCursor(display,bitmap,bitmap,ctypes.byref(color),ctypes.byref(color),0,0)
        lib.XDefaultRootWindow.argtypes=[ctypes.c_void_p]
        lib.XDefaultRootWindow.restype=ctypes.c_ulong
        lib.XDefineCursor(display,lib.XDefaultRootWindow(display),cursor)
        lib.XDefineCursor(display,xid,cursor);lib.XFlush(display)
        subprocess.run([str(ROOT/'build/vdrag'),'2000','4000','3000','6000'],env=env,check=True,timeout=10)
        time.sleep(.3)
        subprocess.run([str(ROOT/'build/vdrag'),'2500','5000','2500','5000'],env=env,check=True,timeout=10)
        time.sleep(.3)
        print('CURSOR',msg('cursor'),flush=True)
        text=(run/'vidow.log').read_text(errors='replace')
        assert text.count('x11: associate xp-owner')>=2,'did not exercise reassociation'
        print('PASS reassociation path exercised',flush=True)
    finally: lib.XCloseDisplay(display)
finally:
    for p in reversed(children):
        if p.poll() is None:
            os.killpg(p.pid,signal.SIGTERM)
            try:p.wait(timeout=5)
            except subprocess.TimeoutExpired:
                os.killpg(p.pid,signal.SIGKILL);p.wait(timeout=5)
    f.close()
    print('isolated artifacts:',run,flush=True)
