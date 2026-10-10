/* 环境变量账本：vidow 自己设的、用户用 vidow.env{} 声明的、以及和 systemd 用户
 * 管理器两个方向的对账。见 env.c。
 *
 * 为什么要有这一份：从 vidow 里起来的 shell 命令继承的是**进程环境**，而由 systemd
 * 拉起来的应用（xdg-desktop-portal、systemd 的 graphical-session.target 里那些、
 * `systemd-run --user` 起来的）继承的是**用户管理器里的环境** —— 两个不打通就会出现
 * 「配置里 exec 起来的程序连得上 vidow，systemd 起来的连的是宿主合成器（或者没有
 * Wayland）」。 */
#ifndef VIDOW_ENV_H
#define VIDOW_ENV_H

#include <stdbool.h>
#include <stddef.h>

struct vidow_server;
struct jbuf;

/* 账本（server->env_vars）+ 导出用的 idle 源。 */
bool vidow_env_init(struct vidow_server *server);
/* 退出：把用户改过又没还回去的变量还原回原样（别污染调用方的环境）。 */
void vidow_env_finish(struct vidow_server *server);

/* 启动早期调一次：把 systemd 用户管理器里 vidow 自己没有的变量吸收进来（哪些
 * 不吸见 env.c 的 blacklist —— WAYLAND_DISPLAY / DISPLAY 这类由我们自己定）。
 * systemd_env = 0 时什么都不做。 */
void vidow_env_absorb_systemd(struct vidow_server *server);

/* 名字合不合法（[A-Za-z_][A-Za-z0-9_]*，长度 < 128）。 */
bool vidow_env_name_ok(const char *name);

/* 设 / 删一个变量：进账本（记着原值，退出时还原），然后排队推给 systemd。 */
bool vidow_env_set(struct vidow_server *server, const char *name,
		const char *value, char *err, size_t errlen);
bool vidow_env_unset(struct vidow_server *server, const char *name, char *err,
		size_t errlen);

/* 推给 systemd 用户管理器 + dbus 激活环境（import-environment +
 * dbus-update-activation-environment --systemd）。later 那个合并到下一次 idle：
 * 配置里连着设十个变量也只 fork 一次。systemd_env_export = 0 时是空操作。 */
void vidow_env_export_now(struct vidow_server *server);
void vidow_env_export_later(struct vidow_server *server);

/* 配置落地后对一次账（vidow_config_sync 里调）。 */
void vidow_env_apply_config(struct vidow_server *server);

/* IPC {"cmd":"env"} 的载荷：我们管的变量（带原值）+ 会推给 systemd 的那些。 */
void vidow_env_json(struct jbuf *b, struct vidow_server *server);

#endif /* VIDOW_ENV_H */
