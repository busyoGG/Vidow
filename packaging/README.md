# 装成默认桌面

## 一、不要 display manager（推荐，跟 tty 起法一致）

```bash
sudo meson install -C build            # 或 sudo install -Dm755 build/vidow /usr/local/bin/vidow
sudo install -Dm644 packaging/vidow.service /etc/systemd/system/vidow.service
sudo systemctl daemon-reload
sudo systemctl set-default graphical.target
sudo systemctl enable vidow.service
```

开机直接进 tty1 的 vidow。`systemctl status vidow` 看状态，`journalctl -u vidow -f` 看日志。

## 二、有 display manager（SDDM / GDM / greetd）

```bash
sudo install -Dm644 packaging/vidow.desktop /usr/share/wayland-sessions/vidow.desktop
```

然后：
- SDDM 自动登录：`/etc/sddm.conf.d/10-vidow.conf`
  ```ini
  [Autologin]
  User=busyo
  Session=vidow
  ```
- GDM / 别的：登录界面里选 "vidow" 会话；要默认就设成自动登录。

## 三、不管哪种方式，这三件 vidow 自己会做

- `XDG_CURRENT_DESKTOP=Vidow` / `XDG_SESSION_TYPE=wayland` / `XDG_SESSION_DESKTOP`
  （`src/config.c`，键位 `env_import` / `desktop` 可关）
- portal 同步：`~/.config/vidow/portals.conf` → `~/.config/xdg-desktop-portal/vidow-portals.conf`
  （`src/portalconf.c`）
- 开机自启应用：`~/.config/vidow/configs/26-autostart.lua`

## 四、常见坑

- 服务起不来、日志里是 `Permission denied` 开 `/dev/dri/card0`：`PAMName=login` 漏了。
- 起了但没输入设备：同上；另外确认用户在 `input` 组（有的发行版需要）。
- 黑屏但日志正常：`/dev/tty1` 被 `getty@tty1` 占着，本文件里那两条
  `Conflicts=` / `After=` 就是干这个的。
