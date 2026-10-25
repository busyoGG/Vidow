#!/usr/bin/env node
/* vidow-remote — 手机（或任何浏览器）远程看 / 控 vidow 桌面的单向服务。
 *
 * 设计要点（都是踩过的）：
 *
 * 1. **不截图，走持续帧流**。每帧 spawn 一次截图进程要 30-80ms，1080p 连
 *    10fps 都够呛。这里起一个常驻 wf-recorder，它走 zwlr_screencopy_v1
 *    从 vidow 拿帧，H.264 编完直接吐到 stdout —— 我们只是把字节流转给浏览器。
 *
 * 2. **视频用分片 MP4 走 WebSocket**。不转码、不重新封装（省一份 CPU），
 *    浏览器端用 MSE 喂 SourceBuffer。分片 MP4 的 moof 边界正好可以边到边喂。
 *
 * 3. **输入全部落到 vidowmsg**。坐标是**逻辑像素**（vidow 的 IPC 口径），
 *    手机端按「画面尺寸 -> 输出逻辑尺寸」的比例换算，和缩放无关。
 *
 * 用法：
 *   node tools/remote/vidow-remote.js [--port 8080] [--output DP-2]
 *                                    [--fps 60] [--bitrate 8M] [--bind 0.0.0.0]
 *
 * 环境：VIDOW_RUNTIME_DIR 覆盖 XDG_RUNTIME_DIR（vidowmsg / wf-recorder 都要它）。
 */

'use strict';

const http = require('node:http');
const { spawn, execFile } = require('node:child_process');
const fs = require('node:fs');
const path = require('node:path');
const crypto = require('node:crypto');

// ---------------------------------------------------------------- 参数

function parse_args(argv) {
	const out = {
		port: 8080,
		bind: '0.0.0.0',
		output: null,
		fps: 60,
		bitrate: '8M',
		vaapi: '/dev/dri/renderD128',   // 传空字符串可强制软件编码
		/* --mode raw   : 裸 annexb H.264，按帧切，给 WebCodecs 用（低延迟）
		 * --mode fmp4  : 分片 MP4，给 MediaSource 用（兼容老浏览器）
		 * auto：默认 raw，客户端不支持 WebCodecs 时自己在页面上回退 */
		/* auto = 按客户端能力自动选：能 WebCodecs 就 raw，否则 fmp4 */
		mode: 'auto',
		/* HTTPS：WebCodecs 需要安全上下文（非 localhost 的 http 里
		 * VideoDecoder 直接不存在），手机必须走 https 才能用低延迟解码。
		 * 证书自签，手机首次访问点一次「继续前往」即可。 */
		https: true,
		cert: path.join(__dirname, 'certs', 'cert.pem'),
		key: path.join(__dirname, 'certs', 'key.pem'),
		vidowmsg: null,
		wfrecorder: 'wf-recorder',
		ffmpeg: 'ffmpeg',
	};
	for (let i = 2; i < argv.length; i++) {
		const a = argv[i];
		const next = () => argv[++i];
		if (a === '--port') out.port = parseInt(next(), 10);
		else if (a === '--bind') out.bind = next();
		else if (a === '--output') out.output = next();
		else if (a === '--fps') out.fps = parseInt(next(), 10);
		else if (a === '--bitrate') out.bitrate = next();
		else if (a === '--mode') out.mode = next();
		else if (a === '--no-https') out.https = false;
		else if (a === '--cert') out.cert = next();
		else if (a === '--key') out.key = next();
		else if (a === '--vidowmsg') out.vidowmsg = next();
		else if (a === '--wf-recorder') out.wfrecorder = next();
		else if (a === '--help' || a === '-h') {
			console.log(fs.readFileSync(__filename, 'utf8')
				.split('\n').slice(2, 22).map((l) => l.replace(/^ \* ?/, '')).join('\n'));
			process.exit(0);
		}
	}
	if (!Number.isFinite(out.port) || out.port <= 0) out.port = 8080;
	if (!Number.isFinite(out.fps) || out.fps <= 0) out.fps = 60;
	return out;
}

const ARGS = parse_args(process.argv);

const RUNTIME = process.env.VIDOW_RUNTIME_DIR || process.env.XDG_RUNTIME_DIR ||
	`/run/user/${process.getuid ? process.getuid() : 1000}`;
const VIDOWMSG = ARGS.vidowmsg || path.join(RUNTIME, '..', '..', '..', 'dev', 'null');
const MSG_BIN = ARGS.vidowmsg ||
	(path.resolve(__dirname, '..', '..', 'build', 'vidowmsg'));

function env_for_child() {
	return { ...process.env, XDG_RUNTIME_DIR: RUNTIME };
}

// ---------------------------------------------------------------- vidowmsg

/* 调 vidowmsg，返回 stdout 文本。加超时：IPC 卡住不能把整个服务拖死。 */
function msg(cmd, args = [], timeout_ms = 4000) {
	return new Promise((resolve, reject) => {
		execFile(MSG_BIN, [cmd, ...args.map(String)],
			{ env: env_for_child(), timeout: timeout_ms, maxBuffer: 8 << 20 },
			(err, stdout, stderr) => {
				if (err) {
					reject(new Error(`${cmd}: ${(stderr || err.message || '').trim()}`));
					return;
				}
				resolve(stdout);
			});
	});
}

function msg_json(cmd, args) {
	return msg(cmd, args).then((s) => JSON.parse(s));
}

/* ---------------------------------------------------------------- 指针队列
 *
 * **踩过的坑**：一开始每个 move 直接 await msg('move-cursor')，而 msg 是
 * execFile —— 每次都要 fork 一个 vidowmsg 进程（约 5~15ms 启动 + IPC 往返）。
 * 手机上手指一划每秒几十个 move，进程排长队，鼠标要「过很久才动」，
 * 而且越划越滞后（队列只增不减）。
 *
 * 改成两级：
 *   1. **合并**：队列里还在等的位置直接覆盖成最新的，中间点没意义。
 *   2. **单飞**：同一时刻只有一个在途，跑完立刻取最新的一条。
 * 结果：无论手指多快，实际进程调用被压到「IPC 往返速度」这个上限。 */
