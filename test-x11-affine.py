#!/usr/bin/env python3
"""Isolated two-output X/Wayland input check; never connects to live compositor."""
import os,subprocess,tempfile,pathlib,time,json,re,signal
root=pathlib.Path(__file__).resolve().parent;run=pathlib.Path(tempfile.mkdtemp(prefix='vidow-affine-'));os.chmod(run,0o700)
conf=run/'config.lua';conf.write_text('xwayland=true\nxwayland_scale="auto"\nvidow.output{ name="HEADLESS-1", scale=1, x=0,y=0 }\nvidow.output{ name="HEADLESS-2", scale=2, x=1920,y=0 }\n')
e=dict(os.environ,VIDOW_PORTAL_NO_RESTART='1',XDG_CONFIG_HOME=str(run/'config'),XDG_DATA_HOME=str(run/'data'),DBUS_SESSION_BUS_ADDRESS='unix:path='+str(run/'no-session-bus'),XDG_RUNTIME_DIR=str(run),WLR_BACKENDS='headless',WLR_HEADLESS_OUTPUTS='2',WLR_LIBINPUT_NO_DEVICES='1',VIDOW_CONFIG=str(conf),VIDOW_LOG='debug',WAYLAND_DISPLAY='',DISPLAY='')
log=open(run/'vidow.log','w');proc=subprocess.Popen([str(root/'build/vidow')],env=e,stdout=log,stderr=log,start_new_session=True);clients=[]
def call(args,check=True):
 p=subprocess.run(args,env=e,capture_output=True,text=True,timeout=10)
 if check and p.returncode:raise RuntimeError(f'{args}: {p.returncode}: {p.stderr} {p.stdout}')
 return p.stdout
