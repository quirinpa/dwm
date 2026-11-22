#ifndef DWM_CTL_H
#define DWM_CTL_H

/**
 * @file dwm_ctl.h
 * @brief Self-contained pure-C X11 + _DWM_CMD + /proc terminal helpers for dwm modules.
 *
 * Shared by both dwm-jarvis.so and ren-dwm.so. Zero libjarvis dependency.
 */

#include "../dwm-types.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

typedef struct {
	char          id[32];
	char          wm_class[64];
	char          title[256];
	int           tag;
	bool          visible;
	bool          focused;
	unsigned long pid;
} dwm_win_info_t;

static double g_dwm_last_term_open_ts = 0.0;

static __attribute__((unused)) double dwm_ctl_now_sec(void)
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return (double)tv.tv_sec + (double)tv.tv_usec / 1e6;
}

static __attribute__((unused)) void dwm_ctl_trim(char *s)
{
	if (!s) return;
	size_t l = strlen(s);
	while (l > 0 && isspace((unsigned char)s[l - 1])) {
		s[--l] = '\0';
	}
}

static __attribute__((unused)) bool dwm_ctl_str_ci(const char *haystack, const char *needle)
{
	if (!haystack || !needle) return false;
	size_t nlen = strlen(needle);
	if (nlen == 0) return true;
	for (; *haystack; haystack++) {
		if (strncasecmp(haystack, needle, nlen) == 0) return true;
	}
	return false;
}

static __attribute__((unused)) bool dwm_ctl_is_terminal_class(const char *cls)
{
	static const char *const TERMS[] = {
		"st-256color", "st", "xterm", "alacritty", "kitty", "foot", "wezterm", "urxvt", "ghostty", NULL
	};
	if (!cls || !*cls) return false;
	for (size_t i = 0; TERMS[i]; i++) {
		if (dwm_ctl_str_ci(cls, TERMS[i])) return true;
	}
	return false;
}

static __attribute__((unused)) pid_t dwm_ctl_spawn_sh(const char *cmd)
{
	if (!cmd || !*cmd) return -1;
	pid_t pid = fork();
	if (pid == 0) {
		setsid();
		int devnull = open("/dev/null", O_RDWR);
		if (devnull >= 0) {
			dup2(devnull, STDIN_FILENO);
			dup2(devnull, STDOUT_FILENO);
			dup2(devnull, STDERR_FILENO);
			if (devnull > 2) close(devnull);
		}
		execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
		_exit(127);
	}
	return pid;
}

static __attribute__((unused)) pid_t dwm_ctl_spawn_argv(char *const argv[])
{
	if (!argv || !argv[0]) return -1;
	int err_pipe[2] = { -1, -1 };
	if (pipe(err_pipe) == 0) {
		fcntl(err_pipe[0], F_SETFD, FD_CLOEXEC);
		fcntl(err_pipe[1], F_SETFD, FD_CLOEXEC);
	}
	pid_t pid = fork();
	if (pid == 0) {
		if (err_pipe[0] >= 0) close(err_pipe[0]);
		setsid();
		int devnull = open("/dev/null", O_RDWR);
		if (devnull >= 0) {
			dup2(devnull, STDIN_FILENO);
			dup2(devnull, STDOUT_FILENO);
			dup2(devnull, STDERR_FILENO);
			if (devnull > 2) close(devnull);
		}
		execvp(argv[0], argv);
		int e = errno;
		if (err_pipe[1] >= 0) {
			ssize_t ign = write(err_pipe[1], &e, sizeof(e));
			(void)ign;
			close(err_pipe[1]);
		}
		_exit(127);
	}
	if (err_pipe[1] >= 0) close(err_pipe[1]);
	if (err_pipe[0] >= 0) {
		int child_err = 0;
		ssize_t n = read(err_pipe[0], &child_err, sizeof(child_err));
		close(err_pipe[0]);
		if (n > 0) {
			errno = child_err;
			return -1;
		}
	}
	return pid;
}