const pointer_queue = [];
let pointer_busy = false;

function enqueue_pointer(cmd, args) {
	/* 只合并**队尾连续的同类 move**：把「还没发出去的历史光标位置」丢掉。
	 * 不能回头去改队列中间的同名项 —— down/up 之间夹着的那次 move 是有
	 * 语义的（拖拽起点），改掉会点错地方。 */
	const last = pointer_queue[pointer_queue.length - 1];
	if (cmd === 'move-cursor' && last && last.cmd === 'move-cursor') {
		last.args = args;
		drain_pointer();
		return;
	}
	pointer_queue.push({ cmd, args });
	// 队列太长（按键 / 点击这类不能合并的）就丢掉最老的
	if (pointer_queue.length > 64) pointer_queue.splice(0, pointer_queue.length - 64);
	drain_pointer();
}

function drain_pointer() {
	if (pointer_busy) return;
	const job = pointer_queue.shift();
	if (!job) return;
	pointer_busy = true;
	msg(job.cmd, job.args)
		.catch((e) => console.error('[pointer]', e.message))
		.finally(() => {
			pointer_busy = false;
			if (pointer_queue.length > 0) drain_pointer();
		});
}

// ---------------------------------------------------------------- 输出/状态

let outputs_cache = { at: 0, list: [] };

async function list_outputs() {
	const now = Date.now();
	if (now - outputs_cache.at < 2000) return outputs_cache.list;
	const list = await msg_json('outputs');
	outputs_cache = { at: now, list };
	return list;
}

/* 当前要看的输出：显式 --output 优先，否则挑「已启用且宽高最大」那块。 */
async function pick_output() {
	const list = await list_outputs();
	if (ARGS.output) {
		const hit = list.find((o) => o.name === ARGS.output);
		if (hit) return hit;
		throw new Error(`找不到输出 ${ARGS.output}`);
	}
	if (list.length === 0) throw new Error('vidow 没报出任何输出');
	const enabled = list.filter((o) => o.enabled);
	const pool = enabled.length > 0 ? enabled : list;
	return pool.slice().sort((a, b) =>
		(b.logical_width * b.logical_height) - (a.logical_width * a.logical_height))[0];
}

// ---------------------------------------------------------------- 帧流

/* 一个「视频会话」= 一个 wf-recorder 进程 + 一组浏览器连接。
 *
 * 只在第一个人连上时起进程，最后一个人走掉后停 —— 空闲时零 CPU。 */
class VideoStream {
	constructor(output_name, fps, bitrate) {
		this.output = output_name;
		this.fps = fps;
		this.bitrate = bitrate;
		this.proc = null;
		this.rec = null;
		this.init_seg = null;
		this.moof = null;
		this.pend = Buffer.alloc(0);
		this.clients = new Set();
		this.seq = 0;
		this.started_at = 0;
	}

	async ensure() {
		if (this.proc || this.starting) return;
		this.starting = true;
		try { await this.start_pipeline(); } finally { this.starting = false; }
	}

