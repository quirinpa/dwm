/**
 * @file mod_dwm.c
 * @brief Pure-C libxylem (XY) adapter plugin for dwm & libjarvis.
 *
 * Implements the universal jarvis_xy_* hooks using direct Xlib (_DWM_CMD + EWMH),
 * /proc process & CWD inspection, statvfs/meminfo system health, MozLZ4 Firefox
 * tab inspection, AF_UNIX dwm-tts socket streaming, and dmenu/dunst integration.
 *
 * Can be loaded via xy_load("mod_dwm") into any Xylem host (bin/jarvis, dwm,
 * dwm-whisperd, or axil).
 */

#include <ttypt/jarvis-types.h>
#include "../../dwm-types.h"
#include <ttypt/xy.h>
#include <ttypt/xy-mod.h>

#include <ctype.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

/* ---------------------------------------------------------------------------
 * Cached State & Constants
 * ------------------------------------------------------------------------- */

typedef struct {
	char phrase[128];
	char exec_cmd[256];
} desktop_app_entry_t;

#define MAX_CACHED_DESKTOP_APPS 256

static desktop_app_entry_t g_desktop_apps[MAX_CACHED_DESKTOP_APPS];
static size_t              g_desktop_app_count = 0;
static bool                g_desktop_apps_loaded = false;

static jarvis_tab_t        g_tabs_cache[JARVIS_MAX_TABS];
static size_t              g_tabs_cache_count = 0;
static time_t              g_tabs_mtime = 0;

static const char *const TERMINAL_CLASSES[] = {
	"st-256color", "st", "xterm", "alacritty", "kitty", "foot", "wezterm", "urxvt", "ghostty", NULL
};

XY_MODULE_API void xy_install(void)
{
}

/* Also listen to dwm's own startup hook when loaded directly inside dwm */
XY_IMPL(int, dwm_xy_on_startup, int, screen_num)
{
	(void)screen_num;
	return 0;
}

/* ---------------------------------------------------------------------------
 * Generic String, Process & Subprocess Helpers
 * ------------------------------------------------------------------------- */

static double now_sec(void)
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return (double)tv.tv_sec + (double)tv.tv_usec / 1e6;
}

static bool str_contains_ci(const char *haystack, const char *needle)
{
	if (!haystack || !needle) return false;
	size_t nlen = strlen(needle);
	if (nlen == 0) return true;
	for (; *haystack; haystack++) {
		if (strncasecmp(haystack, needle, nlen) == 0) return true;
	}
	return false;
}

static void trim_trailing(char *s)
{
	if (!s) return;
	size_t l = strlen(s);
	while (l > 0 && isspace((unsigned char)s[l - 1])) {
		s[--l] = '\0';
	}
}

static void run_cmd_capture(const char *cmd, char *out, size_t out_sz)
{
	if (!out || out_sz == 0) return;
	out[0] = '\0';
	FILE *fp = popen(cmd, "r");
	if (!fp) return;
	size_t rd = fread(out, 1, out_sz - 1, fp);
	out[rd] = '\0';
	trim_trailing(out);
	pclose(fp);
}

static pid_t spawn_detached_sh(const char *cmd)
{
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

static pid_t spawn_detached_argv(char *const argv[])
{
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
		execvp(argv[0], argv);
		_exit(127);
	}
	return pid;
}

static bool is_terminal_class(const char *wm_class)
{
	if (!wm_class || !*wm_class) return false;
	for (size_t i = 0; TERMINAL_CLASSES[i]; i++) {
		if (str_contains_ci(wm_class, TERMINAL_CLASSES[i])) return true;
	}
	return false;
}

/* ---------------------------------------------------------------------------
 * Direct X11 / _DWM_CMD / EWMH Queries & Control (<0.1ms, Zero Subprocesses)
 * ------------------------------------------------------------------------- */

static int send_dwm_cmd2(Display *dpy, Window root, long cmd, long arg1, long arg2)
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

static int send_dwm_cmd(Display *dpy, Window root, long cmd, long arg1)
{
	return send_dwm_cmd2(dpy, root, cmd, arg1, 0);
}

static unsigned long get_cardinal_prop(Display *dpy, Window w, const char *atom_name, unsigned long def_val)
{
	Atom prop_atom = XInternAtom(dpy, atom_name, False);
	Atom actual_type;
	int actual_format;
	unsigned long nitems = 0, bytes_after;
	unsigned char *prop = NULL;
	unsigned long val = def_val;

	if (XGetWindowProperty(dpy, w, prop_atom, 0, 1, False, XA_CARDINAL,
	                       &actual_type, &actual_format, &nitems, &bytes_after, &prop) == Success && prop) {
		if (nitems > 0) {
			val = *(unsigned long *)prop;
		}
		XFree(prop);
	}
	return val;
}

static int mask_to_tag_num(unsigned int mask)
{
	for (int t = 1; t <= 9; t++) {
		if (mask & (1U << (t - 1))) return t;
	}
	return 1;
}

static void get_win_title(Display *dpy, Window w, char *buf, size_t len)
{
	Atom net_wm_name = XInternAtom(dpy, "_NET_WM_NAME", False);
	Atom utf8 = XInternAtom(dpy, "UTF8_STRING", False);
	Atom actual_type;
	int actual_format;
	unsigned long nitems, bytes_after;
	unsigned char *prop = NULL;

	if (!buf || len == 0) return;
	buf[0] = '\0';
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

static void get_win_class(Display *dpy, Window w, char *buf, size_t len)
{
	XClassHint ch = { NULL, NULL };
	if (!buf || len == 0) return;
	buf[0] = '\0';
	if (XGetClassHint(dpy, w, &ch)) {
		snprintf(buf, len, "%s", ch.res_class ? ch.res_class : (ch.res_name ? ch.res_name : "-"));
		if (ch.res_class) XFree(ch.res_class);
		if (ch.res_name) XFree(ch.res_name);
	} else {
		snprintf(buf, len, "-");
	}
}

static void query_x11_windows(jarvis_world_t *w)
{
	Display *dpy = XOpenDisplay(NULL);
	if (!dpy) return;
	Window root = DefaultRootWindow(dpy);

	Atom net_clients = XInternAtom(dpy, "_NET_CLIENT_LIST", False);
	Atom net_active  = XInternAtom(dpy, "_NET_ACTIVE_WINDOW", False);
	Atom actual_type;
	int actual_format;
	unsigned long nitems = 0, bytes_after;
	unsigned char *prop = NULL;
	Window active_win = 0;
	unsigned int cur_mask = (unsigned int)get_cardinal_prop(dpy, root, "_DWM_CUR_TAG", 1UL);

	if (XGetWindowProperty(dpy, root, net_active, 0, 1, False, XA_WINDOW,
	                       &actual_type, &actual_format, &nitems, &bytes_after, &prop) == Success && prop) {
		if (nitems > 0) active_win = *(Window *)prop;
		XFree(prop);
		prop = NULL;
	}

	if (active_win) {
		snprintf(w->focused.id, sizeof(w->focused.id), "0x%08lx", (unsigned long)active_win);
		get_win_class(dpy, active_win, w->focused.wm_class, sizeof(w->focused.wm_class));
		get_win_title(dpy, active_win, w->focused.title, sizeof(w->focused.title));
		unsigned int c_mask = (unsigned int)get_cardinal_prop(dpy, active_win, "_DWM_CLIENT_TAGS", cur_mask);
		w->focused.tag = mask_to_tag_num(c_mask);
		w->focused.visible = (c_mask & cur_mask) != 0;
		w->focused.focused = true;
	}

	nitems = 0;
	if (XGetWindowProperty(dpy, root, net_clients, 0, 1024, False, XA_WINDOW,
	                       &actual_type, &actual_format, &nitems, &bytes_after, &prop) == Success && prop) {
		Window *wins = (Window *)prop;
		jarvis_window_t fallback_other;
		memset(&fallback_other, 0, sizeof(fallback_other));
		bool has_other_on_cur = false;

		for (unsigned long i = 0; i < nitems && w->client_count < JARVIS_MAX_CLIENTS; i++) {
			jarvis_window_t *cw = &w->clients[w->client_count++];
			memset(cw, 0, sizeof(*cw));
			snprintf(cw->id, sizeof(cw->id), "0x%08lx", (unsigned long)wins[i]);
			get_win_class(dpy, wins[i], cw->wm_class, sizeof(cw->wm_class));
			get_win_title(dpy, wins[i], cw->title, sizeof(cw->title));
			unsigned int c_mask = (unsigned int)get_cardinal_prop(dpy, wins[i], "_DWM_CLIENT_TAGS", cur_mask);
			cw->tag = mask_to_tag_num(c_mask);
			cw->visible = (c_mask & cur_mask) != 0;
			cw->focused = (wins[i] == active_win);

			if (wins[i] != active_win) {
				/* Prefer non-terminal windows on current workspace when currently focused on a terminal */
				bool cur_is_term = is_terminal_class(w->focused.wm_class);
				bool cand_is_term = is_terminal_class(cw->wm_class);
				if (cw->visible) {
					if (!has_other_on_cur || (cur_is_term && !cand_is_term && is_terminal_class(w->other.wm_class))) {
						w->other = *cw;
						has_other_on_cur = true;
					}
				} else if (!fallback_other.id[0]) {
					fallback_other = *cw;
				}
			}
		}
		if (!has_other_on_cur && fallback_other.id[0]) {
			w->other = fallback_other;
		}
		XFree(prop);
	}

	XCloseDisplay(dpy);
}

static bool get_focused_window_fast(jarvis_window_t *out_foc)
{
	memset(out_foc, 0, sizeof(*out_foc));
	Display *dpy = XOpenDisplay(NULL);
	if (!dpy) return false;
	Window root = DefaultRootWindow(dpy);
	Atom net_active = XInternAtom(dpy, "_NET_ACTIVE_WINDOW", False);
	Atom actual_type;
	int actual_format;
	unsigned long nitems = 0, bytes_after;
	unsigned char *prop = NULL;
	Window active_win = 0;

	if (XGetWindowProperty(dpy, root, net_active, 0, 1, False, XA_WINDOW,
	                       &actual_type, &actual_format, &nitems, &bytes_after, &prop) == Success && prop) {
		if (nitems > 0) active_win = *(Window *)prop;
		XFree(prop);
	}
	if (active_win) {
		snprintf(out_foc->id, sizeof(out_foc->id), "0x%08lx", (unsigned long)active_win);
		get_win_class(dpy, active_win, out_foc->wm_class, sizeof(out_foc->wm_class));
		get_win_title(dpy, active_win, out_foc->title, sizeof(out_foc->title));
		out_foc->focused = true;
	}
	XCloseDisplay(dpy);
	return active_win != 0;
}

static bool wait_window_focused(const char *target_id_or_class, double timeout_sec)
{
	if (!target_id_or_class || !*target_id_or_class) return false;
	double deadline = now_sec() + timeout_sec;
	while (now_sec() < deadline) {
		jarvis_window_t foc;
		if (get_focused_window_fast(&foc)) {
			if (strcasecmp(foc.id, target_id_or_class) == 0 ||
			    str_contains_ci(foc.wm_class, target_id_or_class) ||
			    str_contains_ci(foc.title, target_id_or_class)) {
				return true;
			}
		}
		usleep(35000);
	}
	return false;
}

static void get_window_geometry(const char *wid_str, int *out_x, int *out_y, int *out_w, int *out_h)
{
	*out_x = 0; *out_y = 0; *out_w = 1920; *out_h = 1080;
	Display *dpy = XOpenDisplay(NULL);
	if (!dpy) return;
	Window root = DefaultRootWindow(dpy);
	Window w = 0;
	if (wid_str && *wid_str) {
		w = (Window)strtoul(wid_str, NULL, 0);
	} else {
		jarvis_window_t foc;
		if (get_focused_window_fast(&foc) && foc.id[0]) {
			w = (Window)strtoul(foc.id, NULL, 0);
		}
	}
	if (w) {
		Window child = 0;
		int rx = 0, ry = 0;
		unsigned int rw = 0, rh = 0, bw = 0, depth = 0;
		Window rret = 0;
		if (XGetGeometry(dpy, w, &rret, &rx, &ry, &rw, &rh, &bw, &depth)) {
			int abs_x = rx, abs_y = ry;
			XTranslateCoordinates(dpy, w, root, 0, 0, &abs_x, &abs_y, &child);
			*out_x = abs_x;
			*out_y = abs_y;
			*out_w = (int)rw;
			*out_h = (int)rh;
		}
	}
	XCloseDisplay(dpy);
}

static void get_mouse_location(int *out_x, int *out_y)
{
	*out_x = 0; *out_y = 0;
	Display *dpy = XOpenDisplay(NULL);
	if (!dpy) return;
	Window root = DefaultRootWindow(dpy);
	Window rret, cret;
	int rx = 0, ry = 0, wx = 0, wy = 0;
	unsigned int mask = 0;
	if (XQueryPointer(dpy, root, &rret, &cret, &rx, &ry, &wx, &wy, &mask)) {
		*out_x = rx;
		*out_y = ry;
	}
	XCloseDisplay(dpy);
}

/* ---------------------------------------------------------------------------
 * Direct /proc Process Tree, CWD, Idle-Shell & SSH Inspection (<0.1ms)
 * ------------------------------------------------------------------------- */

static unsigned long get_window_pid(const char *wid_str)
{
	if (!wid_str || !*wid_str) return 0;
	Window w = (Window)strtoul(wid_str, NULL, 0);
	if (!w) return 0;
	Display *dpy = XOpenDisplay(NULL);
	if (!dpy) return 0;
	unsigned long pid = get_cardinal_prop(dpy, w, "_NET_WM_PID", 0UL);
	XCloseDisplay(dpy);
	return pid;
}

/* Read space-separated child PIDs from /proc/<pid>/task/<pid>/children */
static size_t get_child_pids(unsigned long pid, unsigned long *out_pids, size_t max_pids)
{
	if (!pid || !out_pids || max_pids == 0) return 0;
	char path[128];
	snprintf(path, sizeof(path), "/proc/%lu/task/%lu/children", pid, pid);
	FILE *fp = fopen(path, "r");
	if (!fp) return 0;
	char buf[512];
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

static void read_proc_comm(unsigned long pid, char *out, size_t out_sz)
{
	if (!out || out_sz == 0) return;
	out[0] = '\0';
	char path[128];
	snprintf(path, sizeof(path), "/proc/%lu/comm", pid);
	FILE *fp = fopen(path, "r");
	if (!fp) return;
	if (fgets(out, (int)out_sz, fp)) {
		trim_trailing(out);
	}
	fclose(fp);
}

static void read_proc_cmdline(unsigned long pid, char *out, size_t out_sz)
{
	if (!out || out_sz == 0) return;
	out[0] = '\0';
	char path[128];
	snprintf(path, sizeof(path), "/proc/%lu/cmdline", pid);
	FILE *fp = fopen(path, "rb");
	if (!fp) return;
	size_t rd = fread(out, 1, out_sz - 1, fp);
	fclose(fp);
	for (size_t i = 0; i < rd; i++) {
		if (out[i] == '\0') out[i] = ' ';
	}
	out[rd] = '\0';
	trim_trailing(out);
}

static void resolve_window_cwd_and_fg(const jarvis_window_t *win, char *out_cwd, size_t cwd_sz, char *out_fg, size_t fg_sz)
{
	const char *home = getenv("HOME");
	if (!home || !*home) home = "/tmp";
	if (out_cwd && cwd_sz > 0) snprintf(out_cwd, cwd_sz, "%s", home);
	if (out_fg && fg_sz > 0) out_fg[0] = '\0';
	if (!win || !win->id[0]) return;

	unsigned long wpid = get_window_pid(win->id);
	if (!wpid) return;

	unsigned long sh_pids[16];
	size_t n_sh = get_child_pids(wpid, sh_pids, 16);
	unsigned long sh_pid = n_sh > 0 ? sh_pids[n_sh - 1] : wpid;

	char cwd_link[128], resolved[512];
	snprintf(cwd_link, sizeof(cwd_link), "/proc/%lu/cwd", sh_pid);
	ssize_t rlen = readlink(cwd_link, resolved, sizeof(resolved) - 1);
	if (rlen > 0 && out_cwd && cwd_sz > 0) {
		resolved[rlen] = '\0';
		snprintf(out_cwd, cwd_sz, "%.*s", (int)(cwd_sz - 1), resolved);
	}

	if (out_fg && fg_sz > 0) {
		unsigned long sub_pids[16];
		size_t n_sub = get_child_pids(sh_pid, sub_pids, 16);
		unsigned long target_p = n_sub > 0 ? sub_pids[n_sub - 1] : sh_pid;
		read_proc_cmdline(target_p, out_fg, fg_sz);
		if (!out_fg[0]) {
			read_proc_comm(target_p, out_fg, fg_sz);
		}
	}
}

static bool is_shell_name(const char *comm)
{
	if (!comm || !*comm) return false;
	const char *c = (comm[0] == '-') ? (comm + 1) : comm;
	return strcmp(c, "bash") == 0 || strcmp(c, "zsh") == 0 ||
	       strcmp(c, "sh") == 0 || strcmp(c, "fish") == 0 || strcmp(c, "dash") == 0;
}

static bool is_idle_shell_terminal(const jarvis_window_t *win)
{
	if (!win || !is_terminal_class(win->wm_class) || !win->id[0]) return false;
	unsigned long wpid = get_window_pid(win->id);
	if (!wpid) return false;

	unsigned long sh_pids[16];
	size_t n_sh = get_child_pids(wpid, sh_pids, 16);
	if (n_sh == 0) return false;
	unsigned long sh_pid = sh_pids[n_sh - 1];

	unsigned long sub_pids[16];
	size_t n_sub = get_child_pids(sh_pid, sub_pids, 16);
	if (n_sub == 0) {
		/* Interactive shell has no foreground child process -> idle prompt! */
		return true;
	}

	/* Check if any child is a tmux client attached to an idle shell pane */
	char tmux_out[1024];
	run_cmd_capture("tmux list-clients -F '#{client_pid}\t#{pane_current_command}\t#{pane_pid}' 2>/dev/null",
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
					if (!is_shell_name(pane_cmd)) return false;
					unsigned long pane_children[8];
					return get_child_pids(pane_pid, pane_children, 8) == 0;
				}
			}
		}
	}
	return false;
}

