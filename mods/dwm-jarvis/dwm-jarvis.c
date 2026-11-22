/**
 * @file dwm-jarvis.c
 * @brief dwm-specific libxylem adapter plugin for libjarvis.
 *
 * Owns ONLY dwm-specific behaviors:
 * - X11 + _DWM_CLIENT_TAGS / _DWM_CUR_TAG observation & _ST_SCREEN_TEXT panes
 * - _DWM_CMD workspace switching, window moving/fullscreen/float/focus/close
 * - st / xterm terminal spawning & /proc idle-shell settle
 * - dwm-ctl shortcuts & volume refresh, dwm-rec, dwm-help, dwm-bar voice status, dmenu, dunst
 *
 * Generic desktop/X11/Firefox logic lives in libjarvis, jarvis-x11.so, and jarvis-firefox.so.
 */

#include "../../dwm-types.h"
#include "../dwm_ctl.h"
#include <ttypt/jarvis-types.h>
#include <ttypt/jarvis-desktop.h>
#include <ttypt/xy.h>
#include <ttypt/xy-mod.h>

#include <ctype.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

XY_MODULE_API void xy_install(void)
{
}


static void fill_jarvis_win(jarvis_window_t *dst, const dwm_win_info_t *src)
{
	if (!dst || !src) return;
	snprintf(dst->id, sizeof(dst->id), "%.*s", (int)(sizeof(dst->id) - 1), src->id);
	snprintf(dst->wm_class, sizeof(dst->wm_class), "%.*s", (int)(sizeof(dst->wm_class) - 1), src->wm_class);
	snprintf(dst->title, sizeof(dst->title), "%.*s", (int)(sizeof(dst->title) - 1), src->title);
	dst->tag = src->tag;
	dst->visible = src->visible;
	dst->focused = src->focused;
}

static const dwm_win_info_t *find_dwm_win(const dwm_win_info_t *clients, size_t n, const char *id)
{
	if (!id || !*id) return NULL;
	for (size_t i = 0; i < n; i++) {
		if (strcmp(clients[i].id, id) == 0) return &clients[i];
	}
	return NULL;
}