	async start_pipeline() {
		/* 新管道 = 新的时间轴：初始化段和半截的 moof 必须全部丢掉，
		 * 否则新分片会拼在旧初始化段后面，解出来是黑的（rdy 卡在 1）。 */
		this.init_seg = null;
		this.moof = null;
		this.pend = Buffer.alloc(0);
		/* 链路：wf-recorder 出 mpegts → ffmpeg 转**分片 MP4（fMP4）** →
		 * 我们按 box 边界切，浏览器用原生 MediaSource 直接播。
		 *
		 * 为什么不用 mpegts 直出 + mpegts.js：那套的自定义 loader 内部协议
		 * 太脆（open / onDataArrival 的挂载时序），试了四轮都喂不进去。
		 * fMP4 是 MSE 的原生格式，浏览器端只要 appendBuffer，零第三方库。
		 *
		 * mp4 直出 pipe 是不行的（muxer 要回写文件头），所以中间必须过一道
		 * ffmpeg —— 它做的是**流式重新封装**，视频流是 copy 还是重编看下面。 */
		/* **有显卡就走 VAAPI 硬编**，理由：
		 *   1. 不占 CPU —— 软件 x264 单路 1080p60 能吃掉 1~2 个核；
		 *   2. 更快 —— 实测首 50KB 138ms（软编 184ms）；
		 *   3. wf-recorder 原生支持，不用多一层 ffmpeg 转码。
		 * 拿不到 renderD128 时自动退回 libx264，不会因为没 GPU 就整个挂掉。 */
		const va_device = ARGS.vaapi && fs.existsSync(ARGS.vaapi) ? ARGS.vaapi : null;
		const rec_args = ['-o', this.output, '-f', 'pipe:1', '-m', 'mpegts'];
		if (va_device) {
			/* **不要传 -x**：VAAPI 路径自己会 scale_vaapi 成 nv12，
			 * 再塞个 -x yuv420p 会报
			 * "Impossible to convert between the formats"（实测）。
			 * 留空让 wf-recorder 自己决定。 */
			rec_args.push('-c', 'h264_vaapi', '-d', va_device,
				'-p', `b=${this.bitrate}`,
				'-p', 'g=' + Math.max(1, Math.round(this.fps / 2)),
				'-r', String(this.fps));
		} else {
			rec_args.push('-c', 'libx264',
				'-p', 'preset=ultrafast', '-p', 'tune=zerolatency',
				/* rc-lookahead / sync-lookahead = 0：x264 最容易偷偷攒帧的两个
				 * 参数，关掉能省几十毫秒。 */
				'-p', 'rc-lookahead=0', '-p', 'sync-lookahead=0', '-p', 'sliced-threads=1',
				/* b= 只是目标值，场景变化时 x264 会大幅超发，必须卡 maxrate */
				'-p', `b=${this.bitrate}`, '-p', `maxrate=${this.bitrate}`,
				'-p', `bufsize=${this.bufsize || '2M'}`,
				'-p', 'g=' + Math.max(1, Math.round(this.fps / 2)),
				'-r', String(this.fps), '-x', 'yuv420p');
		}
		console.error(`[stream] 编码: ${va_device ? 'VAAPI 硬编 (' + va_device + ')' : 'libx264 软编'}`);

		/* ---------------- 裸流模式：没有 ffmpeg，直接帧直通 ---------------- */
		if (ARGS.mode === 'raw') {
			rec_args[rec_args.indexOf('mpegts')] = 'h264';   // 裸 annexb 输出
			const cpos = rec_args.indexOf('-c');
			const codec = rec_args[cpos + 1];
			/* aud=1：每帧前插一个 AUD，这是前端切帧的唯一可靠标记。
			 * 没有它就只能靠 slice 的 first_mb_in_slice 猜帧边界，脆。 */
			rec_args.push('-p', 'aud=1');
			if (codec === 'libx264') rec_args.push('-p', 'repeat-headers=1'); // 每个 IDR 前重发 SPS/PPS

			this.raw_mode = true;
			this.sps = null;
			this.pps = null;
			this.avcc_sent = false;
			const rrec = spawn(ARGS.wfrecorder, rec_args,
				{ env: env_for_child(), stdio: ['ignore', 'pipe', 'pipe'] });
			this.proc = rrec;
			this.rec = rrec;
			this.seq++;
			this.started_at = Date.now();

			this.last_data = Date.now();
			this.watchdog = setInterval(() => {
				if (this.clients.size === 0) return;
				if (Date.now() - this.last_data > 5000) {
					console.error('[stream] 5 秒没有数据，重启管道');
					try { rrec.kill('SIGKILL'); } catch {}
				}
			}, 2000);

			rrec.stdout.on('data', (buf) => {
				this.last_data = Date.now();
				this.feed_annexb(buf);
			});
			rrec.stderr.on('data', (buf) => {
				const t = buf.toString().trim();
				if (!t) return;
				for (const line of t.split('\n')) {
					if (/error|fail|impossible/i.test(line)) console.error('[wf-recorder]', line);
				}
			});
			const rteardown = (code, sig) => {
				console.error(`[stream] 裸流结束 code=${code} sig=${sig}`);
				if (this.watchdog) { clearInterval(this.watchdog); this.watchdog = null; }
				if (this.proc !== rrec) return; // stale encoder must not erase its replacement
				this.proc = null; this.rec = null;
				this.sps = null; this.pps = null; this.pend = Buffer.alloc(0);
				if (this.clients.size > 0) {
					setTimeout(() => this.ensure().catch((e) =>
						console.error('[stream] 重启失败', e.message)), 500);
				}
			};
			rrec.on('exit', rteardown);
			rrec.on('error', (e) => console.error('[wf-recorder] 启动失败', e.message));
			return;
		}

		/* ---------------- fMP4 模式：给 MediaSource 用 ---------------- */
		const rec = spawn(ARGS.wfrecorder, rec_args,
			{ env: env_for_child(), stdio: ['ignore', 'pipe', 'pipe'] });

		const ff = spawn(ARGS.ffmpeg, [
			'-hide_banner', '-loglevel', 'error',
			/* **延迟的关键三行**（实测首 100KB 从 6224ms 降到 909ms）。
			 *   nobuffer        不缓存输入
			 *   flush_packets   每个包立刻写出，不等内部缓冲满
			 *   probesize/analyzeduration 归零：默认 ffmpeg 会先读几秒
			 *                   数据「分析流」，这一步在直播里纯属白等 */
			'-fflags', 'nobuffer+flush_packets',
			'-probesize', '32',
			'-analyzeduration', '0',
			'-max_delay', '0',
			'-i', 'pipe:0',
			/* 只做**重新封装**，不重编（-c:v copy）。
			 * 这是当初「手机能出画面」的配置，别动它 —— 曾经为了「提升安卓
			 * 兼容性」改成重编 High/Main profile，结果手机反而永久 rdy=1。
			 * 手机 caps 自检显示它本来就支持 avc1.42C02A（Baseline 4.2），
			 * copy 出来的就是这个 profile，完全够用。 */
			'-c:v', 'copy',
			/* frag_duration 100ms：不等下一个关键帧才切片，每 100ms 就吐一个
			 * 分片，手机端能更早拿到数据（默认只在关键帧切，最坏等 0.5 秒）。 */
			'-frag_duration', '100000',
			'-movflags', '+frag_keyframe+empty_moov+default_base_moof',
			'-f', 'mp4', 'pipe:1',
		], { stdio: ['pipe', 'pipe', 'pipe'] });

		rec.stdout.pipe(ff.stdin);
		rec.stdout.on('error', () => {});
		ff.stdin.on('error', () => {});   // ffmpeg 先退出时 EPIPE

		this.proc = ff;
		this.rec = rec;

		this.seq++;
		this.started_at = Date.now();
		this.init_seg = null;
		this.pend = Buffer.alloc(0);

		/* 看门狗：ffmpeg 有可能「进程还在但不出数据」（转封装卡住 / 源没帧），
		 * 超过 5 秒一个字节都不来就整个重启，不然客户端画面就永久冻住。
		 * 注意 wf-recorder 静音画面时 x264 仍会持续出帧，这里按字节判断够用。 */
		this.last_data = Date.now();
		this.watchdog = setInterval(() => {
			if (this.clients.size === 0) return;
			if (Date.now() - this.last_data > 5000) {
				console.error('[stream] 5 秒没有数据，重启管道');
				try { rec.kill('SIGKILL'); } catch {}
				try { ff.kill('SIGKILL'); } catch {}
			}
		}, 2000);

		ff.stdout.on('data', (buf) => {
			this.last_data = Date.now();
			this.feed_mp4(buf);
		});
		ff.stderr.on('data', (buf) => {
			const s = buf.toString().trim();
			if (s) console.error('[ffmpeg]', s.split('\n').slice(-1)[0]);
		});
		rec.stderr.on('data', (buf) => {
			const s = buf.toString().trim();
			if (s) console.error('[wf-recorder]', s.split('\n').slice(-1)[0]);
		});

		const teardown = (code, sig) => {
			console.error(`[stream] 结束 code=${code} sig=${sig}`);
			if (this.watchdog) { clearInterval(this.watchdog); this.watchdog = null; }
			this.proc = null;
			this.rec = null;
			try { rec.kill('SIGTERM'); } catch {}
			try { ff.kill('SIGTERM'); } catch {}
			if (this.clients.size > 0) {
				setTimeout(() => this.ensure().catch((e) =>
					console.error('[stream] 重启失败', e.message)), 500);
			}
		};
		ff.on('exit', teardown);
		ff.on('error', (e) => console.error('[ffmpeg] 启动失败', e.message));
		rec.on('error', (e) => console.error('[wf-recorder] 启动失败', e.message));
	}

