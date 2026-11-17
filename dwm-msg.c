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
		"  clients                 List managed windows (id, class, title)\n"
		"  focused                 Print currently focused window info\n");
	exit(1);
}

static int
send_cmd(Display *dpy, Window root, long cmd, long arg)
{
	XEvent ev;
	Atom cmd_atom = XInternAtom(dpy, "_DWM_CMD", False);

	memset(&ev, 0, sizeof(ev));
	ev.xclient.type = ClientMessage;
	ev.xclient.window = root;
	ev.xclient.message_type = cmd_atom;
	ev.xclient.format = 32;
	ev.xclient.data.l[0] = cmd;
	ev.xclient.data.l[1] = arg;

	XSendEvent(dpy, root, False, SubstructureRedirectMask | SubstructureNotifyMask, &ev);
	XFlush(dpy);
	return 0;
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
	} else if (!strcmp(argv[1], "clients") || !strcmp(argv[1], "focus-class")) {
		Atom net_clients = XInternAtom(dpy, "_NET_CLIENT_LIST", False);
		Atom actual_type;
		int actual_format;
		unsigned long nitems = 0, bytes_after;
		unsigned char *prop = NULL;

		if (XGetWindowProperty(dpy, root, net_clients, 0, 1024, False, XA_WINDOW,
			&actual_type, &actual_format, &nitems, &bytes_after, &prop) == Success && prop) {
			Window *wins = (Window *)prop;
			unsigned long i;
			for (i = 0; i < nitems; i++) {
				XClassHint ch = { NULL, NULL };
				char title[256];
				XGetClassHint(dpy, wins[i], &ch);
				get_win_title(dpy, wins[i], title, sizeof(title));
				if (!strcmp(argv[1], "clients")) {
					printf("0x%08lx\t%s\t%s\n", wins[i],
						ch.res_class ? ch.res_class : "-",
						title);
				} else if (argc >= 3) {
					if (strcasestr_match(ch.res_class, argv[2]) ||
					    strcasestr_match(ch.res_name, argv[2]) ||
					    strcasestr_match(title, argv[2])) {
						send_cmd(dpy, root, 12, (long)wins[i]);
						if (ch.res_class) XFree(ch.res_class);
						if (ch.res_name) XFree(ch.res_name);
						break;
					}
				}
				if (ch.res_class) XFree(ch.res_class);
				if (ch.res_name) XFree(ch.res_name);
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
