/**
 * @file ren-dwm.c
 * @brief Standalone ren extension for dwm window manager control.
 *
 * ZERO libjarvis dependency — works standalone via `ren -m ren-dwm:ren-mm`.
 * Tools: workspace, window, open_terminal.
 */

#define _GNU_SOURCE
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include <ttypt/libren.h>
#include <ttypt/xy.h>
#include "../dwm_ctl.h"

static int tool_workspace(const char *args, char **out_result)
{
	while (args && *args == ' ') args++;
	if (!args || !*args) {
		*out_result = strdup("Error: usage: workspace <1..9 | move 1..9>");
		return -1;
	}
	bool move = false;
	int ws = 0;
	if (strncasecmp(args, "move ", 5) == 0) {
		move = true;
		ws = atoi(args + 5);
	} else {
		ws = atoi(args);
	}
	if (ws < 1 || ws > 9) {
		*out_result = strdup("Error: workspace must be 1..9");
		return -1;
	}

	Display *dpy = XOpenDisplay(NULL);
	if (!dpy) {
		*out_result = strdup("Error: cannot open X11 display");
		return -1;
	}
	Window root = DefaultRootWindow(dpy);
	char buf[128];
	if (move) {
		dwm_win_info_t foc;
		if (!dwm_ctl_get_focused(&foc) || !foc.id[0]) {
			XCloseDisplay(dpy);
			*out_result = strdup("Error: no focused window to move");
			return -1;
		}
		Window win = (Window)strtoul(foc.id, NULL, 0);
		dwm_ctl_send2(dpy, root, DWM_CMD_TAG_WIN, (long)win, (long)(1U << (ws - 1)));
		snprintf(buf, sizeof(buf), "moved window %s to workspace %d", foc.id, ws);
	} else {
		dwm_ctl_send(dpy, root, DWM_CMD_VIEW, (long)(1U << (ws - 1)));
		snprintf(buf, sizeof(buf), "switched to workspace %d", ws);
	}
	XCloseDisplay(dpy);
	*out_result = strdup(buf);
	return 0;
}