	/* ------------------------------------------------------------ 裸流模式
	 *
	 * 把 annexb 的 H.264 按 **AUD（Access Unit Delimiter，NAL type 9）** 切成
	 * 一帧一帧，直接喂 WebCodecs 的 VideoDecoder。
	 *
	 * 为什么不用 MSE：MSE 是播放器管线，内部固定缓冲 + 时间轴管理，
	 * 实测端到端 200~500ms，参数怎么调都下不来，做不到「跟手」。
	 * WebCodecs 是解码器直通：收到帧 → 解 → 画，中间没有播放器排队。
	 *
	 * wf-recorder 要加 `-p aud=1`，保证每帧前都有 AUD 可切。 */
	feed_annexb(buf) {
		this.pend = this.pend.length ? Buffer.concat([this.pend, buf]) : buf;

		/* 扫描 NAL 边界。返回 {off, hdr}：off 是起始码起点，hdr 是 NAL 头字节位置。
		 * **统一按 4 字节起始码处理**：3 字节码（00 00 01）前面补一个 0 当 4 字节看，
		 * 早期版本在找边界时把 4 字节码当 3 字节算出错一字节，导致读 NAL 头全错、
		 * 一帧都切不出来（实测 0 帧）。 */
		const scan = (b) => {
			const out = [];
			for (let i = 0; i + 3 < b.length; i++) {
				/* **必须先判 4 字节码**：annexb 里 4 字节码是 00 00 00 01，
				 * 先判 3 字节的话会在第 2 个 0 处就匹配成 00 00 01，
				 * off 少一个字节 → hdr 读到的不是 NAL 头 → 一帧都切不出来
				 * （实测每帧只剩 17 字节的 AUD 头）。 */
				if (b[i] === 0 && b[i + 1] === 0 && b[i + 2] === 0 && b[i + 3] === 1) {
					out.push({ off: i, hdr: i + 4 });
					i += 3;
				} else if (b[i] === 0 && b[i + 1] === 0 && b[i + 2] === 1) {
					out.push({ off: i, hdr: i + 3 });
					i += 2;
				}
			}
			return out;
		};

		/* 找**最后一个 AUD** 作为切分点：AUD 是帧的起点标记。
		 * 早期按「最后一个 NAL」切，会把末尾那个 AUD 留在 pend 里，
		 * 下一轮开头就变成 slice 打头、没有 AUD，切帧直接错位
		 * （实测每帧只剩 17 字节的 AUD 头，画面出不来）。 */
		let last_aud = -1;
		{
			let i = 0;
			while (i + 3 < this.pend.length) {
				let hdr = -1, off = -1;
				if (this.pend[i] === 0 && this.pend[i + 1] === 0 && this.pend[i + 2] === 0 && this.pend[i + 3] === 1) {
					off = i; hdr = i + 4; i += 4;
				} else if (this.pend[i] === 0 && this.pend[i + 1] === 0 && this.pend[i + 2] === 1) {
					off = i; hdr = i + 3; i += 3;
				} else { i++; continue; }
				if (hdr < this.pend.length && (this.pend[hdr] & 0x1f) === 9) last_aud = off;
			}
		}
		if (last_aud <= 0) return;        // 还没攒到下一帧的 AUD，等数据

		const cut = last_aud;
		const ready = this.pend.subarray(0, cut);
		this.pend = this.pend.subarray(cut);

		const nals = scan(ready);

		/* 第一遍：取 SPS/PPS，并记下所有 AUD 的偏移 */
		const aud_offs = [];
		for (let k = 0; k < nals.length; k++) {
			const { off, hdr } = nals[k];
			const t = ready[hdr] & 0x1f;
			const next = k + 1 < nals.length ? nals[k + 1].off : ready.length;
			if (t === 7) this.sps = Buffer.from(ready.subarray(off, next));
			else if (t === 8) this.pps = Buffer.from(ready.subarray(off, next));
			else if (t === 9) aud_offs.push(off);
		}

		/* 第二遍：两个 AUD 之间 = 一帧。最后一个 AUD 到末尾也算一帧
		 * （末尾那次已经在 cut 处截断过，是完整的）。 */
		for (let i = 0; i < aud_offs.length; i++) {
			const from = aud_offs[i];
			const to = i + 1 < aud_offs.length ? aud_offs[i + 1] : ready.length;
			if (to > from) this.emit_frame(ready.subarray(from, to));
		}
	}

	emit_frame(frame) {
		if (frame.length < 8) return;
		/* 第一次拿到 SPS/PPS：把 avcC 发给所有客户端（解码器描述只发一次） */
		if (!this.avcc_sent && this.sps && this.pps) {
			this.avcc_sent = true;
			const avcc = this.build_avcc();
			for (const ws of this.clients) {
				if (ws.writable && !ws.destroyed) {
					ws.send_json({ t: 'desc', b64: avcc.toString('base64') });
				}
			}
		}
		/* 新客户端要的第一样东西是 SPS+PPS（解码器描述），
		 * 直接拼在 codec 描述里，保证它一上来就能 configure。 */
		this.frame_count = (this.frame_count || 0) + 1;
		for (const ws of this.clients) {
			if (!ws.writable || ws.destroyed) continue;
			if (ws.writableLength > 2 << 20) { ws.dropped = (ws.dropped || 0) + 1; continue; }
			ws.send(frame);
			this.sent_frags = (this.sent_frags || 0) + 1;
			this.sent_bytes = (this.sent_bytes || 0) + frame.length;
		}
	}