static bool is_interactive_remote_session(const jarvis_window_t *win)
{
	if (!win || !is_terminal_class(win->wm_class) || !win->id[0]) return false;
	unsigned long wpid = get_window_pid(win->id);
	if (!wpid) return false;

	unsigned long sh_pids[16];
	size_t n_sh = get_child_pids(wpid, sh_pids, 16);
	if (n_sh == 0) return false;
	unsigned long sh_pid = sh_pids[n_sh - 1];

	unsigned long sub_pids[16];
	size_t n_sub = get_child_pids(sh_pid, sub_pids, 16);
	for (size_t i = 0; i < n_sub; i++) {
		char comm[64];
		read_proc_comm(sub_pids[i], comm, sizeof(comm));
		if (strcasecmp(comm, "ssh") == 0) return true;
	}
	return false;
}

/* ---------------------------------------------------------------------------
 * Fast System Health, Tmux Panes, MozLZ4 Firefox Tabs & .desktop Scanner
 * ------------------------------------------------------------------------- */

static void populate_fast_system_health(char *out, size_t out_sz)
{
	if (!out || out_sz == 0) return;
	out[0] = '\0';
	int pos = 0;

	struct statvfs st;
	if (statvfs("/", &st) == 0 && st.f_blocks > 0) {
		double total_gb = ((double)st.f_blocks * (double)st.f_frsize) / (1024.0 * 1024.0 * 1024.0);
		double free_gb  = ((double)st.f_bavail * (double)st.f_frsize) / (1024.0 * 1024.0 * 1024.0);
		int used_pct = (int)(100.0 * (1.0 - (free_gb / (total_gb > 0.1 ? total_gb : 0.1))) + 0.5);
		pos += snprintf(out + pos, (size_t)pos < out_sz ? out_sz - (size_t)pos : 0,
		                "Disk /: %.1f GB free / %.0f GB (%d%% used)", free_gb, total_gb, used_pct);
	}

	FILE *mf = fopen("/proc/meminfo", "r");
	if (mf) {
		unsigned long mem_total_kb = 0, mem_avail_kb = 0;
		char line[128];
		while (fgets(line, sizeof(line), mf)) {
			if (strncmp(line, "MemTotal:", 9) == 0) mem_total_kb = strtoul(line + 9, NULL, 10);
			else if (strncmp(line, "MemAvailable:", 13) == 0) mem_avail_kb = strtoul(line + 13, NULL, 10);
			if (mem_total_kb && mem_avail_kb) break;
		}
		fclose(mf);
		if (mem_total_kb > 0) {
			double tot_g = (double)mem_total_kb / (1024.0 * 1024.0);
			double avail_g = (double)mem_avail_kb / (1024.0 * 1024.0);
			double used_g = tot_g - avail_g;
			pos += snprintf(out + pos, (size_t)pos < out_sz ? out_sz - (size_t)pos : 0,
			                "%sRAM: %.1f GB used / %.1f GB (%.1f GB free)",
			                pos > 0 ? " | " : "", used_g, tot_g, avail_g);
		}
	}

	FILE *lf = fopen("/proc/loadavg", "r");
	if (lf) {
		char a[32] = {0}, b[32] = {0}, c[32] = {0};
		if (fscanf(lf, "%31s %31s %31s", a, b, c) == 3) {
			pos += snprintf(out + pos, (size_t)pos < out_sz ? out_sz - (size_t)pos : 0,
			                "%sCPU Load: %s %s %s", pos > 0 ? " | " : "", a, b, c);
		}
		fclose(lf);
	}
}

static bool get_st_screen_text(const char *wid_str, int max_lines, char *out, size_t out_sz)
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
			trim_trailing(out);
			ok = (out[0] != '\0');
		}
		XFree(prop);
	}
	XCloseDisplay(dpy);
	return ok;
}

static void populate_tmux_panes(jarvis_world_t *w, bool active_only, int lines)
{
	char sess_out[2048];
	run_cmd_capture(
		"tmux list-panes -a -F '#{session_name}:#{window_index}.#{pane_index}\t#{pane_current_command}\t#{pane_current_path}\t#{pane_active}\t#{window_active}' 2>/dev/null",
		sess_out, sizeof(sess_out)
	);
	if (sess_out[0]) {
		char *save_line = NULL;
		for (char *line = strtok_r(sess_out, "\n", &save_line);
		     line && w->pane_count < JARVIS_MAX_PANES;
		     line = strtok_r(NULL, "\n", &save_line)) {
			char *parts[5] = {NULL};
			size_t np = 0;
			char *p = line;
			while (p && np < 5) {
				parts[np++] = p;
				char *tab = strchr(p, '\t');
				if (tab) { *tab = '\0'; p = tab + 1; }
				else p = NULL;
			}
			if (np < 3) continue;
			bool is_active = (np >= 4 && strcmp(parts[3], "1") == 0);
			bool win_active = (np >= 5) ? (strcmp(parts[4], "1") == 0) : true;
			if (active_only && !(is_active && win_active)) continue;

			jarvis_pane_t *pane = &w->panes[w->pane_count++];
			memset(pane, 0, sizeof(*pane));
			snprintf(pane->pane, sizeof(pane->pane), "%s", parts[0]);
			snprintf(pane->command, sizeof(pane->command), "%s", parts[1]);
			snprintf(pane->cwd, sizeof(pane->cwd), "%s", parts[2]);
			pane->active = is_active;

			char cmd[256];
			int nlines = (lines < 10) ? 10 : (lines > 120 ? 120 : lines);
			snprintf(cmd, sizeof(cmd), "tmux capture-pane -p -t '%s' -S -%d 2>/dev/null | awk 'NF' | tail -n %d",
			         parts[0], nlines, nlines);
			run_cmd_capture(cmd, pane->tail, sizeof(pane->tail));

			if (active_only && w->pane_count >= 2) break;
		}
	}

	/* Also capture direct X11 _ST_SCREEN_TEXT for visible non-tmux st terminal windows */
	const jarvis_window_t *cands[2] = { &w->focused, &w->other };
	for (int ci = 0; ci < 2 && w->pane_count < JARVIS_MAX_PANES; ci++) {
		const jarvis_window_t *win = cands[ci];
		if (!win->id[0] || !is_terminal_class(win->wm_class)) continue;
		if (ci == 1 && strcmp(win->id, w->focused.id) == 0) continue;

		char st_text[JARVIS_TEXT_LEN];
		if (get_st_screen_text(win->id, lines > 0 ? lines : 30, st_text, sizeof(st_text))) {
			/* Avoid duplicating if tmux pane already captured identical tail */
			if (w->pane_count > 0 && strstr(w->panes[0].tail, st_text)) continue;

			/* Put focused terminal screen first if it's currently active */
			jarvis_pane_t st_pane;
			memset(&st_pane, 0, sizeof(st_pane));
			snprintf(st_pane.pane, sizeof(st_pane.pane), "st:%.58s", win->id);
			resolve_window_cwd_and_fg(win, st_pane.cwd, sizeof(st_pane.cwd),
			                          st_pane.command, sizeof(st_pane.command));
			st_pane.active = (ci == 0);
			snprintf(st_pane.tail, sizeof(st_pane.tail), "%s", st_text);

			if (ci == 0 && w->pane_count > 0) {
				memmove(&w->panes[1], &w->panes[0], w->pane_count * sizeof(jarvis_pane_t));
				w->panes[0] = st_pane;
				w->pane_count++;
			} else {
				w->panes[w->pane_count++] = st_pane;
			}
		}
	}
}

