#!/usr/bin/env python3
"""Read-only synchronized XFixes/output cursor size probe (uses current desktop)."""
import ctypes as c
import json
from pathlib import Path
import subprocess
import time

class Cursor(c.Structure):
    _fields_ = [('x', c.c_short), ('y', c.c_short), ('width', c.c_ushort),
                ('height', c.c_ushort), ('xhot', c.c_ushort), ('yhot', c.c_ushort),
                ('serial', c.c_ulong), ('pixels', c.POINTER(c.c_ulong)),
                ('atom', c.c_ulong), ('name', c.c_char_p)]
x = c.CDLL('libX11.so.6')
f = c.CDLL('libXfixes.so.3')
x.XOpenDisplay.argtypes = [c.c_char_p]
x.XOpenDisplay.restype = c.c_void_p
x.XFree.argtypes = [c.c_void_p]
x.XCloseDisplay.argtypes = [c.c_void_p]
f.XFixesGetCursorImage.argtypes = [c.c_void_p]
f.XFixesGetCursorImage.restype = c.POINTER(Cursor)
msg = str(Path(__file__).resolve().parents[1] / 'build/vidowmsg')
display = x.XOpenDisplay(None)
if not display:
    raise SystemExit('Cannot open current DISPLAY; set DISPLAY to the target Xwayland')
try:
    for _ in range(8):
        before = json.loads(subprocess.check_output([msg, 'cursor'], text=True, timeout=3))
        ptr = f.XFixesGetCursorImage(display)
        if not ptr:
            raise RuntimeError('XFixes returned no cursor')
        v = ptr.contents
        source = dict(width=v.width, height=v.height, hotspot=[v.xhot, v.yhot], serial=v.serial)
        x.XFree(ptr)
        after = json.loads(subprocess.check_output([msg, 'cursor'], text=True, timeout=3))
        stable = all(before.get(k) == after.get(k) for k in ('x', 'y', 'image', 'hardware'))
        print(json.dumps(dict(stable=stable, source=source, output=after)), flush=True)
        time.sleep(.25)
finally:
    x.XCloseDisplay(display)