	/* 把 annexb 的 SPS/PPS 转成 avcC（AVCDecoderConfigurationRecord），
	 * WebCodecs 的 description 要这个格式。 */
	build_avcc() {
		if (!this.sps || !this.pps) return null;
		const sps = this.sps.subarray(this.sps[2] === 1 ? 3 : 4);
		const pps = this.pps.subarray(this.pps[2] === 1 ? 3 : 4);
		const out = Buffer.alloc(11 + sps.length + 1 + 2 + pps.length);
		let o = 0;
		out[o++] = 1; out[o++] = sps[1]; out[o++] = sps[2]; out[o++] = sps[3];
		out[o++] = 0xff; out[o++] = 0xe1;
		out[o++] = sps.length >> 8; out[o++] = sps.length & 0xff;
		sps.copy(out, o); o += sps.length;
		out[o++] = 1;
		out[o++] = pps.length >> 8; out[o++] = pps.length & 0xff;
		pps.copy(out, o); o += pps.length;
		return out.subarray(0, o);
	}

	/* 按 MP4 box 边界切分，投给 MSE。
	 *
	 * fMP4 的结构：ftyp + moov（初始化段，**只发一次**），之后每个
	 * moof+mdat 是一个可独立 append 的媒体分片。必须整盒发，不能截断 ——
	 * MSE 拿到半个 box 会直接 SourceBuffer 报错。 */
	feed_mp4(buf) {
		this.pend = this.pend.length ? Buffer.concat([this.pend, buf]) : buf;
		for (;;) {
			if (this.pend.length < 8) return;
			const size = this.pend.readUInt32BE(0);
			// size==0 表示「到文件尾」，流式场景不该出现；size==1 是 64 位长度
			if (size === 1) {
				if (this.pend.length < 16) return;
				const big = this.pend.readBigUInt64BE(8);
				if (BigInt(this.pend.length) < big) return;
				const box = this.pend.subarray(0, Number(big));
				this.pend = this.pend.subarray(Number(big));
				this.handle_box(box);
				continue;
			}
			if (size < 8) {
				// 对不齐：丢掉重新找（ffmpeg 异常时会发生）
				this.pend = Buffer.alloc(0);
				return;
			}
			if (this.pend.length < size) return;
			const box = this.pend.subarray(0, size);
			this.pend = this.pend.subarray(size);
			this.handle_box(box);
		}
	}

	handle_box(box) {
		const type = box.subarray(4, 8).toString('latin1');
		if (type === 'ftyp' || type === 'moov') {
			/* 初始化段：攒起来。**攒齐（见过 moov）的那一刻**要立刻推给已经在线的
			 * 客户端 —— 它们连上来时 init_seg 还是空的（见 add()），不补发就永远
			 * append 不了媒体分片。 */
			this.init_seg = this.init_seg
				? Buffer.concat([this.init_seg, box]) : box;
			if (type === 'moov') {
				for (const ws of this.clients) {
					if (ws.writable && !ws.destroyed) ws.send(this.init_seg);
				}
			}
			return;
		}
		if (type === 'moof') {
			// 媒体分片的第一半，等 mdat 一起发（见下面 mdat 分支）
			this.moof = box;
			return;
		}
		if (type === 'mdat') {
			const frag = this.moof ? Buffer.concat([this.moof, box]) : box;
			this.moof = null;
			this.broadcast(frag);
			return;
		}
		if (type === 'mfra') return;   // 收尾索引，流式播放用不上
	}

	broadcast(buf) {
		/* 顺手清掉已经断开的客户端，别让死 socket 越攒越多 */
		for (const ws of this.clients) {
			if (ws.destroyed || !ws.writable) {
				this.clients.delete(ws);
				try { ws.destroy(); } catch {}
			}
		}
		for (const ws of this.clients) {
			/* **裸 net.Socket 没有 readyState**（那是浏览器 WebSocket 的字段）。
			 * 判可写要用 socket.writable / destroyed —— 早期写成 readyState === 1，
			 * 结果 undefined === 1 恒假，一个字节都发不出去，客户端只收到控制
			 * 消息、视频永远 0 字节（踩过）。 */
			/* 慢客户端**直接丢帧**，不能让它把整条流拖死：socket.write()
			 * 返回 false 之后 Node 会累积到内存里，写队列一满，上游 wf-recorder
			 * 的 stdout 就被背压卡住 —— 一次都读不出来，所有人一起卡死
			 * （踩过：日志里 stdout 只出 3 个 64KB chunk 就再也没有了）。 */
			if (!ws.writable || ws.destroyed) continue;
			if (ws.writableLength > 4 << 20) {
				ws.dropped = (ws.dropped || 0) + 1;
				continue;
			}
			ws.send(buf);
			this.sent_frags = (this.sent_frags || 0) + 1;
			this.sent_bytes = (this.sent_bytes || 0) + buf.length;
		}
	}

	add(ws) {
		/* 同一个手机反复刷新 / 切后台再回来，会把旧连接留成僵尸。
		 * 最多只留 2 个：新的进来就踢掉最老的 —— 否则 N 个客户端平分
		 * 8Mbps，谁都卡（实测堆到 20 条活跃连接，画面直接不动）。 */
		/* 不踢人：早期版本在这里踢掉"多余"客户端，结果踢人→close→remove→
		 * 杀管道→重启→发 reset，自己把自己锁进黑屏循环（rdy 永远 1）。
		 * 现在只做清理僵尸，真正的并发控制在 upgrade 阶段按 IP 去重。 */
		for (const old of this.clients) {
			if (old.destroyed || !old.writable) {
				this.clients.delete(old);
			}
		}
		this.clients.add(ws);
		/* **取消待执行的停止**：remove() 里留了 1.5 秒宽限期，刷新页面时
		 * 旧连接先断、新连接随后就到。这里不取消的话定时器照常触发，
		 * 刚建立的管道被 kill，新客户端永远拿不到数据 ——
		 * 表现为「第一次能看，一刷新就黑」（实测）。 */
		if (this.stop_timer) {
			clearTimeout(this.stop_timer);
			this.stop_timer = null;
		}
		/* fMP4 必须先给初始化段（ftyp+moov）才认得后面的分片。
		 * 已经就绪就先发，否则等 feed_mp4() 攒齐时补。 */
		if (this.init_seg && ws.writable && !ws.destroyed) {
			ws.send(this.init_seg);
		}
		/* 裸流模式下新客户端要一份 avcC 才能 configure 解码器；
		 * SPS/PPS 可能还没攒到（管道刚起），此时置空，等 feed 出第一帧时补发。 */
		if (this.raw_mode && this.sps && this.pps) {
			ws.send_json({ t: 'desc', b64: this.build_avcc().toString('base64') });
		}
		this.ensure().catch((e) => console.error('[stream]', e.message));
	}