static __attribute__((unused)) void dwm_ctl_run_capture(const char *cmd, char *out, size_t out_sz)
{
	if (!out || out_sz == 0) return;
	out[0] = '\0';
	if (!cmd || !*cmd) return;
	FILE *fp = popen(cmd, "r");
	if (!fp) return;
	size_t rd = fread(out, 1, out_sz - 1, fp);
	out[rd] = '\0';
	dwm_ctl_trim(out);
	pclose(fp);
}

static __attribute__((unused)) int dwm_ctl_send2(Display *dpy, Window root, long cmd, long arg1, long arg2)
{
	XEvent ev;
	Atom cmd_atom = XInternAtom(dpy, "_DWM_CMD", False);
	memset(&ev, 0, sizeof(ev));
	ev.xclient.type = ClientMessage;
	ev.xclient.window = root;
	ev.xclient.message_type = cmd_atom;
	ev.xclient.format = 32;
	ev.xclient.data.l[0] = cmd;
	ev.xclient.data.l[1] = arg1;
	ev.xclient.data.l[2] = arg2;
	XSendEvent(dpy, root, False, SubstructureRedirectMask | SubstructureNotifyMask, &ev);
	XFlush(dpy);
	return 0;
}

static __attribute__((unused)) int dwm_ctl_send(Display *dpy, Window root, long cmd, long arg1)
{
	return dwm_ctl_send2(dpy, root, cmd, arg1, 0);
}

static __attribute__((unused)) unsigned long dwm_ctl_get_cardinal_prop(
	Display *dpy, Window w, const char *atom_name, unsigned long def_val)
{
	Atom prop_atom = XInternAtom(dpy, atom_name, False);
	Atom actual_type;
	int actual_format;
	unsigned long nitems = 0, bytes_after;
	unsigned char *prop = NULL;
	unsigned long val = def_val;

	if (XGetWindowProperty(dpy, w, prop_atom, 0, 1, False, XA_CARDINAL,
	                       &actual_type, &actual_format, &nitems, &bytes_after, &prop) == Success && prop) {
		if (nitems > 0) val = *(unsigned long *)prop;
		XFree(prop);
	}
	return val;
}

static int dwm_ctl_mask_to_tag(unsigned int mask)
{
	for (int t = 1; t <= 9; t++) {
		if (mask & (1U << (t - 1))) return t;
	}
	return 1;
}

static void dwm_ctl_get_title(Display *dpy, Window w, char *buf, size_t len)
{
	if (!buf || len == 0) return;
	buf[0] = '\0';
	Atom net_wm_name = XInternAtom(dpy, "_NET_WM_NAME", False);
	Atom utf8 = XInternAtom(dpy, "UTF8_STRING", False);
	Atom actual_type;
	int actual_format;
	unsigned long nitems, bytes_after;
	unsigned char *prop = NULL;

	if (XGetWindowProperty(dpy, w, net_wm_name, 0, 256, False, utf8,
	                       &actual_type, &actual_format, &nitems, &bytes_after, &prop) == Success && prop) {
		snprintf(buf, len, "%s", (char *)prop);
		XFree(prop);
		return;
	}
	if (XGetWindowProperty(dpy, w, XA_WM_NAME, 0, 256, False, AnyPropertyType,
	                       &actual_type, &actual_format, &nitems, &bytes_after, &prop) == Success && prop) {
		snprintf(buf, len, "%s", (char *)prop);
		XFree(prop);
	}
}

