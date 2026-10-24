#!/usr/bin/env python3
"""跨屏拖拽回归：窗口跟着光标（Mod+左键）走到另一块屏时，必须**当场**画出来。

隔离运行（headless 双输出 + 独立 XDG/DBus/XDG_RUNTIME_DIR），绝不连真实合成器、
不碰用户桌面。

三个断言，各对应一条用户报的 bug：
  A  拖拽中途窗口在新屏上有像素     ——「第一次跨屏拖拽一定不显示」
  B  继续拖到新屏很右边仍然有像素   ——「拖到一定距离窗口就看不见」
  C  模糊底衬按**新屏**采样         ——「跨屏模糊按原屏采样，越界后一片横条」

用法：test-drag-crossscreen.py [--no-blur] [--keep] [--debug]
  --no-blur  关模糊（用来隔离 C 是否由模糊引起）
  --keep     保留临时目录（两张截图 + 合成器日志）
  --debug    打印光标 / 窗口盒子等中间量
环境变量 VIDOW_AB_NO_DRAG_OUTPUT=1 透传给合成器做 A/B（关掉本轮修复）。
"""
import json
import os
import pathlib
import re
import shutil
import signal
import statistics
import subprocess
import sys
import tempfile
import time

ROOT = pathlib.Path(__file__).resolve().parent
BLUR = '--no-blur' not in sys.argv
KEEP = '--keep' in sys.argv
DEBUG = '--debug' in sys.argv

# 两块 headless 输出：1 倍 + 2 倍（用户那台就是这个组合）。
OUT1_W, OUT1_H, OUT1_SCALE = 1280, 720, 1
OUT2_W, OUT2_H, OUT2_SCALE = 1280, 720, 2


def log(*a):
    print(*a, flush=True)


