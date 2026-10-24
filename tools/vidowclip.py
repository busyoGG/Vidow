#!/usr/bin/env python3
"""vidow 剪贴板 GUI：看现在是什么、翻历史、点一条复制回来、顺手改开关。

为什么要有它：剪贴板桥（src/clipboard.c）本身只有「当前这一份」，没有历史、没有
界面 —— 想看/翻/拿回来都得手敲 vidowmsg。这个窗口就是那层壳，走的是同一条 IPC
（没有第二条私有通道，所以 vidowmsg 能做的事它都能做，反过来也一样）。

窗口里有什么：
  · 状态条   CLIPBOARD / PRIMARY 现在是谁在占（origin / x_owner / 几个类型 / 多少字节）
  · 历史列表 时间 / 类型 / 来源 / 一行预览；双击或按「复制回剪贴板」= 拿回来
  · 预览     文本全文、图片缩略图、文件条目列出路径
  · 开关     同步剪贴板 / 同步 PRIMARY / 历史条数 / 历史内存预算（改了立刻 set-config）
  · 塞内容   一行文本塞进剪贴板（或者从历史里复制文件 —— 见下）

文件条目：历史里如果是文件（text/uri-list，比如从文件管理器复制的），列表里标
「文件(N)」，点「复制回剪贴板」会**原样**把 uri-list + x-special/gnome-copied-files
一起发布出去 —— 文件管理器粘出来是「复制这些文件」，终端粘出来是路径文本。要是
想重新拼一份（比如文件已经改名），选它按「复制文件」：那会走 clipboard-set-files。

用法：
  python3 tools/vidowclip.py [--socket PATH]

socket 找法跟 vidowmsg 一样：$VIDOW_SOCKET → $XDG_RUNTIME_DIR/vidow-$WAYLAND_DISPLAY.sock
→ $XDG_RUNTIME_DIR/vidow-wayland-N.sock 里第一个存在的。
"""
import base64
import json
import os
import socket
import sys
import threading
import time
import urllib.parse

import gi

gi.require_version('Gtk', '3.0')
gi.require_version('GdkPixbuf', '2.0')
from gi.repository import GLib, GdkPixbuf, Gtk  # noqa: E402

# 订阅这三个：内容变了（两边各一个）和历史变了（增/删/清/挪到最前）。
EVENTS = ["clipboard-changed", "primary-changed", "clipboard-history-changed"]

IMAGE_PREFIX = "image/"


def resolve_socket(explicit=None):
    if explicit:
        return explicit
    env = os.environ.get("VIDOW_SOCKET")
    if env:
        return env
    runtime = os.environ.get("XDG_RUNTIME_DIR")
    display = os.environ.get("WAYLAND_DISPLAY")
    if runtime and display:
        candidate = os.path.join(runtime, "vidow-%s.sock" % display)
        if os.path.exists(candidate):
            return candidate
    if runtime:
        for i in range(16):
            candidate = os.path.join(runtime, "vidow-wayland-%d.sock" % i)
            if os.path.exists(candidate):
                return candidate
    return None


class IpcError(Exception):
    pass


class Ipc:
    """一次调用一条连接（简单、不会被事件流搅乱）。"""

    def __init__(self, path):
        self.path = path

    def call(self, cmd, **kw):
        req = {"cmd": cmd}
        req.update(kw)
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        sock.settimeout(8.0)
        try:
            sock.connect(self.path)
            sock.sendall((json.dumps(req, ensure_ascii=False) + "\n").encode("utf-8"))
            buf = b""
            while b"\n" not in buf:
                chunk = sock.recv(1 << 20)
                if not chunk:
                    break
                buf += chunk
        finally:
            sock.close()
        if b"\n" not in buf:
            raise IpcError("vidow 没回话（socket 断了？）")
        resp = json.loads(buf.split(b"\n")[0].decode("utf-8", "replace"))
        if not resp.get("ok"):
            raise IpcError(resp.get("error", "命令没做成"))
        return resp.get("result", {})