static void dwm_ctl_fill_win(Display *dpy, Window win, unsigned int cur_tag_mask, bool is_foc, dwm_win_info_t *dst)
{
	memset(dst, 0, sizeof(*dst));
	snprintf(dst->id, sizeof(dst->id), "0x%lx", (unsigned long)win);
	dst->focused = is_foc;

	XClassHint ch = {NULL, NULL};
	if (XGetClassHint(dpy, win, &ch)) {
		if (ch.res_class) {
			snprintf(dst->wm_class, sizeof(dst->wm_class), "%s", ch.res_class);
			XFree(ch.res_class);
		}
		if (ch.res_name) {
			if (!dst->wm_class[0]) snprintf(dst->wm_class, sizeof(dst->wm_class), "%s", ch.res_name);
			XFree(ch.res_name);
		}
	}
	dwm_ctl_get_title(dpy, win, dst->title, sizeof(dst->title));

	unsigned long tags = dwm_ctl_get_cardinal_prop(dpy, win, "_DWM_CLIENT_TAGS", 0);
	if (tags == 0) {
		tags = dwm_ctl_get_cardinal_prop(dpy, win, "_NET_WM_DESKTOP", 0);
		if (tags < 9) tags = (1UL << tags);
	}
	dst->tag = dwm_ctl_mask_to_tag((unsigned int)tags);
	dst->visible = (tags == 0 || (tags & cur_tag_mask) != 0);
	dst->pid = dwm_ctl_get_cardinal_prop(dpy, win, "_NET_WM_PID", 0);
}

static __attribute__((unused)) size_t dwm_ctl_query_windows(
	dwm_win_info_t *out_clients, size_t max_clients,
	dwm_win_info_t *out_foc, dwm_win_info_t *out_oth)
{
	if (out_foc) memset(out_foc, 0, sizeof(*out_foc));
	if (out_oth) memset(out_oth, 0, sizeof(*out_oth));

	Display *dpy = XOpenDisplay(NULL);
	if (!dpy) return 0;

	Window root = DefaultRootWindow(dpy);
	Atom net_active = XInternAtom(dpy, "_NET_ACTIVE_WINDOW", False);
	Atom net_clients = XInternAtom(dpy, "_NET_CLIENT_LIST_STACKING", False);
	Atom actual_type;
	int actual_format;
	unsigned long nitems = 0, bytes_after;
	unsigned char *prop = NULL;
	Window active_win = None;

	unsigned int cur_tag_mask = (unsigned int)dwm_ctl_get_cardinal_prop(dpy, root, "_DWM_CUR_TAG", 1);

	if (XGetWindowProperty(dpy, root, net_active, 0, 1, False, XA_WINDOW,
	                       &actual_type, &actual_format, &nitems, &bytes_after, &prop) == Success && prop) {
		if (nitems > 0) active_win = *(Window *)prop;
		XFree(prop);
	}

	prop = NULL;
	nitems = 0;
	if (XGetWindowProperty(dpy, root, net_clients, 0, 256, False, XA_WINDOW,
	                       &actual_type, &actual_format, &nitems, &bytes_after, &prop) != Success || !prop) {
		net_clients = XInternAtom(dpy, "_NET_CLIENT_LIST", False);
		prop = NULL;
		nitems = 0;
		XGetWindowProperty(dpy, root, net_clients, 0, 256, False, XA_WINDOW,
		                   &actual_type, &actual_format, &nitems, &bytes_after, &prop);
	}

	size_t count = 0;
	bool have_foc = false, have_oth = false;
	if (prop) {
		Window *wins = (Window *)prop;
		for (long idx = (long)nitems - 1; idx >= 0; idx--) {
			Window win = wins[idx];
			dwm_win_info_t info;
			dwm_ctl_fill_win(dpy, win, cur_tag_mask, (win == active_win), &info);
			if (out_clients && count < max_clients) {
				out_clients[count++] = info;
			}
			if (win == active_win && !have_foc) {
				if (out_foc) *out_foc = info;
				have_foc = true;
			} else if (info.visible && win != active_win && !have_oth) {
				if (out_oth) *out_oth = info;
				have_oth = true;
			}
		}
		XFree(prop);
	}

	if (!have_foc && active_win != None) {
		dwm_win_info_t info;
		dwm_ctl_fill_win(dpy, active_win, cur_tag_mask, true, &info);
		if (out_foc) *out_foc = info;
		if (out_clients && count == 0 && max_clients > 0) {
			out_clients[count++] = info;
		}
	}

	XCloseDisplay(dpy);
	return count;
}