static void scan_desktop_dir(const char *dir_path)
{
	DIR *d = opendir(dir_path);
	if (!d) return;
	struct dirent *de;
	while ((de = readdir(d)) != NULL && g_desktop_app_count < MAX_CACHED_DESKTOP_APPS) {
		size_t nlen = strlen(de->d_name);
		if (nlen < 9 || strcmp(de->d_name + nlen - 8, ".desktop") != 0) continue;

		char fpath[512];
		snprintf(fpath, sizeof(fpath), "%s/%s", dir_path, de->d_name);
		FILE *fp = fopen(fpath, "r");
		if (!fp) continue;

		char name[128] = {0}, generic[128] = {0}, exec_cmd[256] = {0};
		bool nodisplay = false, in_entry = false;
		char line[512];
		while (fgets(line, sizeof(line), fp)) {
			char *s = line;
			while (*s && isspace((unsigned char)*s)) s++;
			trim_trailing(s);
			if (s[0] == '[') {
				in_entry = (strcmp(s, "[Desktop Entry]") == 0);
				continue;
			}
			if (!in_entry) continue;
			if (strcmp(s, "NoDisplay=true") == 0 || strcmp(s, "Hidden=true") == 0) {
				nodisplay = true;
				break;
			}
			if (strncmp(s, "Name=", 5) == 0 && !name[0]) {
				snprintf(name, sizeof(name), "%s", s + 5);
			} else if (strncmp(s, "GenericName=", 12) == 0 && !generic[0]) {
				snprintf(generic, sizeof(generic), "%s", s + 12);
			} else if (strncmp(s, "Exec=", 5) == 0 && !exec_cmd[0]) {
				char raw[256];
				snprintf(raw, sizeof(raw), "%s", s + 5);
				char *saveptr = NULL;
				size_t epos = 0;
				for (char *tok = strtok_r(raw, " \t", &saveptr); tok; tok = strtok_r(NULL, " \t", &saveptr)) {
					if (tok[0] == '%' || strncmp(tok, "env", 3) == 0) continue;
					epos += (size_t)snprintf(exec_cmd + epos,
					                         epos < sizeof(exec_cmd) ? sizeof(exec_cmd) - epos : 0,
					                         "%s%s", epos ? " " : "", tok);
				}
			}
		}
		fclose(fp);

		if (!nodisplay && name[0] && exec_cmd[0]) {
			for (char *p = name; *p; p++) *p = (char)tolower((unsigned char)*p);
			desktop_app_entry_t *ae = &g_desktop_apps[g_desktop_app_count++];
			snprintf(ae->phrase, sizeof(ae->phrase), "%s", name);
			snprintf(ae->exec_cmd, sizeof(ae->exec_cmd), "%s", exec_cmd);

			if (generic[0] && g_desktop_app_count < MAX_CACHED_DESKTOP_APPS) {
				for (char *p = generic; *p; p++) *p = (char)tolower((unsigned char)*p);
				if (strcmp(generic, name) != 0) {
					desktop_app_entry_t *ge = &g_desktop_apps[g_desktop_app_count++];
					snprintf(ge->phrase, sizeof(ge->phrase), "%s", generic);
					snprintf(ge->exec_cmd, sizeof(ge->exec_cmd), "%s", exec_cmd);
				}
			}
		}
	}
	closedir(d);
}

static void ensure_desktop_apps_loaded(void)
{
	if (g_desktop_apps_loaded) return;
	g_desktop_apps_loaded = true;
	const char *home = getenv("HOME");
	if (home && *home) {
		char local_apps[512];
		snprintf(local_apps, sizeof(local_apps), "%s/.local/share/applications", home);
		scan_desktop_dir(local_apps);
	}
	scan_desktop_dir("/usr/local/share/applications");
	scan_desktop_dir("/usr/share/applications");
	scan_desktop_dir("/var/lib/snapd/desktop/applications");
}

static const char *resolve_desktop_app_exec(const char *identifier)
{
	if (!identifier || !*identifier) return identifier;
	ensure_desktop_apps_loaded();

	for (size_t i = 0; i < g_desktop_app_count; i++) {
		if (strcasecmp(g_desktop_apps[i].phrase, identifier) == 0) {
			return g_desktop_apps[i].exec_cmd;
		}
	}
	return identifier;
}

/* Native C MozLZ4 block decompressor for Firefox sessionstore recovery.jsonlz4 */
static char *decompress_mozlz4(const char *path, size_t *out_len)
{
	*out_len = 0;
	FILE *fp = fopen(path, "rb");
	if (!fp) return NULL;
	fseek(fp, 0, SEEK_END);
	long fsz = ftell(fp);
	fseek(fp, 0, SEEK_SET);
	if (fsz <= 12 || fsz > 16 * 1024 * 1024) {
		fclose(fp);
		return NULL;
	}
	uint8_t *data = (uint8_t *)malloc((size_t)fsz);
	if (!data) {
		fclose(fp);
		return NULL;
	}
	if (fread(data, 1, (size_t)fsz, fp) != (size_t)fsz || memcmp(data, "mozLz40\0", 8) != 0) {
		free(data);
		fclose(fp);
		return NULL;
	}
	fclose(fp);

	uint32_t uncomp_sz = (uint32_t)data[8] | ((uint32_t)data[9] << 8) |
	                     ((uint32_t)data[10] << 16) | ((uint32_t)data[11] << 24);
	if (uncomp_sz == 0 || uncomp_sz > 64 * 1024 * 1024) {
		free(data);
		return NULL;
	}

	uint8_t *out = (uint8_t *)malloc(uncomp_sz + 1);
	if (!out) {
		free(data);
		return NULL;
	}

	size_t ip = 12, n = (size_t)fsz, op = 0;
	while (ip < n && op < uncomp_sz) {
		uint8_t token = data[ip++];
		size_t lit_len = token >> 4;
		if (lit_len == 15) {
			while (ip < n) {
				uint8_t s = data[ip++];
				lit_len += s;
				if (s != 255) break;
			}
		}
		if (ip + lit_len > n || op + lit_len > uncomp_sz) break;
		memcpy(out + op, data + ip, lit_len);
		ip += lit_len;
		op += lit_len;
		if (ip >= n || op >= uncomp_sz) break;
		if (ip + 2 > n) break;
		size_t offset = (size_t)data[ip] | ((size_t)data[ip + 1] << 8);
		ip += 2;
		if (offset == 0 || offset > op) break;
		size_t match_len = (token & 0x0F) + 4;
		if (match_len == 19) {
			while (ip < n) {
				uint8_t s = data[ip++];
				match_len += s;
				if (s != 255) break;
			}
		}
		size_t start = op - offset;
		for (size_t i = 0; i < match_len && op < uncomp_sz; i++) {
			out[op++] = out[start + i];
		}
	}
	out[op] = '\0';
	free(data);
	*out_len = op;
	return (char *)out;
}

static void populate_firefox_tabs(jarvis_world_t *w)
{
	const char *home = getenv("HOME");
	if (!home || !*home) return;

	const char *pats[2] = {
		"%s/snap/firefox/common/.mozilla/firefox/*.default*/sessionstore-backups/recovery.jsonlz4",
		"%s/.mozilla/firefox/*.default*/sessionstore-backups/recovery.jsonlz4"
	};

	char best_path[512] = {0};
	time_t best_mtime = 0;

	for (int i = 0; i < 2; i++) {
		char pat[512];
		snprintf(pat, sizeof(pat), pats[i], home);
		glob_t gl;
		if (glob(pat, 0, NULL, &gl) == 0) {
			for (size_t j = 0; j < gl.gl_pathc; j++) {
				struct stat st;
				if (stat(gl.gl_pathv[j], &st) == 0 && st.st_mtime > best_mtime) {
					best_mtime = st.st_mtime;
					snprintf(best_path, sizeof(best_path), "%s", gl.gl_pathv[j]);
				}
			}
			globfree(&gl);
		}
	}

	if (!best_path[0]) return;
	if (best_mtime == g_tabs_mtime && g_tabs_cache_count > 0) {
		for (size_t i = 0; i < g_tabs_cache_count && w->tab_count < JARVIS_MAX_TABS; i++) {
			w->tabs[w->tab_count++] = g_tabs_cache[i];
		}
		return;
	}

	size_t raw_len = 0;
	char *raw = decompress_mozlz4(best_path, &raw_len);
	if (!raw) return;

	g_tabs_cache_count = 0;
	const char *p = raw;
	int tab_idx = 1;
	while ((p = strstr(p, "\"entries\":[")) != NULL && g_tabs_cache_count < JARVIS_MAX_TABS) {
		p += 11;
		const char *end_arr = strchr(p, ']');
		if (!end_arr) break;
		const char *url_key = NULL;
		const char *title_key = NULL;
		const char *scan = p;
		while ((scan = strstr(scan, "\"url\":\"")) != NULL && scan < end_arr) {
			url_key = scan;
			const char *tk = strstr(scan, "\"title\":\"");
			if (tk && tk < end_arr) title_key = tk;
			scan += 7;
		}
		if (url_key && url_key < end_arr) {
			url_key += 7;
			const char *url_end = strchr(url_key, '"');
			if (url_end && url_end > url_key) {
				jarvis_tab_t *t = &g_tabs_cache[g_tabs_cache_count++];
				memset(t, 0, sizeof(*t));
				t->index = tab_idx++;
				size_t ulen = (size_t)(url_end - url_key);
				if (ulen >= sizeof(t->url)) ulen = sizeof(t->url) - 1;
				memcpy(t->url, url_key, ulen);
				t->url[ulen] = '\0';

				if (title_key && title_key < end_arr) {
					title_key += 9;
					const char *title_end = strchr(title_key, '"');
					if (title_end && title_end > title_key) {
						size_t tlen = (size_t)(title_end - title_key);
						if (tlen >= sizeof(t->title)) tlen = sizeof(t->title) - 1;
						memcpy(t->title, title_key, tlen);
						t->title[tlen] = '\0';
					}
				}
			}
		}
		p = end_arr + 1;
	}
	free(raw);

	g_tabs_mtime = best_mtime;
	for (size_t i = 0; i < g_tabs_cache_count && w->tab_count < JARVIS_MAX_TABS; i++) {
		w->tabs[w->tab_count++] = g_tabs_cache[i];
	}
}

/* ---------------------------------------------------------------------------
 * Audio / MPRIS / Notification Helpers
 * ------------------------------------------------------------------------- */

static bool mpris_control(const char *action)
{
	char cmd[512], out[256];
	snprintf(cmd, sizeof(cmd),
		"for bus in $(gdbus call --session --dest org.freedesktop.DBus --object-path /org/freedesktop/DBus "
		"--method org.freedesktop.DBus.ListNames 2>/dev/null | tr \"',[]\" ' ' | grep -o 'org\\.mpris\\.MediaPlayer2\\.[^ ]*'); do "
		"gdbus call --session --dest \"$bus\" --object-path /org/mpris/MediaPlayer2 "
		"--method org.mpris.MediaPlayer2.Player.%s 2>/dev/null && echo OK; done",
		action);
	run_cmd_capture(cmd, out, sizeof(out));
	return strstr(out, "OK") != NULL;
}

/* ---------------------------------------------------------------------------
 * Native X11 Input Injection (libXtst.so.6 via dlopen)
 * ------------------------------------------------------------------------- */

typedef struct {
	int  (*fake_key)(Display *, unsigned int, Bool, unsigned long);
	int  (*fake_btn)(Display *, unsigned int, Bool, unsigned long);
	int  (*fake_mot)(Display *, int, int, int, unsigned long);
	bool ready;
	bool tried;
} xtst_syms_t;

static xtst_syms_t g_xtst = {0};

static bool ensure_xtst_loaded(void)
{
	if (g_xtst.tried) return g_xtst.ready;
	g_xtst.tried = true;
	void *h = dlopen("libXtst.so.6", RTLD_LAZY | RTLD_LOCAL);
	if (!h) h = dlopen("libXtst.so", RTLD_LAZY | RTLD_LOCAL);
	if (!h) return false;
	g_xtst.fake_key = (int (*)(Display *, unsigned int, Bool, unsigned long))dlsym(h, "XTestFakeKeyEvent");
	g_xtst.fake_btn = (int (*)(Display *, unsigned int, Bool, unsigned long))dlsym(h, "XTestFakeButtonEvent");
	g_xtst.fake_mot = (int (*)(Display *, int, int, int, unsigned long))dlsym(h, "XTestFakeMotionEvent");
	if (g_xtst.fake_key && g_xtst.fake_btn && g_xtst.fake_mot) {
		g_xtst.ready = true;
	}
	return g_xtst.ready;
}

