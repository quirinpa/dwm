/**
 * @file dwm-ren.c
 * @brief dwm-side libxylem state-tracking and command bridge to ren/jarvis.
 *
 * Loaded inside dwm via ~/.config/dwm/mods.load.
 * - Maintains /tmp/dwm-ren-state-$USER.txt atomically on startup/focus/manage/unmanage
 * - Forwards custom _DWM_CMD opcodes (cmd == 100) non-blockingly to /tmp/ren-jarvis-$USER.sock
 */

#include "../../dwm-types.h"
#include <ttypt/xy.h>
#include <ttypt/xy-mod.h>

#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define MAX_TRACKED_WINS 256

typedef struct {
	unsigned long win_id;
	char          wm_class[64];
	char          title[256];
} dwm_ren_state_t;

static dwm_ren_state_t g_state = {0};
static unsigned long   g_wins[MAX_TRACKED_WINS];
static size_t          g_n_wins = 0;

static void get_state_paths(char *out_path, size_t path_sz, char *out_tmp, size_t tmp_sz)
{
	const char *u = getenv("USER");
	if (!u || !*u) u = "quirinpa";
	snprintf(out_path, path_sz, "/tmp/dwm-ren-state-%s.txt", u);
	snprintf(out_tmp, tmp_sz, "/tmp/dwm-ren-state-%s.tmp", u);
}

static void write_state_file(void)
{
	char path[256], tmp[256];
	get_state_paths(path, sizeof(path), tmp, sizeof(tmp));
	FILE *fp = fopen(tmp, "w");
	if (!fp) return;
	fprintf(fp, "focused_id=0x%lx\nfocused_class=%s\nfocused_title=%s\nclient_count=%zu\nts=%ld\n",
	        g_state.win_id, g_state.wm_class, g_state.title, g_n_wins, (long)time(NULL));
	fclose(fp);
	rename(tmp, path);
}

static void add_win(unsigned long win_id)
{
	if (!win_id) return;
	for (size_t i = 0; i < g_n_wins; i++) {
		if (g_wins[i] == win_id) return;
	}
	if (g_n_wins < MAX_TRACKED_WINS) {
		g_wins[g_n_wins++] = win_id;
	}
}

static void del_win(unsigned long win_id)
{
	for (size_t i = 0; i < g_n_wins; i++) {
		if (g_wins[i] == win_id) {
			g_wins[i] = g_wins[--g_n_wins];
			break;
		}
	}
	if (g_state.win_id == win_id) {
		memset(&g_state, 0, sizeof(g_state));
	}
}

static int send_ren_sock_msg(const char *msg)
{
	if (!msg || !*msg) return 0;
	const char *u = getenv("USER");
	if (!u || !*u) u = "quirinpa";
	struct sockaddr_un addr;
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	snprintf(addr.sun_path, sizeof(addr.sun_path), "/tmp/ren-jarvis-%s.sock", u);

	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
	if (fd < 0) return 0;
	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
		ssize_t ign = send(fd, msg, strlen(msg), MSG_DONTWAIT);
		(void)ign;
	}
	close(fd);
	return 1;
}

XY_MODULE_API void xy_install(void)
{
}

XY_IMPL(int, dwm_xy_on_startup, int, screen_num)
{
	(void)screen_num;
	write_state_file();
	return 0;
}

XY_IMPL(int, dwm_xy_on_focus, unsigned long, win_id, const char *, wm_class, const char *, title)
{
	g_state.win_id = win_id;
	snprintf(g_state.wm_class, sizeof(g_state.wm_class), "%s", wm_class ? wm_class : "");
	snprintf(g_state.title, sizeof(g_state.title), "%s", title ? title : "");
	add_win(win_id);
	write_state_file();
	return 0;
}

XY_IMPL(int, dwm_xy_on_manage, unsigned long, win_id, const char *, wm_class, const char *, title)
{
	add_win(win_id);
	g_state.win_id = win_id;
	snprintf(g_state.wm_class, sizeof(g_state.wm_class), "%s", wm_class ? wm_class : "");
	snprintf(g_state.title, sizeof(g_state.title), "%s", title ? title : "");
	write_state_file();
	return 0;
}

XY_IMPL(int, dwm_xy_on_unmanage, unsigned long, win_id)
{
	del_win(win_id);
	write_state_file();
	return 0;
}

XY_IMPL(int, dwm_xy_on_cmd, long, cmd, long, arg1, long, arg2)
{
	(void)arg2;
	if (cmd == 100) {
		if (arg1 == 0) return send_ren_sock_msg("/stop\n");
		if (arg1 == 1) return send_ren_sock_msg("/reset\n");
	}
	return 0;
}
