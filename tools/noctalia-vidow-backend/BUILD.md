# 编译打过 vidow 补丁的 noctalia

实测通过（2026-10-18，noctalia main，775/775 目标全过）。

## 1. 拿源码

```bash
mkdir -p ~/src && cd ~/src
curl -sSL -o noctalia-main.tar.gz \
  https://github.com/noctalia-dev/noctalia/archive/refs/heads/main.tar.gz
tar xzf noctalia-main.tar.gz
```

（`git clone` 在这台机器上会 TLS 失败，用 tarball。）

## 2. 打补丁

```bash
/home/busyo/Dev/Vidow/tools/noctalia-vidow-backend/apply-to-noctalia.sh ~/src/noctalia-main
```

五处：`compositor_detect.h` / `.cpp` / `meson.build` /
`wayland_workspaces.cpp` / 拷两个源文件。

## 3. stb 头（系统没装 stb 包）

```bash
mkdir -p ~/src/stb/stb && cd ~/src/stb
curl -sSLO --output-dir stb \
  https://raw.githubusercontent.com/nothings/stb/master/stb_image_resize2.h
curl -sSLO --output-dir stb \
  https://raw.githubusercontent.com/nothings/stb/master/stb_image_write.h
```

## 4. 编译

```bash
cd ~/src/noctalia-main
CFLAGS="-I$HOME/src/stb" CXXFLAGS="-I$HOME/src/stb" \
  meson setup build --buildtype=release
ninja -C build
```

**注意**：`C_INCLUDE_PATH` 只对 meson 的 `has_header` 探测有效，**不会**进
build.ninja —— 必须用 `CFLAGS`/`CXXFLAGS`，否则编译到
`src/render/core/stb_image_resize_impl.cpp` 就 fatal error。

## 5. 验证

```bash
strings build/noctalia | grep vidow
#  N11compositors5vidow21VidowWorkspaceBackendE
#  vidow_workspace_backend.cpp
```

## 6. 跑

```bash
~/src/noctalia-main/build/noctalia
```

替换系统包（需要 sudo，自己来）：

```bash
sudo install -m755 build/noctalia /usr/bin/noctalia
```