static __attribute__((unused)) bool dwm_ctl_get_focused(dwm_win_info_t *out)
{
	if (!out) return false;
	memset(out, 0, sizeof(*out));
	Display *dpy = XOpenDisplay(NULL);
	if (!dpy) return false;
	Window root = DefaultRootWindow(dpy);
	Atom net_active = XInternAtom(dpy, "_NET_ACTIVE_WINDOW", False);
	Atom actual_type;
	int actual_format;
	unsigned long nitems = 0, bytes_after;
	unsigned char *prop = NULL;
	Window active_win = None;

	if (XGetWindowProperty(dpy, root, net_active, 0, 1, False, XA_WINDOW,
	                       &actual_type, &actual_format, &nitems, &bytes_after, &prop) == Success && prop) {
		if (nitems > 0) active_win = *(Window *)prop;
		XFree(prop);
	}
	bool ok = false;
	if (active_win != None) {
		unsigned int cur_tag_mask = (unsigned int)dwm_ctl_get_cardinal_prop(dpy, root, "_DWM_CUR_TAG", 1);
		dwm_ctl_fill_win(dpy, active_win, cur_tag_mask, true, out);
		ok = true;
	}
	XCloseDisplay(dpy);
	return ok;
}

static __attribute__((unused)) bool dwm_ctl_wait_focused(const char *id_or_cls, double timeout_sec)
{
	double deadline = dwm_ctl_now_sec() + (timeout_sec > 0 ? timeout_sec : 1.0);
	while (dwm_ctl_now_sec() < deadline) {
		dwm_win_info_t foc;
		if (dwm_ctl_get_focused(&foc)) {
			if (!id_or_cls || !*id_or_cls) return true;
			if (strcasecmp(foc.id, id_or_cls) == 0 ||
			    dwm_ctl_str_ci(foc.wm_class, id_or_cls) ||
			    dwm_ctl_str_ci(foc.title, id_or_cls)) {
				return true;
			}
		}
		usleep(25000);
	}
	return false;
}

static __attribute__((unused)) bool dwm_ctl_get_geometry(const char *wid_str, int *out_x, int *out_y, int *out_w, int *out_h)
{
	*out_x = 0; *out_y = 0; *out_w = 1920; *out_h = 1080;
	Display *dpy = XOpenDisplay(NULL);
	if (!dpy) return false;
	Window root = DefaultRootWindow(dpy);
	Window target = None;
	if (wid_str && *wid_str) {
		target = (Window)strtoul(wid_str, NULL, 0);
	} else {
		Atom net_active = XInternAtom(dpy, "_NET_ACTIVE_WINDOW", False);
		Atom actual_type;
		int actual_format;
		unsigned long nitems = 0, bytes_after;
		unsigned char *prop = NULL;
		if (XGetWindowProperty(dpy, root, net_active, 0, 1, False, XA_WINDOW,
		                       &actual_type, &actual_format, &nitems, &bytes_after, &prop) == Success && prop) {
			if (nitems > 0) target = *(Window *)prop;
			XFree(prop);
		}
	}
	bool ok = false;
	if (target != None) {
		XWindowAttributes attr;
		if (XGetWindowAttributes(dpy, target, &attr)) {
			Window child;
			int rx = 0, ry = 0;
			XTranslateCoordinates(dpy, target, root, 0, 0, &rx, &ry, &child);
			*out_x = rx;
			*out_y = ry;
			*out_w = attr.width;
			*out_h = attr.height;
			ok = true;
		}
	}
	XCloseDisplay(dpy);
	return ok;
}