class Watcher(threading.Thread):
    """订阅事件：一条长连接读 JSON 行，交给 GLib 主线程（idle_add）刷新界面。
    断了就重连（合成器重启 / socket 换名字都算正常事）。"""

    daemon = True

    def __init__(self, ipc, on_event):
        super().__init__()
        self.ipc = ipc
        self.on_event = on_event
        self.stop = False

    def run(self):
        while not self.stop:
            try:
                sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                sock.settimeout(1.0)
                sock.connect(self.ipc.path)
                sock.sendall((json.dumps(
                    {"cmd": "subscribe", "events": EVENTS}) + "\n").encode("utf-8"))
                buf = b""
                while not self.stop:
                    try:
                        chunk = sock.recv(1 << 16)
                    except socket.timeout:
                        continue
                    if not chunk:
                        break
                    buf += chunk
                    while b"\n" in buf:
                        line, buf = buf.split(b"\n", 1)
                        if not line.strip():
                            continue
                        try:
                            event = json.loads(line.decode("utf-8", "replace"))
                        except ValueError:
                            continue
                        GLib.idle_add(self.on_event, event)
                sock.close()
            except OSError:
                pass
            if not self.stop:
                time.sleep(1.0)


def uri_to_path(uri):
    """file:///tmp/a%20b → /tmp/a b（别的 scheme 原样返回）。"""
    if not uri.startswith("file://"):
        return uri
    rest = uri[len("file://"):]
    # file://host/path：host 非空且不是 localhost 的情况这里不管（本地剪贴板见不到）
    slash = rest.find("/")
    if slash > 0 and rest[:slash] != "localhost":
        return rest
    return urllib.parse.unquote(rest)


def human_bytes(count):
    count = float(count or 0)
    if count < 1024:
        return "%d B" % count
    if count < 1024 * 1024:
        return "%.1f KB" % (count / 1024.0)
    return "%.1f MB" % (count / 1048576.0)


def short_time(age_ms):
    stamp = time.time() - max(0.0, age_ms / 1000.0)
    return time.strftime("%H:%M:%S", time.localtime(stamp))


def one_line(text, limit=70):
    text = text.replace("\r\n", "\n").replace("\r", "\n")
    text = " ⏎ ".join(part for part in text.split("\n") if part)
    text = "".join(ch if ch.isprintable() else "·" for ch in text)
    return text[:limit] + ("…" if len(text) > limit else "")