static KeySym map_keysym_token(const char *tok)
{
	if (!tok || !*tok) return NoSymbol;
	if (strcasecmp(tok, "ctrl") == 0 || strcasecmp(tok, "control") == 0) return XStringToKeysym("Control_L");
	if (strcasecmp(tok, "shift") == 0) return XStringToKeysym("Shift_L");
	if (strcasecmp(tok, "alt") == 0) return XStringToKeysym("Alt_L");
	if (strcasecmp(tok, "super") == 0 || strcasecmp(tok, "win") == 0 || strcasecmp(tok, "meta") == 0) return XStringToKeysym("Super_L");
	if (strcasecmp(tok, "enter") == 0 || strcasecmp(tok, "return") == 0) return XStringToKeysym("Return");
	if (strcasecmp(tok, "esc") == 0 || strcasecmp(tok, "escape") == 0) return XStringToKeysym("Escape");
	if (strcasecmp(tok, "space") == 0) return XStringToKeysym("space");
	if (strcasecmp(tok, "tab") == 0) return XStringToKeysym("Tab");
	if (strcasecmp(tok, "backspace") == 0 || strcasecmp(tok, "bs") == 0) return XStringToKeysym("BackSpace");
	if (strcasecmp(tok, "plus") == 0) return XStringToKeysym("equal");
	if (strcasecmp(tok, "minus") == 0) return XStringToKeysym("minus");
	KeySym ks = XStringToKeysym(tok);
	if (ks == NoSymbol && strlen(tok) == 1) {
		ks = (KeySym)(unsigned char)tok[0];
	}
	return ks;
}

static void clear_modifiers_dpy(Display *dpy)
{
	if (!dpy || !ensure_xtst_loaded()) return;
	static const char *const MODS[] = {
		"Super_L", "Super_R", "Shift_L", "Shift_R",
		"Control_L", "Control_R", "Alt_L", "Alt_R", NULL
	};
	for (size_t i = 0; MODS[i]; i++) {
		KeySym ks = XStringToKeysym(MODS[i]);
		if (ks != NoSymbol) {
			KeyCode kc = XKeysymToKeycode(dpy, ks);
			if (kc != 0) g_xtst.fake_key(dpy, kc, False, 0);
		}
	}
	XFlush(dpy);
}

static void x11_press_key_combo(const char *combo)
{
	if (!combo || !*combo) combo = "Return";
	Display *dpy = XOpenDisplay(NULL);
	if (!dpy) return;
	if (!ensure_xtst_loaded()) {
		XCloseDisplay(dpy);
		return;
	}
	clear_modifiers_dpy(dpy);

	/* Split space-separated sequence of chords (e.g. "ctrl+a BackSpace") */
	char seq_buf[256];
	snprintf(seq_buf, sizeof(seq_buf), "%s", combo);
	char *seq_save = NULL;
	for (char *chord = strtok_r(seq_buf, " ", &seq_save); chord; chord = strtok_r(NULL, " ", &seq_save)) {
		char cbuf[128];
		snprintf(cbuf, sizeof(cbuf), "%s", chord);
		KeyCode codes[8];
		size_t n_codes = 0;
		bool need_shift_plus = (strcasestr(chord, "plus") != NULL && !strcasestr(chord, "shift"));
		if (need_shift_plus) {
			KeyCode skc = XKeysymToKeycode(dpy, XStringToKeysym("Shift_L"));
			if (skc != 0) codes[n_codes++] = skc;
		}
		char *csave = NULL;
		for (char *part = strtok_r(cbuf, "+", &csave); part && n_codes < 8; part = strtok_r(NULL, "+", &csave)) {
			KeySym ks = map_keysym_token(part);
			if (ks != NoSymbol) {
				KeyCode kc = XKeysymToKeycode(dpy, ks);
				if (kc != 0) codes[n_codes++] = kc;
			}
		}
		for (size_t i = 0; i < n_codes; i++) {
			g_xtst.fake_key(dpy, codes[i], True, 0);
		}
		XFlush(dpy);
		usleep(8000);
		for (size_t i = n_codes; i > 0; i--) {
			g_xtst.fake_key(dpy, codes[i - 1], False, 0);
		}
		XFlush(dpy);
		usleep(8000);
	}
	XCloseDisplay(dpy);
}

static bool char_to_keysym_and_shift(char c, const char **out_sym, bool *out_shift)
{
	*out_shift = false;
	if (c >= 'a' && c <= 'z') {
		static char s[2];
		s[0] = c; s[1] = '\0';
		*out_sym = s;
		return true;
	}
	if (c >= 'A' && c <= 'Z') {
		static char s[2];
		s[0] = (char)tolower((unsigned char)c); s[1] = '\0';
		*out_sym = s;
		*out_shift = true;
		return true;
	}
	if (c >= '0' && c <= '9') {
		static char s[2];
		s[0] = c; s[1] = '\0';
		*out_sym = s;
		return true;
	}
	switch (c) {
		case ' ':  *out_sym = "space"; return true;
		case '\n': *out_sym = "Return"; return true;
		case '\t': *out_sym = "Tab"; return true;
		case '-':  *out_sym = "minus"; return true;
		case '_':  *out_sym = "minus"; *out_shift = true; return true;
		case '=':  *out_sym = "equal"; return true;
		case '+':  *out_sym = "equal"; *out_shift = true; return true;
		case '/':  *out_sym = "slash"; return true;
		case '?':  *out_sym = "slash"; *out_shift = true; return true;
		case '.':  *out_sym = "period"; return true;
		case '>':  *out_sym = "period"; *out_shift = true; return true;
		case ',':  *out_sym = "comma"; return true;
		case '<':  *out_sym = "comma"; *out_shift = true; return true;
		case ';':  *out_sym = "semicolon"; return true;
		case ':':  *out_sym = "semicolon"; *out_shift = true; return true;
		case '\'': *out_sym = "apostrophe"; return true;
		case '"':  *out_sym = "apostrophe"; *out_shift = true; return true;
		case '[':  *out_sym = "bracketleft"; return true;
		case '{':  *out_sym = "bracketleft"; *out_shift = true; return true;
		case ']':  *out_sym = "bracketright"; return true;
		case '}':  *out_sym = "bracketright"; *out_shift = true; return true;
		case '\\': *out_sym = "backslash"; return true;
		case '|':  *out_sym = "backslash"; *out_shift = true; return true;
		case '`':  *out_sym = "grave"; return true;
		case '~':  *out_sym = "grave"; *out_shift = true; return true;
		case '!':  *out_sym = "1"; *out_shift = true; return true;
		case '@':  *out_sym = "2"; *out_shift = true; return true;
		case '#':  *out_sym = "3"; *out_shift = true; return true;
		case '$':  *out_sym = "4"; *out_shift = true; return true;
		case '%':  *out_sym = "5"; *out_shift = true; return true;
		case '^':  *out_sym = "6"; *out_shift = true; return true;
		case '&':  *out_sym = "7"; *out_shift = true; return true;
		case '*':  *out_sym = "8"; *out_shift = true; return true;
		case '(':  *out_sym = "9"; *out_shift = true; return true;
		case ')':  *out_sym = "0"; *out_shift = true; return true;
		default:   return false;
	}
}

static void x11_type_text(const char *text, bool press_enter)
{
	Display *dpy = XOpenDisplay(NULL);
	if (!dpy) return;
	if (!ensure_xtst_loaded()) {
		XCloseDisplay(dpy);
		return;
	}
	clear_modifiers_dpy(dpy);
	KeyCode shift_kc = XKeysymToKeycode(dpy, XStringToKeysym("Shift_L"));

	if (text && *text) {
		for (const char *p = text; *p; p++) {
			const char *sym_name = NULL;
			bool need_shift = false;
			if (!char_to_keysym_and_shift(*p, &sym_name, &need_shift)) continue;
			KeySym ks = XStringToKeysym(sym_name);
			if (ks == NoSymbol) continue;
			KeyCode kc = XKeysymToKeycode(dpy, ks);
			if (kc == 0) continue;
			if (need_shift && shift_kc != 0) g_xtst.fake_key(dpy, shift_kc, True, 0);
			g_xtst.fake_key(dpy, kc, True, 0);
			g_xtst.fake_key(dpy, kc, False, 0);
			if (need_shift && shift_kc != 0) g_xtst.fake_key(dpy, shift_kc, False, 0);
			XFlush(dpy);
			usleep(1800);
		}
		usleep(15000);
	}
	if (press_enter) {
		KeyCode ret_kc = XKeysymToKeycode(dpy, XStringToKeysym("Return"));
		if (ret_kc != 0) {
			g_xtst.fake_key(dpy, ret_kc, True, 0);
			g_xtst.fake_key(dpy, ret_kc, False, 0);
			XFlush(dpy);
		}
		usleep(25000);
	}
	XCloseDisplay(dpy);
}

static void x11_mouse_click(int x, int y, int btn)
{
	Display *dpy = XOpenDisplay(NULL);
	if (!dpy) return;
	if (ensure_xtst_loaded()) {
		g_xtst.fake_mot(dpy, DefaultScreen(dpy), x, y, 0);
		g_xtst.fake_btn(dpy, (unsigned int)btn, True, 0);
		g_xtst.fake_btn(dpy, (unsigned int)btn, False, 0);
		XFlush(dpy);
	}
	XCloseDisplay(dpy);
}

static void x11_mouse_scroll(int move_x, int move_y, bool need_move, int btn, int count)
{
	Display *dpy = XOpenDisplay(NULL);
	if (!dpy) return;
	if (ensure_xtst_loaded()) {
		if (need_move) {
			g_xtst.fake_mot(dpy, DefaultScreen(dpy), move_x, move_y, 0);
			XFlush(dpy);
		}
		for (int i = 0; i < count; i++) {
			g_xtst.fake_btn(dpy, (unsigned int)btn, True, 0);
			g_xtst.fake_btn(dpy, (unsigned int)btn, False, 0);
			XFlush(dpy);
			if (i + 1 < count) usleep(15000);
		}
	}
	XCloseDisplay(dpy);
}

static void url_encode(const char *src, char *dst, size_t dst_sz)
{
	static const char hex[] = "0123456789ABCDEF";
	size_t w = 0;
	for (const unsigned char *p = (const unsigned char *)src; *p && w + 4 < dst_sz; p++) {
		if (isalnum(*p) || *p == '-' || *p == '_' || *p == '.' || *p == '~') {
			dst[w++] = (char)*p;
		} else if (*p == ' ') {
			dst[w++] = '+';
		} else {
			dst[w++] = '%';
			dst[w++] = hex[*p >> 4];
			dst[w++] = hex[*p & 0x0F];
		}
	}
	dst[w] = '\0';
}

static void json_escape_into(const char *src, char *dst, size_t dst_sz)
{
	if (!dst || dst_sz == 0) return;
	if (!src) { dst[0] = '\0'; return; }
	size_t w = 0;
	for (const unsigned char *p = (const unsigned char *)src; *p && w + 3 < dst_sz; p++) {
		if (*p == '"' || *p == '\\') {
			dst[w++] = '\\';
			dst[w++] = (char)*p;
		} else if (*p == '\n') {
			dst[w++] = '\\';
			dst[w++] = 'n';
		} else if (*p == '\r') {
			dst[w++] = '\\';
			dst[w++] = 'r';
		} else if (*p == '\t') {
			dst[w++] = '\\';
			dst[w++] = 't';
		} else if (*p >= 32) {
			dst[w++] = (char)*p;
		}
	}
	dst[w] = '\0';
}

/* ---------------------------------------------------------------------------
 * Live Browser Accessibility Tree Inspector (AT-SPI2 via libatspi.so.0)
 * ------------------------------------------------------------------------- */

typedef struct {
	int   (*init)(void);
	void *(*get_desktop)(int);
	int   (*get_child_count)(void *, void **);
	void *(*get_child_at_index)(void *, int, void **);
	char *(*get_name)(void *, void **);
	char *(*get_role_name)(void *, void **);
	char *(*text_get_text)(void *, int, int, void **);
	void *(*get_hyperlink)(void *);
	char *(*hyperlink_get_uri)(void *, int, void **);
	void *(*get_document_iface)(void *);
	char *(*doc_get_attr)(void *, const char *, void **);
	void  (*g_unref)(void *);
	void  (*g_free)(void *);
	bool  ready;
	bool  tried;
} atspi_syms_t;

static atspi_syms_t g_atspi = {0};

