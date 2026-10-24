#!/usr/bin/env python3
"""Read-only repository test. Start only an isolated headless Vidow session."""
import os, pathlib, tempfile, subprocess, time, re, json, signal, hashlib
from Xlib import X, display, Xatom
from PIL import Image, ImageChops, ImageStat
ROOT=pathlib.Path(os.environ.get('VIDOW_TEST_ROOT',str(pathlib.Path(__file__).resolve().parent)))
BLUR=os.environ.get('VIDOW_TEST_POPUP_BLUR','false')
LATE=os.environ.get('VIDOW_TEST_POPUP_LATE','false')=='true'
GROW=os.environ.get('VIDOW_TEST_POPUP_GROW','false')=='true'
assert BLUR in ('false','true'), 'VIDOW_TEST_POPUP_BLUR must be false or true'
RUN=pathlib.Path(tempfile.mkdtemp(prefix='vidow-argb-')); RUN.chmod(0o700)
conf=RUN/'config.lua'
conf.write_text(('xwayland=true\nxwayland_scale="auto"\npopup_blur_enabled=false\nvidow.output{ name="HEADLESS-1", scale=1, x=0,y=0 }\nvidow.output{ name="HEADLESS-2", scale=2, x=1920,y=0 }\n').replace('popup_blur_enabled=false','popup_blur_enabled='+BLUR))
e=dict(os.environ,XDG_RUNTIME_DIR=str(RUN),WLR_BACKENDS='headless',WLR_HEADLESS_OUTPUTS='2',WLR_LIBINPUT_NO_DEVICES='1',VIDOW_CONFIG=str(conf),VIDOW_LOG='debug',WAYLAND_DISPLAY='',DISPLAY='')
e.update(VIDOW_PORTAL_NO_RESTART='1',XDG_CONFIG_HOME=str(RUN/'config'),XDG_DATA_HOME=str(RUN/'data'),DBUS_SESSION_BUS_ADDRESS='unix:path='+str(RUN/'no-session-bus'))
log=open(RUN/'vidow.log','w'); proc=subprocess.Popen([str(ROOT/'build/vidow')],env=e,stdout=log,stderr=log,start_new_session=True)
d=None; results=[]; failures=[]
def call(args):
 p=subprocess.run(args,env=e,capture_output=True,text=True,timeout=15)
 if p.returncode: raise RuntimeError((args,p.returncode,p.stdout,p.stderr))
 return p.stdout
def msg(*args): return call([str(ROOT/'build/vidowmsg'),*args])
def capture(output,path): call(['grim','-o',output,'-s','1',str(path)]); return Image.open(path).convert('RGB')
def descendants(pid):
 try:
  kids=[int(s) for s in pathlib.Path(f'/proc/{pid}/task/{pid}/children').read_text().split()]
 except FileNotFoundError:return []
 return kids+sum((descendants(k) for k in kids),[])
