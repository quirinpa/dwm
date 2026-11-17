/* See LICENSE file for copyright and license details.
 * dwm-msg: lightweight X11 IPC client for dwm (_DWM_CMD ClientMessage + EWMH queries)
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

static void
usage(void)
{
	fprintf(stderr,
		"usage: dwm-msg <command> [arg]\n"
		"commands:\n"
		"  reload                  Reload ~/.Xresources colors & geometry live\n"
		"  view <1..9|0>           Switch to tag 1..9 (or 0 for all tags)\n"
		"  tag <1..9|0>            Move focused window to tag 1..9 (or 0 for all)\n"
		"  togglefloating          Toggle floating state on focused window\n"
		"  togglefullscreen        Toggle fullscreen on focused window\n"
		"  zoom                    Swap focused window with master\n"
		"  focusstack <next|prev>  Focus next (+1) or previous (-1) window\n"
		"  killclient              Close focused window\n"
		"  layout <tile|float|monocle|toggle>\n"
		"  gaps <+N|-N|0>          Adjust or reset window gaps\n"
		"  togglebar               Toggle top status bar visibility\n"
		"  focus-win <win_id>      Focus window by X11 Window ID (hex or dec)\n"
		"  focus-class <name>      Focus first window matching WM_CLASS / title\n"
		"  tag-win <win_id> <1..9> Move specific window ID to tag 1..9 without switching focus\n"
		"  pull-win <win_id>       Pull specific window ID onto current workspace and focus it\n"
		"  pull-class <name>       Pull first window matching WM_CLASS / title onto current workspace\n"
		"  close-win <win_id>      Close specific window ID without switching focus\n"
		"  curtag                  Print current active workspace number (1..9)\n"
		"  other                   Print the unfocused window on the current workspace\n"
		"  clients                 List managed windows (id, class, title, tag, visible)\n"
		"  focused                 Print currently focused window info\n");
	exit(1);
}

static int
send_cmd2(Display *dpy, Window root, long cmd, long arg1, long arg2)
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

static int
send_cmd(Display *dpy, Window root, long cmd, long arg)
{
	return send_cmd2(dpy, root, cmd, arg, 0);
}

static unsigned int
get_cardinal_prop(Display *dpy, Window w, const char *atom_name, unsigned int def_val)
{
	Atom prop_atom = XInternAtom(dpy, atom_name, False);
	Atom actual_type;
	int actual_format;
	unsigned long nitems = 0, bytes_after;
	unsigned char *prop = NULL;
	unsigned int val = def_val;

	if (XGetWindowProperty(dpy, w, prop_atom, 0, 1, False, XA_CARDINAL,
		&actual_type, &actual_format, &nitems, &bytes_after, &prop) == Success && prop) {
		if (nitems > 0)
			val = (unsigned int)(*(unsigned long *)prop);
		XFree(prop);
	}
	return val;
}

static int
mask_to_tag_num(unsigned int mask)
{
	int t;
	for (t = 1; t <= 9; t++) {
		if (mask & (1U << (t - 1)))
			return t;
	}
	return 1;
}

static void
get_win_title(Display *dpy, Window w, char *buf, size_t len)
{
	Atom net_wm_name = XInternAtom(dpy, "_NET_WM_NAME", False);
	Atom utf8 = XInternAtom(dpy, "UTF8_STRING", False);
	Atom actual_type;
	int actual_format;
	unsigned long nitems, bytes_after;
	unsigned char *prop = NULL;

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

static int
strcasestr_match(const char *haystack, const char *needle)
{
	size_t nlen;
	if (!haystack || !needle)
		return 0;
	nlen = strlen(needle);
	if (nlen == 0)
		return 1;
	for (; *haystack; haystack++) {
		size_t i = 0;
		while (i < nlen && haystack[i] &&
		       tolower((unsigned char)haystack[i]) == tolower((unsigned char)needle[i]))
			i++;
		if (i == nlen)
			return 1;
	}
	return 0;
}

int
main(int argc, char *argv[])
{
	Display *dpy;
	Window root;

	if (argc < 2)
		usage();

	if (!(dpy = XOpenDisplay(NULL))) {
		fprintf(stderr, "dwm-msg: cannot open display\n");
		return 1;
	}
	root = DefaultRootWindow(dpy);

	if (!strcmp(argv[1], "reload")) {
		send_cmd(dpy, root, 1, 0);
	} else if (!strcmp(argv[1], "view") && argc >= 3) {
		int t = atoi(argv[2]);
		unsigned int mask = (t == 0) ? ~0U : (t >= 1 && t <= 9 ? (1U << (t - 1)) : 0);
		if (mask)
			send_cmd(dpy, root, 2, (long)mask);
	} else if (!strcmp(argv[1], "tag") && argc >= 3) {
		int t = atoi(argv[2]);
		unsigned int mask = (t == 0) ? ~0U : (t >= 1 && t <= 9 ? (1U << (t - 1)) : 0);
		if (mask)
			send_cmd(dpy, root, 3, (long)mask);
	} else if (!strcmp(argv[1], "togglefloating") || !strcmp(argv[1], "float")) {
		send_cmd(dpy, root, 4, 0);
	} else if (!strcmp(argv[1], "togglefullscreen") || !strcmp(argv[1], "fullscreen")) {
		send_cmd(dpy, root, 5, 0);
	} else if (!strcmp(argv[1], "zoom")) {
		send_cmd(dpy, root, 6, 0);
	} else if (!strcmp(argv[1], "focusstack")) {
		long dir = 1;
		if (argc >= 3 && (!strcmp(argv[2], "prev") || !strcmp(argv[2], "-1") || !strcmp(argv[2], "up")))
			dir = -1;
		send_cmd(dpy, root, 7, dir);
	} else if (!strcmp(argv[1], "killclient") || !strcmp(argv[1], "close")) {
		send_cmd(dpy, root, 8, 0);
	} else if (!strcmp(argv[1], "layout")) {
		long idx = -1;
		if (argc >= 3) {
			if (!strcmp(argv[2], "tile") || !strcmp(argv[2], "0"))
				idx = 0;
			else if (!strcmp(argv[2], "float") || !strcmp(argv[2], "1"))
				idx = 1;
			else if (!strcmp(argv[2], "monocle") || !strcmp(argv[2], "2"))
				idx = 2;
		}
		send_cmd(dpy, root, 9, idx);
	} else if (!strcmp(argv[1], "gaps") && argc >= 3) {
		send_cmd(dpy, root, 10, strtol(argv[2], NULL, 10));
	} else if (!strcmp(argv[1], "togglebar")) {
		send_cmd(dpy, root, 11, 0);
	} else if (!strcmp(argv[1], "focus-win") && argc >= 3) {
		Window w = (Window)strtoul(argv[2], NULL, 0);
		send_cmd(dpy, root, 12, (long)w);
	} else if (!strcmp(argv[1], "tag-win") && argc >= 4) {
		Window w = (Window)strtoul(argv[2], NULL, 0);
		int t = atoi(argv[3]);
		unsigned int mask = (t == 0) ? ~0U : (t >= 1 && t <= 9 ? (1U << (t - 1)) : 0);
		if (w && mask)
			send_cmd2(dpy, root, 13, (long)w, (long)mask);
	} else if (!strcmp(argv[1], "pull-win") && argc >= 3) {
		Window w = (Window)strtoul(argv[2], NULL, 0);
		if (w)
			send_cmd(dpy, root, 14, (long)w);
	} else if (!strcmp(argv[1], "close-win") && argc >= 3) {
		Window w = (Window)strtoul(argv[2], NULL, 0);
		if (w)
			send_cmd(dpy, root, 15, (long)w);
	} else if (!strcmp(argv[1], "curtag")) {
		unsigned int cur_mask = get_cardinal_prop(dpy, root, "_DWM_CUR_TAG", 1U);
		printf("%d\n", mask_to_tag_num(cur_mask));
	} else if (!strcmp(argv[1], "clients") || !strcmp(argv[1], "focus-class") ||
	           !strcmp(argv[1], "pull-class") || !strcmp(argv[1], "other")) {
		Atom net_clients = XInternAtom(dpy, "_NET_CLIENT_LIST", False);
		Atom net_active = XInternAtom(dpy, "_NET_ACTIVE_WINDOW", False);
		Atom actual_type;
		int actual_format;
		unsigned long nitems = 0, bytes_after;
		unsigned char *prop = NULL;
		unsigned char *act_prop = NULL;
		Window active_win = 0;
		unsigned int cur_mask = get_cardinal_prop(dpy, root, "_DWM_CUR_TAG", 1U);

		if (XGetWindowProperty(dpy, root, net_active, 0, 1, False, XA_WINDOW,
			&actual_type, &actual_format, &nitems, &bytes_after, &act_prop) == Success && act_prop) {
			if (nitems > 0)
				active_win = *(Window *)act_prop;
			XFree(act_prop);
		}

		nitems = 0;
		if (XGetWindowProperty(dpy, root, net_clients, 0, 1024, False, XA_WINDOW,
			&actual_type, &actual_format, &nitems, &bytes_after, &prop) == Success && prop) {
			Window *wins = (Window *)prop;
			unsigned long i;
			Window fallback_other = 0;
			char fallback_cls[128] = "-", fallback_title[256] = "";
			int fallback_tag = 1;

			for (i = 0; i < nitems; i++) {
				XClassHint ch = { NULL, NULL };
				char title[256];
				unsigned int c_mask = get_cardinal_prop(dpy, wins[i], "_DWM_CLIENT_TAGS", cur_mask);
				int c_tag = mask_to_tag_num(c_mask);
				int on_cur = (c_mask & cur_mask) ? 1 : 0;

				XGetClassHint(dpy, wins[i], &ch);
				get_win_title(dpy, wins[i], title, sizeof(title));
				if (!strcmp(argv[1], "clients")) {
					printf("0x%08lx\t%s\t%s\t%d\t%d\n", wins[i],
						ch.res_class ? ch.res_class : "-",
						title, c_tag, on_cur);
				} else if (!strcmp(argv[1], "other")) {
					if (wins[i] != active_win) {
						if (on_cur) {
							printf("0x%08lx\t%s\t%s\t%d\n", wins[i],
								ch.res_class ? ch.res_class : "-",
								title, c_tag);
							if (ch.res_class) XFree(ch.res_class);
							if (ch.res_name) XFree(ch.res_name);
							fallback_other = 0;
							break;
						} else if (!fallback_other) {
							fallback_other = wins[i];
							snprintf(fallback_cls, sizeof(fallback_cls), "%s",
								ch.res_class ? ch.res_class : "-");
							snprintf(fallback_title, sizeof(fallback_title), "%s", title);
							fallback_tag = c_tag;
						}
					}
				} else if (argc >= 3) {
					if (strcasestr_match(ch.res_class, argv[2]) ||
					    strcasestr_match(ch.res_name, argv[2]) ||
					    strcasestr_match(title, argv[2])) {
						long op = !strcmp(argv[1], "pull-class") ? 14 : 12;
						send_cmd(dpy, root, op, (long)wins[i]);
						if (ch.res_class) XFree(ch.res_class);
						if (ch.res_name) XFree(ch.res_name);
						break;
					}
				}
				if (ch.res_class) XFree(ch.res_class);
				if (ch.res_name) XFree(ch.res_name);
			}
			if (!strcmp(argv[1], "other") && fallback_other) {
				printf("0x%08lx\t%s\t%s\t%d\n", fallback_other,
					fallback_cls, fallback_title, fallback_tag);
			}
			XFree(prop);
		}
	} else if (!strcmp(argv[1], "focused")) {
		Atom net_active = XInternAtom(dpy, "_NET_ACTIVE_WINDOW", False);
		Atom actual_type;
		int actual_format;
		unsigned long nitems = 0, bytes_after;
		unsigned char *prop = NULL;

		if (XGetWindowProperty(dpy, root, net_active, 0, 1, False, XA_WINDOW,
			&actual_type, &actual_format, &nitems, &bytes_after, &prop) == Success && prop && nitems > 0) {
			Window w = *(Window *)prop;
			XClassHint ch = { NULL, NULL };
			char title[256];
			XGetClassHint(dpy, w, &ch);
			get_win_title(dpy, w, title, sizeof(title));
			printf("0x%08lx\t%s\t%s\n", w, ch.res_class ? ch.res_class : "-", title);
			if (ch.res_class) XFree(ch.res_class);
			if (ch.res_name) XFree(ch.res_name);
			XFree(prop);
		}
	} else {
		usage();
	}

	XCloseDisplay(dpy);
	return 0;
}