def main() -> int:
    run = pathlib.Path(tempfile.mkdtemp(prefix='vidow-dragcross-'))
    os.chmod(run, 0o700)
    conf = run / 'config.lua'
    conf.write_text(
        # Mod+左键 = interactive-move：用户实际用来拖窗口的那条绑定。
        'vidow.bind("Mod+mouse-left", "interactive-move")\n'
        'vidow.bind("Mod+mouse-right", "interactive-resize")\n'
        'decorations=true\n'
        'vidow.output{ name="HEADLESS-1", scale=%d, x=0,y=0 }\n'
        'vidow.output{ name="HEADLESS-2", scale=%d, x=%d,y=0 }\n'
        % (OUT1_SCALE, OUT2_SCALE, OUT1_W))
    env = dict(os.environ,
               VIDOW_PORTAL_NO_RESTART='1',
               XDG_CONFIG_HOME=str(run / 'config'),
               XDG_DATA_HOME=str(run / 'data'),
               DBUS_SESSION_BUS_ADDRESS='unix:path=' + str(run / 'no-bus'),
               XDG_RUNTIME_DIR=str(run),
               WLR_BACKENDS='headless',
               WLR_HEADLESS_OUTPUTS='2',
               WLR_LIBINPUT_NO_DEVICES='1',
               # 装饰 / 模糊只支持 GLES2（pixman 那条路 fx.c 会直接关掉它们）。
               WLR_RENDERER=os.environ.get('WLR_RENDERER', 'gles2'),
               VIDOW_CONFIG=str(conf),
               VIDOW_LOG='info',
               WAYLAND_DISPLAY='',
               DISPLAY='')
    for k in ('VIDOW_AB_NO_DRAG_OUTPUT', 'VIDOW_TRACE_DRAG'):
        if os.environ.get(k):
            env[k] = os.environ[k]

    log_path = run / 'vidow.log'
    logf = open(log_path, 'w')
    proc = subprocess.Popen([str(ROOT / 'build/vidow')], env=env,
                            stdout=logf, stderr=logf, start_new_session=True)
    clients = []
    try:
        wl = xd = None
        for _ in range(100):
            text = log_path.read_text(errors='replace')
            w = re.findall(r'WAYLAND_DISPLAY=([\w.-]+)', text)
            x = re.findall(r'DISPLAY=(:\d+)', text)
            if w and x:
                wl, xd = w[-1], x[-1]
                break
            time.sleep(0.1)
        assert wl and xd, '隔离显示没起来'
        env.update(WAYLAND_DISPLAY=wl, DISPLAY=xd)
        log(f'ISOLATED {run}  {wl}  {xd}  blur={BLUR}')

        ipc = str(ROOT / 'build/vidowmsg')
        vdrag = str(ROOT / 'build/vdrag')

        def msg(*args, check=True):
            p = subprocess.run([ipc, *args], env=env, capture_output=True,
                               text=True, timeout=15)
            if check and p.returncode:
                raise RuntimeError(f'{args}: {p.stderr.strip()}')
            return p.stdout

        # 逻辑尺寸 = 设备像素 / scale
        msg('output-mode', 'HEADLESS-1', str(OUT1_W * OUT1_SCALE),
            str(OUT1_H * OUT1_SCALE))
        msg('output-mode', 'HEADLESS-2', str(OUT2_W * OUT2_SCALE),
            str(OUT2_H * OUT2_SCALE))
        time.sleep(1.0)
        outs = {o['name']: o for o in json.loads(msg('outputs'))}
        log('OUTPUTS', {k: (v['scale'], v['layout']) for k, v in outs.items()})
        lay = {k: v['layout'] for k, v in outs.items()}
        total_w = max(v['x'] + v['width'] for v in lay.values())
        total_h = max(v['y'] + v['height'] for v in lay.values())

        def norm(x, y):
            """逻辑坐标 -> 虚拟指针的 0..10000 归一化坐标（基准是整张布局）。"""
            return (round(x * 10000 / total_w), round(y * 10000 / total_h))

        # 一个持续出帧的窗口（每帧换色）：在新屏上「画出来了」就等于那块有
        # 非背景像素，比看静态图更硬。
        g = subprocess.Popen([str(ROOT / 'build/frames'), 'drag-cross', '120'],
                             env=env, stdout=subprocess.DEVNULL,
                             stderr=subprocess.DEVNULL, start_new_session=True)
        clients.append(g)
        time.sleep(1.5)
        wins = json.loads(msg('windows'))
        assert wins, '没有客户端窗口'
        wid = wins[0]['id']
        msg('move-window-to-output', '--id', str(wid), 'HEADLESS-1')
        time.sleep(1.0)
        win = next(w for w in json.loads(msg('windows')) if w['id'] == wid)
        geo = win['geometry']
        log('WINDOW', geo)

        # 抓点必须落在**客户端表面**上（frames 的 buffer 很小，几何框里可能
        # 是空的），并且在窗口右侧，好让窗口能跟着光标走进新屏。
        grab_x = geo['x'] + min(geo['width'] - 40, 200)
        grab_y = geo['y'] + min(geo['height'] - 40, 120)
        l2 = lay['HEADLESS-2']
        cross = (l2['x'] + 200, grab_y)                # 刚跨进 2 倍屏
        far = (l2['x'] + l2['width'] - 120, grab_y)    # 2 倍屏很右边
        log(f'GRAB logical {(grab_x, grab_y)}  cross {cross}  far {far}')

        snap = run / 'snap.png'

        def shot(tag):
            subprocess.run(['grim', '-s', '1.0', str(snap)], env=env,
                           check=True, timeout=30)
            out = snap.with_suffix(f'.{tag}.png')
            shutil.copy(snap, out)
            return out

        def begin_move(lx, ly):
            """光标挪到窗口里的 (lx,ly)，再走 Mod+左键那条动作起拖。

            虚拟指针发不了修饰键，所以「起拖」直接调 keybind.c 里
            Mod+mouse-left 绑的那个动作（interactive-move，目标都是光标底下
            那个窗口）；之后的跟手 / 松手是真指针事件，走通用抓取那条路。
            """
            nx, ny = norm(lx, ly)
            subprocess.run([vdrag, str(nx), str(ny), str(nx), str(ny),
                            '--move-only', '--step-ms', '10', '--wait', '150'],
                           env=env, check=True, timeout=30,
                           stdout=subprocess.DEVNULL)
            time.sleep(0.2)
            pick = json.loads(msg('pick-window', str(lx), str(ly)))
            assert pick.get('window'), f'光标下没有窗口: {pick}'
            msg('action', 'interactive-move')

        def drag_and_shoot(tag, x0, y0, x1, y1):
            """一次连续拖拽：走到终点后**按住不放**，截图，再松手。

            拖拽中途是唯一的证据窗口 —— 松手之后窗口已经换了归属屏，什么都
            看不出来。每段用一次新的 vdrag 会话：指针设备会重新 attach，在
            一次拖拽中间换会话会丢抓取（实测窗口停在半路）。
            """
            pr = subprocess.Popen([vdrag, str(x0), str(y0), str(x1), str(y1),
                                   '--step-ms', '60', '--hold', '--wait', '3000'],
                                  env=env, stdout=subprocess.PIPE, text=True,
                                  bufsize=1)
            held = pr.stdout.readline()
            assert held.startswith('held'), f'vdrag {tag} 没到终点: {held!r}'
            time.sleep(1.0)   # 等合成器把最后几帧交上去
            path = shot(tag)
            pr.wait(timeout=60)
            assert pr.returncode == 0, f'vdrag {tag} 退出码 {pr.returncode}'
            return path

        # ---- 度量 -----------------------------------------------------------
        from PIL import Image

        def cells(path, x0):
            """从 x0 到图右边界：返回 (非背景像素占比, 非背景像素的 x 列表)。

            背景参考色取图右上角那一小块（拖拽把窗口放在屏幕中段时，那里一定
            还是空桌面）；不能取右下角 —— 窗口一跨过去就压在那儿了。
            """
            im = Image.open(path).convert('RGB')
            base = im.getpixel((im.width - 2, 2))
            hits = 0
            total = 0
            xs = []
            for y in range(0, im.height, 3):
                for x in range(x0, im.width, 3):
                    p = im.getpixel((x, y))
                    total += 1
                    if (abs(p[0] - base[0]) + abs(p[1] - base[1]) +
                            abs(p[2] - base[2])) > 30:
                        hits += 1
                        xs.append(x)
            return (hits / total if total else 0.0), xs

        results = {}

        # ---- A：Mod+左键起拖，拖进 2 倍屏，按住截图
        begin_move(grab_x, grab_y)
        mid = drag_and_shoot('mid', *norm(grab_x, grab_y),
                             *norm(cross[0], cross[1]))
        # grim 抓的是逻辑布局大小的图：2 倍屏在图上从逻辑 x=OUT1_W 开始。
        fa, xs_mid = cells(mid, OUT1_W)
        results['A_new_screen_pixels'] = fa
        log(f'A 拖拽中途新屏非背景占比 = {fa:.3f}  像素 x 范围 = '
            f'{(min(xs_mid), max(xs_mid)) if xs_mid else None}')

        # ---- B：从 cross 继续拖到 far，按住截图
        begin_move(cross[0], cross[1])
        far_shot = drag_and_shoot('far', *norm(cross[0], cross[1]),
                                  *norm(far[0], far[1]))
        fb, xs_far = cells(far_shot, OUT1_W)
        cx_far = statistics.fmean(xs_far) if xs_far else 0.0
        results['B_far_pixels'] = fb
        results['B_centroid'] = cx_far
        log(f'B 拖到更远处非背景占比 = {fb:.3f}  像素重心 x = {cx_far:.0f}')

        win = next(w for w in json.loads(msg('windows')) if w['id'] == wid)
        results['geometry_after'] = win['geometry']
        log('松手后窗口几何', win['geometry'])
        if DEBUG:
            import re as _re
            txt = log_path.read_text(errors='replace')
            n_enter = len(_re.findall(r'拖拽', txt))
            log('DEBUG clip 行:',
                [l.split('] ')[-1] for l in txt.splitlines()
                 if ' clip:' in l][-4:])
            log('DEBUG trace 条数:', txt.count('TRACE drag'))

        # ---- C：模糊底衬按新屏采样
        if BLUR:
            bd = shot('blur')
            im = Image.open(bd).convert('RGB')
            right = im.crop((OUT1_W, 0, im.width, im.height))
            # 窗口左外侧那条竖带：按新屏采样时是「新屏背景的模糊」，按老屏
            # 采样时是「原屏边缘的横条」（老屏那块已经被采完了，UV 被夹住）。
            band_x = max(0, win['geometry']['x'] - OUT1_W - 12)
            base = right.getpixel((right.width - 2, 2))
            col = [right.getpixel((band_x, y))
                   for y in range(4, right.height - 4, 2)]
            var = statistics.fmean(
                abs(p[0] - base[0]) + abs(p[1] - base[1]) +
                abs(p[2] - base[2]) for p in col)
            results['C_blur_band'] = var
            log(f'C 窗口左外侧底衬带与背景的平均色差 = {var:.1f}')

        # ---- 判定 -----------------------------------------------------------
        # A：窗口只占新屏一半宽，满打满算约 0.48。要 0.15 = 至少画出窗口的
        #    三分之一，肉眼绝不可能看漏。
        ok = results['A_new_screen_pixels'] > 0.15
        # B：还画在新屏上，而且重心真的更靠右（证明跟手，不是静图）
        ok = ok and results['B_far_pixels'] > 0.05
        ok = ok and results['B_centroid'] > OUT1_W + OUT2_W // 2
        if BLUR:
            ok = ok and results['C_blur_band'] > 0.5
        log('RESULT', 'PASS' if ok else 'FAIL', results)
        return 0 if ok else 1
    finally:
        for c in clients:
            try:
                os.killpg(os.getpgid(c.pid), signal.SIGTERM)
            except ProcessLookupError:
                pass
        try:
            os.killpg(os.getpgid(proc.pid), signal.SIGTERM)
            proc.wait(timeout=5)
        except Exception:
            try:
                os.killpg(os.getpgid(proc.pid), signal.SIGKILL)
            except ProcessLookupError:
                pass
        if not KEEP:
            shutil.rmtree(run, ignore_errors=True)
        else:
            print('kept:', run)


if __name__ == '__main__':
    sys.exit(main())
