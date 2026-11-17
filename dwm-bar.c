/* See LICENSE file for copyright and license details.
 * dwm-bar: zero-fork, event-driven X11 status bar daemon configured via ~/.Xresources
 */
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <linux/wireless.h>
#include <alsa/asoundlib.h>
#include <X11/Xatom.h>
#include <X11/XKBlib.h>
#include <X11/Xlib.h>
#include <X11/Xresource.h>

#define MAX(A, B) ((A) > (B) ? (A) : (B))

/* dwm status escape bytes matching dwm.c drawbar() / buttonpress() */
#define C_KBD    "\013"
#define C_WRN    "\014"
#define C_OFF    "\015"
#define C_ICO    "\016"
#define C_VAL    "\017"
#define C_SYS_OK "\020"
#define C_MEM    "\023"
#define C_CPU    "\024"
#define C_VOL    "\025"
#define C_VMU    "\026"
#define C_NET    "\027"
#define C_BOK    "\030"
#define C_BWN    "\031"
#define C_BCR    "\032"
#define C_DAT    "\033"
#define C_VOC    "\034"
#define C_GER    "\035"
#define C_TUP    "\036"
#define C_TDN    "\037"

typedef struct {
	int interval_sec;
	char ping_host[128];
	int ping_port;
	int mem_warn;
	int mem_crit;
	int cpu_warn;
	int cpu_crit;
	int bat_warn;
	int bat_crit;
	char modules[256];
} BarConfig;

static BarConfig cfg = {
	.interval_sec = 5,
	.ping_host    = "tty.pt",
	.ping_port    = 443,
	.mem_warn     = 70,
	.mem_crit     = 85,
	.cpu_warn     = 4,
	.cpu_crit     = 10,
	.bat_warn     = 30,
	.bat_crit     = 15,
	.modules      = "host mem cpu voice vol net bat kbd cal date time gear"
};

static volatile sig_atomic_t running = 1;
static volatile sig_atomic_t force_refresh = 0;

static void
on_sigusr1(int sig)
{
	(void)sig;
	force_refresh = 1;
}

static void
on_sigterm(int sig)
{
	(void)sig;
	running = 0;
}

static int
read_small_file(const char *path, char *buf, size_t buflen)
{
	int fd;
	ssize_t n;

	if (!buflen)
		return -1;
	buf[0] = '\0';
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	n = read(fd, buf, buflen - 1);
	close(fd);
	if (n <= 0)
		return -1;
	buf[n] = '\0';
	while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
		buf[--n] = '\0';
	return (int)n;
}

static void
load_bar_xrdb(Display *dpy)
{
	Display *xdpy;
	char *resm;
	XrmDatabase db;
	char *type;
	XrmValue v;

	xdpy = XOpenDisplay(NULL);
	if (!xdpy)
		xdpy = dpy;
	resm = XResourceManagerString(xdpy);
	if (!resm) {
		if (xdpy != dpy)
			XCloseDisplay(xdpy);
		return;
	}
	db = XrmGetStringDatabase(resm);
	if (XrmGetResource(db, "dwm.bar.interval", "*", &type, &v) && v.addr)
		cfg.interval_sec = MAX(1, atoi(v.addr));
	if (XrmGetResource(db, "dwm.bar.ping_host", "*", &type, &v) && v.addr)
		snprintf(cfg.ping_host, sizeof(cfg.ping_host), "%s", v.addr);
	if (XrmGetResource(db, "dwm.bar.ping_port", "*", &type, &v) && v.addr)
		cfg.ping_port = atoi(v.addr);
	if (XrmGetResource(db, "dwm.bar.mem_warn", "*", &type, &v) && v.addr)
		cfg.mem_warn = atoi(v.addr);
	if (XrmGetResource(db, "dwm.bar.mem_crit", "*", &type, &v) && v.addr)
		cfg.mem_crit = atoi(v.addr);
	if (XrmGetResource(db, "dwm.bar.cpu_warn", "*", &type, &v) && v.addr)
		cfg.cpu_warn = atoi(v.addr);
	if (XrmGetResource(db, "dwm.bar.cpu_crit", "*", &type, &v) && v.addr)
		cfg.cpu_crit = atoi(v.addr);
	if (XrmGetResource(db, "dwm.bar.bat_warn", "*", &type, &v) && v.addr)
		cfg.bat_warn = atoi(v.addr);
	if (XrmGetResource(db, "dwm.bar.bat_crit", "*", &type, &v) && v.addr)
		cfg.bat_crit = atoi(v.addr);
	if (XrmGetResource(db, "dwm.bar.modules", "*", &type, &v) && v.addr)
		snprintf(cfg.modules, sizeof(cfg.modules), "%s", v.addr);
	XrmDestroyDatabase(db);
	if (xdpy != dpy)
		XCloseDisplay(xdpy);
}

