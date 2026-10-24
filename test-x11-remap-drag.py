#!/usr/bin/env python3
"""X11 remap + held-drag regression; isolated headless desktop only.
Usage: python3 test-x11-remap-drag.py [--binary build/vidow] [--scale 1|2]
Artifacts remain in the printed temporary directory.
"""
import argparse
import os,sys,json,re,time,signal,subprocess,tempfile
from pathlib import Path
from PIL import Image
ap=argparse.ArgumentParser();ap.add_argument('--binary',default='build/vidow');ap.add_argument('--scale',type=int,choices=[1,2],default=1);args=ap.parse_args()
ROOT=Path(__file__).resolve().parent; run=Path(tempfile.mkdtemp(prefix='vidow-remap-drag-audit-')); run.chmod(0o700)
samples={}
conf=run/'config.lua'; conf.write_text('xwayland=true\njelly='+os.environ.get('AUDIT_JELLY','true')+'\nwindow_move_ms=0\nvidow.bind("Mod+mouse-left", "interactive-move")\n')
os.mkfifo(run/'actions')
env=dict(os.environ,XDG_RUNTIME_DIR=str(run),XDG_CONFIG_HOME=str(run/'config'),XDG_DATA_HOME=str(run/'data'),DBUS_SESSION_BUS_ADDRESS='unix:path='+str(run/'no-bus'),WLR_BACKENDS='headless',WLR_HEADLESS_OUTPUTS='1',WLR_RENDERER='gles2',WLR_LIBINPUT_NO_DEVICES='1',VIDOW_CONFIG=str(conf),VIDOW_LOG='debug',VIDOW_JELLY_DEBUG='1',VIDOW_TEST_ACTION='file:'+str(run/'actions'),VIDOW_PORTAL_NO_RESTART='1',WAYLAND_DISPLAY='',DISPLAY='')
children=[]; log=(run/'vidow.log').open('w')
def spawn(a):
 p=subprocess.Popen(a,env=env,stdout=log,stderr=log,start_new_session=True);children.append(p);return p
def msg(*a): return subprocess.check_output([str(ROOT/'build/vidowmsg'),*a],env=env,text=True,timeout=10)
def win(): return next(w for w in json.loads(msg('windows')) if w.get('title')=='xp-owner')
def shot(label):
 path=run/(label+'.png');subprocess.run(['grim',str(path)],env=env,check=True,timeout=10)
 im=Image.open(path).convert('RGB'); pts=[(i%im.width,i//im.width) for i,(r,g,b) in enumerate(im.getdata()) if b>=150 and r<60 and g<60]
 print(label,'blue',len(pts),'bbox',([min(x for x,y in pts),min(y for x,y in pts),max(x for x,y in pts),max(y for x,y in pts)] if pts else None),flush=True)
 samples[label]=len(pts)
 (run/(label+'.json')).write_text(json.dumps(json.loads(msg('windows')),indent=2))
def act(s,delay=.2):
 fifo.write(s+'\n');fifo.flush();time.sleep(delay)
def drag(label):
 w=win();g=w['geometry'];cx=g['x']+min(140,g['width']//2);cy=g['y']+min(140,g['height']//2)
 act(f'pointer-motion {cx} {cy}');act('pointer-button left press mod');
 for n in range(1,5):act(f'pointer-motion {cx+30*n} {cy+15*n}',.12)
 shot(label+'-held');act('pointer-button left release mod',.8);shot(label+'-released')
try:
 p=spawn([str(Path(args.binary).resolve())]);
 for _ in range(120):
  text=(run/'vidow.log').read_text(errors='replace');wl=re.findall(r'WAYLAND_DISPLAY=([\w.-]+)',text);xd=re.findall(r'DISPLAY=(:\d+)',text)
  if wl and xd:env.update(WAYLAND_DISPLAY=wl[-1],DISPLAY=xd[-1]);break
  if p.poll() is not None:raise RuntimeError('compositor exited')
  time.sleep(.1)
 else:raise RuntimeError('startup timeout')
 fifo=(run/'actions').open('w')
 subprocess.run(['xrandr','--query'],env=env,check=True,stdout=subprocess.DEVNULL,timeout=10);time.sleep(.8)
 if args.scale==2:
  msg('output-scale','HEADLESS-1','2');time.sleep(.8)
  text=(run/'vidow.log').read_text(errors='replace');env['DISPLAY']=re.findall(r'DISPLAY=(:\d+)',text)[-1]
 spawn([str(ROOT/'build/xpopup'),'owner','0','0','500','300',f'scale={args.scale}']);time.sleep(2)
 shot('first-map');drag('first-drag');xid=str(win()['x11']['window_id'])
 subprocess.run(['xdotool','windowunmap',xid],env=env,check=True,timeout=10);time.sleep(.6)
 subprocess.run(['xdotool','windowmap',xid],env=env,check=True,timeout=10);time.sleep(2)
 shot('second-map');drag('second-drag')
 text=(run/'vidow.log').read_text(errors='replace');print('associate count',text.count('x11: associate xp-owner'),'jelly begin count',text.count('起手'),flush=True)
 for label in ['first-map','first-drag-held','second-map','second-drag-held']:
  assert samples[label]>1000, f'{label}: missing client content ({samples[label]} pixels)'
 assert text.count('x11: associate xp-owner')>=2, 'remap did not re-associate surface'
 if os.environ.get('AUDIT_JELLY','true')=='true':
  assert text.count('起手')>=2, 'test did not exercise jelly twice'
 print(f'PASS X11 remap and held drag at {args.scale}x',flush=True)
finally:
 for p in reversed(children):
  if p.poll() is None:
   os.killpg(p.pid,signal.SIGTERM)
   try:p.wait(timeout=5)
   except subprocess.TimeoutExpired:os.killpg(p.pid,signal.SIGKILL);p.wait(timeout=5)
 log.close();print('ARTIFACT',run,flush=True)