class ClipPage(Gtk.Box):
    """设置窗口里的「剪贴板」这一页。

    原来是独立的 ClipWindow；现在只是 SettingsWindow 右边 Notebook 的一页，
    所以不再自己 set_default_size / connect destroy —— 窗口生命周期归外壳。
    """

    def __init__(self, ipc):
        super().__init__(orientation=Gtk.Orientation.VERTICAL, spacing=6)
        self.set_border_width(8)
        self.ipc = ipc
        self.entries = []
        self.selected = None       # 选中的那条（dict）
        self.dirty = False

        root = self

        self.status = Gtk.Label(xalign=0.0)
        self.status.set_line_wrap(True)
        root.pack_start(self.status, False, False, 0)

        root.pack_start(Gtk.Separator(orientation=Gtk.Orientation.HORIZONTAL),
                        False, False, 0)
        panes = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=8)
        root.pack_start(panes, True, True, 0)

        # ---- 历史列表 ----
        left = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=4)
        panes.pack_start(left, True, True, 0)
        left.pack_start(Gtk.Label(label="历史（新的在上面）", xalign=0.0),
                        False, False, 0)
        self.store = Gtk.ListStore(str, str, str, str, int)  # 时间 类型 来源 预览 id
        self.view = Gtk.TreeView(model=self.store)
        self.view.set_headers_visible(True)
        for idx, title, width in ((0, "时间", 80), (1, "类型", 90),
                                  (2, "来源", 90), (3, "内容", -1)):
            renderer = Gtk.CellRendererText()
            renderer.set_property("ellipsize", 3)
            column = Gtk.TreeViewColumn(title, renderer, text=idx)
            column.set_min_width(width)
            self.view.append_column(column)
        self.view.connect("cursor-changed", self.on_cursor_changed)
        self.view.connect("row-activated", self.on_row_activated)
        scroll = Gtk.ScrolledWindow()
        scroll.set_policy(Gtk.PolicyType.AUTOMATIC, Gtk.PolicyType.AUTOMATIC)
        scroll.add(self.view)
        left.pack_start(scroll, True, True, 0)

        buttons = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=4)
        left.pack_start(buttons, False, False, 0)
        self.btn_use = Gtk.Button(label="复制回剪贴板")
        self.btn_use.connect("clicked", lambda _b: self.use_selected())
        buttons.pack_start(self.btn_use, False, False, 0)
        self.btn_files = Gtk.Button(label="复制文件")
        self.btn_files.set_tooltip_text(
            "按路径重新拼一份 uri-list（文件改名/移动后用这个；只是原样拿回来用"
            "「复制回剪贴板」）")
        self.btn_files.connect("clicked", lambda _b: self.copy_files_selected())
        buttons.pack_start(self.btn_files, False, False, 0)
        btn_del = Gtk.Button(label="删除")
        btn_del.connect("clicked", lambda _b: self.delete_selected())
        buttons.pack_start(btn_del, False, False, 0)
        btn_clear = Gtk.Button(label="清空历史")
        btn_clear.connect("clicked", lambda _b: self.clear_history())
        buttons.pack_start(btn_clear, False, False, 0)

        # ---- 预览 ----
        right = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=4)
        panes.pack_start(right, True, True, 0)
        self.preview_title = Gtk.Label(label="预览", xalign=0.0)
        right.pack_start(self.preview_title, False, False, 0)
        self.stack = Gtk.Stack()
        self.stack.set_transition_type(Gtk.StackTransitionType.CROSSFADE)
        right.pack_start(self.stack, True, True, 0)

        self.text_view = Gtk.TextView()
        self.text_view.set_editable(False)
        self.text_view.set_cursor_visible(False)
        self.text_view.set_wrap_mode(Gtk.WrapMode.WORD_CHAR)
        text_scroll = Gtk.ScrolledWindow()
        text_scroll.add(self.text_view)
        self.stack.add_named(text_scroll, "text")

        self.image = Gtk.Image()
        image_box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL)
        image_box.pack_start(self.image, True, True, 0)
        self.stack.add_named(image_box, "image")

        self.file_list = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=2)
        file_scroll = Gtk.ScrolledWindow()
        file_scroll.add(self.file_list)
        self.stack.add_named(file_scroll, "files")

        self.placeholder = Gtk.Label(label="（选一条看内容）", xalign=0.0)
        self.stack.add_named(self.placeholder, "empty")

        # ---- 塞内容 ----
        setter = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=4)
        root.pack_start(setter, False, False, 0)
        setter.pack_start(Gtk.Label(label="设为剪贴板："), False, False, 0)
        self.set_entry = Gtk.Entry()
        self.set_entry.set_placeholder_text("一行文本…")
        self.set_entry.connect("activate", lambda _w: self.set_text())
        setter.pack_start(self.set_entry, True, True, 0)
        btn_set = Gtk.Button(label="塞进去")
        btn_set.connect("clicked", lambda _b: self.set_text())
        setter.pack_start(btn_set, False, False, 0)
        btn_clear_clip = Gtk.Button(label="清空剪贴板")
        btn_clear_clip.connect("clicked", lambda _b: self.clear_clipboard())
        setter.pack_start(btn_clear_clip, False, False, 0)

        # ---- 开关 ----
        switches = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=10)
        root.pack_start(switches, False, False, 0)
        self.sw_clip = Gtk.CheckButton(label="同步剪贴板")
        self.sw_clip.connect("toggled", self.on_config_toggled, "clipboard")
        switches.pack_start(self.sw_clip, False, False, 0)
        self.sw_primary = Gtk.CheckButton(label="同步 PRIMARY（选中即复制）")
        self.sw_primary.connect("toggled", self.on_config_toggled,
                                "clipboard_primary")
        switches.pack_start(self.sw_primary, False, False, 0)
        switches.pack_start(Gtk.Label(label="历史条数："), False, False, 0)
        self.sp_history = Gtk.SpinButton.new_with_range(0, 200, 1)
        self.sp_history.connect("value-changed", self.on_history_count)
        switches.pack_start(self.sp_history, False, False, 0)
        switches.pack_start(Gtk.Label(label="历史内存上限（MiB）："), False, False, 0)
        self.sp_bytes = Gtk.SpinButton.new_with_range(0, 4096, 8)
        self.sp_bytes.connect("value-changed", self.on_history_bytes)
        switches.pack_start(self.sp_bytes, False, False, 0)
        self.hint = Gtk.Label(xalign=0.0)
        self.hint.set_line_wrap(True)
        root.pack_start(self.hint, False, False, 0)

        self.suppress = False  # 程序改控件时别把 toggled 再发回去
        self.refresh(force=True)
        GLib.timeout_add(500, self.on_tick)
        Watcher(self.ipc, self.on_event).start()

    # ---------------------------------------------------------------- 刷新

    def on_tick(self):
        # 事件是「有事发生」的通知，内容本身按秒兜底拉一遍（订阅掉了也不瞎）
        self.refresh()
        return True

    def on_event(self, _event):
        self.dirty = True
        return False

    def refresh(self, force=False):
        if not (self.dirty or force):
            return
        self.dirty = False
        try:
            state = self.ipc.call("clipboard")
            history = self.ipc.call("clipboard-history")
        except (IpcError, OSError, ValueError) as exc:
            self.status.set_text("连不上 vidow：%s" % exc)
            return
        self.update_status(state)
        self.update_switches(state)
        self.update_history(history)
        self.update_preview()

    def update_status(self, state):
        parts = []
        for key, name in (("clipboard", "CLIPBOARD"), ("primary", "PRIMARY")):
            sel = state.get(key) or {}
            origin = {"x11": "X11", "wayland": "Wayland", "none": "空"}.get(
                sel.get("origin"), sel.get("origin", "?"))
            bits = ["%s: %s" % (name, origin)]
            if sel.get("types"):
                bits.append("%d 类型" % sel["types"])
                bits.append(human_bytes(sel.get("bytes", 0)))
            if sel.get("x_owner"):
                bits.append("X owner=0x%x%s" % (
                    sel["x_owner"],
                    "（我们）" if sel.get("x_owner_is_ours") else ""))
            if sel.get("publishing"):
                bits.append("已发布")
            if sel.get("fetching_x11"):
                bits.append("正在取 X11 的内容")
            parts.append(" · ".join(bits))
        clip = state.get("clipboard") or {}
        bridge = "接管中" if clip.get("bridging") else "没接管（退回 wlroots 那套）"
        self.status.set_text("%s   ｜   桥：%s" % ("　　".join(parts), bridge))

    def update_switches(self, state):
        self.suppress = True
        self.sw_clip.set_active(bool(state.get("enabled")))
        self.sw_primary.set_active(bool(state.get("primary_enabled")))
        self.suppress = False

    def update_history(self, history):
        self.entries = history.get("entries", [])
        limit = history.get("limit", 0)
        self.suppress = True
        if not self.sp_history.has_focus():
            self.sp_history.set_value(limit)
        if not self.sp_bytes.has_focus():
            self.sp_bytes.set_value(history.get("limit_bytes", 0) // (1 << 20))
        self.suppress = False
        self.hint.set_text(
            "历史 %d/%s 条 · %s/%s（clipboard_history / "
            "clipboard_history_bytes；0 条 = 不留历史）" % (
                history.get("count", 0), limit if limit > 0 else "∞",
                human_bytes(history.get("bytes", 0)),
                human_bytes(history.get("limit_bytes", 0))))
        wanted = self.selected["id"] if self.selected else None
        self.store.clear()
        self.selected = None
        for entry in self.entries:
            self.store.append([
                short_time(entry.get("age_ms", 0)),
                self.kind_label(entry),
                self.origin_label(entry),
                self.row_preview(entry),
                int(entry.get("id", 0)),
            ])
            if wanted is not None and int(entry.get("id", 0)) == wanted:
                self.selected = entry
        if self.selected is None and self.entries:
            # 没选过（或选中的那条被删了）：默认落回最新的一条，预览不至于空着
            self.selected = self.entries[0]
            self.view.set_cursor(Gtk.TreePath.new_from_indices([0]))
        elif self.selected is not None:
            for i, entry in enumerate(self.entries):
                if int(entry.get("id", 0)) == wanted:
                    self.view.set_cursor(Gtk.TreePath.new_from_indices([i]))
                    break
        self.btn_use.set_sensitive(self.selected is not None)
        self.btn_files.set_sensitive(
            self.selected is not None and bool(self.selected.get("is_file")))

    @staticmethod
    def kind_label(entry):
        if entry.get("is_file"):
            return "文件(%d)" % len(entry.get("files", []))
        mime = entry.get("mime") or ""
        if mime.startswith(IMAGE_PREFIX):
            return "图片"
        if mime.startswith("text/") or mime in ("UTF8_STRING", "STRING"):
            return "文本"
        return mime or "?"

    @staticmethod
    def origin_label(entry):
        where = "选区" if entry.get("selection") == "primary" else "剪贴板"
        return "%s/%s" % (where, "X11" if entry.get("origin") == "x11" else "Wl")

    @staticmethod
    def row_preview(entry):
        files = entry.get("files") or []
        if files:
            names = [os.path.basename(uri_to_path(u)) for u in files]
            return one_line("、".join(names) or "（文件）")
        text = entry.get("text")
        if text:
            return one_line(text)
        return "%s（%s）" % (entry.get("mime") or "?",
                             human_bytes(entry.get("size", 0)))

    # ---------------------------------------------------------------- 预览

    def update_preview(self):
        entry = self.selected
        self.btn_files.set_sensitive(entry is not None and
                                     bool(entry.get("is_file")))
        if entry is None:
            self.preview_title.set_text("预览")
            self.stack.set_visible_child_name("empty")
            return
        self.preview_title.set_text("预览 · %s · %s · %s" % (
            entry.get("mime") or "?", human_bytes(entry.get("size", 0)),
            self.origin_label(entry)))
        if entry.get("is_file"):
            self.show_files(entry)
            return
        mime = entry.get("mime") or ""
        if mime.startswith(IMAGE_PREFIX):
            if self.show_image(entry):
                return
        if mime.startswith("text/") or mime in ("UTF8_STRING", "STRING"):
            self.show_text(entry)
            return
        self.stack.set_visible_child_name("empty")

    def fetch(self, entry, base64_=False):
        return self.ipc.call("clipboard-history-get", id=int(entry.get("id", 0)),
                             base64=base64_)

    def show_text(self, entry):
        try:
            data = self.fetch(entry)
        except (IpcError, OSError, ValueError) as exc:
            self.preview_title.set_text("预览失败：%s" % exc)
            return
        text = data.get("text")
        if text is None and data.get("base64"):
            raw = base64.b64decode(data["base64"])
            text = raw.decode("utf-8", "replace")
        self.text_view.get_buffer().set_text(text or "")
        self.stack.set_visible_child_name("text")

    def show_image(self, entry):
        try:
            data = self.fetch(entry, base64_=True)
            raw = base64.b64decode(data.get("base64", ""))
            loader = GdkPixbuf.PixbufLoader()
            loader.write(raw)
            loader.close()
            pixbuf = loader.get_pixbuf()
        except Exception:  # noqa: BLE001 —— 图片解不开就当看不了，不炸窗口
            return False
        if pixbuf is None:
            return False
        width = max(64, self.stack.get_allocated_width() - 16)
        height = max(64, self.stack.get_allocated_height() - 16)
        scale = min(width / float(pixbuf.get_width() or 1),
                    height / float(pixbuf.get_height() or 1), 1.0)
        self.image.set_from_pixbuf(pixbuf.scale_simple(
            max(1, int(pixbuf.get_width() * scale)),
            max(1, int(pixbuf.get_height() * scale)),
            GdkPixbuf.InterpType.BILINEAR))
        self.stack.set_visible_child_name("image")
        return True

    def show_files(self, entry):
        for child in self.file_list.get_children():
            self.file_list.remove(child)
        for uri in entry.get("files") or []:
            row = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=6)
            row.pack_start(Gtk.Label(label="📄"), False, False, 0)
            label = Gtk.Label(label=uri_to_path(uri), xalign=0.0)
            label.set_selectable(True)
            row.pack_start(label, True, True, 0)
            self.file_list.pack_start(row, False, False, 0)
        note = Gtk.Label(xalign=0.0)
        note.set_line_wrap(True)
        note.set_text("「复制回剪贴板」= 原样发布 uri-list + x-special/gnome-copied-files"
                      "（文件管理器粘出来是复制文件）；「复制文件」= 按这些路径重新拼一份"
                      "（文件改名/移动后用）。")
        self.file_list.pack_start(note, False, False, 0)
        self.file_list.show_all()
        self.stack.set_visible_child_name("files")

    # ---------------------------------------------------------------- 动作

    def on_cursor_changed(self, view):
        model, path = view.get_selection().get_selected()
        if path is None:
            self.selected = None
        else:
            wanted = model[path][4]
            self.selected = next((e for e in self.entries
                                  if int(e.get("id", 0)) == wanted), None)
        self.update_preview()

    def on_row_activated(self, view, path, column):
        self.use_selected()

    def use_selected(self):
        if self.selected is None:
            return
        self.act("clipboard-history-use", id=int(self.selected.get("id", 0)))

    def copy_files_selected(self):
        if self.selected is None:
            return
        paths = [uri_to_path(uri) for uri in self.selected.get("files") or []]
        if not paths:
            return
        self.act("clipboard-set-files", files=paths)

    def delete_selected(self):
        if self.selected is None:
            return
        self.act("clipboard-history-delete", id=int(self.selected.get("id", 0)))

    def clear_history(self):
        self.act("clipboard-history-clear")

    def clear_clipboard(self):
        self.act("clipboard-clear")

    def set_text(self):
        text = self.set_entry.get_text()
        if not text:
            return
        if self.act("clipboard-set", text=text):
            self.set_entry.set_text("")

    def on_config_toggled(self, button, key):
        if self.suppress:
            return
        self.act("set-config", key=key,
                 value="true" if button.get_active() else "false")

    def on_history_count(self, spin):
        if self.suppress:
            return
        self.act("set-config", key="clipboard_history",
                 value=str(int(spin.get_value())))

    def on_history_bytes(self, spin):
        if self.suppress:
            return
        self.act("set-config", key="clipboard_history_bytes",
                 value=str(int(spin.get_value()) * (1 << 20)))

    def act(self, cmd, **kw):
        try:
            self.ipc.call(cmd, **kw)
        except (IpcError, OSError, ValueError) as exc:
            self.status.set_text("%s 没做成：%s" % (cmd, exc))
            return False
        self.refresh(force=True)
        return True