static int
probe_host_tcp(const char *host, int port, int timeout_ms)
{
	struct addrinfo hints, *res = NULL, *rp;
	char port_str[16];
	int sock = -1, ok = 0;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	snprintf(port_str, sizeof(port_str), "%d", port > 0 ? port : 443);

	if (getaddrinfo(host, port_str, &hints, &res) != 0 || !res)
		return 0;

	for (rp = res; rp; rp = rp->ai_next) {
		sock = socket(rp->ai_family, rp->ai_socktype | SOCK_NONBLOCK | SOCK_CLOEXEC, rp->ai_protocol);
		if (sock < 0)
			continue;
		if (connect(sock, rp->ai_addr, rp->ai_addrlen) == 0) {
			ok = 1;
			close(sock);
			break;
		}
		if (errno == EINPROGRESS) {
			struct pollfd pfd = { .fd = sock, .events = POLLOUT };
			if (poll(&pfd, 1, timeout_ms) > 0 && (pfd.revents & POLLOUT)) {
				int err = 0;
				socklen_t elen = sizeof(err);
				if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &err, &elen) == 0 && err == 0)
					ok = 1;
			}
		}
		close(sock);
		if (ok)
			break;
	}
	freeaddrinfo(res);
	return ok;
}

static void
seg_host(char *out, size_t len, const char *user, time_t now, time_t *last_probe, int *cached_up)
{
	char state_path[256], buf[32];

	snprintf(state_path, sizeof(state_path), "/tmp/dwm-tty-%s.state", user);
	if (now - *last_probe >= 15 || *last_probe == 0) {
		*last_probe = now;
		*cached_up = probe_host_tcp(cfg.ping_host, cfg.ping_port, 800);
		int fd = open(state_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
		if (fd >= 0) {
			const char *s = *cached_up ? "up\n" : "down\n";
			ssize_t wr = write(fd, s, strlen(s));
			(void)wr;
			close(fd);
		}
	} else if (read_small_file(state_path, buf, sizeof(buf)) > 0) {
		*cached_up = (strcmp(buf, "up") == 0);
	}
	snprintf(out, len, "%s  ", *cached_up ? C_TUP : C_TDN);
}

static void
seg_mem(char *out, size_t len)
{
	char buf[1024], *p;
	long total = 0, avail = 0;

	if (read_small_file("/proc/meminfo", buf, sizeof(buf)) <= 0) {
		out[0] = '\0';
		return;
	}
	if ((p = strstr(buf, "MemTotal:")))
		total = strtol(p + 9, NULL, 10);
	if ((p = strstr(buf, "MemAvailable:")))
		avail = strtol(p + 13, NULL, 10);
	if (total <= 0) {
		out[0] = '\0';
		return;
	}
	long used = total - avail;
	int pct = (int)((used * 100) / total);
	double gib = (double)used / 1048576.0;

	if (pct >= cfg.mem_crit)
		snprintf(out, len, "%s    %.1fG", C_CPU, gib);
	else if (pct >= cfg.mem_warn)
		snprintf(out, len, "%s    %.1fG", C_MEM, gib);
	else
		snprintf(out, len, "%s%s   %s %.1fG", C_SYS_OK, C_ICO, C_VAL, gib);
}

static void
seg_cpu(char *out, size_t len)
{
	char buf[128], *sp;
	double load = 0.0;

	if (read_small_file("/proc/loadavg", buf, sizeof(buf)) <= 0) {
		out[0] = '\0';
		return;
	}
	if ((sp = strchr(buf, ' ')))
		*sp = '\0';
	load = strtod(buf, NULL);
	int load_int = (int)load;

	if (load_int >= cfg.cpu_crit)
		snprintf(out, len, "%s    %.16s", C_CPU, buf);
	else if (load_int >= cfg.cpu_warn)
		snprintf(out, len, "%s    %.16s", C_MEM, buf);
	else
		snprintf(out, len, "%s%s   %s %.16s", C_SYS_OK, C_ICO, C_VAL, buf);
}

static void
seg_voice(char *out, size_t len, const char *user)
{
	char path[256], buf[64];

	out[0] = '\0';
	snprintf(path, sizeof(path), "/tmp/dwm-voice-%s.state", user);
	if (read_small_file(path, buf, sizeof(buf)) <= 0)
		return;
	if (!strncmp(buf, "rec", 3))
		snprintf(out, len, "%s    REC", C_VOC);
	else if (!strncmp(buf, "transcribing", 12))
		snprintf(out, len, "%s    ...", C_BWN);
}

static void
seg_vol(char *out, size_t len)
{
	snd_mixer_t *handle = NULL;
	snd_mixer_selem_id_t *sid = NULL;
	snd_mixer_elem_t *elem = NULL;
	long min = 0, max = 0, val = 0;
	int sw = 1;

	out[0] = '\0';
	if (snd_mixer_open(&handle, 0) < 0)
		return;
	if (snd_mixer_attach(handle, "default") < 0 ||
	    snd_mixer_selem_register(handle, NULL, NULL) < 0 ||
	    snd_mixer_load(handle) < 0) {
		snd_mixer_close(handle);
		return;
	}
	snd_mixer_selem_id_alloca(&sid);
	snd_mixer_selem_id_set_index(sid, 0);
	snd_mixer_selem_id_set_name(sid, "Master");
	elem = snd_mixer_find_selem(handle, sid);
	if (elem) {
		snd_mixer_selem_get_playback_volume_range(elem, &min, &max);
		snd_mixer_selem_get_playback_volume(elem, SND_MIXER_SCHN_FRONT_LEFT, &val);
		snd_mixer_selem_get_playback_switch(elem, SND_MIXER_SCHN_FRONT_LEFT, &sw);
		int pct = (max > min) ? (int)(((val - min) * 100 + (max - min) / 2) / (max - min)) : 0;
		if (!sw) {
			snprintf(out, len, "%s    ×", C_VMU);
		} else {
			const char *glyph = (pct == 0) ? "" : (pct < 40 ? "" : "");
			snprintf(out, len, "%s   %s%s %d%%", C_VOL, glyph, C_VAL, pct);
		}
	}
	snd_mixer_close(handle);
}

static void
seg_net(char *out, size_t len, int ioctl_sock)
{
	DIR *d;
	struct dirent *de;
	char path[512], state[32];

	out[0] = '\0';
	d = opendir("/sys/class/net");
	if (!d) {
		snprintf(out, len, "%s%s    no signal", C_NET, C_OFF);
		return;
	}
	/* 1. Check Wi-Fi interfaces (wl*) */
	while ((de = readdir(d))) {
		if (strncmp(de->d_name, "wl", 2) != 0)
			continue;
		snprintf(path, sizeof(path), "/sys/class/net/%.64s/operstate", de->d_name);
		if (read_small_file(path, state, sizeof(state)) > 0 && !strcmp(state, "up")) {
			struct iwreq wreq;
			char essid[IW_ESSID_MAX_SIZE + 1] = {0};
			memset(&wreq, 0, sizeof(wreq));
			snprintf(wreq.ifr_name, IFNAMSIZ, "%.*s", IFNAMSIZ - 1, de->d_name);
			wreq.u.essid.pointer = essid;
			wreq.u.essid.length = IW_ESSID_MAX_SIZE;
			if (ioctl_sock >= 0 && ioctl(ioctl_sock, SIOCGIWESSID, &wreq) == 0 && essid[0])
				snprintf(out, len, "%s   %s %.48s", C_NET, C_VAL, essid);
			else
				snprintf(out, len, "%s   ", C_NET);
			closedir(d);
			return;
		}
	}
	rewinddir(d);
	/* 2. Check Ethernet interfaces (en* / eth*) */
	while ((de = readdir(d))) {
		if (strncmp(de->d_name, "en", 2) != 0 && strncmp(de->d_name, "eth", 3) != 0)
			continue;
		snprintf(path, sizeof(path), "/sys/class/net/%.64s/operstate", de->d_name);
		if (read_small_file(path, state, sizeof(state)) > 0 && !strcmp(state, "up")) {
			snprintf(out, len, "%s   %s %.32s", C_NET, C_VAL, de->d_name);
			closedir(d);
			return;
		}
	}
	closedir(d);
	snprintf(out, len, "%s%s    no signal", C_NET, C_OFF);
}

static void
seg_bat(char *out, size_t len)
{
	char cap_s[32], status[32], ac_s[16];
	int cap = 0, ac = 0;

	out[0] = '\0';
	if (read_small_file("/sys/class/power_supply/AC/online", ac_s, sizeof(ac_s)) > 0 ||
	    read_small_file("/sys/class/power_supply/AC0/online", ac_s, sizeof(ac_s)) > 0)
		ac = (ac_s[0] == '1');

	if (read_small_file("/sys/class/power_supply/BAT0/capacity", cap_s, sizeof(cap_s)) <= 0 &&
	    read_small_file("/sys/class/power_supply/BAT1/capacity", cap_s, sizeof(cap_s)) <= 0)
		return;
	cap = atoi(cap_s);
	if (read_small_file("/sys/class/power_supply/BAT0/status", status, sizeof(status)) <= 0)
		read_small_file("/sys/class/power_supply/BAT1/status", status, sizeof(status));

	const char *icon = (cap >= 85) ? "" : (cap >= 60 ? "" : (cap >= 35 ? "" : (cap >= 15 ? "" : "")));
	if (ac || !strcmp(status, "Charging") || !strcmp(status, "Full") || !strcmp(status, "Not charging")) {
		snprintf(out, len, "%s   %s %d%%", C_BOK, C_VAL, cap);
	} else if (cap < cfg.bat_crit) {
		snprintf(out, len, "%s   %s %d%%", C_BCR, icon, cap);
	} else if (cap < cfg.bat_warn) {
		snprintf(out, len, "%s   %s %d%%", C_BWN, icon, cap);
	} else {
		snprintf(out, len, "%s   %s%s %d%%", C_BOK, icon, C_VAL, cap);
	}
}

static void
seg_kbd(Display *dpy, Window root, Atom xkb_rules_atom, char *out, size_t len)
{
	XkbStateRec state;
	Atom actual;
	int fmt;
	unsigned long n, ba;
	unsigned char *prop = NULL;
	char layout_up[16] = "US";

	memset(&state, 0, sizeof(state));
	XkbGetState(dpy, XkbUseCoreKbd, &state);
	if (XGetWindowProperty(dpy, root, xkb_rules_atom, 0, 256, False, XA_STRING,
		&actual, &fmt, &n, &ba, &prop) == Success && prop && n > 2) {
		char *rules = (char *)prop;
		char *model = rules + strlen(rules) + 1;
		char *layouts = model + strlen(model) + 1;
		/* If comma-separated layouts, pick state.group */
		int g = state.group;
		char *tok = layouts;
		while (g > 0 && (tok = strchr(tok, ',')) != NULL) {
			tok++;
			g--;
		}
		if (tok && *tok) {
			size_t i = 0;
			while (tok[i] && tok[i] != ',' && i < sizeof(layout_up) - 1) {
				layout_up[i] = (char)toupper((unsigned char)tok[i]);
				i++;
			}
			layout_up[i] = '\0';
		}
		XFree(prop);
	}
	snprintf(out, len, "%s   %s %s", C_KBD, C_VAL, layout_up);
}

static void
seg_cal(char *out, size_t len, const char *user)
{
	char path[256], buf[128], *nl;

	out[0] = '\0';
	snprintf(path, sizeof(path), "/tmp/dwm-cal-%s.upcoming", user);
	if (read_small_file(path, buf, sizeof(buf)) <= 0)
		return;
	if ((nl = strchr(buf, '\n')))
		*nl = '\0';
	if (buf[0])
		snprintf(out, len, "%s%s    %.80s", C_DAT, C_WRN, buf);
}

int
main(void)
{
	Display *dpy;
	Window root;
	Atom xkb_rules_atom;
	int xkb_ev_base = 0, xkb_err_base = 0, xkb_op = 0, xkb_maj = XkbMajorVersion, xkb_min = XkbMinorVersion;
	int inot_fd = -1, ioctl_sock = -1;
	const char *user = getenv("USER");
	char pid_path[256];
	time_t last_probe = 0, last_cal_check = 0, last_cal_sync = 0;
	int cached_tty_up = 1;

	if (!user || !*user)
		user = "quirinpa";

	/* Write PID file so dwm-ctl / dwm-voice can send SIGUSR1 */
	snprintf(pid_path, sizeof(pid_path), "/tmp/dwm-bar-%s.pid", user);
	{
		FILE *pf = fopen(pid_path, "w");
		if (pf) {
			fprintf(pf, "%d\n", (int)getpid());
			fclose(pf);
		}
	}

	signal(SIGUSR1, on_sigusr1);
	signal(SIGTERM, on_sigterm);
	signal(SIGINT, on_sigterm);
	signal(SIGCHLD, SIG_IGN);

	if (!(dpy = XOpenDisplay(NULL))) {
		fprintf(stderr, "dwm-bar: cannot open display\n");
		return 1;
	}
	root = DefaultRootWindow(dpy);
	XrmInitialize();
	load_bar_xrdb(dpy);

	xkb_rules_atom = XInternAtom(dpy, "_XKB_RULES_NAMES", False);
	XSelectInput(dpy, root, PropertyChangeMask);
	if (XkbQueryExtension(dpy, &xkb_op, &xkb_ev_base, &xkb_err_base, &xkb_maj, &xkb_min)) {
		XkbSelectEvents(dpy, XkbUseCoreKbd,
			XkbNewKeyboardNotifyMask | XkbStateNotifyMask,
			XkbNewKeyboardNotifyMask | XkbStateNotifyMask);
	}

	inot_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	if (inot_fd >= 0) {
		inotify_add_watch(inot_fd, "/tmp", IN_CLOSE_WRITE | IN_MOVED_TO | IN_DELETE | IN_CREATE);
	}
	ioctl_sock = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);

	while (running) {
		time_t now = time(NULL);
		struct tm tm_now;
		localtime_r(&now, &tm_now);

		/* Background calendar sync & upcoming check (same cadence as before) */
		if (now - last_cal_sync >= 14400) {
			last_cal_sync = now;
			if (fork() == 0) {
				close(ConnectionNumber(dpy));
				execl("/home/quirinpa/.local/bin/dwm-cal", "dwm-cal", "sync", "--quiet", (char *)NULL);
				_exit(0);
			}
		} else if (now - last_cal_check >= 60) {
			last_cal_check = now;
			if (fork() == 0) {
				close(ConnectionNumber(dpy));
				execl("/home/quirinpa/.local/bin/dwm-cal", "dwm-cal", "refresh-upcoming", (char *)NULL);
				_exit(0);
			}
		}

		/* Assemble status bar according to cfg.modules */
		char status[512];
		status[0] = '\0';
		char mods_copy[256];
		snprintf(mods_copy, sizeof(mods_copy), "%s", cfg.modules);

		char *saveptr = NULL;
		for (char *tok = strtok_r(mods_copy, " \t,", &saveptr); tok; tok = strtok_r(NULL, " \t,", &saveptr)) {
			char seg[128] = {0};
			if (!strcmp(tok, "host"))
				seg_host(seg, sizeof(seg), user, now, &last_probe, &cached_tty_up);
			else if (!strcmp(tok, "mem"))
				seg_mem(seg, sizeof(seg));
			else if (!strcmp(tok, "cpu"))
				seg_cpu(seg, sizeof(seg));
			else if (!strcmp(tok, "voice"))
				seg_voice(seg, sizeof(seg), user);
			else if (!strcmp(tok, "vol"))
				seg_vol(seg, sizeof(seg));
			else if (!strcmp(tok, "net"))
				seg_net(seg, sizeof(seg), ioctl_sock);
			else if (!strcmp(tok, "bat"))
				seg_bat(seg, sizeof(seg));
			else if (!strcmp(tok, "kbd"))
				seg_kbd(dpy, root, xkb_rules_atom, seg, sizeof(seg));
			else if (!strcmp(tok, "cal"))
				seg_cal(seg, sizeof(seg), user);
			else if (!strcmp(tok, "date"))
				snprintf(seg, sizeof(seg), "%s   %s %02d/%02d", C_DAT, C_VAL, tm_now.tm_mday, tm_now.tm_mon + 1);
			else if (!strcmp(tok, "time"))
				snprintf(seg, sizeof(seg), "%s   %s %02d:%02d", C_DAT, C_VAL, tm_now.tm_hour, tm_now.tm_min);
			else if (!strcmp(tok, "gear"))
				snprintf(seg, sizeof(seg), "   %s  ", C_GER);

			if (seg[0])
				strncat(status, seg, sizeof(status) - strlen(status) - 1);
		}

		XStoreName(dpy, root, status);
		XFlush(dpy);
		force_refresh = 0;

		/* Wait until next interval OR immediate XKB / inotify / SIGUSR1 wakeup */
		struct pollfd pfds[2];
		int nfds = 0;
		pfds[nfds].fd = ConnectionNumber(dpy);
		pfds[nfds].events = POLLIN;
		nfds++;
		if (inot_fd >= 0) {
			pfds[nfds].fd = inot_fd;
			pfds[nfds].events = POLLIN;
			nfds++;
		}

		int ret = poll(pfds, nfds, cfg.interval_sec * 1000);
		if (ret > 0) {
			if (pfds[0].revents & POLLIN) {
				while (XPending(dpy)) {
					XEvent ev;
					XNextEvent(dpy, &ev);
					if (ev.type == PropertyNotify && ev.xproperty.window == root &&
					    ev.xproperty.atom == XA_RESOURCE_MANAGER) {
						load_bar_xrdb(dpy);
					}
				}
			}
			if (nfds > 1 && (pfds[1].revents & POLLIN)) {
				char ibuf[1024];
				while (read(inot_fd, ibuf, sizeof(ibuf)) > 0)
					;
			}
		}
	}

	if (ioctl_sock >= 0)
		close(ioctl_sock);
	if (inot_fd >= 0)
		close(inot_fd);
	XCloseDisplay(dpy);
	return 0;
}