	remove(ws) {
		console.error(`[stream] 断开一条，剩 ${this.clients.size - 1}；累计发出 ${this.sent_frags || 0} 片 / ${((this.sent_bytes || 0) / 1024 / 1024).toFixed(1)}MB`);
		this.clients.delete(ws);
		/* 踢人时这里是 size===0，但下一秒新客户端就 hello 了。留 1.5 秒
		 * 的宽限期：真有新连接就取消停止，别把正在用的管道反复杀掉重建。 */
		if (this.clients.size === 0 && this.proc) {
			if (this.stop_timer) clearTimeout(this.stop_timer);
			this.stop_timer = setTimeout(() => this.stop_pipeline(), 1500);
			return;
		}
		if (this.clients.size > 0 && this.stop_timer) {
			clearTimeout(this.stop_timer);
			this.stop_timer = null;
		}
	}

	stop_pipeline() {
		this.stop_timer = null;
		if (this.clients.size > 0 || !this.proc) return;
			try { this.proc.kill('SIGTERM'); } catch {}
			try { this.rec && this.rec.kill('SIGTERM'); } catch {}
			this.proc = null;
			this.rec = null;
			this.init_seg = null;
			this.moof = null;
			this.pend = Buffer.alloc(0);
			if (this.watchdog) { clearInterval(this.watchdog); this.watchdog = null; }
	}
}

const streams = new Map(); // output_name -> VideoStream

function stream_for(name) {
	let s = streams.get(name);
	if (!s) {
		s = new VideoStream(name, ARGS.fps, ARGS.bitrate);
		streams.set(name, s);
	}
	return s;
}

// ---------------------------------------------------------------- WebSocket

/* 手写的最小 RFC6455 实现（只用文本帧和二进制帧）。
 * 不引 ws 依赖：这工具要能直接 node 跑起来，不装任何东西。 */
const WS_MAGIC = '258EAFA5-E914-47DA-95CA-C5AB0DC85B11';

function ws_accept(key) {
	return crypto.createHash('sha1')
		.update(key + WS_MAGIC).digest('base64');
}

function ws_frame(opcode, payload) {
	const len = payload.length;
	let header;
	if (len < 126) {
		header = Buffer.from([0x80 | opcode, len]);
	} else if (len < 65536) {
		header = Buffer.alloc(4);
		header[0] = 0x80 | opcode;
		header[1] = 126;
		header.writeUInt16BE(len, 2);
	} else {
		header = Buffer.alloc(10);
		header[0] = 0x80 | opcode;
		header[1] = 127;
		header.writeBigUInt64BE(BigInt(len), 2);
	}
	return Buffer.concat([header, payload]);
}

function ws_setup(req, socket) {
	socket.setNoDelay(true);
	socket.send = function (data) {
		if (socket.destroyed) return;
		const text = typeof data === 'string';
		const buf = text ? Buffer.from(data) : data;
		try {
			socket.write(ws_frame(text ? 0x1 : 0x2, buf));
		} catch { /* 对端走了 */ }
	};
	socket.send_json = function (obj) {
		socket.send(JSON.stringify(obj));
	};

	let buf = Buffer.alloc(0);
	socket.on('data', (d) => {
		buf = Buffer.concat([buf, d]);
		for (;;) {
			if (buf.length < 2) return;
			const opcode = buf[0] & 0x0f;
			const masked = (buf[1] & 0x80) !== 0;
			let len = buf[1] & 0x7f;
			let off = 2;
			if (len === 126) {
				if (buf.length < 4) return;
				len = buf.readUInt16BE(2); off = 4;
			} else if (len === 127) {
				if (buf.length < 10) return;
				len = Number(buf.readBigUInt64BE(2)); off = 10;
			}
			const mask_len = masked ? 4 : 0;
			if (buf.length < off + mask_len + len) return;
			let payload = buf.subarray(off + mask_len, off + mask_len + len);
			if (masked) {
				const mask = buf.subarray(off, off + 4);
				const out = Buffer.allocUnsafe(len);
				for (let i = 0; i < len; i++) out[i] = payload[i] ^ mask[i & 3];
				payload = out;
			}
			buf = buf.subarray(off + mask_len + len);

			if (opcode === 0x8) { socket.end(); return; }
			if (opcode === 0x9) { socket.write(ws_frame(0xa, payload)); continue; }
			if (opcode === 0x1) {
				let req_obj;
				try { req_obj = JSON.parse(payload.toString()); } catch { continue; }
				handle_ws_message(socket, req_obj);
			}
		}
	});
	socket.on('error', () => {});
	return socket;
}

// ---------------------------------------------------------------- 输入处理

/* 指针状态：手机端的触摸要变成 vidowmsmg 的一串 move-cursor /
 * pointer-button。这里的 key 是「指针 id」，多点触控也只取第一根手指
 * （第二根用于双指滚动）。 */
const pointers = new Map();