static int tool_window(const char *args, char **out_result)
{
	while (args && *args == ' ') args++;
	if (!args || !*args) {
		*out_result = strdup("Error: usage: window <close | fullscreen | float | focus <id_or_class_or_title|other|next|previous|master> | layout <tile|float|monocle|next> | gaps <delta>>");
		return -1;
	}

	Display *dpy = XOpenDisplay(NULL);
	if (!dpy) {
		*out_result = strdup("Error: cannot open X11 display");
		return -1;
	}
	Window root = DefaultRootWindow(dpy);

	if (strcasecmp(args, "close") == 0) {
		dwm_ctl_send(dpy, root, DWM_CMD_KILLCLIENT, 0);
		XCloseDisplay(dpy);
		*out_result = strdup("closed window");
		return 0;
	}
	if (strcasecmp(args, "fullscreen") == 0) {
		dwm_ctl_send(dpy, root, DWM_CMD_TOGGLEFULLSCREEN, 0);
		XCloseDisplay(dpy);
		*out_result = strdup("toggled fullscreen");
		return 0;
	}
	if (strcasecmp(args, "float") == 0) {
		dwm_ctl_send(dpy, root, DWM_CMD_TOGGLEFLOATING, 0);
		XCloseDisplay(dpy);
		*out_result = strdup("toggled floating");
		return 0;
	}
	if (strncasecmp(args, "layout ", 7) == 0) {
		const char *l = args + 7;
		while (*l == ' ') l++;
		long idx = -1;
		if (strcasecmp(l, "tile") == 0) idx = 0;
		else if (strcasecmp(l, "float") == 0) idx = 1;
		else if (strcasecmp(l, "monocle") == 0) idx = 2;
		dwm_ctl_send(dpy, root, DWM_CMD_SETLAYOUT, idx);
		XCloseDisplay(dpy);
		char buf[96];
		snprintf(buf, sizeof(buf), "set layout %s", *l ? l : "next");
		*out_result = strdup(buf);
		return 0;
	}
	if (strncasecmp(args, "gaps ", 5) == 0) {
		int delta = atoi(args + 5);
		dwm_ctl_send(dpy, root, DWM_CMD_SETGAPS, (long)delta);
		XCloseDisplay(dpy);
		char buf[64];
		snprintf(buf, sizeof(buf), "adjusted gaps %+d", delta);
		*out_result = strdup(buf);
		return 0;
	}
	if (strncasecmp(args, "focus ", 6) == 0) {
		const char *target = args + 6;
		while (*target == ' ') target++;
		if (strcasecmp(target, "previous") == 0) {
			dwm_ctl_send(dpy, root, DWM_CMD_FOCUSSTACK, -1);
			XCloseDisplay(dpy);
			*out_result = strdup("focused previous window");
			return 0;
		}
		if (strcasecmp(target, "next") == 0) {
			dwm_ctl_send(dpy, root, DWM_CMD_FOCUSSTACK, 1);
			XCloseDisplay(dpy);
			*out_result = strdup("focused next window");
			return 0;
		}
		if (strcasecmp(target, "master") == 0) {
			dwm_ctl_send(dpy, root, DWM_CMD_ZOOM, 0);
			XCloseDisplay(dpy);
			*out_result = strdup("swapped with master");
			return 0;
		}
		XCloseDisplay(dpy);

		dwm_win_info_t clients[32], foc, oth;
		size_t n = dwm_ctl_query_windows(clients, 32, &foc, &oth);
		if (strcasecmp(target, "other") == 0) {
			if (!oth.id[0]) {
				*out_result = strdup("Error: no other window available");
				return -1;
			}
			bool ok = dwm_ctl_focus_win(oth.id, 1.0);
			char buf[160];
			snprintf(buf, sizeof(buf), "%s other window: %s", ok ? "focused" : "failed", oth.wm_class);
			*out_result = strdup(buf);
			return ok ? 0 : -1;
		}
		if (strncmp(target, "0x", 2) == 0) {
			bool ok = dwm_ctl_focus_win(target, 1.2);
			*out_result = strdup(ok ? "focused window" : "Error: failed to focus window");
			return ok ? 0 : -1;
		}
		for (int pass = 0; pass < 2; pass++) {
			for (size_t i = 0; i < n; i++) {
				if (pass == 0 && !clients[i].visible) continue;
				if (dwm_ctl_str_ci(clients[i].wm_class, target) ||
				    dwm_ctl_str_ci(clients[i].title, target)) {
					bool ok = dwm_ctl_focus_win(clients[i].id, 1.2);
					char buf[192];
					snprintf(buf, sizeof(buf), "%s %s: %.64s",
					         ok ? "focused" : "failed to focus",
					         clients[i].wm_class, clients[i].title);
					*out_result = strdup(buf);
					return ok ? 0 : -1;
				}
			}
		}
		char err[192];
		snprintf(err, sizeof(err), "Error: window not found: %.120s", target);
		*out_result = strdup(err);
		return -1;
	}

	XCloseDisplay(dpy);
	*out_result = strdup("Error: usage: window <close | fullscreen | float | focus <target> | layout <tile|float|monocle|next> | gaps <delta>>");
	return -1;
}

static int tool_open_terminal(const char *args, char **out_result)
{
	while (args && *args == ' ') args++;
	char msg[256] = {0}, err[256] = {0};
	if (!args || !*args) {
		if (dwm_ctl_open_terminal("st", msg, sizeof(msg), err, sizeof(err))) {
			*out_result = strdup(msg);
			return 0;
		}
		*out_result = strdup(err[0] ? err : "Error: failed to open terminal");
		return -1;
	}

	char wrapped[1024];
	snprintf(wrapped, sizeof(wrapped), "%.900s; exec \"${SHELL:-/bin/sh}\"", args);
	dwm_win_info_t prev;
	dwm_ctl_get_focused(&prev);
	char *const argv[] = { "st", "-e", "/bin/sh", "-c", wrapped, NULL };
	pid_t pid = dwm_ctl_spawn_argv(argv);
	if (pid <= 0) {
		*out_result = strdup("Error: failed to launch st");
		return -1;
	}
	double deadline = dwm_ctl_now_sec() + 2.0;
	usleep(40000);
	while (dwm_ctl_now_sec() < deadline) {
		dwm_win_info_t foc;
		if (dwm_ctl_get_focused(&foc) && dwm_ctl_str_ci(foc.wm_class, "st-256color")) {
			if (!prev.id[0] || strcasecmp(foc.id, prev.id) != 0) break;
		}
		usleep(35000);
	}
	char out[512];
	snprintf(out, sizeof(out), "opened terminal running: %.300s", args);
	*out_result = strdup(out);
	return 0;
}

XY_MODULE_API void xy_install(void)
{
	ren_register_tool("workspace", "workspace <1..9 | move 1..9>", tool_workspace);
	ren_register_tool("window", "window <close | fullscreen | float | focus <class_or_title|other|next|previous|master> | layout <tile|float|monocle|next> | gaps <delta>>", tool_window);
	ren_register_tool("open_terminal", "open_terminal [command]", tool_open_terminal);
}