static bool ensure_atspi_loaded(void)
{
	if (g_atspi.tried) return g_atspi.ready;
	g_atspi.tried = true;

	void *h_atspi = dlopen("libatspi.so.0", RTLD_LAZY | RTLD_LOCAL);
	void *h_gobj  = dlopen("libgobject-2.0.so.0", RTLD_LAZY | RTLD_LOCAL);
	void *h_glib  = dlopen("libglib-2.0.so.0", RTLD_LAZY | RTLD_LOCAL);
	if (!h_atspi || !h_gobj || !h_glib) return false;

	g_atspi.init               = (int (*)(void))dlsym(h_atspi, "atspi_init");
	g_atspi.get_desktop        = (void *(*)(int))dlsym(h_atspi, "atspi_get_desktop");
	g_atspi.get_child_count    = (int (*)(void *, void **))dlsym(h_atspi, "atspi_accessible_get_child_count");
	g_atspi.get_child_at_index = (void *(*)(void *, int, void **))dlsym(h_atspi, "atspi_accessible_get_child_at_index");
	g_atspi.get_name           = (char *(*)(void *, void **))dlsym(h_atspi, "atspi_accessible_get_name");
	g_atspi.get_role_name      = (char *(*)(void *, void **))dlsym(h_atspi, "atspi_accessible_get_role_name");
	g_atspi.text_get_text      = (char *(*)(void *, int, int, void **))dlsym(h_atspi, "atspi_text_get_text");
	g_atspi.get_hyperlink      = (void *(*)(void *))dlsym(h_atspi, "atspi_accessible_get_hyperlink");
	g_atspi.hyperlink_get_uri  = (char *(*)(void *, int, void **))dlsym(h_atspi, "atspi_hyperlink_get_uri");
	g_atspi.get_document_iface = (void *(*)(void *))dlsym(h_atspi, "atspi_accessible_get_document_iface");
	g_atspi.doc_get_attr       = (char *(*)(void *, const char *, void **))dlsym(h_atspi, "atspi_document_get_document_attribute_value");
	g_atspi.g_unref            = (void (*)(void *))dlsym(h_gobj, "g_object_unref");
	g_atspi.g_free             = (void (*)(void *))dlsym(h_glib, "g_free");

	if (!g_atspi.init || !g_atspi.get_desktop || !g_atspi.get_child_count ||
	    !g_atspi.get_child_at_index || !g_atspi.get_name || !g_atspi.get_role_name ||
	    !g_atspi.g_unref || !g_atspi.g_free) {
		return false;
	}
	g_atspi.init();
	g_atspi.ready = true;
	return true;
}

static void walk_atspi_browser_node(void *node, int depth, bool in_doc,
                                    char *out, size_t out_sz, size_t *off, int *visited)
{
	if (!node || depth > 20 || *visited > 350 || *off + 128 >= out_sz) return;
	(*visited)++;

	void *err = NULL;
	char *role = g_atspi.get_role_name(node, &err);
	err = NULL;
	char *name = g_atspi.get_name(node, &err);

	bool now_in_doc = in_doc;
	if (role) {
		if (strcmp(role, "document web") == 0 || strcmp(role, "document frame") == 0) {
			now_in_doc = true;
			char *doc_url = NULL;
			if (g_atspi.get_document_iface && g_atspi.doc_get_attr) {
				void *doc_if = g_atspi.get_document_iface(node);
				if (doc_if) {
					void *derr = NULL;
					doc_url = g_atspi.doc_get_attr(doc_if, "DocURL", &derr);
					g_atspi.g_unref(doc_if);
				}
			}
			*off += (size_t)snprintf(out + *off, out_sz - *off,
			                         "[%s] %s%s%s\n",
			                         role,
			                         (name && *name) ? name : "",
			                         (doc_url && *doc_url) ? " url=" : "",
			                         (doc_url && *doc_url) ? doc_url : "");
			if (doc_url) g_atspi.g_free(doc_url);
		} else if (!in_doc && strcmp(role, "frame") == 0 && name && *name) {
			*off += (size_t)snprintf(out + *off, out_sz - *off, "[frame] %s\n", name);
		} else if (now_in_doc &&
		           (strcmp(role, "heading") == 0 ||
		            strcmp(role, "paragraph") == 0 ||
		            strcmp(role, "link") == 0 ||
		            strcmp(role, "entry") == 0 ||
		            strcmp(role, "push button") == 0 ||
		            strcmp(role, "list item") == 0 ||
		            strcmp(role, "section") == 0 ||
		            strcmp(role, "static") == 0 ||
		            strcmp(role, "text") == 0)) {
			char *txt = NULL;
			if (g_atspi.text_get_text) {
				void *terr = NULL;
				txt = g_atspi.text_get_text(node, 0, 400, &terr);
			}
			char *uri = NULL;
			if (strcmp(role, "link") == 0 && g_atspi.get_hyperlink && g_atspi.hyperlink_get_uri) {
				void *hl = g_atspi.get_hyperlink(node);
				if (hl) {
					void *herr = NULL;
					uri = g_atspi.hyperlink_get_uri(hl, 0, &herr);
					g_atspi.g_unref(hl);
				}
			}
			/* Clean embedded Private-Use / object-replacement glyphs (0xEF 0xBF 0xBC) from AT-SPI text */
			const char *best_txt = (txt && *txt) ? txt : ((name && *name) ? name : "");
			char clean_txt[420] = {0};
			size_t cw = 0;
			for (const unsigned char *p = (const unsigned char *)best_txt; *p && cw + 2 < sizeof(clean_txt); ) {
				if (p[0] == 0xEF && p[1] == 0xBF && p[2] == 0xBC) {
					p += 3;
					continue;
				}
				if (*p == '\n' || *p == '\r') {
					clean_txt[cw++] = ' ';
					p++;
					continue;
				}
				clean_txt[cw++] = (char)*p++;
			}
			clean_txt[cw] = '\0';

			if (clean_txt[0] || (uri && *uri)) {
				*off += (size_t)snprintf(out + *off, out_sz - *off,
				                         "[%s] %s%s%s\n",
				                         role, clean_txt,
				                         (uri && *uri) ? " -> " : "",
				                         (uri && *uri) ? uri : "");
			}
			if (txt) g_atspi.g_free(txt);
			if (uri) g_atspi.g_free(uri);
		}
	}

	if (role) g_atspi.g_free(role);
	if (name) g_atspi.g_free(name);

	err = NULL;
	int n = g_atspi.get_child_count(node, &err);
	if (n > 64) n = 64;
	for (int i = 0; i < n && *off + 128 < out_sz; i++) {
		err = NULL;
		void *ch = g_atspi.get_child_at_index(node, i, &err);
		if (ch) {
			walk_atspi_browser_node(ch, depth + 1, now_in_doc, out, out_sz, off, visited);
			g_atspi.g_unref(ch);
		}
	}
}

static void extract_html_fallback(const char *file_url, char *out, size_t out_sz, size_t *off)
{
	if (!file_url || strncmp(file_url, "file://", 7) != 0 || *off + 128 >= out_sz) return;
	const char *path = file_url + 7;
	FILE *fp = fopen(path, "r");
	if (!fp) return;
	char buf[4096];
	size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
	fclose(fp);
	buf[n] = '\0';

	/* Extract text and href attributes from local HTML file */
	*off += (size_t)snprintf(out + *off, out_sz - *off, "[local_page %s] ", file_url);
	bool in_tag = false, in_script_or_style = false;
	for (size_t i = 0; i < n && *off + 64 < out_sz; i++) {
		if (buf[i] == '<') {
			in_tag = true;
			if (strncasecmp(buf + i, "<script", 7) == 0 || strncasecmp(buf + i, "<style", 6) == 0)
				in_script_or_style = true;
			else if (strncasecmp(buf + i, "</script", 8) == 0 || strncasecmp(buf + i, "</style", 7) == 0)
				in_script_or_style = false;
			else if (strncasecmp(buf + i, "<a ", 3) == 0) {
				const char *href = strcasestr(buf + i, "href=\"");
				const char *gt = strchr(buf + i, '>');
				if (href && gt && href < gt) {
					href += 6;
					const char *q = strchr(href, '"');
					if (q && q < gt) {
						*off += (size_t)snprintf(out + *off, out_sz - *off, " [link href=%.*s] ",
						                         (int)(q - href), href);
					}
				}
			}
			continue;
		}
		if (buf[i] == '>') {
			in_tag = false;
			if (*off > 0 && out[*off - 1] != ' ') out[(*off)++] = ' ';
			continue;
		}
		if (!in_tag && !in_script_or_style) {
			char c = buf[i];
			if (c == '\n' || c == '\r' || c == '\t') c = ' ';
			if (c == ' ' && *off > 0 && out[*off - 1] == ' ') continue;
			out[(*off)++] = c;
		}
	}
	out[*off] = '\0';
}

static bool inspect_browser_a11y(const jarvis_world_t *w, char *out, size_t out_sz)
{
	if (!out || out_sz == 0) return false;
	out[0] = '\0';
	size_t off = 0;

	if (ensure_atspi_loaded()) {
		void *desktop = g_atspi.get_desktop(0);
		if (desktop) {
			void *err = NULL;
			int n_apps = g_atspi.get_child_count(desktop, &err);
			for (int i = 0; i < n_apps && off + 128 < out_sz; i++) {
				err = NULL;
				void *app = g_atspi.get_child_at_index(desktop, i, &err);
				if (!app) continue;
				err = NULL;
				char *aname = g_atspi.get_name(app, &err);
				if (aname && (str_contains_ci(aname, "Firefox") ||
				              str_contains_ci(aname, "Chromium") ||
				              str_contains_ci(aname, "Chrome") ||
				              str_contains_ci(aname, "Brave"))) {
					int visited = 0;
					walk_atspi_browser_node(app, 0, false, out, out_sz, &off, &visited);
				}
				if (aname) g_atspi.g_free(aname);
				g_atspi.g_unref(app);
			}
			g_atspi.g_unref(desktop);
		}
	}

	/* If AT-SPI2 only returned frame title or was empty, append active tab info & local HTML fallback */
	if (w && !strstr(out, "[document web]")) {
		for (size_t i = 0; i < w->client_count && off + 128 < out_sz; i++) {
			if (str_contains_ci(w->clients[i].wm_class, "firefox") ||
			    str_contains_ci(w->clients[i].wm_class, "chromium") ||
			    str_contains_ci(w->clients[i].wm_class, "brave")) {
				if (!strstr(out, w->clients[i].title)) {
					off += (size_t)snprintf(out + off, out_sz - off,
					                        "[window] %s\n", w->clients[i].title);
				}
			}
		}
		for (size_t i = 0; i < w->tab_count && off + 128 < out_sz; i++) {
			off += (size_t)snprintf(out + off, out_sz - off,
			                        "[tab %d] %s (%s)\n",
			                        w->tabs[i].index, w->tabs[i].title, w->tabs[i].url);
			if (strncmp(w->tabs[i].url, "file://", 7) == 0) {
				extract_html_fallback(w->tabs[i].url, out, out_sz, &off);
			}
		}
	}

	return off > 0;
}

static bool focus_window_by_id_str(const char *wid_str, double timeout_sec)
{
	if (!wid_str || !*wid_str) return false;
	Window w = (Window)strtoul(wid_str, NULL, 0);
	if (!w) return false;
	Display *dpy = XOpenDisplay(NULL);
	if (!dpy) return false;
	send_dwm_cmd(dpy, DefaultRootWindow(dpy), DWM_CMD_FOCUS_WIN, (long)w);
	XCloseDisplay(dpy);
	return wait_window_focused(wid_str, timeout_sec);
}

static bool focus_firefox_window(void)
{
	jarvis_world_t w;
	memset(&w, 0, sizeof(w));
	query_x11_windows(&w);
	for (size_t i = 0; i < w.client_count; i++) {
		if (w.clients[i].visible && str_contains_ci(w.clients[i].wm_class, "firefox")) {
			return focus_window_by_id_str(w.clients[i].id, 1.2);
		}
	}
	for (size_t i = 0; i < w.client_count; i++) {
		if (str_contains_ci(w.clients[i].wm_class, "firefox")) {
			return focus_window_by_id_str(w.clients[i].id, 1.2);
		}
	}
	return false;
}

/* ---------------------------------------------------------------------------
 * Xylem Hook 1: jarvis_xy_observe
 * ------------------------------------------------------------------------- */

XY_IMPL(int, jarvis_xy_observe, jarvis_world_t *, w)
{
	if (!w) return 0;
	snprintf(w->wm, sizeof(w->wm), "dwm");

	/* 1. X11 Windows (focused, other, clients) */
	query_x11_windows(w);

	/* 2. Focused terminal CWD & foreground process */
	const jarvis_window_t *term_win = &w->focused;
	if (!is_terminal_class(term_win->wm_class) && is_terminal_class(w->other.wm_class)) {
		term_win = &w->other;
	}
	resolve_window_cwd_and_fg(term_win, w->focused_cwd, sizeof(w->focused_cwd),
	                          w->focused_process, sizeof(w->focused_process));

	/* 3. Fast system health (<0.05ms) */
	populate_fast_system_health(w->system_health, sizeof(w->system_health));

	/* 4. Active tmux panes */
	populate_tmux_panes(w, true, 20);

	/* 5. Firefox open tabs (cached by recovery.jsonlz4 mtime) & live browser accessibility tree */
	populate_firefox_tabs(w);
	inspect_browser_a11y(w, w->browser_page, sizeof(w->browser_page));

	/* 6. Discovered .desktop applications */
	ensure_desktop_apps_loaded();
	for (size_t i = 0; i < g_desktop_app_count && w->custom_entity_count < JARVIS_MAX_APPS; i++) {
		jarvis_live_entity_t *ce = &w->custom_entities[w->custom_entity_count++];
		snprintf(ce->type, sizeof(ce->type), "gui.application");
		snprintf(ce->id, sizeof(ce->id), "%.*s", (int)(sizeof(ce->id) - 1), g_desktop_apps[i].exec_cmd);
		snprintf(ce->phrase, sizeof(ce->phrase), "%.*s", (int)(sizeof(ce->phrase) - 1), g_desktop_apps[i].phrase);
	}

	return 0;
}