static __attribute__((unused)) bool dwm_ctl_focus_win(const char *wid_str, double timeout_sec)
{
	if (!wid_str || !*wid_str) return false;
	Window w = (Window)strtoul(wid_str, NULL, 0);
	if (!w) return false;
	Display *dpy = XOpenDisplay(NULL);
	if (!dpy) return false;
	dwm_ctl_send(dpy, DefaultRootWindow(dpy), DWM_CMD_FOCUS_WIN, (long)w);
	XCloseDisplay(dpy);
	bool ok = dwm_ctl_wait_focused(wid_str, timeout_sec);
	if (ok) {
		double deadline = dwm_ctl_now_sec() + 0.20;
		int wx = -1, wy = 0, ww = 0, wh = 0;
		while (dwm_ctl_now_sec() < deadline) {
			dwm_ctl_get_geometry(wid_str, &wx, &wy, &ww, &wh);
			if (wx >= 0) break;
			usleep(15000);
		}
		if (ww > 0 && wh > 0) {
			Display *wdpy = XOpenDisplay(NULL);
			if (wdpy) {
				XWarpPointer(wdpy, None, w, 0, 0, 0, 0, ww / 2, wh / 2);
				XFlush(wdpy);
				XCloseDisplay(wdpy);
			}
		}
		usleep(35000);
	}
	return ok;
}

static __attribute__((unused)) bool dwm_ctl_get_st_screen_text(
	const char *wid_str, int max_lines, char *out, size_t out_sz)
{
	if (!out || out_sz == 0) return false;
	out[0] = '\0';
	if (!wid_str || !*wid_str) return false;
	Window w = (Window)strtoul(wid_str, NULL, 0);
	if (!w) return false;

	Display *dpy = XOpenDisplay(NULL);
	if (!dpy) return false;

	Atom prop_atom = XInternAtom(dpy, "_ST_SCREEN_TEXT", False);
	Atom utf8 = XInternAtom(dpy, "UTF8_STRING", False);
	Atom actual_type = None;
	int actual_format = 0;
	unsigned long nitems = 0, bytes_after = 0;
	unsigned char *prop = NULL;
	bool ok = false;

	if (XGetWindowProperty(dpy, w, prop_atom, 0, 16384, False, utf8,
	                       &actual_type, &actual_format, &nitems, &bytes_after, &prop) == Success && prop) {
		if (nitems > 0) {
			const char *text = (const char *)prop;
			const char *start = text;
			if (max_lines > 0) {
				int nl = 0;
				for (const char *p = text + nitems - 1; p >= text; p--) {
					if (*p == '\n') {
						nl++;
						if (nl > max_lines) {
							start = p + 1;
							break;
						}
					}
				}
			}
			snprintf(out, out_sz, "%s", start);
			dwm_ctl_trim(out);
			ok = (out[0] != '\0');
		}
		XFree(prop);
	}
	XCloseDisplay(dpy);
	return ok;
}

static size_t dwm_ctl_get_child_pids(unsigned long pid, unsigned long *out_pids, size_t max_pids)
{
	if (!pid || !out_pids || max_pids == 0) return 0;
	char path[128];
	snprintf(path, sizeof(path), "/proc/%lu/task/%lu/children", pid, pid);
	FILE *fp = fopen(path, "r");
	if (!fp) return 0;
	char buf[1024];
	size_t count = 0;
	if (fgets(buf, sizeof(buf), fp)) {
		char *saveptr = NULL;
		for (char *tok = strtok_r(buf, " \t\n", &saveptr); tok && count < max_pids; tok = strtok_r(NULL, " \t\n", &saveptr)) {
			unsigned long cp = strtoul(tok, NULL, 10);
			if (cp > 0) out_pids[count++] = cp;
		}
	}
	fclose(fp);
	return count;
}

static void dwm_ctl_read_proc_comm(unsigned long pid, char *out, size_t out_sz)
{
	out[0] = '\0';
	if (!pid || out_sz == 0) return;
	char path[128];
	snprintf(path, sizeof(path), "/proc/%lu/comm", pid);
	FILE *fp = fopen(path, "r");
	if (!fp) return;
	if (fgets(out, (int)out_sz, fp)) dwm_ctl_trim(out);
	fclose(fp);
}