class SettingsWindow(Gtk.Window):
    """vidow 设置。

    外形按「设置」来：左边一列分类，右边是这一类的面板。
    目前只有「剪贴板」一类 —— 后面加别的设置项就再 append 一个页面。
    """

    PAGES = [
        # (图标, 标题, 工厂)
        ("📋", "剪贴板", ClipPage),
    ]

    def __init__(self, ipc):
        super().__init__(title="vidow 设置")
        self.set_default_size(980, 660)
        self.connect("destroy", Gtk.main_quit)

        outer = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=0)
        self.add(outer)

        # ---- 左侧分类 ----
        side = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=2)
        side.set_size_request(180, -1)
        side.get_style_context().add_class("sidebar")
        side.set_border_width(10)
        outer.pack_start(side, False, False, 0)

        # ---- 右侧内容 ----
        self.stack = Gtk.Stack()
        self.stack.set_hexpand(True)
        self.stack.set_vexpand(True)
        outer.pack_start(self.stack, True, True, 0)

        self.buttons = []
        self.pages = {}
        for icon, title, factory in self.PAGES:
            page = factory(ipc)
            self.stack.add_named(page, title)
            self.pages[title] = page

            btn = Gtk.Button()
            box = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=8)
            ico = Gtk.Label(label=icon)
            lbl = Gtk.Label(label=title, xalign=0.0)
            box.pack_start(ico, False, False, 0)
            box.pack_start(lbl, True, True, 0)
            btn.add(box)
            btn.get_style_context().add_class("flat")
            btn.connect("clicked", self.on_nav, title)
            side.pack_start(btn, False, False, 0)
            self.buttons.append((title, btn))

        side.pack_start(Gtk.Box(), True, True, 0)   # 把下半截顶开
        self.select(self.PAGES[0][1])

    def on_nav(self, _btn, title):
        self.select(title)

    def select(self, title):
        self.stack.set_visible_child_name(title)
        for name, btn in self.buttons:
            on = (name == title)
            btn.get_style_context().remove_class("suggested-action")
            if on:
                btn.get_style_context().add_class("suggested-action")