try:
 for _ in range(80):
  text=(RUN/'vidow.log').read_text(errors='replace'); w=re.findall(r'WAYLAND_DISPLAY=([\w-]+)',text); x=re.findall(r'DISPLAY=(:\d+)',text)
  if w and x:break
  if proc.poll() is not None:raise RuntimeError('compositor exited')
  time.sleep(.1)
 assert w and x,'isolated displays absent'
 assert x[-1]!=':0','refusing real desktop'
 e.update(WAYLAND_DISPLAY=w[-1],DISPLAY=x[-1]); print('ISOLATED',RUN,w[-1],x[-1],'PID',proc.pid,flush=True)
 msg('output-mode','HEADLESS-1','1920','1080');msg('output-mode','HEADLESS-2','3840','2160');time.sleep(1)
 print('OUTPUTS',msg('outputs'),flush=True)
 d=display.Display(e['DISPLAY']); screen=d.screen(); root=screen.root
 visuals=[(dep.depth,v) for dep in screen.allowed_depths for v in dep.visuals if v.visual_class==X.TrueColor]
 v32=next(v for depth,v in visuals if depth==32)
 print('VISUALS root_depth',screen.root_depth,'root_visual',hex(screen.root_visual),'ARGB',v32._data,flush=True)
 font=d.open_font('fixed')
 for output,xpos,scale,localx in [('HEADLESS-1',400,1,400),('HEADLESS-2',4200,2,180)]:
  for depth in [24,32]:
   visual=screen.root_visual if depth==24 else v32.visual_id
   cmap=root.create_colormap(visual,X.AllocNone)
   # Independent X virtual cells map 3840x2160 to 1920x1080.
   # Scene placement is verified independently of Wayland output scale.
   win=root.create_window(xpos,400,1 if GROW else 600,1 if GROW else 300,0,depth,X.InputOutput,visual,colormap=cmap,background_pixel=0,border_pixel=0,override_redirect=0 if LATE else 1,event_mask=X.ExposureMask|X.StructureNotifyMask)
   title=f'argb-depth-{depth}-{output}'
   win.set_wm_name(title);win.set_wm_class('argb-probe','ARGBProbe')
   if not LATE:
    win.change_property(d.intern_atom('_NET_WM_WINDOW_TYPE'),Xatom.ATOM,32,[d.intern_atom('_NET_WM_WINDOW_TYPE_POPUP_MENU')])
   baseline=capture(output,RUN/f'{output}-{depth}-baseline.png')
   win.map();d.sync();time.sleep(.3)
   if LATE:
    win.change_property(d.intern_atom('_NET_WM_WINDOW_TYPE'),Xatom.ATOM,32,[d.intern_atom('_NET_WM_WINDOW_TYPE_POPUP_MENU')])
    d.sync();time.sleep(.3)
   if GROW or LATE:
    win.configure(x=xpos,y=400,width=600,height=300);d.sync();time.sleep(.3)
   gc=win.create_gc(foreground=0xffe8e8e8 if depth==32 else 0xe8e8e8,font=font.id)
   def repaint():
    def fill(color,xx,yy,ww,hh):gc.change(foreground=color);win.fill_rectangle(gc,xx,yy,ww,hh)
    fill(0xffe8e8e8 if depth==32 else 0xe8e8e8,0,0,600,300)
    # Four quadrant patches: opaque red/green and premultiplied 50% red/blue.
    patches=[(0xffff3030,20,55),(0xff20d050,310,55),(0x80800000,20,170),(0x80000080,310,170)]
    for color,xx,yy in patches:fill(color if depth==32 else color&0xffffff,xx,yy,260,95)
    gc.change(foreground=0xff101010 if depth==32 else 0x101010);win.draw_text(gc,20,25,b'ARGB MENU: text + opaque + half alpha')
    gc.change(foreground=0xffffffff if depth==32 else 0xffffff);win.draw_text(gc,30,90,b'OPAQUE RED');win.draw_text(gc,320,90,b'OPAQUE GREEN');win.draw_text(gc,30,210,b'50% RED');win.draw_text(gc,320,210,b'50% BLUE')
   for frame in range(30):
    while d.pending_events():d.next_event()
    repaint();d.sync();time.sleep(.05)
   raw=win.get_image(0,0,600,300,X.ZPixmap,0xffffffff)
   (RUN/f'{title}.bgra').write_bytes(raw.data)
   src=Image.frombytes('RGBA',(600,300),raw.data,'raw','BGRA');src.save(RUN/f'{title}-source.png')
   text=(RUN/'vidow.log').read_text(errors='replace')
   places=re.findall(r'place '+re.escape(title)+r' -> ([-\d]+),([-\d]+) (\d+)x(\d+)',text)
   print('PLACE',title,places,flush=True)
   assert places, 'popup placement log not found'
   px,py,pw,ph=map(int,places[-1]); ox=0 if output=='HEADLESS-1' else 1920
   wanted=(200,200,300,150) if output=='HEADLESS-1' else (2100,200,300,150)
   assert (px,py,pw,ph)==wanted, (title,'incorrect scene placement',(px,py,pw,ph),wanted)
   assert raw.depth==depth, (title,'incorrect actual X depth',raw.depth)
   print('PICK',title,msg('pick-window',str(px+pw//2),str(py+ph//2)),flush=True)
   print('RAW_RGBA',title,[src.getpixel(p) for p in [(150,120),(450,120),(150,240),(450,240)]],flush=True)
   shot=capture(output,RUN/f'{title}-grim.png'); actual=shot.crop((px-ox,py,px-ox+pw,py+ph));actual.save(RUN/f'{title}-crop.png')
   # Composite source against screenshot of the same desktop region before mapping.
   bg=baseline.crop((px-ox,py,px-ox+pw,py+ph)).convert('RGBA')
   resized=src.resize((pw,ph),Image.Resampling.BILINEAR)
   if depth==24:resized.putalpha(255)
   else:
    # XRender ARGB pixels are premultiplied. Unpremultiply before PIL compositing.
    resized=Image.frombytes('RGBA',(600,300),raw.data,'raw','BGRa').resize((pw,ph),Image.Resampling.BILINEAR)
   expected=Image.alpha_composite(bg,resized).convert('RGB');expected.save(RUN/f'{title}-expected.png')
   delta=ImageChops.difference(actual,expected); mean=ImageStat.Stat(delta).mean
   samples=[]
   for sx,sy in [(150,120),(450,120),(150,240),(450,240)]:
    q=(int(sx*pw/600),int(sy*ph/300));samples.append({'point':q,'actual':actual.getpixel(q),'expected':expected.getpixel(q)})
   black=sum(1 for r,g,b in actual.getdata() if max(r,g,b)<10)
   result=dict(title=title,depth=raw.depth,visual=hex(visual),scene=[px,py,pw,ph],shot_size=shot.size,mean_abs_rgb_error=mean,black_fraction=black/(pw*ph),samples=samples)
   results.append(result);print('RESULT',json.dumps(result),flush=True)
   assert shot.size==(1920,1080), (title,'incorrect screenshot dimensions',shot.size)
   source_wanted=[(255,48,48,255),(32,208,80,255),(128,0,0,128 if depth==32 else 255),(0,0,128,128 if depth==32 else 255)]
   for point,wanted_rgba in zip([(150,120),(450,120),(150,240),(450,240)],source_wanted):
    assert src.getpixel(point)==wanted_rgba, (title,'incorrect source RGBA',point,src.getpixel(point),wanted_rgba)
   for patch in samples:
    if max(abs(a-b) for a,b in zip(patch['actual'],patch['expected']))>3:failures.append((title,patch))
   win.destroy();cmap.free();d.sync();time.sleep(.3)
 (RUN/'results.json').write_text(json.dumps(results,indent=2))
 assert not failures, 'popup pixel mismatches: '+json.dumps(failures)
 print('RESULT PASS: geometry, source depth/RGBA and four color patches on both outputs; blur='+BLUR,RUN,flush=True)
finally:
 if d:
  try:d.close()
  except Exception:pass
 kids=descendants(proc.pid)
 if proc.poll() is None:os.killpg(proc.pid,signal.SIGTERM)
 try:proc.wait(timeout=5)
 except subprocess.TimeoutExpired:os.killpg(proc.pid,signal.SIGKILL);proc.wait(timeout=5)
 # Only exact descendants from this dedicated compositor; never global name killing.
 for pid in kids:
  try:os.kill(pid,signal.SIGTERM)
  except ProcessLookupError:pass
 log.close()
 print('CLEANUP compositor',proc.pid,'returncode',proc.returncode,'dedicated_descendants',kids,'ARTIFACTS',RUN,flush=True)