static bool dwm_ctl_is_shell_name(const char *comm)
{
	if (!comm || !*comm) return false;
	const char *c = (comm[0] == '-') ? (comm + 1) : comm;
	return strcmp(c, "bash") == 0 || strcmp(c, "zsh") == 0 ||
	       strcmp(c, "sh") == 0 || strcmp(c, "fish") == 0 || strcmp(c, "dash") == 0;
}

static __attribute__((unused)) bool dwm_ctl_is_idle_shell_win(const dwm_win_info_t *win)
{
	if (!win || !dwm_ctl_is_terminal_class(win->wm_class) || !win->pid) return false;
	unsigned long sh_pids[16];
	size_t n_sh = dwm_ctl_get_child_pids(win->pid, sh_pids, 16);
	if (n_sh == 0) return false;
	unsigned long sh_pid = sh_pids[n_sh - 1];
	unsigned long sub_pids[16];
	size_t n_sub = dwm_ctl_get_child_pids(sh_pid, sub_pids, 16);
	if (n_sub == 0) return true;

	char tmux_out[1024];
	dwm_ctl_run_capture(
		"tmux list-clients -F '#{client_pid}\t#{pane_current_command}\t#{pane_pid}' 2>/dev/null",
		tmux_out, sizeof(tmux_out));
	if (tmux_out[0]) {
		char *save_line = NULL;
		for (char *line = strtok_r(tmux_out, "\n", &save_line); line; line = strtok_r(NULL, "\n", &save_line)) {
			char *t1 = strchr(line, '\t');
			char *t2 = t1 ? strchr(t1 + 1, '\t') : NULL;
			if (!t1 || !t2) continue;
			*t1 = '\0';
			*t2 = '\0';
			unsigned long cpid = strtoul(line, NULL, 10);
			const char *pane_cmd = t1 + 1;
			unsigned long pane_pid = strtoul(t2 + 1, NULL, 10);
			for (size_t i = 0; i < n_sub; i++) {
				if (sub_pids[i] == cpid) {
					if (dwm_ctl_is_shell_name(pane_cmd)) {
						unsigned long pane_kids[8];
						if (dwm_ctl_get_child_pids(pane_pid, pane_kids, 8) == 0) return true;
					}
					return false;
				}
			}
		}
	}
	return false;
}

static __attribute__((unused)) bool dwm_ctl_is_interactive_remote(const dwm_win_info_t *win)
{
	if (!win || !dwm_ctl_is_terminal_class(win->wm_class) || !win->pid) return false;
	unsigned long sh_pids[8];
	size_t n_sh = dwm_ctl_get_child_pids(win->pid, sh_pids, 8);
	if (n_sh == 0) return false;
	unsigned long sh_pid = sh_pids[n_sh - 1];
	unsigned long sub_pids[8];
	size_t n_sub = dwm_ctl_get_child_pids(sh_pid, sub_pids, 8);
	for (size_t i = 0; i < n_sub; i++) {
		char comm[64];
		dwm_ctl_read_proc_comm(sub_pids[i], comm, sizeof(comm));
		if (strcmp(comm, "ssh") == 0 || strcmp(comm, "mosh-client") == 0) {
			return true;
		}
	}
	return false;
}