/* ---------------------------------------------------------------------------
 * Xylem Hook 2: jarvis_xy_execute
 * ------------------------------------------------------------------------- */

static double g_last_term_open_ts = 0.0;

static void op_open_terminal(const char *cmd, jarvis_adapter_result_t *res)
{
	const char *term_cmd = (cmd && *cmd) ? cmd : "st";
	jarvis_window_t prev_foc;
	get_focused_window_fast(&prev_foc);
	const char *term_cls = (strncmp(term_cmd, "xterm", 5) == 0) ? "xterm" : "st-256color";

	pid_t pid = 0;
	if (strcmp(term_cmd, "st") == 0 || strcmp(term_cmd, "xterm") == 0) {
		char *const argv[] = { (char *)term_cmd, NULL };
		pid = spawn_detached_argv(argv);
	} else {
		pid = spawn_detached_sh(term_cmd);
	}

	bool focused_new = false;
	double deadline = now_sec() + 1.5;
	usleep(40000);
	while (now_sec() < deadline) {
		jarvis_window_t foc;
		if (get_focused_window_fast(&foc) && str_contains_ci(foc.wm_class, term_cls)) {
			if (!prev_foc.id[0] || strcasecmp(foc.id, prev_foc.id) != 0) {
				focused_new = true;
				break;
			}
		}
		usleep(35000);
	}

	if (!focused_new) {
		wait_window_focused(term_cls, 0.6);
	}

	/* Wait briefly for interactive shell startup (.bashrc/.zshrc) subshells to settle before typing */
	if (pid > 0) {
		double t_end = now_sec() + 0.70;
		int idle_hits = 0;
		usleep(60000);
		while (now_sec() < t_end) {
			unsigned long sh_pids[8];
			size_t n_sh = get_child_pids((unsigned long)pid, sh_pids, 8);
			if (n_sh > 0) {
				unsigned long sh_pid = sh_pids[n_sh - 1];
				/* If the direct child of /bin/sh -c is st itself, descend one more level to the shell */
				char comm[64];
				read_proc_comm(sh_pid, comm, sizeof(comm));
				if (!is_shell_name(comm)) {
					unsigned long inner_sh[8];
					size_t n_in = get_child_pids(sh_pid, inner_sh, 8);
					if (n_in > 0) sh_pid = inner_sh[n_in - 1];
					else { usleep(35000); continue; }
				}
				unsigned long sub_pids[8];
				if (get_child_pids(sh_pid, sub_pids, 8) == 0) {
					if (++idle_hits >= 2) break;
				} else {
					idle_hits = 0;
				}
			}
			usleep(35000);
		}
	}
	usleep(40000);
	g_last_term_open_ts = now_sec();

	res->handled = true;
	res->success = true;
	snprintf(res->output, sizeof(res->output), "opened terminal: %s", term_cmd);
}