async function handle_ws_message(ws, m) {
	try {
		if (process.env.VIDOW_REMOTE_DEBUG) {
			console.error('[msg]', JSON.stringify(m));
		}
		// Browser coordinates are output-local; IPC expects Wayland layout-global.
		if (['move', 'down', 'click'].includes(m.t)) {
			const out = ws.output || await pick_output();
			const box = out.layout || { x: 0, y: 0 };
			if (!Number.isFinite(m.x) || !Number.isFinite(m.y)) return;
			m = { ...m, x: m.x + box.x, y: m.y + box.y };
		}
		switch (m.t) {
			case 'caps': {
				console.error('[caps]', JSON.stringify(m));
				break;
			}
			case 'hello': {
				/* 客户端声明支持 WebCodecs 且当前是安全上下文时走 raw，
				 * 否则退回 fmp4（MediaSource）。服务端 --mode 显式指定时以它为准。 */
				if (ARGS.mode === 'auto') {
					ARGS.mode = m.wc ? 'raw' : 'fmp4';
				}
				const out = await pick_output();
				ws.output = out;
				ws.send_json({
					t: 'out',
					name: out.name,
					w: out.logical_width,
					h: out.logical_height,
					scale: out.scale,
					fps: ARGS.fps,
					/* 模式和解码器描述：
					 *   raw  → 前端用 WebCodecs，description = avcC
					 *   fmp4 → 前端用 MediaSource，忽略 description */
					mode: ARGS.mode,
					desc: ARGS.mode === 'raw' ? (ws.stream && ws.stream.avcc_b64) : null,
				});
				/* 重复 hello 时（页面重连、或同一条连接发了两次）必须先从
				 * 旧流里摘掉，否则同一个 ws 被 add 多次，断线时只 remove 一次，
				 * 剩下的僵尸连接会把 clients 越堆越多（实测堆到 26 条），
				 * 广播时给死 socket 写数据，所有人的画面都卡住。 */
				if (ws.stream) ws.stream.remove(ws);
				ws.stream = stream_for(out.name);
				ws.stream.add(ws);
				break;
			}
			/* 指针类走合并队列（见 enqueue_pointer），**不能 await** ——
			 * await 会让每条 move 都等上一条跑完，手指快时队列越堆越长。 */
			case 'move':
				enqueue_pointer('move-cursor', [Math.round(m.x), Math.round(m.y)]);
				break;
			case 'down':
				enqueue_pointer('move-cursor', [Math.round(m.x), Math.round(m.y)]);
				enqueue_pointer('pointer-button', ['press', m.btn || 'left']);
				break;
			case 'up':
				enqueue_pointer('pointer-button', ['release', m.btn || 'left']);
				break;
			case 'click':
				enqueue_pointer('move-cursor', [Math.round(m.x), Math.round(m.y)]);
				enqueue_pointer('pointer-button', ['click', m.btn || 'left', m.count || 1]);
				break;
			case 'scroll':
				enqueue_pointer('pointer-scroll', [Math.round(m.dx || 0), Math.round(m.dy || 0)]);
				break;
			case 'key':
				await msg('send-key', ['tap', m.key]);
				break;
			case 'text':
				await msg('type-text', [m.text]);
				break;
			case 'ws': {   // 切工作区
				/* 命令名是 focus-workspace（不是 workspace —— 后者 vidowmsg
				 * 报「用不了第 1 个位置参数」，按钮点了毫无反应就是这个）。 */
				await msg('focus-workspace', [String(m.id)]);
				break;
			}
			case 'paste': {
				/* 中文 / emoji 这类打不出来的字符：写进剪贴板再发 Ctrl+V。
				 * vidow 的 type-text 按键盘布局映射键码，非 ASCII 会直接
				 * 报「打不出来」—— 剪贴板是唯一稳的路。 */
				const txt = String(m.text || '');
				if (!txt) break;
				await msg('clipboard-set', ['--text', txt]);
				await msg('send-key', ['tap', 'ctrl+v']);
				break;
			}
			case 'action': {
				/* 跑一个 vidow 合成器动作（spawn kitty / close-window /
				 * focus-workspace next …）。见前端 ⌘ 面板的注释：
				 * 注入按键**不经过绑定表**，所以 Mod+某键 必须走这条路。 */
				await msg('action', [String(m.a || '')]);
				break;
			}
			case 'out': {  // 换一块屏看
				/* 每块屏一个独立的抓帧管道（streams 按名字存着，切回来不用重开）。
				 * 老的那条**不杀** —— 它可能在给别的客户端推流；没人看了自己会停
				 * （VideoStream 的 5 秒无数据 watchdog + del() 时清）。 */
				const target = String(m.name || '');
				if (!target) break;
				const st = await msg_json('outputs', []);
				const list = Array.isArray(st) ? st : (st.outputs || []);
				const hit = list.find((o) => o.name === target);
				if (!hit) { ws.send_json({ t: 'err', msg: '没有这块屏：' + target }); break; }
				ws.output = hit;
				if (ws.stream) ws.stream.remove(ws);
				ws.stream = stream_for(hit.name);
				ws.stream.add(ws);
				ws.send_json({
					t: 'out',
					name: hit.name,
					w: hit.logical_width || hit.width,
					h: hit.logical_height || hit.height,
					scale: hit.scale || 1,
					fps: ARGS.fps,
					mode: ARGS.mode,
					desc: null,
				});
				break;
			}
			default:
				break;
		}
	} catch (e) {
		ws.send_json({ t: 'err', msg: e.message });
	}
}

// ---------------------------------------------------------------- HTTP

const INDEX = path.join(__dirname, 'index.html');

/* HTTPS 是 WebCodecs 的硬前提：Chrome 只在安全上下文里暴露 VideoDecoder。
 * 没有证书文件时自动退回 http（功能仍在，只是手机端会走 MSE 高延迟路径）。 */
const https_on = ARGS.https && fs.existsSync(ARGS.cert) && fs.existsSync(ARGS.key);
const server = (https_on
	? require('node:https').createServer(
		{ cert: fs.readFileSync(ARGS.cert), key: fs.readFileSync(ARGS.key) }, handler)
	: http.createServer(handler));