def self_check(ipc):
    """--check：不建窗口，只把 IPC 这条路走一遍（脚本里判断能不能用）。"""
    state = ipc.call("clipboard")
    history = ipc.call("clipboard-history")
    print("socket   %s" % ipc.path)
    print("clipboard enabled=%s bridging=%s origin=%s" % (
        state.get("enabled"), (state.get("clipboard") or {}).get("bridging"),
        (state.get("clipboard") or {}).get("origin")))
    print("history  %d 条 / %d 字节（上限 %s 条）" % (
        history.get("count"), history.get("bytes"), history.get("limit")))
    for entry in history.get("entries", [])[:5]:
        print("  #%s %s %s %s" % (entry.get("id"), entry.get("mime"),
                                  "文件" if entry.get("is_file") else "",
                                  one_line(entry.get("text") or "", 40)))
    return 0


def main():
    socket_path = None
    argv = sys.argv[1:]
    check = False
    while argv:
        arg = argv.pop(0)
        if arg == "--socket" and argv:
            socket_path = argv.pop(0)
        elif arg == "--check":
            check = True
        elif arg in ("-h", "--help"):
            print(__doc__)
            return 0
        else:
            print("vidowclip.py: 不认识的参数 %s（--socket PATH / --check）" % arg)
            return 2
    path = resolve_socket(socket_path)
    if path is None or not os.path.exists(path):
        print("vidowclip.py: 找不到 vidow 的 socket（--socket 指一个，或设好 "
              "VIDOW_SOCKET / XDG_RUNTIME_DIR + WAYLAND_DISPLAY）")
        return 2
    ipc = Ipc(path)
    if check:
        try:
            return self_check(ipc)
        except (IpcError, OSError, ValueError) as exc:
            print("vidowclip.py: IPC 走不通：%s" % exc)
            return 1
    window = SettingsWindow(ipc)
    window.show_all()
    Gtk.main()
    return 0


if __name__ == "__main__":
    sys.exit(main())