XY_IMPL(int, jarvis_xy_execute, const jarvis_dsl_t *, dsl, jarvis_adapter_result_t *, res)
{
	if (!dsl || !res || res->handled) return 0;
	const char *op = dsl->op;

	/* 1. focus_relative_window */
	if (strcmp(op, "focus_relative_window") == 0 ||
	    (strcmp(op, "focus_window") == 0 &&
	     (strcmp(dsl->target.relation, "other") == 0 ||
	      strcmp(dsl->target.relation, "previous") == 0 ||
	      strcmp(dsl->target.relation, "next") == 0 ||
	      strcmp(dsl->target.relation, "master") == 0))) {
		const char *rel = dsl->target.relation[0] ? dsl->target.relation : "other";
		res->handled = true;
		if (strcmp(rel, "other") == 0) {
			jarvis_world_t w;
			memset(&w, 0, sizeof(w));
			query_x11_windows(&w);
			if (w.other.id[0]) {
				res->success = focus_window_by_id_str(w.other.id, 1.0);
				snprintf(res->output, sizeof(res->output), "focused other window: %s", w.other.wm_class);
			} else {
				res->success = false;
				snprintf(res->error, sizeof(res->error), "No other window available");
			}
			return 0;
		}
		Display *dpy = XOpenDisplay(NULL);
		if (!dpy) {
			res->success = false;
			snprintf(res->error, sizeof(res->error), "Cannot open X11 display");
			return 0;
		}
		Window root = DefaultRootWindow(dpy);
		if (strcmp(rel, "previous") == 0) {
			send_dwm_cmd(dpy, root, DWM_CMD_FOCUSSTACK, -1);
			snprintf(res->output, sizeof(res->output), "focused previous window");
			res->success = true;
		} else if (strcmp(rel, "next") == 0) {
			send_dwm_cmd(dpy, root, DWM_CMD_FOCUSSTACK, 1);
			snprintf(res->output, sizeof(res->output), "focused next window");
			res->success = true;
		} else if (strcmp(rel, "master") == 0) {
			send_dwm_cmd(dpy, root, DWM_CMD_ZOOM, 0);
			snprintf(res->output, sizeof(res->output), "swapped with master");
			res->success = true;
		} else {
			snprintf(res->error, sizeof(res->error), "Unknown relation: %s", rel);
			res->success = false;
		}
		XCloseDisplay(dpy);
		return 0;
	}

	/* 2. focus_window */
	if (strcmp(op, "focus_window") == 0) {
		res->handled = true;
		const char *ident = dsl->target.identifier[0] ? dsl->target.identifier :
		                    (dsl->target.title_contains[0] ? dsl->target.title_contains : dsl->target.class_contains);
		if (!ident || !*ident) {
			res->success = false;
			snprintf(res->error, sizeof(res->error), "focus_window requires target.identifier");
			return 0;
		}
		if (strncmp(ident, "0x", 2) == 0) {
			res->success = focus_window_by_id_str(ident, 1.2);
			snprintf(res->output, sizeof(res->output), "focused window %s", ident);
			return 0;
		}
		jarvis_world_t w;
		memset(&w, 0, sizeof(w));
		query_x11_windows(&w);
		for (int pass = 0; pass < 2; pass++) {
			for (size_t i = 0; i < w.client_count; i++) {
				if (pass == 0 && !w.clients[i].visible) continue;
				if (str_contains_ci(w.clients[i].wm_class, ident) ||
				    str_contains_ci(w.clients[i].title, ident)) {
					res->success = focus_window_by_id_str(w.clients[i].id, 1.2);
					snprintf(res->output, sizeof(res->output), "focused %s: %.40s",
					         w.clients[i].wm_class, w.clients[i].title);
					return 0;
				}
			}
		}
		res->success = false;
		snprintf(res->error, sizeof(res->error), "Window not found: %s", ident);
		return 0;
	}

	/* 3. open_terminal */
	if (strcmp(op, "open_terminal") == 0) {
		op_open_terminal(dsl->target.command[0] ? dsl->target.command : "st", res);
		return 0;
	}

	/* 4. open_application */
	if (strcmp(op, "open_application") == 0) {
		const char *ident = dsl->target.identifier[0] ? dsl->target.identifier :
		                    (dsl->target.command[0] ? dsl->target.command : dsl->args.text);
		if (!ident || !*ident) {
			res->handled = true;
			res->success = false;
			snprintf(res->error, sizeof(res->error), "open_application requires target.identifier");
			return 0;
		}
		const char *resolved = resolve_desktop_app_exec(ident);
		if (strcasecmp(resolved, "terminal") == 0 || strcasecmp(resolved, "st") == 0 || strcasecmp(resolved, "xterm") == 0) {
			op_open_terminal(strcasecmp(resolved, "terminal") == 0 ? "st" : resolved, res);
			return 0;
		}
		spawn_detached_sh(resolved);
		char base_bin[64] = {0};
		snprintf(base_bin, sizeof(base_bin), "%.*s", (int)(sizeof(base_bin) - 1), resolved);
		char *sp = strchr(base_bin, ' ');
		if (sp) *sp = '\0';
		wait_window_focused(base_bin, 1.0);
		res->handled = true;
		res->success = true;
		snprintf(res->output, sizeof(res->output), "launched %s", resolved);
		return 0;
	}

	/* 5. open_url & search_web */
	if (strcmp(op, "open_url") == 0 || strcmp(op, "search_web") == 0) {
		res->handled = true;
		char url[768] = {0};
		if (strcmp(op, "search_web") == 0) {
			const char *q = dsl->target.query[0] ? dsl->target.query : dsl->args.text;
			if (!q || !*q) {
				res->success = false;
				snprintf(res->error, sizeof(res->error), "search_web requires target.query");
				return 0;
			}
			char enc[512];
			url_encode(q, enc, sizeof(enc));
			snprintf(url, sizeof(url), "https://www.google.com/search?q=%s", enc);
		} else {
			if (!dsl->target.url[0]) {
				res->success = false;
				snprintf(res->error, sizeof(res->error), "open_url requires target.url");
				return 0;
			}
			if (strncmp(dsl->target.url, "http://", 7) == 0 || strncmp(dsl->target.url, "https://", 8) == 0) {
				snprintf(url, sizeof(url), "%s", dsl->target.url);
			} else {
				snprintf(url, sizeof(url), "https://%s", dsl->target.url);
			}
		}
		char *const argv[] = { "firefox", "--new-tab", url, NULL };
		spawn_detached_argv(argv);
		focus_firefox_window();
		res->success = true;
		snprintf(res->output, sizeof(res->output), "opened URL: %s", url);
		return 0;
	}

	/* 6. spawn_shell_command */
	if (strcmp(op, "spawn_shell_command") == 0) {
		res->handled = true;
		const char *cmd = dsl->target.command;
		if (!cmd[0]) {
			res->success = false;
			snprintf(res->error, sizeof(res->error), "spawn_shell_command requires target.command");
			return 0;
		}
		jarvis_world_t w;
		memset(&w, 0, sizeof(w));
		query_x11_windows(&w);

		if (strcmp(dsl->target.relation, "other") == 0) {
			if (!w.other.id[0]) {
				res->success = false;
				snprintf(res->error, sizeof(res->error), "No other window to run command in");
				return 0;
			}
			focus_window_by_id_str(w.other.id, 0.8);
			x11_type_text(cmd, true);
			if (w.focused.id[0] && strcmp(w.focused.id, w.other.id) != 0) {
				focus_window_by_id_str(w.focused.id, 0.4);
			}
			res->success = true;
			snprintf(res->output, sizeof(res->output), "ran in other window (%s): %s", w.other.wm_class, cmd);
			return 0;
		}

		bool just_opened_term = (g_last_term_open_ts > 0.0 && (now_sec() - g_last_term_open_ts) < 2.0);
		if (!just_opened_term &&
		    (dsl->target.new_terminal ||
		     !(is_idle_shell_terminal(&w.focused) || is_interactive_remote_session(&w.focused)))) {
			jarvis_adapter_result_t tmp_res;
			memset(&tmp_res, 0, sizeof(tmp_res));
			op_open_terminal("st", &tmp_res);
		}
		g_last_term_open_ts = 0.0;
		x11_type_text(cmd, true);
		res->success = true;
		snprintf(res->output, sizeof(res->output), "ran command: %s", cmd);
		return 0;
	}

	/* 7. type_text, type_and_enter, press_key */
	if (strcmp(op, "type_text") == 0 || strcmp(op, "type_and_enter") == 0 || strcmp(op, "press_key") == 0) {
		res->handled = true;
		jarvis_world_t w;
		memset(&w, 0, sizeof(w));
		bool to_other = (strcmp(dsl->target.relation, "other") == 0);
		if (to_other) {
			query_x11_windows(&w);
			if (!w.other.id[0]) {
				res->success = false;
				snprintf(res->error, sizeof(res->error), "No other window to type into");
				return 0;
			}
			focus_window_by_id_str(w.other.id, 0.8);
		}

		if (strcmp(op, "press_key") == 0) {
			const char *combo = dsl->args.key_combo[0] ? dsl->args.key_combo : "Return";
			x11_press_key_combo(combo);
			usleep(20000);
			snprintf(res->output, sizeof(res->output), "key: %s", combo);
		} else {
			bool enter = (strcmp(op, "type_and_enter") == 0);
			x11_type_text(dsl->args.text, enter);
			snprintf(res->output, sizeof(res->output), "%s: %.40s",
			         enter ? "typed+enter" : "typed", dsl->args.text);
		}

		if (to_other && w.focused.id[0]) {
			focus_window_by_id_str(w.focused.id, 0.4);
		}
		res->success = true;
		return 0;
	}

	/* 8. click & scroll */
	if (strcmp(op, "click") == 0) {
		res->handled = true;
		int x = dsl->target.x, y = dsl->target.y;
		if (x == 0 && y == 0) {
			int wx, wy, ww, wh;
			get_window_geometry(NULL, &wx, &wy, &ww, &wh);
			x = wx + ww / 2;
			y = wy + wh / 2;
		}
		int btn = dsl->args.button > 0 ? dsl->args.button : 1;
		x11_mouse_click(x, y, btn);
		res->success = true;
		snprintf(res->output, sizeof(res->output), "click at (%d,%d) button %d", x, y, btn);
		return 0;
	}

	if (strcmp(op, "scroll") == 0) {
		res->handled = true;
		const char *dir = dsl->args.direction[0] ? dsl->args.direction : "down";
		int amount = dsl->args.amount > 0 ? dsl->args.amount : 5;
		if (amount > 20) amount = 20;
		int btn = 5;
		if (strcasecmp(dir, "up") == 0) btn = 4;
		else if (strcasecmp(dir, "left") == 0) btn = 6;
		else if (strcasecmp(dir, "right") == 0) btn = 7;

		int wx, wy, ww, wh, mx, my;
		get_window_geometry(NULL, &wx, &wy, &ww, &wh);
		get_mouse_location(&mx, &my);
		bool need_move = !(mx >= wx && mx <= wx + ww && my >= wy && my <= wy + wh);
		x11_mouse_scroll(wx + ww / 2, wy + wh / 2, need_move, btn, amount);
		usleep(20000);
		res->success = true;
		snprintf(res->output, sizeof(res->output), "scrolled %s x%d", dir, amount);
		return 0;
	}

	/* 9. workspace_switch, workspace_move_window, window_fullscreen, window_float, window_close */
	if (strcmp(op, "workspace_switch") == 0) {
		res->handled = true;
		int ws = dsl->args.workspace;
		if (ws >= 1 && ws <= 9) {
			Display *dpy = XOpenDisplay(NULL);
			if (dpy) {
				send_dwm_cmd(dpy, DefaultRootWindow(dpy), DWM_CMD_VIEW, (long)(1U << (ws - 1)));
				XCloseDisplay(dpy);
			}
			res->success = true;
			snprintf(res->output, sizeof(res->output), "switched to workspace %d", ws);
		} else {
			res->success = false;
			snprintf(res->error, sizeof(res->error), "workspace must be 1-9");
		}
		return 0;
	}

	if (strcmp(op, "workspace_move_window") == 0) {
		res->handled = true;
		int ws = dsl->args.workspace;
		if (ws < 1 || ws > 9) {
			res->success = false;
			snprintf(res->error, sizeof(res->error), "invalid workspace");
			return 0;
		}
		jarvis_world_t w;
		memset(&w, 0, sizeof(w));
		query_x11_windows(&w);
		const char *target_id = dsl->target.identifier;
		if (!target_id[0] && strcmp(dsl->target.relation, "other") == 0) {
			target_id = w.other.id;
		}
		if (!target_id[0]) {
			target_id = w.focused.id;
		}
		Window win = target_id[0] ? (Window)strtoul(target_id, NULL, 0) : 0;
		if (!win) {
			res->success = false;
			snprintf(res->error, sizeof(res->error), "no target window to move");
			return 0;
		}
		Display *dpy = XOpenDisplay(NULL);
		if (dpy) {
			send_dwm_cmd2(dpy, DefaultRootWindow(dpy), DWM_CMD_TAG_WIN, (long)win, (long)(1U << (ws - 1)));
			XCloseDisplay(dpy);
		}
		res->success = true;
		snprintf(res->output, sizeof(res->output), "moved window %s to workspace %d", target_id, ws);
		return 0;
	}

	if (strcmp(op, "window_fullscreen") == 0 || strcmp(op, "window_float") == 0) {
		res->handled = true;
		Display *dpy = XOpenDisplay(NULL);
		if (dpy) {
			long cmd_op = (strcmp(op, "window_fullscreen") == 0) ? DWM_CMD_TOGGLEFULLSCREEN : DWM_CMD_TOGGLEFLOATING;
			send_dwm_cmd(dpy, DefaultRootWindow(dpy), cmd_op, 0);
			XCloseDisplay(dpy);
		}
		res->success = true;
		snprintf(res->output, sizeof(res->output), "%s",
		         strcmp(op, "window_fullscreen") == 0 ? "toggled fullscreen" : "toggled floating");
		return 0;
	}

	if (strcmp(op, "window_close") == 0) {
		res->handled = true;
		if (strcasecmp(dsl->target.kind, "tab") == 0) {
			x11_press_key_combo("ctrl+w");
			res->success = true;
			snprintf(res->output, sizeof(res->output), "closed browser tab");
			return 0;
		}
		jarvis_world_t w;
		memset(&w, 0, sizeof(w));
		query_x11_windows(&w);
		Display *dpy = XOpenDisplay(NULL);
		if (!dpy) {
			res->success = false;
			return 0;
		}
		Window root = DefaultRootWindow(dpy);
		if (strcmp(dsl->target.relation, "other") == 0 && w.other.id[0]) {
			Window ow = (Window)strtoul(w.other.id, NULL, 0);
			send_dwm_cmd(dpy, root, DWM_CMD_CLOSE_WIN, (long)ow);
			snprintf(res->output, sizeof(res->output), "closed other window (%s)", w.other.wm_class);
			res->success = true;
		} else if (dsl->target.identifier[0]) {
			Window tw = (Window)strtoul(dsl->target.identifier, NULL, 0);
			if (tw) send_dwm_cmd(dpy, root, DWM_CMD_CLOSE_WIN, (long)tw);
			else send_dwm_cmd(dpy, root, DWM_CMD_KILLCLIENT, 0);
			snprintf(res->output, sizeof(res->output), "closed window");
			res->success = true;
		} else {
			send_dwm_cmd(dpy, root, DWM_CMD_KILLCLIENT, 0);
			snprintf(res->output, sizeof(res->output), "closed window");
			res->success = true;
		}
		XCloseDisplay(dpy);
		return 0;
	}

	/* 10. Media & Volume & Recording */
	if (strcmp(op, "media_play") == 0) {
		res->handled = true;
		const char *q = dsl->target.query;
		if (q[0]) {
			jarvis_window_t prev_foc;
			get_focused_window_fast(&prev_foc);
			bool preserve_foc = dsl->args.background || is_terminal_class(prev_foc.wm_class);
			mpris_control("Pause");

			char enc[512], yt_url[768], resolve_cmd[1024], vid_id[64] = {0};
			url_encode(q, enc, sizeof(enc));
			snprintf(
				resolve_cmd, sizeof(resolve_cmd),
				"curl -s -L --max-time 3.5 -A 'Mozilla/5.0 (X11; Linux x86_64; rv:128.0) Gecko/20100101 Firefox/128.0' "
				"'https://www.youtube.com/results?search_query=%s' 2>/dev/null | "
				"grep -o '\"videoId\":\"[a-zA-Z0-9_-]\\{11\\}\"' | head -n 1 | cut -d'\"' -f4",
				enc
			);
			run_cmd_capture(resolve_cmd, vid_id, sizeof(vid_id));
			if (strlen(vid_id) == 11) {
				snprintf(yt_url, sizeof(yt_url), "https://www.youtube.com/watch?v=%s&autoplay=1", vid_id);
			} else {
				snprintf(yt_url, sizeof(yt_url), "https://www.youtube.com/results?search_query=%s", enc);
			}
			char *const argv[] = { "firefox", "--new-tab", yt_url, NULL };
			spawn_detached_argv(argv);
			if (!preserve_foc) {
				focus_firefox_window();
			} else if (prev_foc.id[0]) {
				focus_window_by_id_str(prev_foc.id, 0.5);
			}
			res->success = true;
			snprintf(res->output, sizeof(res->output), "playing: %s", q);
			return 0;
		}
		if (!mpris_control("Play")) {
			x11_press_key_combo("XF86AudioPlay");
		}
		res->success = true;
		snprintf(res->output, sizeof(res->output), "resumed media");
		return 0;
	}

	if (strcmp(op, "media_pause") == 0) {
		res->handled = true;
		if (!mpris_control("Pause")) {
			x11_press_key_combo("XF86AudioPlay");
		}
		res->success = true;
		snprintf(res->output, sizeof(res->output), "paused media");
		return 0;
	}

	if (strcmp(op, "media_next") == 0 || strcmp(op, "media_prev") == 0) {
		res->handled = true;
		bool is_next = (strcmp(op, "media_next") == 0);
		mpris_control(is_next ? "Next" : "Previous");
		res->success = true;
		snprintf(res->output, sizeof(res->output), "%s track", is_next ? "next" : "previous");
		return 0;
	}

	if (strcmp(op, "media_volume") == 0) {
		res->handled = true;
		const char *act = dsl->args.action[0] ? dsl->args.action : "volume_up";
		const char *sub = "vol-up";
		if (strcmp(act, "volume_down") == 0) sub = "vol-down";
		else if (strcmp(act, "volume_mute") == 0 || strcmp(act, "mute") == 0) sub = "vol-mute";
		char *const argv[] = { "dwm-ctl", (char *)sub, NULL };
		spawn_detached_argv(argv);
		res->success = true;
		snprintf(res->output, sizeof(res->output), "volume: %s", act);
		return 0;
	}

	if (strcmp(op, "record_start") == 0 || strcmp(op, "record_stop") == 0) {
		res->handled = true;
		char cmd[256];
		if (strcmp(op, "record_start") == 0) {
			const char *mode = dsl->args.mode[0] ? dsl->args.mode : "both";
			snprintf(cmd, sizeof(cmd), "dwm-rec start %s 2>/dev/null", mode);
		} else {
			snprintf(cmd, sizeof(cmd), "dwm-rec stop 2>/dev/null");
		}
		run_cmd_capture(cmd, res->output, sizeof(res->output));
		res->success = true;
		return 0;
	}

	/* 11. wait, wm_help, ask_agent, tts_interrupt, answer_question */
	if (strcmp(op, "wait") == 0) {
		res->handled = true;
		double sec = dsl->args.seconds > 0.0 ? dsl->args.seconds : 1.0;
		if (sec > 10.0) sec = 10.0;
		usleep((useconds_t)(sec * 1e6));
		res->success = true;
		snprintf(res->output, sizeof(res->output), "waited %.1fs", sec);
		return 0;
	}

	if (strcmp(op, "wm_help") == 0) {
		res->handled = true;
		const char *q = dsl->target.query[0] ? dsl->target.query : dsl->args.text;
		if (!q[0]) {
			char *const argv[] = { "dwm-help", "show", NULL };
			spawn_detached_argv(argv);
			snprintf(res->output, sizeof(res->output), "opened dwm-help");
		} else {
			char cmd[640];
			snprintf(cmd, sizeof(cmd), "dwm-help ask \"%s\" 2>/dev/null", q);
			run_cmd_capture(cmd, res->output, sizeof(res->output));
		}
		res->success = true;
		return 0;
	}

	if (strcmp(op, "ask_agent") == 0) {
		res->handled = true;
		const char *q = dsl->target.query[0] ? dsl->target.query : dsl->args.text;
		if (!q[0]) {
			res->success = false;
			snprintf(res->error, sizeof(res->error), "ask_agent requires target.query");
			return 0;
		}
		char *const argv[] = { "dwm-jarvis-pi", "send", (char *)q, NULL };
		spawn_detached_argv(argv);
		res->success = true;
		snprintf(res->output, sizeof(res->output), "sent to Pi agent: %s", q);
		return 0;
	}

	if (strcmp(op, "tts_interrupt") == 0) {
		res->handled = true;
		res->success = true;
		snprintf(res->output, sizeof(res->output), "stopped speaking");
		return 0;
	}

	if (strcmp(op, "tts_control") == 0) {
		res->handled = true;
		const char *act = dsl->args.action[0] ? dsl->args.action : "on";
		bool turn_on = (strcasecmp(act, "off") != 0 && strcasecmp(act, "0") != 0 && strcasecmp(act, "disable") != 0);
		const char *home = getenv("HOME");
		if (home && *home) {
			char dir[512], path[512];
			snprintf(dir, sizeof(dir), "%s/.config/dwm-voice", home);
			mkdir(dir, 0755);
			snprintf(path, sizeof(path), "%s/tts_enabled", dir);
			FILE *fp = fopen(path, "w");
			if (fp) {
				fprintf(fp, "%s\n", turn_on ? "1" : "0");
				fclose(fp);
			}
		}
		res->success = true;
		snprintf(res->output, sizeof(res->output), "%s",
		         turn_on ? "Voice enabled. At your service." : "Voice disabled.");
		return 0;
	}

	if (strcmp(op, "answer_question") == 0) {
		res->handled = true;
		const char *ans = dsl->args.answer[0] ? dsl->args.answer :
		                  (dsl->args.text[0] ? dsl->args.text : dsl->target.query);
		if (!ans[0]) {
			res->success = false;
			snprintf(res->error, sizeof(res->error), "answer_question requires args.answer");
			return 0;
		}
		char *const argv[] = {
			"notify-send", "-t", "8000", "-h", "string:x-dunst-stack-tag:jarvis-answer",
			"  Jarvis", (char *)ans, NULL
		};
		spawn_detached_argv(argv);
		res->success = true;
		snprintf(res->output, sizeof(res->output), "%s", ans);
		return 0;
	}

	/* 12. ReAct Agent Inspection Tools: inspect_terminal, inspect_browser, inspect_clipboard, inspect_system, run_background_command */
	if (strcmp(op, "inspect_terminal") == 0) {
		res->handled = true;
		jarvis_world_t w;
		memset(&w, 0, sizeof(w));
		query_x11_windows(&w);
		const jarvis_window_t *win = (strcmp(dsl->target.relation, "other") == 0) ? &w.other : &w.focused;
		if (!is_terminal_class(win->wm_class) && is_terminal_class(w.other.wm_class)) {
			win = &w.other;
		}
		resolve_window_cwd_and_fg(win, w.focused_cwd, sizeof(w.focused_cwd),
		                          w.focused_process, sizeof(w.focused_process));
		char raw_tail[1000] = {0};
		if (!get_st_screen_text(win->id, 40, raw_tail, sizeof(raw_tail))) {
			populate_tmux_panes(&w, false, 40);
			if (w.pane_count > 0) {
				snprintf(raw_tail, sizeof(raw_tail), "%.999s", w.panes[0].tail);
			}
		}
		char esc_title[180] = {0}, esc_cwd[260] = {0}, esc_fg[200] = {0}, esc_tail[1100] = {0};
		json_escape_into(win->title, esc_title, sizeof(esc_title));
		json_escape_into(w.focused_cwd, esc_cwd, sizeof(esc_cwd));
		json_escape_into(w.focused_process, esc_fg, sizeof(esc_fg));
		json_escape_into(raw_tail, esc_tail, sizeof(esc_tail));
		snprintf(
			res->output, sizeof(res->output),
			"{\"window\":{\"id\":\"%s\",\"class\":\"%s\",\"title\":\"%s\"},"
			"\"cwd\":\"%s\",\"foreground_process\":\"%s\","
			"\"pane_tail\":\"%s\"}",
			win->id, win->wm_class, esc_title,
			esc_cwd, esc_fg, esc_tail
		);
		res->success = true;
		return 0;
	}

	if (strcmp(op, "inspect_browser") == 0) {
		res->handled = true;
		jarvis_world_t w;
		memset(&w, 0, sizeof(w));
		query_x11_windows(&w);
		populate_firefox_tabs(&w);
		inspect_browser_a11y(&w, w.browser_page, sizeof(w.browser_page));

		char esc_a11y[1400] = {0};
		json_escape_into(w.browser_page, esc_a11y, sizeof(esc_a11y));

		char tabs_buf[512] = {0};
		size_t toff = 0;
		for (size_t i = 0; i < w.tab_count && toff + 80 < sizeof(tabs_buf); i++) {
			char et[96] = {0}, eu[160] = {0};
			json_escape_into(w.tabs[i].title, et, sizeof(et));
			json_escape_into(w.tabs[i].url, eu, sizeof(eu));
			toff += (size_t)snprintf(tabs_buf + toff, sizeof(tabs_buf) - toff,
			                         "%s{\"index\":%d,\"title\":\"%s\",\"url\":\"%s\"}",
			                         i > 0 ? "," : "", w.tabs[i].index, et, eu);
		}

		snprintf(res->output, sizeof(res->output),
		         "{\"accessibility_tree\":\"%s\",\"open_tabs\":[%s]}",
		         esc_a11y, tabs_buf);
		res->success = true;
		return 0;
	}

	if (strcmp(op, "inspect_clipboard") == 0) {
		res->handled = true;
		char prim[768] = {0}, clip[768] = {0};
		run_cmd_capture("xclip -o -selection primary 2>/dev/null || xsel -op 2>/dev/null", prim, sizeof(prim));
		run_cmd_capture("xclip -o -selection clipboard 2>/dev/null || xsel -ob 2>/dev/null", clip, sizeof(clip));
		snprintf(res->output, sizeof(res->output),
		         "{\"primary_selection\":\"%.700s\",\"clipboard\":\"%.700s\"}", prim, clip);
		res->success = true;
		return 0;
	}

	if (strcmp(op, "inspect_system") == 0) {
		res->handled = true;
		const char *aspect = dsl->args.aspect[0] ? dsl->args.aspect :
		                     (dsl->target.query[0] ? dsl->target.query : "all");
		char health[256] = {0}, top_cpu[768] = {0}, top_mem[768] = {0}, disk[512] = {0}, ports[768] = {0};
		populate_fast_system_health(health, sizeof(health));
		if (strcasecmp(aspect, "all") == 0 || strcasecmp(aspect, "memory") == 0 ||
		    strcasecmp(aspect, "ram") == 0 || strcasecmp(aspect, "processes") == 0 ||
		    strcasecmp(aspect, "cpu") == 0) {
			run_cmd_capture("ps -eo pid,comm,%cpu,%mem --sort=-%cpu | head -n 7", top_cpu, sizeof(top_cpu));
			run_cmd_capture("ps -eo pid,comm,%cpu,%mem --sort=-%mem | head -n 7", top_mem, sizeof(top_mem));
		}
		if (strcasecmp(aspect, "all") == 0 || strcasecmp(aspect, "disk") == 0 || strcasecmp(aspect, "storage") == 0) {
			struct statvfs vfs_root;
			if (statvfs("/", &vfs_root) == 0 && vfs_root.f_blocks > 0) {
				double tot_gb = (double)(vfs_root.f_blocks * vfs_root.f_frsize) / (1024.0 * 1024.0 * 1024.0);
				double avail_gb = (double)(vfs_root.f_bavail * vfs_root.f_frsize) / (1024.0 * 1024.0 * 1024.0);
				double used_gb = tot_gb - avail_gb;
				snprintf(disk, sizeof(disk), "/: %.1fG used / %.1fG total (%.1fG free)", used_gb, tot_gb, avail_gb);
			}
		}
		if (strcasecmp(aspect, "all") == 0 || strcasecmp(aspect, "ports") == 0 || strcasecmp(aspect, "network") == 0) {
			run_cmd_capture("ss -tulpn 2>/dev/null | head -n 15", ports, sizeof(ports));
		}
		snprintf(res->output, sizeof(res->output),
		         "{\"health\":\"%.200s\",\"top_cpu\":\"%.400s\",\"top_mem\":\"%.400s\",\"disk\":\"%.320s\",\"listening_ports\":\"%.450s\"}",
		         health, top_cpu, top_mem, disk, ports);
		res->success = true;
		return 0;
	}

	if (strcmp(op, "run_background_command") == 0 && dsl->target.command[0]) {
		res->handled = true;
		jarvis_world_t w;
		memset(&w, 0, sizeof(w));
		query_x11_windows(&w);
		resolve_window_cwd_and_fg(&w.focused, w.focused_cwd, sizeof(w.focused_cwd),
		                          w.focused_process, sizeof(w.focused_process));
		char full_cmd[1024];
		if (w.focused_cwd[0]) {
			snprintf(full_cmd, sizeof(full_cmd), "cd \"%s\" 2>/dev/null && (%s) 2>&1",
			         w.focused_cwd, dsl->target.command);
		} else {
			snprintf(full_cmd, sizeof(full_cmd), "(%s) 2>&1", dsl->target.command);
		}
		run_cmd_capture(full_cmd, res->output, sizeof(res->output));
		res->success = true;
		return 0;
	}

	return 0;
}