async function handler(req, res) {
	if (!https_on && req.headers['x-forwarded-proto'] === 'https') { /* 反代场景，忽略 */ }
	const url = new URL(req.url, 'http://x');
	if (url.pathname === '/' || url.pathname === '/index.html') {
		res.writeHead(200, {
			'content-type': 'text/html; charset=utf-8',
			/* 页面是开发中的东西，改一版就想让手机立刻拿到。不缓存最省事
			 * （踩过：改了 index.html 手机还在跑老代码，白查半天）。 */
			'cache-control': 'no-store, must-revalidate',
		});
		res.end(fs.readFileSync(INDEX));
		return;
	}
	if (url.pathname === '/mpegts.js') {
		res.writeHead(200, {
			'content-type': 'text/javascript; charset=utf-8',
			'cache-control': 'public, max-age=86400',
		});
		res.end(fs.readFileSync(path.join(__dirname, 'mpegts.js')));
		return;
	}
	/* 前端日志回传：手机上没有控制台，按钮「点了没反应」只能靠这条定位。 */
	if (url.pathname === '/api/dbg') {
		console.log('[FE] ' + (url.searchParams.get('m') || ''));
		res.writeHead(204, { 'cache-control': 'no-store' });
		res.end();
		return;
	}
	if (url.pathname === '/api/state') {
		try {
			const out = await pick_output();
			const [wss, focused, outs] = await Promise.all([
				msg_json('workspaces').catch(() => []),
				msg_json('focused').catch(() => ({})),
				msg_json('outputs').catch(() => []),
			]);
			res.writeHead(200, { 'content-type': 'application/json' });
			res.end(JSON.stringify({
				output: {
					name: out.name, w: out.logical_width, h: out.logical_height,
					scale: out.scale,
				},
				/* 前端「切换显示器」要用：不带这份，按钮只能提示「拿不到列表」。 */
				outputs: (Array.isArray(outs) ? outs : []).map((o) => ({
					name: o.name,
					w: o.logical_width, h: o.logical_height,
					scale: o.scale,
					enabled: o.enabled,
				})),
				workspaces: wss.map((w) => ({
					id: w.id, idx: w.idx, name: w.name,
					active: w.active, output: w.output, shown: w.shown,
				})),
				focused,
				fps: ARGS.fps,
			}));
		} catch (e) {
			res.writeHead(500, { 'content-type': 'application/json' });
			res.end(JSON.stringify({ error: e.message }));
		}
		return;
	}
	res.writeHead(404); res.end('not found');
}

server.on('upgrade', (req, socket, head) => {
	if (!req.headers['sec-websocket-key']) {
		socket.destroy(); return;
	}
	const accept = ws_accept(req.headers['sec-websocket-key']);
	socket.write(
		'HTTP/1.1 101 Switching Protocols\r\n' +
		'Upgrade: websocket\r\n' +
		'Connection: Upgrade\r\n' +
		`Sec-WebSocket-Accept: ${accept}\r\n\r\n`);
	const ws = ws_setup(req, socket);
	ws.on('close', (had_err) => {
		if (process.env.VIDOW_REMOTE_DEBUG) console.error(`[ws] 关闭 hadError=${had_err}`);
		if (ws.stream) ws.stream.remove(ws);
	});
	/* 对端异常退出时直接销毁即可，不必刷日志（手机切后台会频繁触发） */
	ws.on('error', () => { try { ws.destroy(); } catch {} });
	ws.on('end', () => { try { ws.destroy(); } catch {} });
	/* 心跳：手机切后台 / 断网时 TCP 往往**不会**触发 close，连接就僵在那儿
	 * 一直占着带宽。定期发 ping，同时把「很久没有任何动静」的踢掉。 */
	ws.last_seen = Date.now();
	ws.on('data', () => { ws.last_seen = Date.now(); });
	const hb = setInterval(() => {
		if (ws.destroyed) { clearInterval(hb); return; }
		if (Date.now() - ws.last_seen > 30000) {
			console.error('[ws] 30 秒无动静，断开');
			clearInterval(hb);
			try { ws.destroy(); } catch {}
			return;
		}
		try { ws.write(ws_frame(0x9, Buffer.alloc(0))); } catch {}
	}, 10000);
	ws.on('close', () => clearInterval(hb));
	// Upgrade can arrive together with the first WebSocket frame.
	if (head && head.length) ws.emit('data', head);
});

// ---------------------------------------------------------------- 启动

(async () => {
	try {
		const out = await pick_output();
		console.log(`vidow-remote: 输出 ${out.name} ${out.logical_width}x` +
			`${out.logical_height} (scale ${out.scale})，${ARGS.fps}fps`);
	} catch (e) {
		console.error('vidow-remote: 找不到可用的输出：', e.message);
		console.error(`  （XDG_RUNTIME_DIR=${RUNTIME}，vidowmsg=${MSG_BIN}）`);
	}
	server.on('error', (err) => {
		if (err.code === 'EADDRINUSE') {
			console.error(`vidow-remote: 端口 ${ARGS.port} 已经被占用（本机 nginx 常年占着 8080）。`);
			console.error(`  换个端口：--port 8088`);
		} else if (err.code === 'EACCES') {
			console.error(`vidow-remote: 没有权限监听 ${ARGS.port}（1024 以下要 root）。`);
			console.error(`  换到 8088 这类高位端口。`);
		} else {
			console.error('vidow-remote: 监听失败：', err.message);
		}
		process.exit(1);
	});
	server.listen(ARGS.port, ARGS.bind, () => {
		const nets = require('node:os').networkInterfaces();
		const ips = [];
		for (const list of Object.values(nets)) {
			for (const n of list || []) {
				if (n.family === 'IPv4' && !n.internal) ips.push(n.address);
			}
		}
		const proto = https_on ? 'https' : 'http';
		console.log(`vidow-remote: ${proto}://${ARGS.bind}:${ARGS.port}  (mode=${ARGS.mode})`);
		for (const ip of ips) console.log(`  手机打开: ${proto}://${ip}:${ARGS.port}`);
		if (https_on) console.log('  自签证书：手机上点「继续前往」即可，之后 WebCodecs 才可用');
	});
})();

process.on('SIGINT', () => {
	for (const s of streams.values()) if (s.proc) s.proc.kill('SIGTERM');
	process.exit(0);
});
