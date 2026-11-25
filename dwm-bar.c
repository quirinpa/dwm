/* See LICENSE file for copyright and license details.
 * dwm-bar: zero-fork, event-driven X11 status bar daemon configured via ~/.Xresources
 */
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
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
	.modules      = "host voice mem cpu vol net bat kbd cal date time gear"
};

static volatile sig_atomic_t running = 1;
static int sig_pipe[2] = { -1, -1 };

static void
on_sigusr1(int sig)
{
	int saved_errno = errno;
	(void)sig;
	if (sig_pipe[1] >= 0) {
		char b = 1;
		ssize_t wr = write(sig_pipe[1], &b, 1);
		(void)wr;
	}
	errno = saved_errno;
}

static void
on_sigterm(int sig)
{
	int saved_errno = errno;
	(void)sig;
	running = 0;
	if (sig_pipe[1] >= 0) {
		char b = 0;
		ssize_t wr = write(sig_pipe[1], &b, 1);
		(void)wr;
	}
	errno = saved_errno;
}

static void
drain_sig_pipe(void)
{
	char buf[64];
	if (sig_pipe[0] < 0)
		return;
	while (read(sig_pipe[0], buf, sizeof(buf)) > 0)
		;
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
seg_host(char *out, size_t len, const char *user, time_t now, time_t *last_probe, int *cached_up, int dpy_fd)
{
	char state_path[256], buf[32];

	snprintf(state_path, sizeof(state_path), "/tmp/dwm-tty-%s.state", user);
	if (read_small_file(state_path, buf, sizeof(buf)) > 0)
		*cached_up = (strcmp(buf, "up") == 0);

	if (now - *last_probe >= 15 || *last_probe == 0) {
		int prev_up = *cached_up;
		*last_probe = now;
		pid_t pid = fork();
		if (pid == 0) {
			if (dpy_fd >= 0)
				close(dpy_fd);
			if (sig_pipe[0] >= 0)
				close(sig_pipe[0]);
			if (sig_pipe[1] >= 0)
				close(sig_pipe[1]);
			int up = probe_host_tcp(cfg.ping_host, cfg.ping_port, 800);
			char tmp_path[280];
			snprintf(tmp_path, sizeof(tmp_path), "%s.tmp.%d", state_path, (int)getpid());
			int fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
			if (fd >= 0) {
				const char *s = up ? "up\n" : "down\n";
				ssize_t wr = write(fd, s, strlen(s));
				(void)wr;
				close(fd);
				rename(tmp_path, state_path);
			}
			if (up != prev_up)
				kill(getppid(), SIGUSR1);
			_exit(0);
		}
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

typedef struct {
	int online;
	int busy;
	char model[32];
	unsigned long tokens;
	unsigned long max_tokens;
} RenStatus;

static int
has_bar_module(const char *modules, const char *name)
{
	char copy[256];
	char *saveptr = NULL;

	snprintf(copy, sizeof(copy), "%s", modules);
	for (char *tok = strtok_r(copy, " \t,", &saveptr); tok; tok = strtok_r(NULL, " \t,", &saveptr)) {
		if (!strcmp(tok, name))
			return 1;
	}
	return 0;
}

static void
format_ren_tokens(unsigned long tokens, char *out, size_t len)
{
	if (tokens >= 1000000UL)
		snprintf(out, len, "%.1fM", (double)tokens / 1000000.0);
	else if (tokens >= 1000UL)
		snprintf(out, len, "%.1fk", (double)tokens / 1000.0);
	else if (tokens > 0)
		snprintf(out, len, "%lu", tokens);
	else
		out[0] = '\0';
}

static int
query_ren_status(const char *user, RenStatus *out)
{
	static ino_t unsupported_ino = 0;
	struct sockaddr_un addr;
	struct stat st;
	char buf[256];
	int fd;

	memset(out, 0, sizeof(*out));
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	snprintf(addr.sun_path, sizeof(addr.sun_path), "/tmp/ren-jarvis-%s.sock", user);

	if (stat(addr.sun_path, &st) < 0 || !S_ISSOCK(st.st_mode))
		return 0;
	if (unsupported_ino != 0 && st.st_ino == unsupported_ino)
		return 0;

	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return 0;

	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		if (errno != EINPROGRESS) {
			close(fd);
			return 0;
		}
		struct pollfd pfd = { .fd = fd, .events = POLLOUT };
		int err = 0;
		socklen_t elen = sizeof(err);
		if (poll(&pfd, 1, 80) <= 0 || !(pfd.revents & POLLOUT) ||
		    getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen) < 0 || err != 0) {
			close(fd);
			return 0;
		}
	}

	if (send(fd, "STATUS\n", 7, MSG_NOSIGNAL) != 7) {
		close(fd);
		return 0;
	}

	struct pollfd rfd = { .fd = fd, .events = POLLIN };
	if (poll(&rfd, 1, 100) <= 0 || !(rfd.revents & POLLIN)) {
		close(fd);
		unsupported_ino = st.st_ino;
		return 0;
	}

	ssize_t n = recv(fd, buf, sizeof(buf) - 1, 0);
	close(fd);
	if (n <= 0)
		return 0;
	buf[n] = '\0';
	if (strncmp(buf, "STATUS\t", 7) != 0) {
		unsupported_ino = st.st_ino;
		return 0;
	}

	char state[16] = {0}, model[32] = {0};
	unsigned long tok = 0, max_tok = 0, bytes = 0;
	if (sscanf(buf + 7, "%15[^\t]\t%31[^\t]\t%lu\t%lu\t%lu",
	           state, model, &tok, &max_tok, &bytes) < 3)
		return 0;

	unsupported_ino = 0;
	out->online = 1;
	out->busy = (strcmp(state, "busy") == 0);
	snprintf(out->model, sizeof(out->model), "%s", model);
	out->tokens = tok;
	out->max_tokens = max_tok;
	return 1;
}

static void
seg_ren(char *out, size_t len, const char *user)
{
	RenStatus rs;
	char tok_s[24];

	out[0] = '\0';
	if (!query_ren_status(user, &rs))
		return;

	format_ren_tokens(rs.tokens, tok_s, sizeof(tok_s));
	if (rs.busy) {
		snprintf(out, len, "%s%s    ren%s %.16s%s%s",
			C_VOC, C_WRN, C_VAL, rs.model, tok_s[0] ? " " : "", tok_s);
	} else {
		snprintf(out, len, "%s%s   %s %.16s%s%s",
			C_VOC, C_ICO, C_VAL, rs.model, tok_s[0] ? " " : "", tok_s);
	}
}

static void
seg_voice(char *out, size_t len, const char *user)
{
	char path[256], buf[160], jpid_path[256], jpid_buf[32];
	char apid_path[256], apid_buf[32];
	char ren_path[256], ren_buf[64], rpid_path[256], rpid_buf[32];
	char ren_tag[48] = "", extra_tags[96];
	int ren_working = 0, scr_rec = 0, acting = 0, jarvis_on = 0;

	out[0] = '\0';
	if (!has_bar_module(cfg.modules, "ren")) {
		RenStatus rs;
		if (query_ren_status(user, &rs)) {
			char tok_s[24];
			format_ren_tokens(rs.tokens, tok_s, sizeof(tok_s));
			if (rs.busy) {
				if (tok_s[0])
					snprintf(ren_tag, sizeof(ren_tag), "%s  ren %s%s", C_WRN, C_VAL, tok_s);
				else
					snprintf(ren_tag, sizeof(ren_tag), "%s  ren", C_WRN);
			} else if (tok_s[0]) {
				snprintf(ren_tag, sizeof(ren_tag), "%s %s %s", C_ICO, C_VAL, tok_s);
			}
		} else {
			snprintf(ren_path, sizeof(ren_path), "/tmp/dwm-ren-%s.state", user);
			if (read_small_file(ren_path, ren_buf, sizeof(ren_buf)) > 0 &&
			    !strncmp(ren_buf, "working", 7)) {
				ren_working = 1;
			}
			if (ren_working)
				snprintf(ren_tag, sizeof(ren_tag), "%s  ren", C_ICO);
		}
	}
	snprintf(rpid_path, sizeof(rpid_path), "/tmp/dwm-rec-%s.pid", user);
	if (read_small_file(rpid_path, rpid_buf, sizeof(rpid_buf)) > 0) {
		int rpid = atoi(rpid_buf);
		if (rpid > 1 && kill((pid_t)rpid, 0) == 0)
			scr_rec = 1;
	}
	snprintf(extra_tags, sizeof(extra_tags), "%s%s",
		scr_rec ? C_VOC "  REC" : "",
		ren_tag);

	snprintf(apid_path, sizeof(apid_path), "/tmp/jarvis-acting-%s.pid", user);
	if (read_small_file(apid_path, apid_buf, sizeof(apid_buf)) > 0) {
		int apid = atoi(apid_buf);
		if (apid > 1 && kill((pid_t)apid, 0) == 0)
			acting = 1;
	}

	snprintf(path, sizeof(path), "/tmp/jarvis-voice-%s.state", user);
	int tts_speaking = 0, processing = 0;
	if (read_small_file(path, buf, sizeof(buf)) > 0) {
		if (!strncmp(buf, "tts-speaking", 12)) {
			tts_speaking = 1;
			jarvis_on = 1;
		} else if (!strncmp(buf, "processing", 10)) {
			processing = 1;
			jarvis_on = 1;
		} else if (!strncmp(buf, "jarvis-cmd", 10) ||
		    !strncmp(buf, "cmd", 3) ||
		    !strncmp(buf, "acting", 6)) {
			acting = 1;
		} else if (!strncmp(buf, "jarvis", 6)) {
			jarvis_on = 1;
		}
	}

	snprintf(jpid_path, sizeof(jpid_path), "/tmp/jarvis-%s.pid", user);
	if (read_small_file(jpid_path, jpid_buf, sizeof(jpid_buf)) > 0) {
		int jpid = atoi(jpid_buf);
		if (jpid > 1 && kill((pid_t)jpid, 0) == 0)
			jarvis_on = 1;
	}

	if (tts_speaking) {
		/* Green robot icon: Jarvis is actively speaking */
		snprintf(out, len, "%s%s   %s", C_VOC, C_BOK, extra_tags);
	} else if (processing) {
		/* Blue/cyan robot icon: Jarvis is processing/executing */
		snprintf(out, len, "%s%s   %s", C_VOC, C_NET, extra_tags);
	} else if (acting) {
		/* Red robot icon: Jarvis heard wake word / capturing command */
		snprintf(out, len, "%s   %s", C_VOC, extra_tags);
	} else if (jarvis_on) {
		/* Yellow robot icon: Jarvis is On and listening for wake word */
		snprintf(out, len, "%s%s   %s", C_VOC, C_WRN, extra_tags);
	} else {
		/* Silver robot icon: Jarvis is Off (lockdown) */
		snprintf(out, len, "%s%s   %s", C_VOC, C_VAL, extra_tags);
	}
}

static snd_mixer_t *g_vol_mixer = NULL;
static snd_mixer_elem_t *g_vol_elem = NULL;

static void
vol_mixer_close(void)
{
	if (g_vol_mixer) {
		snd_mixer_close(g_vol_mixer);
		g_vol_mixer = NULL;
	}
	g_vol_elem = NULL;
}

static int
vol_mixer_ensure(void)
{
	if (g_vol_mixer && g_vol_elem)
		return 1;
	vol_mixer_close();
	if (snd_mixer_open(&g_vol_mixer, 0) < 0) {
		g_vol_mixer = NULL;
		return 0;
	}
	if (snd_mixer_attach(g_vol_mixer, "default") < 0 ||
	    snd_mixer_selem_register(g_vol_mixer, NULL, NULL) < 0 ||
	    snd_mixer_load(g_vol_mixer) < 0) {
		vol_mixer_close();
		return 0;
	}
	snd_mixer_selem_id_t *sid = NULL;
	snd_mixer_selem_id_alloca(&sid);
	snd_mixer_selem_id_set_index(sid, 0);
	snd_mixer_selem_id_set_name(sid, "Master");
	g_vol_elem = snd_mixer_find_selem(g_vol_mixer, sid);
	if (!g_vol_elem) {
		vol_mixer_close();
		return 0;
	}
	return 1;
}

static void
seg_vol(char *out, size_t len)
{
	long min = 0, max = 0, val = 0;
	int sw = 1;

	out[0] = '\0';
	for (int attempt = 0; attempt < 2; attempt++) {
		if (!vol_mixer_ensure())
			return;
		if (snd_mixer_handle_events(g_vol_mixer) < 0 ||
		    snd_mixer_selem_get_playback_volume_range(g_vol_elem, &min, &max) < 0 ||
		    snd_mixer_selem_get_playback_volume(g_vol_elem, SND_MIXER_SCHN_FRONT_LEFT, &val) < 0 ||
		    snd_mixer_selem_get_playback_switch(g_vol_elem, SND_MIXER_SCHN_FRONT_LEFT, &sw) < 0) {
			vol_mixer_close();
			continue;
		}
		int pct = (max > min) ? (int)(((val - min) * 100 + (max - min) / 2) / (max - min)) : 0;
		if (!sw) {
			snprintf(out, len, "%s    ×", C_VMU);
		} else {
			const char *glyph = (pct == 0) ? "" : (pct < 40 ? "" : "");
			snprintf(out, len, "%s   %s%s %d%%", C_VOL, glyph, C_VAL, pct);
		}
		return;
	}
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
seg_kbd(Display *dpy, Window root, Atom xkb_rules_atom, const char *user, char *out, size_t len)
{
	static char last_kbd[16] = "";
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
	if (strcmp(layout_up, last_kbd) != 0) {
		char kpath[256], ktmp[272];
		snprintf(last_kbd, sizeof(last_kbd), "%s", layout_up);
		snprintf(kpath, sizeof(kpath), "/tmp/dwm-kbd-%s.state", user);
		snprintf(ktmp, sizeof(ktmp), "%s.tmp", kpath);
		FILE *fp = fopen(ktmp, "w");
		if (fp) {
			for (size_t i = 0; layout_up[i]; i++)
				fputc(tolower((unsigned char)layout_up[i]), fp);
			fputc('\n', fp);
			fclose(fp);
			rename(ktmp, kpath);
		}
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
	int ioctl_sock = -1;
	const char *user = getenv("USER");
	char pid_path[256], last_status[512] = "";
	time_t last_probe = 0, last_cal_check = 0, last_cal_sync = 0;
	int cached_tty_up = 1;

	if (!user || !*user) {
		struct passwd *pw = getpwuid(getuid());
		user = (pw && pw->pw_name && *pw->pw_name) ? pw->pw_name : "user";
	}
	{
		const char *home = getenv("HOME");
		const char *cur_path = getenv("PATH");
		if (home && *home) {
			char lbin[512];
			snprintf(lbin, sizeof(lbin), "%s/.local/bin", home);
			if (!cur_path || !*cur_path)
				setenv("PATH", lbin, 1);
			else if (!strstr(cur_path, lbin)) {
				char npath[4096];
				snprintf(npath, sizeof(npath), "%s:%s", lbin, cur_path);
				setenv("PATH", npath, 1);
			}
		}
	}

	/* Write PID file so dwm-ctl / dwm-voice can send SIGUSR1 */
	snprintf(pid_path, sizeof(pid_path), "/tmp/dwm-bar-%s.pid", user);
	{
		FILE *pf = fopen(pid_path, "w");
		if (pf) {
			fprintf(pf, "%d\n", (int)getpid());
			fclose(pf);
		}
	}

	if (pipe(sig_pipe) == 0) {
		for (int i = 0; i < 2; i++) {
			fcntl(sig_pipe[i], F_SETFL, O_NONBLOCK);
			fcntl(sig_pipe[i], F_SETFD, FD_CLOEXEC);
		}
	}

	{
		struct sigaction sa;
		memset(&sa, 0, sizeof(sa));
		sa.sa_handler = on_sigusr1;
		sigemptyset(&sa.sa_mask);
		sa.sa_flags = SA_RESTART;
		sigaction(SIGUSR1, &sa, NULL);
	}
	signal(SIGTERM, on_sigterm);
	signal(SIGINT, on_sigterm);
	signal(SIGCHLD, SIG_IGN);
	signal(SIGPIPE, SIG_IGN);

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

	ioctl_sock = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);

	while (running) {
		drain_sig_pipe();
		time_t now = time(NULL);
		struct tm tm_now;
		localtime_r(&now, &tm_now);

		/* Background calendar sync & upcoming check (same cadence as before) */
		if (now - last_cal_sync >= 14400) {
			last_cal_sync = now;
			if (fork() == 0) {
				close(ConnectionNumber(dpy));
				execlp("dwm-cal", "dwm-cal", "sync", "--quiet", (char *)NULL);
				_exit(0);
			}
		} else if (now - last_cal_check >= 60) {
			last_cal_check = now;
			if (fork() == 0) {
				close(ConnectionNumber(dpy));
				execlp("dwm-cal", "dwm-cal", "refresh-upcoming", (char *)NULL);
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
				seg_host(seg, sizeof(seg), user, now, &last_probe, &cached_tty_up, ConnectionNumber(dpy));
			else if (!strcmp(tok, "mem"))
				seg_mem(seg, sizeof(seg));
			else if (!strcmp(tok, "cpu"))
				seg_cpu(seg, sizeof(seg));
			else if (!strcmp(tok, "voice"))
				seg_voice(seg, sizeof(seg), user);
			else if (!strcmp(tok, "ren"))
				seg_ren(seg, sizeof(seg), user);
			else if (!strcmp(tok, "vol"))
				seg_vol(seg, sizeof(seg));
			else if (!strcmp(tok, "net"))
				seg_net(seg, sizeof(seg), ioctl_sock);
			else if (!strcmp(tok, "bat"))
				seg_bat(seg, sizeof(seg));
			else if (!strcmp(tok, "kbd"))
				seg_kbd(dpy, root, xkb_rules_atom, user, seg, sizeof(seg));
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

		if (strcmp(status, last_status) != 0) {
			snprintf(last_status, sizeof(last_status), "%s", status);
			XStoreName(dpy, root, status);
			XFlush(dpy);
		}

		/* Wait until next interval OR immediate XKB / XRDB / SIGUSR1 wakeup.
		 * Self-pipe guarantees zero lost signals even if SIGUSR1 arrives mid-render.
		 * Self-generated WM_NAME PropertyNotify events are drained without re-rendering.
		 */
		struct timespec deadline_ts;
		clock_gettime(CLOCK_MONOTONIC, &deadline_ts);
		deadline_ts.tv_sec += cfg.interval_sec;

		while (running) {
			struct timespec cur_ts;
			clock_gettime(CLOCK_MONOTONIC, &cur_ts);
			long rem_ms = (deadline_ts.tv_sec - cur_ts.tv_sec) * 1000L
			            + (deadline_ts.tv_nsec - cur_ts.tv_nsec) / 1000000L;
			if (rem_ms <= 0)
				break;

			struct pollfd pfds[2];
			pfds[0].fd = ConnectionNumber(dpy);
			pfds[0].events = POLLIN;
			pfds[0].revents = 0;
			pfds[1].fd = sig_pipe[0];
			pfds[1].events = POLLIN;
			pfds[1].revents = 0;

			int nfds = (sig_pipe[0] >= 0) ? 2 : 1;
			int ret = poll(pfds, (nfds_t)nfds, (int)rem_ms);
			if (ret == 0)
				break;
			if (ret < 0) {
				if (errno == EINTR)
					continue;
				break;
			}

			int need_redraw = 0;
			if (nfds == 2 && (pfds[1].revents & POLLIN))
				need_redraw = 1;

			if (pfds[0].revents & POLLIN) {
				while (XPending(dpy)) {
					XEvent ev;
					XNextEvent(dpy, &ev);
					if (ev.type == PropertyNotify && ev.xproperty.window == root) {
						if (ev.xproperty.atom == XA_RESOURCE_MANAGER) {
							load_bar_xrdb(dpy);
							need_redraw = 1;
						} else if (ev.xproperty.atom == xkb_rules_atom) {
							need_redraw = 1;
						}
					} else if (ev.type == xkb_ev_base) {
						need_redraw = 1;
					}
				}
			}
			if (need_redraw)
				break;
		}
	}

	vol_mixer_close();
	if (sig_pipe[0] >= 0)
		close(sig_pipe[0]);
	if (sig_pipe[1] >= 0)
		close(sig_pipe[1]);
	if (ioctl_sock >= 0)
		close(ioctl_sock);
	XCloseDisplay(dpy);
	return 0;
}