def msg(*a):return call([str(root/'build/vidowmsg'),*a])
try:
 for _ in range(60):
  text=(run/'vidow.log').read_text(errors='replace');w=re.findall(r'WAYLAND_DISPLAY=([\w-]+)',text);x=re.findall(r'DISPLAY=(:\d+)',text)
  if w and x:break
  time.sleep(.1)
 assert w and x, 'test display not found'
 e.update(WAYLAND_DISPLAY=w[-1],DISPLAY=x[-1]);print('ISOLATED',run,e['WAYLAND_DISPLAY'],e['DISPLAY'],flush=True)
 msg('output-mode','HEADLESS-1','1920','1080');msg('output-mode','HEADLESS-2','3840','2160');time.sleep(1)
 outputs=json.loads(msg('outputs'));print('WAYLAND',[(o['name'],o['scale'],o['layout']) for o in outputs],flush=True)
 randr=call(['xrandr','--query']);print('\n'.join(l for l in randr.splitlines() if l.startswith('Screen') or ' connected' in l),flush=True)
 assert 'current 7680 x 2160' in randr, 'X union incorrect'
 evlog=open(run/'xev.log','w');ev=subprocess.Popen(['stdbuf','-oL','xev','-name','affine-probe','-geometry','500x300'],env=e,stdout=evlog,stderr=evlog,start_new_session=True);clients.append(ev);time.sleep(2)
 windows=json.loads(msg('windows'));v=next(v for v in windows if v.get('title')=='affine-probe');msg('move-window-to-output','--id',str(v['id']),'HEADLESS-2');time.sleep(1)
 window_id=v['id']
 v=next(z for z in json.loads(msg('windows')) if z['id']==window_id);g=v['geometry'];print('WINDOW',json.dumps(v),flush=True)
 layoutw=max(o['layout']['x']+o['layout']['width'] for o in outputs);layouth=max(o['layout']['y']+o['layout']['height'] for o in outputs)
 for fraction in [.1,.5,.9]:
  px=g['x']+g['width']*fraction;py=g['y']+g['height']*.5
  ax=round(px*10000/layoutw);ay=round(py*10000/layouth)
  before=(run/'xev.log').read_text(errors='replace').count('ButtonPress event')
  call([str(root/'build/vdrag'),str(ax),str(ay),str(ax),str(ay)]);time.sleep(.2)
  text=(run/'xev.log').read_text(errors='replace');after=text.count('ButtonPress event');assert after>before,f'click missing fraction {fraction}'
  print('CLICK PASS',fraction,flush=True)
 # Real pointer motion while pressed: capture client coordinates, not XTest.
 ax=round((g['x']+g['width']*.2)*10000/layoutw);bx=round((g['x']+g['width']*.8)*10000/layoutw);ay=round((g['y']+g['height']*.5)*10000/layouth)
 call([str(root/'build/vdrag'),str(ax),str(ay),str(bx),str(ay)]);time.sleep(.2)
 text=(run/'xev.log').read_text(errors='replace');print('INPUT press/release',text.count('ButtonPress event'),text.count('ButtonRelease event'),flush=True)
 assert text.count('ButtonPress event')==text.count('ButtonRelease event')==4
 blocks=re.split(r'\n\n+',text)
 drag=[b for b in blocks if 'MotionNotify event' in b and 'state 0x100' in b]
 pts=[tuple(map(int,re.search(r'time \d+, \(([-\d]+),([-\d]+)\)',b).groups())) for b in drag]
 assert len(pts)>=10 and all(a[0]<b[0] for a,b in zip(pts,pts[1:])),pts
 print('DRAG PASS',pts[0],pts[-1],len(pts),flush=True)
 # A client-placed popup in X coordinates must map back to the same Wayland output.
 poplog=open(run/'popup.log','w');pop=subprocess.Popen([str(root/'build/xpopup'),'popup','or','4200','400','600','300'],env=e,stdout=poplog,stderr=poplog,start_new_session=True);clients.append(pop);time.sleep(1)
 # IPC windows intentionally omits unmanaged popups; inspect compositor placement
 # and prove input delivery to the popup itself instead.
 text=(run/'vidow.log').read_text(errors='replace')
 assert 'place xp-popup -> 2100,200 300x150' in text, 'popup placement incorrect'
 # First activation of an unfocused override-redirect client may be consumed
 # by Xwayland focus establishment. Warm up, then require both test clicks.
 call([str(root/'build/vdrag'),str(round(2250*10000/layoutw)),str(round(275*10000/layouth)),str(round(2250*10000/layoutw)),str(round(275*10000/layouth))]);time.sleep(.3)
 baseline=(run/'popup.log').read_text().count('click ')
 for frac in [.1,.9]:
  px=2100+300*frac;py=275
  call([str(root/'build/vdrag'),str(round(px*10000/layoutw)),str(round(py*10000/layouth)),str(round(px*10000/layoutw)),str(round(py*10000/layouth))]);time.sleep(.2)
 clicks=re.findall(r'click (\d+),(\d+) btn1',(run/'popup.log').read_text())
 clicks=clicks[baseline:]
 assert len(clicks)==2,clicks
 assert abs(int(clicks[0][0])-60)<=3 and abs(int(clicks[1][0])-540)<=3,clicks
 print('POPUP PASS placement 2100,200 300x150; clicks',clicks,flush=True)
 # Move the normal window back to the 1x output and validate both edges.
 os.killpg(pop.pid,signal.SIGTERM);pop.wait(timeout=3)
 msg('move-window-to-output','--id',str(v['id']),'HEADLESS-1');time.sleep(.6)
 left=next(z for z in json.loads(msg('windows')) if z['id']==v['id']);lg=left['geometry']
 for frac in [.1,.9]:
  ax=round((lg['x']+lg['width']*frac)*10000/layoutw);ay=round((lg['y']+lg['height']*.5)*10000/layouth)
  before=(run/'xev.log').read_text(errors='replace').count('ButtonPress event')
  call([str(root/'build/vdrag'),str(ax),str(ay),str(ax),str(ay)]);time.sleep(.2)
  assert (run/'xev.log').read_text(errors='replace').count('ButtonPress event')>before,'1x click missing'
 print('CROSS-OUTPUT PASS right 2x -> left 1x, both edges receive clicks',flush=True)
 print('RESULT PASS (Wayland unchanged, X union, real clicks, continuous grab, popup geometry)',run,flush=True)
finally:
 for p in clients:
  if p.poll() is None:os.killpg(p.pid,signal.SIGTERM)
 if proc.poll() is None:os.killpg(proc.pid,signal.SIGTERM)
 try:proc.wait(timeout=5)
 except subprocess.TimeoutExpired:os.killpg(proc.pid,signal.SIGKILL);proc.wait()
 print('ARTIFACTS',run,flush=True)