static void populate_st_screen_panes(jarvis_world_t *w, const dwm_win_info_t *clients, size_t n, int lines)
{
	if (!w) return;
	if (w->pane_count == 0) {
		jarvis_desktop_populate_tmux_panes(w, true, lines > 0 ? lines : 20);
	}
	const jarvis_window_t *cands[2] = { &w->focused, &w->other };
	for (int ci = 0; ci < 2 && w->pane_count < JARVIS_MAX_PANES; ci++) {
		const jarvis_window_t *win = cands[ci];
		if (!win->id[0] || !dwm_ctl_is_terminal_class(win->wm_class)) continue;
		if (ci == 1 && strcmp(win->id, w->focused.id) == 0) continue;

		char st_text[JARVIS_TEXT_LEN];
		if (dwm_ctl_get_st_screen_text(win->id, lines > 0 ? lines : 30, st_text, sizeof(st_text))) {
			if (w->pane_count > 0 && strstr(w->panes[0].tail, st_text)) continue;

			jarvis_pane_t st_pane;
			memset(&st_pane, 0, sizeof(st_pane));
			snprintf(st_pane.pane, sizeof(st_pane.pane), "st:%.58s", win->id);
			const dwm_win_info_t *dwin = find_dwm_win(clients, n, win->id);
			if (dwin && dwin->pid > 0) {
				jarvis_desktop_resolve_pid_cwd_fg(dwin->pid,
					st_pane.cwd, sizeof(st_pane.cwd),
					st_pane.command, sizeof(st_pane.command));
			}
			st_pane.active = (ci == 0);
			snprintf(st_pane.tail, sizeof(st_pane.tail), "%.*s", (int)(sizeof(st_pane.tail) - 1), st_text);

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

XY_IMPL(int, jarvis_xy_observe, jarvis_world_t *, w)
{
	if (!w) return 0;
	snprintf(w->wm, sizeof(w->wm), "dwm");

	dwm_win_info_t dclients[JARVIS_MAX_CLIENTS];
	dwm_win_info_t dfoc, doth;
	size_t dn = dwm_ctl_query_windows(dclients, JARVIS_MAX_CLIENTS, &dfoc, &doth);

	if (w->client_count == 0 && dn > 0) {
		fill_jarvis_win(&w->focused, &dfoc);
		fill_jarvis_win(&w->other, &doth);
		for (size_t i = 0; i < dn && w->client_count < JARVIS_MAX_CLIENTS; i++) {
			fill_jarvis_win(&w->clients[w->client_count++], &dclients[i]);
		}
	}

	if (!w->focused_cwd[0]) {
		const dwm_win_info_t *term_win = &dfoc;
		if (!dwm_ctl_is_terminal_class(term_win->wm_class) &&
		    dwm_ctl_is_terminal_class(doth.wm_class)) {
			term_win = &doth;
		}
		if (term_win->pid > 0) {
			jarvis_desktop_resolve_pid_cwd_fg(term_win->pid,
				w->focused_cwd, sizeof(w->focused_cwd),
				w->focused_process, sizeof(w->focused_process));
		}
	}

	populate_st_screen_panes(w, dclients, dn, 25);
	return 0;
}

static bool inject_type_and_enter(const char *cmd, char *err_buf, size_t err_sz);

XY_IMPL(int, jarvis_xy_execute, const jarvis_dsl_t *, dsl, jarvis_adapter_result_t *, res)
{
	if (!dsl || !res || res->handled) return 0;
	const char *op = dsl->op;

	/* 1. focus_relative_window (or focus_window with relative relation) */
	if (strcmp(op, "focus_relative_window") == 0 ||
	    (strcmp(op, "focus_window") == 0 &&
	     (strcmp(dsl->target.relation, "other") == 0 ||
	      strcmp(dsl->target.relation, "previous") == 0 ||
	      strcmp(dsl->target.relation, "next") == 0 ||
	      strcmp(dsl->target.relation, "master") == 0))) {
		const char *rel = dsl->target.relation[0] ? dsl->target.relation : "other";
		res->handled = true;
		if (strcmp(rel, "other") == 0) {
			dwm_win_info_t dclients[JARVIS_MAX_CLIENTS];
			dwm_win_info_t dfoc, doth;
			dwm_ctl_query_windows(dclients, JARVIS_MAX_CLIENTS, &dfoc, &doth);
			if (doth.id[0]) {
				res->success = dwm_ctl_focus_win(doth.id, 1.0);
				snprintf(res->output, sizeof(res->output), "focused other window: %s", doth.wm_class);
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
			dwm_ctl_send(dpy, root, DWM_CMD_FOCUSSTACK, -1);
			snprintf(res->output, sizeof(res->output), "focused previous window");
			res->success = true;
		} else if (strcmp(rel, "next") == 0) {
			dwm_ctl_send(dpy, root, DWM_CMD_FOCUSSTACK, 1);
			snprintf(res->output, sizeof(res->output), "focused next window");
			res->success = true;
		} else if (strcmp(rel, "master") == 0) {
			dwm_ctl_send(dpy, root, DWM_CMD_ZOOM, 0);
			snprintf(res->output, sizeof(res->output), "swapped with master");
			res->success = true;
		} else {
			snprintf(res->error, sizeof(res->error), "Unknown relation: %s", rel);
			res->success = false;
		}
		XCloseDisplay(dpy);
		return 0;
	}

	/* 2. focus_window via DWM_CMD_FOCUS_WIN (handles tag switching automatically) */
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
			res->success = dwm_ctl_focus_win(ident, 1.2);
			snprintf(res->output, sizeof(res->output), "focused window %s", ident);
			return 0;
		}
		dwm_win_info_t dclients[JARVIS_MAX_CLIENTS];
		dwm_win_info_t dfoc, doth;
		size_t dn = dwm_ctl_query_windows(dclients, JARVIS_MAX_CLIENTS, &dfoc, &doth);
		for (int pass = 0; pass < 2; pass++) {
			for (size_t i = 0; i < dn; i++) {
				if (pass == 0 && !dclients[i].visible) continue;
				if (dwm_ctl_str_ci(dclients[i].wm_class, ident) ||
				    dwm_ctl_str_ci(dclients[i].title, ident)) {
					res->success = dwm_ctl_focus_win(dclients[i].id, 1.2);
					snprintf(res->output, sizeof(res->output), "focused %s: %.40s",
					         dclients[i].wm_class, dclients[i].title);
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
		res->handled = true;
		res->success = dwm_ctl_open_terminal(
			dsl->target.command[0] ? dsl->target.command : "st",
			res->output, sizeof(res->output), res->error, sizeof(res->error));
		return 0;
	}

	/* 4. open_application: dwm-specific shortcuts + terminal (other apps fall through to jarvis-desktop) */
	if (strcmp(op, "open_application") == 0) {
		const char *ident = dsl->target.identifier[0] ? dsl->target.identifier :
		                    (dsl->target.command[0] ? dsl->target.command : dsl->args.text);
		if (!ident || !*ident) return 0;

		const char *shortcut = dwm_ctl_shortcut_exec(ident);
		if (shortcut) {
			res->handled = true;
			dwm_ctl_spawn_sh(shortcut);
			res->success = true;
			snprintf(res->output, sizeof(res->output), "launched %s", shortcut);
			return 0;
		}
		if (strcasecmp(ident, "terminal") == 0 || strcasecmp(ident, "st") == 0 || strcasecmp(ident, "xterm") == 0) {
			res->handled = true;
			res->success = dwm_ctl_open_terminal(
				strcasecmp(ident, "terminal") == 0 ? "st" : ident,
				res->output, sizeof(res->output), res->error, sizeof(res->error));
			return 0;
		}
		return 0;
	}

	/* 5. spawn_shell_command (opens/settles st or focuses target, then types via jarvis-x11) */
	if (strcmp(op, "spawn_shell_command") == 0) {
		res->handled = true;
		const char *cmd = dsl->target.command;
		if (!cmd[0]) {
			res->success = false;
			snprintf(res->error, sizeof(res->error), "spawn_shell_command requires target.command");
			return 0;
		}
		dwm_win_info_t dclients[JARVIS_MAX_CLIENTS];
		dwm_win_info_t dfoc, doth;
		dwm_ctl_query_windows(dclients, JARVIS_MAX_CLIENTS, &dfoc, &doth);

		if (strcmp(dsl->target.relation, "other") == 0) {
			if (!doth.id[0]) {
				res->success = false;
				snprintf(res->error, sizeof(res->error), "No other window to run command in");
				return 0;
			}
			dwm_ctl_focus_win(doth.id, 0.8);
			bool ok = inject_type_and_enter(cmd, res->error, sizeof(res->error));
			if (dfoc.id[0] && strcmp(dfoc.id, doth.id) != 0) {
				dwm_ctl_focus_win(dfoc.id, 0.4);
			}
			res->success = ok;
			if (ok) {
				snprintf(res->output, sizeof(res->output), "ran in other window (%s): %s", doth.wm_class, cmd);
			}
			return 0;
		}

		bool just_opened_term = (g_dwm_last_term_open_ts > 0.0 && (dwm_ctl_now_sec() - g_dwm_last_term_open_ts) < 2.0);
		if (!just_opened_term &&
		    (dsl->target.new_terminal ||
		     !(dwm_ctl_is_idle_shell_win(&dfoc) || dwm_ctl_is_interactive_remote(&dfoc)))) {
			char omsg[256] = {0}, oerr[256] = {0};
			if (!dwm_ctl_open_terminal("st", omsg, sizeof(omsg), oerr, sizeof(oerr))) {
				g_dwm_last_term_open_ts = 0.0;
				res->success = false;
				snprintf(res->error, sizeof(res->error), "%s", oerr[0] ? oerr : "Failed to open terminal");
				return 0;
			}
		}
		g_dwm_last_term_open_ts = 0.0;
		res->success = inject_type_and_enter(cmd, res->error, sizeof(res->error));
		if (res->success) {
			snprintf(res->output, sizeof(res->output), "ran command: %s", cmd);
		}
		return 0;
	}

	/* 6. workspace_switch, workspace_move_window, window_fullscreen, window_float, window_close */
	if (strcmp(op, "workspace_switch") == 0) {
		res->handled = true;
		int ws = dsl->args.workspace;
		if (ws >= 1 && ws <= 9) {
			Display *dpy = XOpenDisplay(NULL);
			if (dpy) {
				dwm_ctl_send(dpy, DefaultRootWindow(dpy), DWM_CMD_VIEW, (long)(1U << (ws - 1)));
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
		dwm_win_info_t dclients[JARVIS_MAX_CLIENTS];
		dwm_win_info_t dfoc, doth;
		dwm_ctl_query_windows(dclients, JARVIS_MAX_CLIENTS, &dfoc, &doth);
		const char *target_id = dsl->target.identifier;
		if (!target_id[0] && strcmp(dsl->target.relation, "other") == 0) target_id = doth.id;
		if (!target_id[0]) target_id = dfoc.id;
		Window win = target_id[0] ? (Window)strtoul(target_id, NULL, 0) : 0;
		if (!win) {
			res->success = false;
			snprintf(res->error, sizeof(res->error), "no target window to move");
			return 0;
		}
		Display *dpy = XOpenDisplay(NULL);
		if (dpy) {
			dwm_ctl_send2(dpy, DefaultRootWindow(dpy), DWM_CMD_TAG_WIN, (long)win, (long)(1U << (ws - 1)));
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
			dwm_ctl_send(dpy, DefaultRootWindow(dpy), cmd_op, 0);
			XCloseDisplay(dpy);
		}
		res->success = true;
		snprintf(res->output, sizeof(res->output), "%s",
		         strcmp(op, "window_fullscreen") == 0 ? "toggled fullscreen" : "toggled floating");
		return 0;
	}

	if (strcmp(op, "window_close") == 0) {
		/* Leave tab-close to jarvis-x11 (Ctrl+W) */
		if (strcasecmp(dsl->target.kind, "tab") == 0) {
			return 0;
		}
		res->handled = true;
		dwm_win_info_t dclients[JARVIS_MAX_CLIENTS];
		dwm_win_info_t dfoc, doth;
		dwm_ctl_query_windows(dclients, JARVIS_MAX_CLIENTS, &dfoc, &doth);
		Display *dpy = XOpenDisplay(NULL);
		if (!dpy) {
			res->success = false;
			snprintf(res->error, sizeof(res->error), "Cannot open X11 display");
			return 0;
		}
		Window root = DefaultRootWindow(dpy);
		if (strcmp(dsl->target.relation, "other") == 0 && doth.id[0]) {
			Window ow = (Window)strtoul(doth.id, NULL, 0);
			dwm_ctl_send(dpy, root, DWM_CMD_CLOSE_WIN, (long)ow);
			snprintf(res->output, sizeof(res->output), "closed other window (%s)", doth.wm_class);
			res->success = true;
		} else if (dsl->target.identifier[0]) {
			Window tw = (Window)strtoul(dsl->target.identifier, NULL, 0);
			if (tw) dwm_ctl_send(dpy, root, DWM_CMD_CLOSE_WIN, (long)tw);
			else dwm_ctl_send(dpy, root, DWM_CMD_KILLCLIENT, 0);
			snprintf(res->output, sizeof(res->output), "closed window");
			res->success = true;
		} else {
			dwm_ctl_send(dpy, root, DWM_CMD_KILLCLIENT, 0);
			snprintf(res->output, sizeof(res->output), "closed window");
			res->success = true;
		}
		XCloseDisplay(dpy);
		return 0;
	}

	/* 7. dwm volume bar integration, dwm-rec, dwm-help */
	if (strcmp(op, "media_volume") == 0) {
		res->handled = true;
		const char *act = dsl->args.action[0] ? dsl->args.action : "volume_up";
		const char *sub = "vol-up";
		if (strcmp(act, "volume_down") == 0) sub = "vol-down";
		else if (strcmp(act, "volume_mute") == 0 || strcmp(act, "mute") == 0) sub = "vol-mute";
		char *const argv[] = { "dwm-ctl", (char *)sub, NULL };
		dwm_ctl_spawn_argv(argv);
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
		dwm_ctl_run_capture(cmd, res->output, sizeof(res->output));
		res->success = true;
		return 0;
	}

	if (strcmp(op, "wm_help") == 0) {
		res->handled = true;
		const char *q = dsl->target.query[0] ? dsl->target.query : dsl->args.text;
		if (!q[0]) {
			char *const argv[] = { "dwm-help", "show", NULL };
			dwm_ctl_spawn_argv(argv);
			snprintf(res->output, sizeof(res->output), "opened dwm-help");
		} else {
			char cmd[640];
			snprintf(cmd, sizeof(cmd), "dwm-help ask \"%s\" 2>/dev/null", q);
			dwm_ctl_run_capture(cmd, res->output, sizeof(res->output));
		}
		res->success = true;
		return 0;
	}

	/* 8. inspect_terminal (_ST_SCREEN_TEXT + /proc + tmux fallback) */
	if (strcmp(op, "inspect_terminal") == 0) {
		res->handled = true;
		dwm_win_info_t dclients[JARVIS_MAX_CLIENTS];
		dwm_win_info_t dfoc, doth;
		dwm_ctl_query_windows(dclients, JARVIS_MAX_CLIENTS, &dfoc, &doth);
		const dwm_win_info_t *win = (strcmp(dsl->target.relation, "other") == 0) ? &doth : &dfoc;
		if (!dwm_ctl_is_terminal_class(win->wm_class) && dwm_ctl_is_terminal_class(doth.wm_class)) {
			win = &doth;
		}
		char cwd[256] = {0}, fg[192] = {0};
		if (win->pid > 0) {
			jarvis_desktop_resolve_pid_cwd_fg(win->pid, cwd, sizeof(cwd), fg, sizeof(fg));
		}
		char raw_tail[1000] = {0};
		if (!dwm_ctl_get_st_screen_text(win->id, 40, raw_tail, sizeof(raw_tail))) {
			jarvis_world_t tw;
			memset(&tw, 0, sizeof(tw));
			jarvis_desktop_populate_tmux_panes(&tw, false, 40);
			if (tw.pane_count > 0) {
				snprintf(raw_tail, sizeof(raw_tail), "%.999s", tw.panes[0].tail);
			}
		}
		char esc_title[180] = {0}, esc_cwd[260] = {0}, esc_fg[200] = {0}, esc_tail[1100] = {0};
		jarvis_desktop_json_escape(win->title, esc_title, sizeof(esc_title));
		jarvis_desktop_json_escape(cwd, esc_cwd, sizeof(esc_cwd));
		jarvis_desktop_json_escape(fg, esc_fg, sizeof(esc_fg));
		jarvis_desktop_json_escape(raw_tail, esc_tail, sizeof(esc_tail));
		snprintf(
			res->output, sizeof(res->output),
			"{\"window\":{\"id\":\"%s\",\"class\":\"%s\",\"title\":\"%s\"},"
			"\"cwd\":\"%s\",\"foreground_process\":\"%s\","
			"\"pane_tail\":\"%s\"}",
			win->id, win->wm_class, esc_title, cwd, fg, esc_tail
		);
		res->success = true;
		return 0;
	}

	/* 9. inspect_windows (dwm tag-enriched client list) */
	if (strcmp(op, "inspect_windows") == 0) {
		res->handled = true;
		dwm_win_info_t dclients[JARVIS_MAX_CLIENTS];
		dwm_win_info_t dfoc, doth;
		size_t dn = dwm_ctl_query_windows(dclients, JARVIS_MAX_CLIENTS, &dfoc, &doth);
		size_t off = 0;
		off += (size_t)snprintf(res->output + off, sizeof(res->output) - off,
		                        "{\"focused\":\"%s\",\"other\":\"%s\",\"windows\":[",
		                        dfoc.id, doth.id);
		for (size_t i = 0; i < dn && off + 160 < sizeof(res->output); i++) {
			char ec[64] = {0}, et[128] = {0};
			jarvis_desktop_json_escape(dclients[i].wm_class, ec, sizeof(ec));
			jarvis_desktop_json_escape(dclients[i].title, et, sizeof(et));
			off += (size_t)snprintf(res->output + off, sizeof(res->output) - off,
			                        "%s{\"id\":\"%s\",\"class\":\"%s\",\"title\":\"%s\",\"tag\":%d,\"visible\":%s,\"focused\":%s}",
			                        i > 0 ? "," : "",
			                        dclients[i].id, ec, et, dclients[i].tag,
			                        dclients[i].visible ? "true" : "false",
			                        dclients[i].focused ? "true" : "false");
		}
		snprintf(res->output + off, sizeof(res->output) - off, "]}");
		res->success = true;
		return 0;
	}

	return 0;
}

static bool inject_type_and_enter(const char *cmd, char *err_buf, size_t err_sz)
{
	jarvis_dsl_t tdsl;
	jarvis_adapter_result_t tres;
	memset(&tdsl, 0, sizeof(tdsl));
	memset(&tres, 0, sizeof(tres));
	snprintf(tdsl.op, sizeof(tdsl.op), "type_and_enter");
	snprintf(tdsl.args.text, sizeof(tdsl.args.text), "%.*s",
	         (int)(sizeof(tdsl.args.text) - 1), cmd);
	int rret = 0;
	XY_CALL(&rret, jarvis_xy_execute, &tdsl, &tres);
	if (tres.handled && tres.success) return true;
	if (err_buf && err_sz > 0) {
		snprintf(err_buf, err_sz, "%s",
		         tres.error[0] ? tres.error : "jarvis-x11 failed to type command into terminal");
	}
	return false;
}

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
	dwm_ctl_run_capture(cmd, out, sizeof(out));
	return (out[0] && strncasecmp(out, yes_lbl, strlen(yes_lbl)) == 0) ? 1 : 0;
}

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
	snprintf(acting_file, sizeof(acting_file), "/tmp/jarvis-acting-%s.pid", user);

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
	dwm_ctl_run_capture(cmd, out_selection, out_sz);
	unlink(tmp_in);
	return out_selection[0] ? 1 : 0;
}

XY_IMPL(int, jarvis_xy_notify, const char *, title, const char *, body, int, play_earcon)
{
	if (title && *title) {
		char *const argv[] = {
			"notify-send", "-t", "3500", "-h", "string:x-dunst-stack-tag:voice",
			(char *)title, (char *)(body ? body : ""), NULL
		};
		dwm_ctl_spawn_argv(argv);
	}
	if (play_earcon) {
		const char *user = getenv("USER");
		if (!user || !*user) user = "quirinpa";
		char wav_path[128];
		snprintf(wav_path, sizeof(wav_path), "/tmp/jarvis-done-%s.wav", user);
		if (access(wav_path, R_OK) != 0)
			snprintf(wav_path, sizeof(wav_path), "/tmp/dwm-voice-done-%s.wav", user);
		if (access(wav_path, R_OK) == 0) {
			char *const argv[] = { "pw-play", "--volume=0.30", wav_path, NULL };
			dwm_ctl_spawn_argv(argv);
		}
	}
	return 0;
}