/* ---------------------------------------------------------------------------
 * Xylem Hook 3: jarvis_xy_confirm (Destructive Command dmenu Guardrail)
 * ------------------------------------------------------------------------- */

XY_IMPL(int, jarvis_xy_confirm, const char *, description)
{
	const char *auto_c = getenv("JARVIS_AUTO_CONFIRM");
	if ((auto_c && strcmp(auto_c, "1") == 0) || !getenv("DISPLAY")) {
		return 1;
	}
	const char *yes_lbl = getenv("JARVIS_CONFIRM_YES");
	const char *no_lbl = getenv("JARVIS_CONFIRM_NO");
	if (!yes_lbl || !*yes_lbl) yes_lbl = "1: OK";
	if (!no_lbl || !*no_lbl) no_lbl = "0: Cancel";
	char cmd[512], out[64];
	snprintf(cmd, sizeof(cmd),
		"printf '%s\\n%s\\n' | dmenu -i -l 2 -p ' (%.42s):' "
		"-fn 'Ubuntu Sans:style=Medium:size=12:antialias=true:autohint=true' "
		"-nb '#1e1e2e' -nf '#cdd6f4' -sb '#f38ba8' -sf '#11111b' 2>/dev/null",
		yes_lbl, no_lbl, description ? description : "...");
	run_cmd_capture(cmd, out, sizeof(out));
	return (out[0] && strncasecmp(out, yes_lbl, strlen(yes_lbl)) == 0) ? 1 : 0;
}

/* ---------------------------------------------------------------------------
 * Xylem Hook 4: jarvis_xy_voice_status (dwm-bar robot indicator state)
 * ------------------------------------------------------------------------- */

static void refresh_dwm_bar(const char *user)
{
	char bar_pid_file[128];
	snprintf(bar_pid_file, sizeof(bar_pid_file), "/tmp/dwm-bar-%s.pid", user);
	FILE *fp = fopen(bar_pid_file, "r");
	if (fp) {
		long pid = 0;
		if (fscanf(fp, "%ld", &pid) == 1 && pid > 1) {
			kill((pid_t)pid, SIGUSR1);
		}
		fclose(fp);
	}
}

XY_IMPL(int, jarvis_xy_voice_status, int, state, const char *, partial_text)
{
	(void)partial_text;
	const char *user = getenv("USER");
	if (!user || !*user) user = "quirinpa";
	char acting_file[128];
	snprintf(acting_file, sizeof(acting_file), "/tmp/dwm-acting-%s.pid", user);

	if (state == JARVIS_VOICE_ACTING) {
		FILE *fp = fopen(acting_file, "w");
		if (fp) {
			fprintf(fp, "%d\n", (int)getpid());
			fclose(fp);
		}
	} else {
		unlink(acting_file);
	}
	refresh_dwm_bar(user);
	return 0;
}

/* ---------------------------------------------------------------------------
 * Xylem Hook 7: jarvis_xy_prompt (dmenu Semantic Command Bar)
 * ------------------------------------------------------------------------- */

XY_IMPL(int, jarvis_xy_prompt, const char *, prompt_label, const char *, candidates_lines, char *, out_selection, size_t, out_sz)
{
	if (!out_selection || out_sz == 0) return 0;
	out_selection[0] = '\0';
	if (!getenv("DISPLAY")) return 0;

	const char *user = getenv("USER");
	if (!user || !*user) user = "quirinpa";
	char tmp_in[128];
	snprintf(tmp_in, sizeof(tmp_in), "/tmp/jarvis-dmenu-%s.in", user);
	FILE *fp = fopen(tmp_in, "w");
	if (fp) {
		if (candidates_lines && *candidates_lines) {
			fputs(candidates_lines, fp);
			fputc('\n', fp);
		}
		fclose(fp);
	}

	char cmd[512];
	snprintf(cmd, sizeof(cmd),
		"dmenu -i -l 12 -p '%s' "
		"-fn 'Ubuntu Sans:style=Medium:size=12:antialias=true:autohint=true' "
		"-nb '#11111b' -nf '#a6adc8' -sb '#7c6ff2' -sf '#ffffff' < '%s' 2>/dev/null",
		(prompt_label && *prompt_label) ? prompt_label : " Jarvis:",
		tmp_in);
	run_cmd_capture(cmd, out_selection, out_sz);
	unlink(tmp_in);
	return out_selection[0] ? 1 : 0;
}

/* ---------------------------------------------------------------------------
 * Xylem Hook 8: jarvis_xy_notify (Dunst HUD Toast & Earcon Chime)
 * ------------------------------------------------------------------------- */

XY_IMPL(int, jarvis_xy_notify, const char *, title, const char *, body, int, play_earcon)
{
	if (title && *title) {
		char *const argv[] = {
			"notify-send", "-t", "3500", "-h", "string:x-dunst-stack-tag:voice",
			(char *)title, (char *)(body ? body : ""), NULL
		};
		spawn_detached_argv(argv);
	}
	if (play_earcon) {
		const char *user = getenv("USER");
		if (!user || !*user) user = "quirinpa";
		char wav_path[128];
		snprintf(wav_path, sizeof(wav_path), "/tmp/dwm-voice-done-%s.wav", user);
		if (access(wav_path, R_OK) == 0) {
			char *const argv[] = { "pw-play", "--volume=0.30", wav_path, NULL };
			spawn_detached_argv(argv);
		}
	}
	return 0;
}