static __attribute__((unused)) bool dwm_ctl_open_terminal(
	const char *cmd, char *out_msg, size_t msg_sz, char *out_err, size_t err_sz)
{
	const char *term_cmd = (cmd && *cmd) ? cmd : "st";
	char resolved_term[512] = {0};
	if (strchr(term_cmd, '/') == NULL && strchr(term_cmd, ' ') == NULL) {
		const char *home = getenv("HOME");
		if (home && *home) {
			char local_cand[512];
			snprintf(local_cand, sizeof(local_cand), "%s/.local/bin/%s", home, term_cmd);
			if (access(local_cand, X_OK) == 0) {
				snprintf(resolved_term, sizeof(resolved_term), "%s", local_cand);
			}
		}
	}
	const char *exec_cmd = resolved_term[0] ? resolved_term : term_cmd;
	dwm_win_info_t prev_foc;
	dwm_ctl_get_focused(&prev_foc);
	const char *term_cls = (strncmp(term_cmd, "xterm", 5) == 0) ? "xterm" : "st-256color";

	pid_t pid = 0;
	if (strchr(exec_cmd, ' ') == NULL) {
		char *const argv[] = { (char *)exec_cmd, NULL };
		pid = dwm_ctl_spawn_argv(argv);
	} else {
		pid = dwm_ctl_spawn_sh(exec_cmd);
	}

	if (pid <= 0) {
		if (out_err && err_sz) {
			snprintf(out_err, err_sz, "Failed to launch terminal '%s': %s", term_cmd, strerror(errno));
		}
		return false;
	}

	bool focused_new = false;
	double deadline = dwm_ctl_now_sec() + 2.0;
	usleep(40000);
	while (dwm_ctl_now_sec() < deadline) {
		dwm_win_info_t foc;
		if (dwm_ctl_get_focused(&foc) && dwm_ctl_str_ci(foc.wm_class, term_cls)) {
			if (!prev_foc.id[0] || strcasecmp(foc.id, prev_foc.id) != 0) {
				focused_new = true;
				break;
			}
		}
		usleep(35000);
	}

	if (!focused_new) {
		if (out_err && err_sz) {
			snprintf(out_err, err_sz, "Terminal window did not focus: %s", term_cmd);
		}
		return false;
	}

	if (pid > 0) {
		double t_end = dwm_ctl_now_sec() + 0.70;
		int idle_hits = 0;
		usleep(60000);
		while (dwm_ctl_now_sec() < t_end) {
			unsigned long sh_pids[8];
			size_t n_sh = dwm_ctl_get_child_pids((unsigned long)pid, sh_pids, 8);
			if (n_sh > 0) {
				unsigned long sh_pid = sh_pids[n_sh - 1];
				char comm[64];
				dwm_ctl_read_proc_comm(sh_pid, comm, sizeof(comm));
				if (!dwm_ctl_is_shell_name(comm)) {
					unsigned long inner_sh[8];
					size_t n_in = dwm_ctl_get_child_pids(sh_pid, inner_sh, 8);
					if (n_in > 0) sh_pid = inner_sh[n_in - 1];
					else { usleep(35000); continue; }
				}
				unsigned long sub_pids[8];
				if (dwm_ctl_get_child_pids(sh_pid, sub_pids, 8) == 0) {
					if (++idle_hits >= 2) break;
				} else {
					idle_hits = 0;
				}
			}
			usleep(35000);
		}
	}
	usleep(40000);
	g_dwm_last_term_open_ts = dwm_ctl_now_sec();

	if (out_msg && msg_sz) {
		snprintf(out_msg, msg_sz, "opened terminal: %s", term_cmd);
	}
	return true;
}

static __attribute__((unused)) const char *dwm_ctl_shortcut_exec(const char *ident)
{
	if (!ident || !*ident) return NULL;
	if (strcasecmp(ident, "sys_monitor") == 0)    return "dwm-ctl sys-monitor";
	if (strcasecmp(ident, "calendar") == 0)       return "dwm-cal popup";
	if (strcasecmp(ident, "control_center") == 0) return "dwm-ctl";
	if (strcasecmp(ident, "wifi") == 0)           return "dwm-net wifi";
	if (strcasecmp(ident, "battery") == 0)        return "dwm-ctl battery-info";
	if (strcasecmp(ident, "power_menu") == 0)     return "dwm-ctl power-menu";
	if (strcasecmp(ident, "shortcuts_help") == 0) return "dwm-help show";
	return NULL;
}

#endif /* DWM_CTL_H */
