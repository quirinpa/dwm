/* See LICENSE file for copyright and license details.
 * dwm-whisperd: Unified real-time streaming PipeWire + libwhisper C daemon
 * with persistent circular float32 PCM ring buffer, dedicated non-blocking
 * background decode worker thread with ggml abort_callback, live status-bar
 * and OSD partial transcription, and 0ms speculative silence endpointing.
 */
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "whisper.h"

#define SAMPLE_RATE      16000
#define FRAME_MS         20
#define FRAME_SAMPLES    ((SAMPLE_RATE * FRAME_MS) / 1000) /* 320 samples */
#define FRAME_BYTES      (FRAME_SAMPLES * (int)sizeof(int16_t))
#define RING_FRAMES      2000 /* 40 seconds circular float32 PCM buffer */
#define RING_SAMPLES     (RING_FRAMES * FRAME_SAMPLES)
#define MAX_UTTER_FRAMES 1500 /* 30 seconds max utterance */
#define WARM_HOLD_MS     60000L

static volatile sig_atomic_t running          = 1;
static volatile sig_atomic_t g_stop_req       = 0;
static volatile sig_atomic_t g_cancel_req     = 0;
static volatile sig_atomic_t cli_stop_req     = 0;
static volatile sig_atomic_t cli_cancel_req   = 0;

static int  g_preempt_cli = -1;
static char g_preempt_buf[2048];

/* Shared PipeWire capture ring buffer */
static struct {
	pthread_mutex_t mtx;
	pthread_cond_t  cond;
	float           pcm[RING_SAMPLES];
	float           db[RING_FRAMES];
	int64_t         total_frames;
	double          noise_floor_db;
	int             env_frames;
	int             active_users;
	long            hold_until_ms;
	int             in_speech_hint;
	int             pw_open;
} g_audio = {
	.mtx            = PTHREAD_MUTEX_INITIALIZER,
	.cond           = PTHREAD_COND_INITIALIZER,
	.total_frames   = 0,
	.noise_floor_db = -48.0,
	.env_frames     = 0,
	.active_users   = 0,
	.hold_until_ms  = 0,
	.in_speech_hint = 0,
	.pw_open        = 0
};

static const char *WAKE_INITIAL_PROMPT = "Jarvis.";
static const char *CMD_INITIAL_PROMPT =
	"Go to workspace 1, 2, 3, 4, 5, and then focus browser, firefox, ghostty, WhatsApp. "
	"Move window to workspace 1, 2, 3, 4, 5. "
	"Open a new tab, close tab, next tab, type, press enter, escape, tab, control c, "
	"click, double click, right click, scroll down, scroll up, "
	"mute volume, volume up, volume down, toggle fullscreen, close window, show pi, stop pi, reset pi.";

/* Non-blocking background Whisper decode worker for ctx_main */
static struct {
	pthread_mutex_t       mtx;
	pthread_cond_t        cond;
	struct whisper_context *ctx;
	int                   n_threads;
	float                *pcm;
	int                   n_samples;
	char                  lang[32];
	const char           *prompt;
	int                   max_tok;
	int                   is_partial;
	int64_t               job_voiced_f;
	int                   has_job;
	int                   busy;
	volatile sig_atomic_t abort_flag;
	int                   has_result;
	char                  result_text[4096];
	int64_t               result_voiced_f;
	int                   result_is_partial;
	int                   result_all_silent;
	int64_t               done_at_frame;
} g_dec = {
	.mtx               = PTHREAD_MUTEX_INITIALIZER,
	.cond              = PTHREAD_COND_INITIALIZER,
	.ctx               = NULL,
	.n_threads         = 6,
	.pcm               = NULL,
	.n_samples         = 0,
	.prompt            = NULL,
	.has_job           = 0,
	.busy              = 0,
	.abort_flag        = 0,
	.has_result        = 0,
	.result_all_silent = 0,
	.done_at_frame     = 0
};

static void
on_sig(int sig)
{
	(void)sig;
	running = 0;
	g_dec.abort_flag = 1;
	pthread_cond_broadcast(&g_audio.cond);
	pthread_cond_broadcast(&g_dec.cond);
}

static void
on_cli_sigint(int sig)
{
	(void)sig;
	cli_stop_req = 1;
}

static void
on_cli_sigusr1(int sig)
{
	(void)sig;
	cli_cancel_req = 1;
}

static void
quiet_log(enum ggml_log_level level, const char *text, void *user_data)
{
	(void)level;
	(void)text;
	(void)user_data;
}

static bool
whisper_abort_cb(void *user_data)
{
	(void)user_data;
	return (!running || g_cancel_req || g_dec.abort_flag) ? true : false;
}

static long
now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long)ts.tv_sec * 1000L + (long)(ts.tv_nsec / 1000000L);
}

static void
write_all(int fd, const char *buf, size_t len)
{
	while (len > 0) {
		ssize_t w = write(fd, buf, len);
		if (w <= 0)
			break;
		buf += w;
		len -= (size_t)w;
	}
}

static const char *
get_username(void)
{
	const char *u = getenv("USER");
	return (u && *u) ? u : "quirinpa";
}

static void
notify_bar(void)
{
	char path[256];
	FILE *fp;
	int pid = 0;

	snprintf(path, sizeof(path), "/tmp/dwm-bar-%s.pid", get_username());
	fp = fopen(path, "r");
	if (!fp)
		return;
	if (fscanf(fp, "%d", &pid) == 1 && pid > 1)
		kill((pid_t)pid, SIGUSR1);
	fclose(fp);
}

static void
notify_osd_async(const char *title, const char *body, int timeout_ms)
{
	if (!title || !*title)
		return;
	if (fork() == 0) {
		char tbuf[32];
		int fd = open("/dev/null", O_WRONLY);
		if (fd >= 0) {
			dup2(fd, STDOUT_FILENO);
			dup2(fd, STDERR_FILENO);
			close(fd);
		}
		snprintf(tbuf, sizeof(tbuf), "%d", timeout_ms > 0 ? timeout_ms : 4000);
		execlp("notify-send", "notify-send",
		       "-t", tbuf,
		       "-h", "string:x-dunst-stack-tag:voice",
		       title, (body && *body) ? body : "", (char *)NULL);
		_exit(0);
	}
}

static void
write_state_file(const char *path, const char *prefix, const char *detail, const char *osd_title)
{
	char tmp[512];
	FILE *fp;

	if (!path || !*path || strcmp(path, "-") == 0)
		return;
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	fp = fopen(tmp, "wb");
	if (!fp)
		return;
	if (detail && *detail)
		fprintf(fp, "%s:%s\n", prefix, detail);
	else
		fprintf(fp, "%s\n", prefix);
	fclose(fp);
	rename(tmp, path);
	notify_bar();

	if (osd_title && *osd_title && detail && *detail)
		notify_osd_async(osd_title, detail, 6000);
}

static void
play_chime_async(const char *chime_wav)
{
	if (!chime_wav || !*chime_wav || strcmp(chime_wav, "-") == 0)
		return;
	if (access(chime_wav, R_OK) != 0)
		return;
	if (fork() == 0) {
		int fd = open("/dev/null", O_WRONLY);
		if (fd >= 0) {
			dup2(fd, STDOUT_FILENO);
			dup2(fd, STDERR_FILENO);
			close(fd);
		}
		execlp("pw-play", "pw-play", "--volume=0.45", chime_wav, (char *)NULL);
		_exit(0);
	}
}

/* Dynamically resolve "kbd" language from /tmp/dwm-kbd-$USER.state so switching
 * X11 keyboard layout (Mod+Space) updates Whisper language mid-flight.
 */
static void
resolve_lang(const char *req_lang, char *out, size_t out_sz)
{
	if (!req_lang || !*req_lang || strcmp(req_lang, "kbd") == 0) {
		char path[256], buf[32] = {0};
		snprintf(path, sizeof(path), "/tmp/dwm-kbd-%s.state", get_username());
		FILE *fp = fopen(path, "r");
		if (fp) {
			if (fgets(buf, sizeof(buf), fp)) {
				for (size_t i = 0; buf[i]; i++)
					buf[i] = (char)tolower((unsigned char)buf[i]);
				if (strncmp(buf, "pt", 2) == 0) {
					snprintf(out, out_sz, "pt");
					fclose(fp);
					return;
				}
			}
			fclose(fp);
		}
		snprintf(out, out_sz, "en");
		return;
	}
	snprintf(out, out_sz, "%s", req_lang);
}

static double
calc_frame_db(const int16_t *samples, size_t n)
{
	double sum = 0.0;
	size_t i;

	if (!n)
		return -100.0;
	for (i = 0; i < n; i++) {
		double s = (double)samples[i] / 32768.0;
		sum += s * s;
	}
	double rms = sqrt(sum / (double)n);
	if (rms < 1e-5)
		return -100.0;
	return 20.0 * log10(rms);
}

static int
spawn_pw_record(pid_t *out_pid)
{
	int pfd[2];
	if (pipe(pfd) < 0)
		return -1;
	pid_t pid = fork();
	if (pid < 0) {
		close(pfd[0]);
		close(pfd[1]);
		return -1;
	}
	if (pid == 0) {
		close(pfd[0]);
		dup2(pfd[1], STDOUT_FILENO);
		close(pfd[1]);
		int devnull = open("/dev/null", O_WRONLY);
		if (devnull >= 0) {
			dup2(devnull, STDERR_FILENO);
			close(devnull);
		}
		execlp("pw-record", "pw-record", "--raw", "--rate=16000",
		       "--channels=1", "--format=s16", "--latency=20ms", "-", (char *)NULL);
		_exit(127);
	}
	close(pfd[1]);
	*out_pid = pid;
	return pfd[0];
}

static void
close_pw_record(int *fd, pid_t *pid)
{
	if (*fd >= 0) {
		close(*fd);
		*fd = -1;
	}
	if (*pid > 0) {
		kill(*pid, SIGTERM);
		*pid = -1;
	}
}

static void *
audio_thread_fn(void *arg)
{
	int pw_fd = -1;
	pid_t pw_pid = -1;
	int16_t raw[FRAME_SAMPLES];

	(void)arg;
	while (running) {
		pthread_mutex_lock(&g_audio.mtx);
		int need = (g_audio.active_users > 0) || (now_ms() < g_audio.hold_until_ms);
		if (!need) {
			if (pw_fd >= 0) {
				g_audio.pw_open = 0;
				pthread_mutex_unlock(&g_audio.mtx);
				close_pw_record(&pw_fd, &pw_pid);
				pthread_mutex_lock(&g_audio.mtx);
			}
			while (running && g_audio.active_users <= 0 && now_ms() >= g_audio.hold_until_ms)
				pthread_cond_wait(&g_audio.cond, &g_audio.mtx);
			pthread_mutex_unlock(&g_audio.mtx);
			continue;
		}
		pthread_mutex_unlock(&g_audio.mtx);

		if (pw_fd < 0) {
			pw_fd = spawn_pw_record(&pw_pid);
			if (pw_fd < 0) {
				struct timespec ts = {0, 50000000L};
				nanosleep(&ts, NULL);
				continue;
			}
			pthread_mutex_lock(&g_audio.mtx);
			g_audio.pw_open = 1;
			pthread_cond_broadcast(&g_audio.cond);
			pthread_mutex_unlock(&g_audio.mtx);
		}

		size_t got = 0;
		while (got < (size_t)FRAME_BYTES) {
			ssize_t r = read(pw_fd, (char *)raw + got, (size_t)FRAME_BYTES - got);
			if (r <= 0) {
				if (r < 0 && errno == EINTR && running)
					continue;
				break;
			}
			got += (size_t)r;
		}
		if (got < (size_t)FRAME_BYTES) {
			close_pw_record(&pw_fd, &pw_pid);
			pthread_mutex_lock(&g_audio.mtx);
			g_audio.pw_open = 0;
			pthread_mutex_unlock(&g_audio.mtx);
			struct timespec ts = {0, 20000000L};
			nanosleep(&ts, NULL);
			continue;
		}

		double db = calc_frame_db(raw, FRAME_SAMPLES);

		pthread_mutex_lock(&g_audio.mtx);
		int slot = (int)(g_audio.total_frames % RING_FRAMES);
		float *dst = g_audio.pcm + (size_t)slot * FRAME_SAMPLES;
		for (int i = 0; i < FRAME_SAMPLES; i++)
			dst[i] = (float)raw[i] / 32768.0f;
		g_audio.db[slot] = (float)db;

		/* Adaptive noise floor tracker:
		 *  - Fast downward tracking (0.80 / 0.20) when ambient gets quieter
		 *  - Slow upward tracking (0.96 / 0.04) when idle and below -20 dB
		 *  - Gentle upward drift (0.992 / 0.008) even if in_speech_hint is stuck
		 *    so a noisy room or fan never permanently locks VAD in speech!
		 */
		if (db < g_audio.noise_floor_db) {
			g_audio.noise_floor_db = 0.80 * g_audio.noise_floor_db + 0.20 * db;
		} else if (!g_audio.in_speech_hint && db < -20.0) {
			if (g_audio.env_frames < 15) {
				g_audio.noise_floor_db = 0.75 * g_audio.noise_floor_db + 0.25 * db;
				g_audio.env_frames++;
			} else if (db < g_audio.noise_floor_db + 8.0) {
				g_audio.noise_floor_db = 0.95 * g_audio.noise_floor_db + 0.05 * db;
			}
		} else if (g_audio.in_speech_hint && db < -24.0) {
			g_audio.noise_floor_db = 0.992 * g_audio.noise_floor_db + 0.008 * db;
		}

		if (g_audio.noise_floor_db > -25.0)
			g_audio.noise_floor_db = -25.0;
		if (g_audio.noise_floor_db < -62.0)
			g_audio.noise_floor_db = -62.0;

		g_audio.total_frames++;
		pthread_cond_broadcast(&g_audio.cond);
		pthread_mutex_unlock(&g_audio.mtx);
	}

	close_pw_record(&pw_fd, &pw_pid);
	return NULL;
}

static void
audio_acquire(void)
{
	pthread_mutex_lock(&g_audio.mtx);
	g_audio.active_users++;
	int64_t f0 = g_audio.total_frames;
	pthread_cond_broadcast(&g_audio.cond);

	long deadline = now_ms() + 350L;
	while (running && !g_audio.pw_open && g_audio.total_frames == f0) {
		long rem = deadline - now_ms();
		if (rem <= 0)
			break;
		struct timespec ts;
		clock_gettime(CLOCK_REALTIME, &ts);
		ts.tv_nsec += (rem > 25 ? 25 : rem) * 1000000L;
		if (ts.tv_nsec >= 1000000000L) {
			ts.tv_sec += 1;
			ts.tv_nsec -= 1000000000L;
		}
		pthread_cond_timedwait(&g_audio.cond, &g_audio.mtx, &ts);
	}
	pthread_mutex_unlock(&g_audio.mtx);
}

static void
audio_release(long hold_ms)
{
	pthread_mutex_lock(&g_audio.mtx);
	if (g_audio.active_users > 0)
		g_audio.active_users--;
	g_audio.in_speech_hint = 0;
	long until = now_ms() + hold_ms;
	if (until > g_audio.hold_until_ms)
		g_audio.hold_until_ms = until;
	pthread_cond_broadcast(&g_audio.cond);
	pthread_mutex_unlock(&g_audio.mtx);
}

static int
ring_extract_pcm(int64_t start_f, int64_t end_f, float *out_pcm, int max_samples)
{
	int n_samples = 0;

	pthread_mutex_lock(&g_audio.mtx);
	if (end_f > g_audio.total_frames)
		end_f = g_audio.total_frames;
	if (end_f - start_f > RING_FRAMES - 4)
		start_f = end_f - (RING_FRAMES - 4);
	if (start_f < 0)
		start_f = 0;

	for (int64_t f = start_f; f < end_f; f++) {
		if (n_samples + FRAME_SAMPLES > max_samples)
			break;
		int slot = (int)(f % RING_FRAMES);
		memcpy(out_pcm + n_samples, g_audio.pcm + (size_t)slot * FRAME_SAMPLES,
		       (size_t)FRAME_SAMPLES * sizeof(float));
		n_samples += FRAME_SAMPLES;
	}
	pthread_mutex_unlock(&g_audio.mtx);
	return n_samples;
}

static int
load_wav_f32(const char *path, float **out_pcm, int *out_samples)
{
	FILE *fp = fopen(path, "rb");
	if (!fp)
		return -1;

	if (fseek(fp, 0, SEEK_END) != 0) {
		fclose(fp);
		return -1;
	}
	long fsize = ftell(fp);
	if (fsize <= 44 || fsize > 64 * 1024 * 1024) {
		fclose(fp);
		return -1;
	}
	if (fseek(fp, 44, SEEK_SET) != 0) {
		fclose(fp);
		return -1;
	}

	size_t pcm_bytes = (size_t)(fsize - 44);
	int n_samples = (int)(pcm_bytes / sizeof(int16_t));
	int16_t *raw = malloc((size_t)n_samples * sizeof(int16_t));
	float *f32 = malloc((size_t)n_samples * sizeof(float));
	if (!raw || !f32) {
		free(raw);
		free(f32);
		fclose(fp);
		return -1;
	}

	size_t n_read = fread(raw, sizeof(int16_t), (size_t)n_samples, fp);
	fclose(fp);
	n_samples = (int)n_read;

	for (int i = 0; i < n_samples; i++)
		f32[i] = (float)raw[i] / 32768.0f;

	free(raw);
	*out_pcm = f32;
	*out_samples = n_samples;
	return 0;
}

static int
compute_audio_ctx(int n_samples, int is_wake, int is_partial)
{
	int frames = (n_samples / 320) + (is_partial ? 32 : 36);
	int aligned = ((frames + 31) / 32) * 32;
	int min_ctx = is_wake ? 96 : 128;
	if (aligned < min_ctx)
		aligned = min_ctx;
	if (aligned > 1500)
		aligned = 1500;
	return aligned;
}

static void
trim_inplace(char *s)
{
	for (char *c = s; *c; c++) {
		if (*c == '\r' || *c == '\n' || *c == '\t')
			*c = ' ';
	}
	char *p = s;
	while (*p && isspace((unsigned char)*p))
		p++;
	if (p != s)
		memmove(s, p, strlen(p) + 1);
	size_t len = strlen(s);
	while (len > 0 && isspace((unsigned char)s[len - 1]))
		s[--len] = '\0';
}

/* Collapse immediate repeated word sequences (1..8 words) in-place so a noisy
 * clip never outputs "Go to workspace 1 Go to workspace 1".
 */
static void
collapse_repetitions(char *s)
{
	char copy[4096];
	char *words[256];
	int n_words = 0;

	if (!s || !*s)
		return;
	snprintf(copy, sizeof(copy), "%s", s);

	char *save = NULL;
	for (char *tok = strtok_r(copy, " ", &save); tok && n_words < 256;
	     tok = strtok_r(NULL, " ", &save)) {
		words[n_words++] = tok;
	}
	if (n_words < 2)
		return;

	int keep[256];
	for (int i = 0; i < n_words; i++)
		keep[i] = 1;

	for (int span = 8; span >= 1; span--) {
		for (int i = 0; i + 2 * span <= n_words; ) {
			if (!keep[i]) {
				i++;
				continue;
			}
			int match = 1;
			for (int k = 0; k < span; k++) {
				const char *a = words[i + k];
				const char *b = words[i + span + k];
				size_t la = strlen(a), lb = strlen(b);
				while (la > 0 && ispunct((unsigned char)a[la - 1]))
					la--;
				while (lb > 0 && ispunct((unsigned char)b[lb - 1]))
					lb--;
				if (la == 0 || la != lb || strncasecmp(a, b, la) != 0) {
					match = 0;
					break;
				}
			}
			if (match && (span >= 2 || strlen(words[i]) >= 4)) {
				for (int k = 0; k < span; k++)
					keep[i + span + k] = 0;
				/* Shift compactly and re-check for 3+ repetitions */
				int dst = 0;
				for (int j = 0; j < n_words; j++) {
					if (keep[j]) {
						words[dst] = words[j];
						keep[dst] = 1;
						dst++;
					}
				}
				n_words = dst;
			} else {
				i++;
			}
		}
	}

	s[0] = '\0';
	for (int i = 0; i < n_words; i++) {
		if (i > 0)
			strcat(s, " ");
		strcat(s, words[i]);
	}
}

static int
is_hallucination(const char *s)
{
	while (*s && isspace((unsigned char)*s))
		s++;
	if (!*s)
		return 1;
	if (*s == '[' || *s == '(' || *s == '*')
		return 1;

	int has_alpha = 0;
	for (const char *p = s; *p; p++) {
		if (isalpha((unsigned char)*p) || ((unsigned char)*p >= 0xC0)) {
			has_alpha = 1;
			break;
		}
	}
	if (!has_alpha)
		return 1;

	if (strcasecmp(s, "you") == 0 || strcasecmp(s, "you.") == 0 ||
	    strcasecmp(s, "thank you.") == 0 || strcasecmp(s, "thank you") == 0 ||
	    strcasecmp(s, "thanks for watching!") == 0 || strcasecmp(s, "thanks for watching.") == 0 ||
	    strcasecmp(s, "obrigado.") == 0 || strcasecmp(s, "obrigado") == 0 ||
	    strcasecmp(s, "legenda") == 0 || strcasecmp(s, "legendas") == 0 ||
	    strcasecmp(s, "subtitles") == 0 || strcasecmp(s, "the") == 0)
		return 1;
	return 0;
}

static void
run_whisper_slice(struct whisper_context *ctx, int n_threads, const float *pcm, int n_samples,
                  const char *lang, const char *prompt, int max_tok, int is_wake, int is_partial,
                  int use_abort_cb, char *out_text, size_t out_sz, int *out_all_silent)
{
	char eff_lang[32];

	out_text[0] = '\0';
	if (out_all_silent)
		*out_all_silent = 0;
	if (n_samples < 1600)
		return;

	resolve_lang(lang, eff_lang, sizeof(eff_lang));

	if (is_partial) {
		int cap = (n_samples / 2200) + 6;
		if (cap < max_tok)
			max_tok = cap;
	}

	struct whisper_full_params wparams = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
	wparams.n_threads        = n_threads;
	wparams.single_segment   = false;
	wparams.no_context       = true;
	wparams.no_timestamps    = false;
	wparams.suppress_blank   = true;
	wparams.suppress_nst     = true;
	wparams.temperature      = 0.0f;
	wparams.temperature_inc  = 0.0f;
	wparams.greedy.best_of   = 1;
	wparams.max_tokens       = max_tok;
	wparams.audio_ctx        = compute_audio_ctx(n_samples, is_wake, is_partial);

	if (use_abort_cb) {
		wparams.abort_callback = whisper_abort_cb;
		wparams.abort_callback_user_data = NULL;
	}

	if (!eff_lang[0] || strcmp(eff_lang, "auto") == 0) {
		wparams.language = "auto";
		wparams.detect_language = false;
	} else {
		wparams.language = eff_lang;
	}

	if (prompt && *prompt && strcmp(prompt, "-") != 0)
		wparams.initial_prompt = prompt;

	if (whisper_full(ctx, wparams, pcm, n_samples) == 0) {
		int n_seg = whisper_full_n_segments(ctx);
		int valid_seg = 0;
		char prev_seg[1024] = {0};
		for (int i = 0; i < n_seg; i++) {
			float nsp = whisper_full_get_segment_no_speech_prob(ctx, i);
			if (nsp > 0.72f)
				continue;
			const char *seg = whisper_full_get_segment_text(ctx, i);
			if (!seg)
				continue;
			char seg_clean[1024];
			snprintf(seg_clean, sizeof(seg_clean), "%s", seg);
			trim_inplace(seg_clean);
			if (is_hallucination(seg_clean))
				continue;
			if (prev_seg[0] != '\0' && strcasecmp(prev_seg, seg_clean) == 0)
				continue;
			snprintf(prev_seg, sizeof(prev_seg), "%s", seg_clean);
			valid_seg++;
			if (out_text[0] != '\0')
				strncat(out_text, " ", out_sz - strlen(out_text) - 1);
			strncat(out_text, seg_clean, out_sz - strlen(out_text) - 1);
		}
		if (out_all_silent && valid_seg == 0)
			*out_all_silent = 1;
	}
	trim_inplace(out_text);
	collapse_repetitions(out_text);
}

/* Dedicated background worker thread for ctx_main so the 20ms PipeWire VAD loop
 * never blocks during speculative or final Whisper passes.
 */
static void *
decode_worker_fn(void *arg)
{
	(void)arg;
	while (running) {
		pthread_mutex_lock(&g_dec.mtx);
		while (running && !g_dec.has_job)
			pthread_cond_wait(&g_dec.cond, &g_dec.mtx);
		if (!running) {
			pthread_mutex_unlock(&g_dec.mtx);
			break;
		}

		g_dec.has_job = 0;
		g_dec.busy = 1;
		g_dec.abort_flag = 0;
		int n_samples = g_dec.n_samples;
		char lang[32];
		snprintf(lang, sizeof(lang), "%s", g_dec.lang);
		const char *prompt = g_dec.prompt;
		int max_tok = g_dec.max_tok;
		int is_partial = g_dec.is_partial;
		int64_t job_vf = g_dec.job_voiced_f;
		pthread_mutex_unlock(&g_dec.mtx);

		char decoded[4096] = {0};
		int all_silent = 0;
		run_whisper_slice(g_dec.ctx, g_dec.n_threads, g_dec.pcm, n_samples,
		                  lang, prompt, max_tok, 0, is_partial, 1,
		                  decoded, sizeof(decoded), &all_silent);

		pthread_mutex_lock(&g_audio.mtx);
		int64_t cur_audio_f = g_audio.total_frames;
		pthread_mutex_unlock(&g_audio.mtx);

		pthread_mutex_lock(&g_dec.mtx);
		g_dec.busy = 0;
		g_dec.done_at_frame = cur_audio_f;
		if (!g_dec.abort_flag && !g_cancel_req) {
			snprintf(g_dec.result_text, sizeof(g_dec.result_text), "%s", decoded);
			g_dec.result_voiced_f = job_vf;
			g_dec.result_is_partial = is_partial;
			g_dec.result_all_silent = all_silent;
			g_dec.has_result = 1;
		}
		pthread_cond_broadcast(&g_dec.cond);
		pthread_mutex_unlock(&g_dec.mtx);
	}
	return NULL;
}

static void
decode_reset_session(void)
{
	pthread_mutex_lock(&g_dec.mtx);
	g_dec.has_job = 0;
	g_dec.abort_flag = 1;
	while (g_dec.busy)
		pthread_cond_wait(&g_dec.cond, &g_dec.mtx);
	g_dec.abort_flag = 0;
	g_dec.has_result = 0;
	g_dec.result_text[0] = '\0';
	g_dec.result_voiced_f = -1;
	g_dec.result_all_silent = 0;
	g_dec.done_at_frame = 0;
	pthread_mutex_unlock(&g_dec.mtx);
}

static int
decode_try_submit(int64_t start_f, int64_t end_f, int64_t voiced_f,
                  const char *lang, const char *prompt, int max_tok, int is_partial)
{
	pthread_mutex_lock(&g_dec.mtx);
	if (g_dec.busy || g_dec.has_job) {
		pthread_mutex_unlock(&g_dec.mtx);
		return 0;
	}
	int n_samp = ring_extract_pcm(start_f, end_f, g_dec.pcm,
	                              MAX_UTTER_FRAMES * FRAME_SAMPLES);
	if (n_samp < 2400) {
		pthread_mutex_unlock(&g_dec.mtx);
		return 0;
	}
	g_dec.n_samples = n_samp;
	snprintf(g_dec.lang, sizeof(g_dec.lang), "%s", lang ? lang : "kbd");
	g_dec.prompt = prompt;
	g_dec.max_tok = max_tok;
	g_dec.is_partial = is_partial;
	g_dec.job_voiced_f = voiced_f;
	g_dec.abort_flag = 0;
	g_dec.has_job = 1;
	pthread_cond_broadcast(&g_dec.cond);
	pthread_mutex_unlock(&g_dec.mtx);
	return 1;
}

static void
decode_force_submit_final(int64_t start_f, int64_t end_f, int64_t voiced_f,
                          const char *lang, const char *prompt, int max_tok)
{
	pthread_mutex_lock(&g_dec.mtx);
	if (g_dec.busy) {
		if (!g_dec.is_partial && g_dec.job_voiced_f >= voiced_f - 1) {
			pthread_mutex_unlock(&g_dec.mtx);
			return;
		}
		g_dec.abort_flag = 1;
		while (g_dec.busy)
			pthread_cond_wait(&g_dec.cond, &g_dec.mtx);
	}
	g_dec.has_job = 0;
	g_dec.has_result = 0;
	int n_samp = ring_extract_pcm(start_f, end_f, g_dec.pcm,
	                              MAX_UTTER_FRAMES * FRAME_SAMPLES);
	if (n_samp >= 2400) {
		g_dec.n_samples = n_samp;
		snprintf(g_dec.lang, sizeof(g_dec.lang), "%s", lang ? lang : "kbd");
		g_dec.prompt = prompt;
		g_dec.max_tok = max_tok;
		g_dec.is_partial = 0;
		g_dec.job_voiced_f = voiced_f;
		g_dec.abort_flag = 0;
		g_dec.has_job = 1;
		pthread_cond_broadcast(&g_dec.cond);
	}
	pthread_mutex_unlock(&g_dec.mtx);
}

static int
is_punct_or_space(char c)
{
	return isspace((unsigned char)c) ||
	       c == ',' || c == '.' || c == '!' || c == '?' ||
	       c == ';' || c == ':' || c == '-' || c == '"' || c == '\'';
}

/* Pure-C wake-word prefix matcher.
 * Strips all repeated leading greetings + wake words in a loop so
 * "Jarvis! Jarvis! Jarvis" or "Hey Jarvis, Jarvis" never looks like a command.
 * Returns:
 *   0 = no wake word matched
 *   1 = WAKE_ONLY (only "Jarvis" / greetings spoken)
 *   2 = WAKE_ONESHOT ("Jarvis <command>" spoken, command written to out_cmd)
 */
static int
match_wake_prefix(const char *text, const char *wake_word, char *out_cmd, size_t out_sz)
{
	static const char *greetings[] = {
		"hey", "hi", "oh", "oi", "ola", "ol\xc3\xa1", "hola",
		"ok", "okay", "ei", "yo", "al\xc3\xb4", "alo", NULL
	};
	static const char *aliases[] = {
		"jarvis", "charvis", "jarves", "j\xc3\xa1rvis", "travis",
		"jervis", "service", "jorge", "chaves", "javis", "jarv",
		"darvis", "garvis", "jarbas", "javes", "j\xc3\xa1 fiz", "ja fiz", NULL
	};
	const char *p = text;
	int matched_any = 0;

	if (out_cmd && out_sz > 0)
		out_cmd[0] = '\0';
	if (!p)
		return 0;

	for (;;) {
		while (*p && is_punct_or_space(*p))
			p++;
		if (!*p)
			break;

		const char *scan = p;
		for (int i = 0; greetings[i]; i++) {
			size_t glen = strlen(greetings[i]);
			if (strncasecmp(scan, greetings[i], glen) == 0 &&
			    (scan[glen] == '\0' || is_punct_or_space(scan[glen]))) {
				scan += glen;
				while (*scan && is_punct_or_space(*scan))
					scan++;
				break;
			}
		}

		const char *after = NULL;
		if (wake_word && *wake_word) {
			size_t wlen = strlen(wake_word);
			if (strncasecmp(scan, wake_word, wlen) == 0 &&
			    (scan[wlen] == '\0' || is_punct_or_space(scan[wlen]))) {
				after = scan + wlen;
			}
		}
		if (!after) {
			for (int i = 0; aliases[i]; i++) {
				size_t alen = strlen(aliases[i]);
				if (strncasecmp(scan, aliases[i], alen) == 0 &&
				    (scan[alen] == '\0' || is_punct_or_space(scan[alen]))) {
					after = scan + alen;
					break;
				}
			}
		}
		if (!after)
			break;

		matched_any = 1;
		p = after;
	}

	if (!matched_any)
		return 0;

	while (*p && is_punct_or_space(*p))
		p++;

	char clean[1024];
	snprintf(clean, sizeof(clean), "%s", p);
	trim_inplace(clean);
	size_t clen = strlen(clean);
	while (clen > 0 && is_punct_or_space(clean[clen - 1]))
		clean[--clen] = '\0';

	if (is_hallucination(clean))
		return 1; /* WAKE_ONLY */

	for (int i = 0; greetings[i]; i++) {
		if (strcasecmp(clean, greetings[i]) == 0)
			return 1;
	}

	int alnum_cnt = 0;
	for (size_t i = 0; i < clen; i++) {
		if (isalnum((unsigned char)clean[i]) || ((unsigned char)clean[i] >= 0xC0))
			alnum_cnt++;
	}

	if (alnum_cnt >= 3) {
		if (out_cmd && out_sz > 0)
			snprintf(out_cmd, out_sz, "%s", clean);
		return 2; /* WAKE_ONESHOT */
	}
	return 1; /* WAKE_ONLY */
}

/* Word-boundary search for wake word anywhere in a short Stage-1 utterance
 * so a slight mic click or breath before "Jarvis" never prevents wake detection.
 */
static int
contains_wake_word(const char *text, const char *wake_word)
{
	static const char *aliases[] = {
		"jarvis", "charvis", "jarves", "j\xc3\xa1rvis", "travis",
		"jervis", "service", "jorge", "chaves", "javis", "jarv",
		"darvis", "garvis", "jarbas", "javes", "j\xc3\xa1 fiz", "ja fiz", NULL
	};
	if (!text || !*text)
		return 0;

	for (const char *p = text; *p; p++) {
		if (p > text && !is_punct_or_space(p[-1]))
			continue;
		if (wake_word && *wake_word) {
			size_t wlen = strlen(wake_word);
			if (strncasecmp(p, wake_word, wlen) == 0 &&
			    (p[wlen] == '\0' || is_punct_or_space(p[wlen])))
				return 1;
		}
		for (int i = 0; aliases[i]; i++) {
			size_t alen = strlen(aliases[i]);
			if (strncasecmp(p, aliases[i], alen) == 0 &&
			    (p[alen] == '\0' || is_punct_or_space(p[alen])))
				return 1;
		}
	}
	return 0;
}

static void
poll_control_socket(int srv, int cli, int is_jarvis_idle)
{
	if (cli >= 0) {
		struct pollfd cpfd;
		cpfd.fd = cli;
		cpfd.events = POLLIN | POLLHUP | POLLERR;
		cpfd.revents = 0;
		if (poll(&cpfd, 1, 0) > 0) {
			char peek;
			if ((cpfd.revents & (POLLHUP | POLLERR)) ||
			    ((cpfd.revents & POLLIN) && recv(cli, &peek, 1, MSG_PEEK | MSG_DONTWAIT) == 0)) {
				g_cancel_req = 1;
				g_dec.abort_flag = 1;
				return;
			}
		}
	}

	struct pollfd pfd;
	pfd.fd = srv;
	pfd.events = POLLIN;
	pfd.revents = 0;

	while (poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN)) {
		struct sockaddr_in caddr;
		socklen_t clen = sizeof(caddr);
		int cfd = accept(srv, (struct sockaddr *)&caddr, &clen);
		if (cfd < 0)
			break;

		char cbuf[2048];
		ssize_t n = read(cfd, cbuf, sizeof(cbuf) - 1);
		if (n <= 0) {
			close(cfd);
			continue;
		}
		cbuf[n] = '\0';

		if (strncmp(cbuf, "GET /", 5) == 0 || strncmp(cbuf, "PING", 4) == 0) {
			const char *ok = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nOK";
			write_all(cfd, ok, strlen(ok));
			close(cfd);
		} else if (strncmp(cbuf, "STOP", 4) == 0) {
			g_stop_req = 1;
			write_all(cfd, "OK\n", 3);
			close(cfd);
		} else if (strncmp(cbuf, "CANCEL", 6) == 0) {
			g_cancel_req = 1;
			g_dec.abort_flag = 1;
			write_all(cfd, "OK\n", 3);
			close(cfd);
		} else if (is_jarvis_idle &&
		           (strncmp(cbuf, "STREAM|", 7) == 0 || strncmp(cbuf, "JARVIS_WAIT|", 12) == 0) &&
		           g_preempt_cli < 0) {
			g_preempt_cli = cfd;
			snprintf(g_preempt_buf, sizeof(g_preempt_buf), "%s", cbuf);
			g_cancel_req = 1;
			g_dec.abort_flag = 1;
			break;
		} else {
			close(cfd);
		}
	}
}

static void
wait_next_audio_frame(int64_t target_frame, int max_wait_ms)
{
	pthread_mutex_lock(&g_audio.mtx);
	if (g_audio.total_frames < target_frame && running && !g_stop_req && !g_cancel_req) {
		struct timespec ts;
		clock_gettime(CLOCK_REALTIME, &ts);
		ts.tv_nsec += (long)max_wait_ms * 1000000L;
		if (ts.tv_nsec >= 1000000000L) {
			ts.tv_sec += 1;
			ts.tv_nsec -= 1000000000L;
		}
		pthread_cond_timedwait(&g_audio.cond, &g_audio.mtx, &ts);
	}
	pthread_mutex_unlock(&g_audio.mtx);
}

/* Unified Live Streaming Handler:
 *   STREAM|<mode:cmd|type|clip>|<lang>|<thresh_db>|<silence_ms>|<wait_ms>|<state_file>
 *
 *Features:
 *  1. Dynamic Peak-Relative Endpointing: tracks peak_speech_db during the utterance
 *     and sets eff_keep_db = max(eff_start_db - 3.5, peak_speech_db - 14.0).
 *     Even if room/fan noise is -28 dB, once you speak at -14 dB, dropping back to
 *     -28 dB immediately counts as silence and stops reliably!
 *  2. Transcript Stability Endpointing: if two consecutive streaming decodes return
 *     the exact same non-empty text (or Whisper reports no_speech on trailing audio)
 *     and your energy has dropped 8 dB below peak for >=360ms, auto-stops cleanly.
 */
static void
handle_stream(int srv, int cli, const char *mode, const char *lang,
              double thresh_db, int silence_ms, int wait_ms, const char *state_file)
{
	int is_cmd = (strcmp(mode, "cmd") == 0);
	int max_tok = is_cmd ? 64 : 112;
	const char *cmd_prompt = is_cmd ? CMD_INITIAL_PROMPT : NULL;
	const char *state_prefix = is_cmd ? "cmd" : "rec";
	const char *osd_title = is_cmd ? "  Command (Streaming)..." : "  Dictation (Streaming)...";
	int preroll_f = 8; /* 160 ms clean pre-roll */

	g_stop_req = 0;
	g_cancel_req = 0;
	decode_reset_session();
	audio_acquire();

	pthread_mutex_lock(&g_audio.mtx);
	int64_t session_start_f = g_audio.total_frames;
	int64_t ignore_until_f = is_cmd ? (session_start_f + 9) : session_start_f;
	int64_t min_preroll_f  = is_cmd ? ignore_until_f :
	                         ((session_start_f > preroll_f) ? (session_start_f - preroll_f) : 0);
	int64_t scan_f = session_start_f;
	int64_t speech_start_f = min_preroll_f;
	pthread_mutex_unlock(&g_audio.mtx);

	int in_speech = 0;
	int consec_voiced = 0;
	int voiced_frames = 0;
	int silence_frames = 0;
	int post_change_frames = 0;
	int unchanged_decodes = 0;
	double peak_speech_db = -100.0;
	int64_t last_voiced_f = scan_f;
	int64_t decoded_voiced_f = -1;
	int64_t final_submitted_voiced_f = -1;
	char live_text[4096] = {0};
	long t_start = now_ms();

	while (running && !g_stop_req && !g_cancel_req) {
		poll_control_socket(srv, cli, 0);
		if (g_stop_req || g_cancel_req)
			break;

		wait_next_audio_frame(scan_f + 1, 20);

		/* Pick up any completed non-blocking background decode */
		int stop_from_decode = 0;
		pthread_mutex_lock(&g_dec.mtx);
		if (g_dec.has_result) {
			g_dec.has_result = 0;
			int all_sil = g_dec.result_all_silent;
			char rtext[4096];
			snprintf(rtext, sizeof(rtext), "%s", g_dec.result_text);
			int64_t rvf = g_dec.result_voiced_f;
			pthread_mutex_unlock(&g_dec.mtx);

			if (rtext[0] != '\0') {
				if (strcmp(live_text, rtext) != 0) {
					snprintf(live_text, sizeof(live_text), "%s", rtext);
					unchanged_decodes = 0;
					post_change_frames = 0;
					decoded_voiced_f = rvf;
					write_state_file(state_file, state_prefix, live_text, osd_title);
				} else {
					unchanged_decodes++;
					decoded_voiced_f = rvf;
					if (silence_ms > 0 && unchanged_decodes >= 1 &&
					    (silence_frames >= 8 || post_change_frames >= 24)) {
						stop_from_decode = 1;
					}
				}
			} else if (all_sil && live_text[0] != '\0' && silence_ms > 0 &&
			           post_change_frames >= 18) {
				stop_from_decode = 1;
			}
		} else {
			pthread_mutex_unlock(&g_dec.mtx);
		}

		if (stop_from_decode)
			break;

		pthread_mutex_lock(&g_audio.mtx);
		int64_t avail_f = g_audio.total_frames;
		double nf_db = g_audio.noise_floor_db;
		double eff_start_db = thresh_db;
		if (nf_db + 7.5 > eff_start_db)
			eff_start_db = nf_db + 7.5;
		if (eff_start_db > -18.0)
			eff_start_db = -18.0;

		while (scan_f < avail_f) {
			float db = g_audio.db[scan_f % RING_FRAMES];
			if (scan_f < ignore_until_f) {
				scan_f++;
				continue;
			}
			if (!in_speech) {
				if (db >= eff_start_db) {
					consec_voiced++;
					if (consec_voiced >= 2) {
						in_speech = 1;
						g_audio.in_speech_hint = 1;
						voiced_frames = consec_voiced;
						silence_frames = 0;
						post_change_frames = 0;
						peak_speech_db = db;
						speech_start_f = (scan_f > preroll_f) ? (scan_f - preroll_f) : 0;
						if (speech_start_f < min_preroll_f)
							speech_start_f = min_preroll_f;
						last_voiced_f = scan_f;
					}
				} else {
					consec_voiced = 0;
				}
			} else {
				if (db > peak_speech_db)
					peak_speech_db = db;
				else if (db > eff_start_db - 6.0)
					peak_speech_db = 0.992 * peak_speech_db + 0.008 * db;

				/* Dynamic peak-relative keep threshold:
				 * Gently tracks peak_speech_db - 16.0 dB (capped at -28.0 dB) so
				 * softer words at the end of a sentence are preserved while ambient
				 * room/fan noise still triggers silence reliably.
				 */
				double eff_keep_db = eff_start_db - 3.0;
				double peak_rel_db = peak_speech_db - 16.0;
				if (peak_rel_db > -28.0)
					peak_rel_db = -28.0;
				if (voiced_frames >= 5 && peak_rel_db > eff_keep_db)
					eff_keep_db = peak_rel_db;

				if (db >= eff_keep_db) {
					voiced_frames++;
					silence_frames = 0;
					last_voiced_f = scan_f;
				} else {
					silence_frames++;
				}
				if (live_text[0] != '\0' && db < peak_speech_db - 8.5)
					post_change_frames++;
				else if (db >= peak_speech_db - 5.0)
					post_change_frames = 0;
			}
			scan_f++;
		}
		pthread_mutex_unlock(&g_audio.mtx);

		if (!in_speech) {
			if (wait_ms > 0 && (now_ms() - t_start) >= wait_ms) {
				decode_reset_session();
				audio_release(WARM_HOLD_MS);
				write_all(cli, "TIMEOUT\n", 8);
				return;
			}
			continue;
		}

		/* Check auto-endpointing */
		if (silence_ms > 0 && (silence_frames * FRAME_MS) >= silence_ms)
			break;
		/* Fallback: if transcript has not changed for 1.2s while energy stayed >8.5dB below peak */
		if (silence_ms > 0 && live_text[0] != '\0' && post_change_frames >= 60)
			break;
		if ((scan_f - speech_start_f) >= MAX_UTTER_FRAMES - 10)
			break;

		/* 1. Speculative final decode at 180ms of pause (9 frames) with 200ms tail: */
		if (silence_frames >= 9 && voiced_frames >= 6 &&
		    final_submitted_voiced_f < last_voiced_f) {
			int64_t end_f = last_voiced_f + 10;
			if (end_f > scan_f)
				end_f = scan_f;
			decode_force_submit_final(speech_start_f, end_f, last_voiced_f, lang, cmd_prompt, max_tok);
			final_submitted_voiced_f = last_voiced_f;
		}
		/* 2. Periodic live preview while speaking: */
		else if (silence_frames < 5 && (scan_f - speech_start_f) >= 14 &&
		         last_voiced_f > decoded_voiced_f + 8) {
			pthread_mutex_lock(&g_dec.mtx);
			int can_run = (!g_dec.busy && !g_dec.has_job &&
			               (scan_f - g_dec.done_at_frame) >= 15);
			pthread_mutex_unlock(&g_dec.mtx);
			if (can_run) {
				int64_t end_f = last_voiced_f + 6;
				if (end_f > scan_f)
					end_f = scan_f;
				decode_try_submit(speech_start_f, end_f, last_voiced_f, lang, cmd_prompt, max_tok, 1);
			}
		}
	}

	if (g_cancel_req) {
		decode_reset_session();
		audio_release(WARM_HOLD_MS);
		write_all(cli, "CANCELLED\n", 10);
		return;
	}

	if (!in_speech && g_stop_req) {
		pthread_mutex_lock(&g_audio.mtx);
		int64_t cur_f = g_audio.total_frames;
		pthread_mutex_unlock(&g_audio.mtx);
		speech_start_f = (cur_f > 25) ? (cur_f - 25) : 0;
		last_voiced_f = cur_f;
		voiced_frames = 10;
	}

	if (voiced_frames * FRAME_MS < 160 && !g_stop_req) {
		decode_reset_session();
		audio_release(WARM_HOLD_MS);
		write_all(cli, "EMPTY\n", 6);
		return;
	}

	/* Ensure a full-accuracy final pass covering last_voiced_f + 200ms tail is completed */
	if (final_submitted_voiced_f < last_voiced_f - 1) {
		int64_t end_f = last_voiced_f + 10;
		decode_force_submit_final(speech_start_f, end_f, last_voiced_f, lang, cmd_prompt, max_tok);
	}

	pthread_mutex_lock(&g_dec.mtx);
	while ((g_dec.busy || g_dec.has_job) && running && !g_cancel_req)
		pthread_cond_wait(&g_dec.cond, &g_dec.mtx);
	if (g_dec.has_result && g_dec.result_text[0] != '\0') {
		snprintf(live_text, sizeof(live_text), "%s", g_dec.result_text);
		g_dec.has_result = 0;
	}
	pthread_mutex_unlock(&g_dec.mtx);

	audio_release(WARM_HOLD_MS);

	if (live_text[0] == '\0') {
		write_all(cli, "EMPTY\n", 6);
	} else {
		char reply[4200];
		snprintf(reply, sizeof(reply), "OK|%s\n", live_text);
		write_all(cli, reply, strlen(reply));
	}
}

/* Always-On Streaming Jarvis Wake + Command handler (kept for optional background mode):
 *   JARVIS_WAIT|<lang>|<thresh_db>|<cmd_silence_ms>|<wake_word>|<state_file>|<chime_wav>
 */
static void
handle_jarvis_wait(int srv, int cli, struct whisper_context *ctx_wake, int n_threads,
                   const char *lang, double thresh_db, int cmd_silence_ms,
                   const char *wake_word, const char *state_file, const char *chime_wav)
{
	int preroll_f = 10; /* 200 ms pre-roll */
	float *wake_pcm = malloc((size_t)MAX_UTTER_FRAMES * FRAME_SAMPLES * sizeof(float));
	if (!wake_pcm) {
		write_all(cli, "EMPTY\n", 6);
		return;
	}

	g_stop_req = 0;
	g_cancel_req = 0;
	decode_reset_session();
	audio_acquire();

	pthread_mutex_lock(&g_audio.mtx);
	int64_t scan_f = g_audio.total_frames;
	pthread_mutex_unlock(&g_audio.mtx);

	int in_speech = 0;
	int consec_voiced = 0;
	int voiced_frames = 0;
	int silence_frames = 0;
	double peak_speech_db = -100.0;
	int64_t speech_start_f = scan_f;
	int64_t last_voiced_f = scan_f;

	int wake_confirmed = 0;
	int waiting_post_wake = 0;
	int chime_played = 0;
	int non_jarvis_utter = 0;
	int post_change_frames = 0;
	int unchanged_decodes = 0;
	long post_wake_deadline_ms = 0;
	int64_t chime_ignore_until_f = 0;
	int64_t wake_end_f = 0;
	int64_t cmd_start_f = scan_f;
	int64_t decoded_wake_voiced_f = -1;
	int64_t decoded_cmd_voiced_f = -1;
	int64_t final_submitted_voiced_f = -1;
	char live_cmd[4096] = {0};

	if (cmd_silence_ms < 360)
		cmd_silence_ms = 360;

	while (running && !g_stop_req && !g_cancel_req) {
		poll_control_socket(srv, cli, !wake_confirmed);
		if (g_stop_req || g_cancel_req)
			break;

		wait_next_audio_frame(scan_f + 1, 20);

		int stop_from_decode = 0;
		if (wake_confirmed && !waiting_post_wake) {
			pthread_mutex_lock(&g_dec.mtx);
			if (g_dec.has_result) {
				g_dec.has_result = 0;
				int all_sil = g_dec.result_all_silent;
				char raw_res[4096];
				snprintf(raw_res, sizeof(raw_res), "%s", g_dec.result_text);
				int64_t res_vf = g_dec.result_voiced_f;
				pthread_mutex_unlock(&g_dec.mtx);

				if (raw_res[0] != '\0') {
					char stripped[2048];
					int wm = match_wake_prefix(raw_res, wake_word,
					                           stripped, sizeof(stripped));
					const char *candidate = NULL;
					if (wm == 2 && stripped[0])
						candidate = stripped;
					else if (wm == 0)
						candidate = raw_res;

					if (candidate && *candidate) {
						int changed = (strcmp(live_cmd, candidate) != 0);
						if (changed) {
							snprintf(live_cmd, sizeof(live_cmd), "%s", candidate);
							unchanged_decodes = 0;
							post_change_frames = 0;
							decoded_cmd_voiced_f = res_vf;
							write_state_file(state_file, "cmd", live_cmd,
							                 "  Jarvis Command (Live)...");
						} else {
							unchanged_decodes++;
							decoded_cmd_voiced_f = res_vf;
							if (unchanged_decodes >= 1 &&
							    (silence_frames >= 12 || post_change_frames >= 30)) {
								stop_from_decode = 1;
							}
						}
					}
				} else if (all_sil && live_cmd[0] != '\0' && post_change_frames >= 18) {
					stop_from_decode = 1;
				}
			} else {
				pthread_mutex_unlock(&g_dec.mtx);
			}
		}

		pthread_mutex_lock(&g_audio.mtx);
		int64_t avail_f = g_audio.total_frames;
		double nf_db = g_audio.noise_floor_db;
		double eff_start_db = thresh_db;
		if (nf_db + 7.5 > eff_start_db)
			eff_start_db = nf_db + 7.5;
		if (eff_start_db > -18.0)
			eff_start_db = -18.0;

		while (scan_f < avail_f) {
			float db = g_audio.db[scan_f % RING_FRAMES];

			if (scan_f < chime_ignore_until_f) {
				scan_f++;
				continue;
			}

			if (wake_confirmed && waiting_post_wake) {
				if (db >= eff_start_db) {
					consec_voiced++;
					if (consec_voiced >= 2) {
						waiting_post_wake = 0;
						in_speech = 1;
						g_audio.in_speech_hint = 1;
						voiced_frames = consec_voiced;
						silence_frames = 0;
						post_change_frames = 0;
						unchanged_decodes = 0;
						peak_speech_db = db;
						cmd_start_f = (scan_f > 8) ? (scan_f - 8) : 0;
						if (cmd_start_f < wake_end_f)
							cmd_start_f = wake_end_f;
						last_voiced_f = scan_f;
						decoded_cmd_voiced_f = -1;
						final_submitted_voiced_f = -1;
					}
				} else {
					consec_voiced = 0;
				}
			} else if (!in_speech) {
				if (db >= eff_start_db) {
					consec_voiced++;
					if (consec_voiced >= 2) {
						in_speech = 1;
						g_audio.in_speech_hint = 1;
						voiced_frames = consec_voiced;
						silence_frames = 0;
						post_change_frames = 0;
						peak_speech_db = db;
						non_jarvis_utter = 0;
						speech_start_f = (scan_f > preroll_f) ? (scan_f - preroll_f) : 0;
						if (speech_start_f < chime_ignore_until_f)
							speech_start_f = chime_ignore_until_f;
						cmd_start_f = speech_start_f;
						last_voiced_f = scan_f;
						decoded_wake_voiced_f = -1;
					}
				} else {
					consec_voiced = 0;
				}
			} else {
				if (db > peak_speech_db)
					peak_speech_db = db;
				else if (db > eff_start_db - 6.0)
					peak_speech_db = 0.992 * peak_speech_db + 0.008 * db;

				double eff_keep_db = eff_start_db - 3.0;
				double peak_rel_db = peak_speech_db - 16.0;
				if (peak_rel_db > -28.0)
					peak_rel_db = -28.0;
				if (voiced_frames >= 5 && peak_rel_db > eff_keep_db)
					eff_keep_db = peak_rel_db;

				if (db >= eff_keep_db) {
					voiced_frames++;
					silence_frames = 0;
					last_voiced_f = scan_f;
				} else {
					silence_frames++;
				}
				if (live_cmd[0] != '\0' && db < peak_speech_db - 8.5)
					post_change_frames++;
				else if (db >= peak_speech_db - 5.0)
					post_change_frames = 0;
			}
			scan_f++;
		}
		pthread_mutex_unlock(&g_audio.mtx);

		if (wake_confirmed && waiting_post_wake) {
			if (now_ms() >= post_wake_deadline_ms) {
				decode_reset_session();
				audio_release(WARM_HOLD_MS);
				free(wake_pcm);
				write_all(cli, "TIMEOUT\n", 8);
				return;
			}
			continue;
		}

		if (!in_speech)
			continue;

		if (!wake_confirmed) {
			int64_t utter_f = scan_f - speech_start_f;

			if (non_jarvis_utter) {
				if (silence_frames >= 10 || utter_f > 250) {
					in_speech = 0;
					consec_voiced = 0;
					voiced_frames = 0;
					silence_frames = 0;
					non_jarvis_utter = 0;
					pthread_mutex_lock(&g_audio.mtx);
					g_audio.in_speech_hint = 0;
					pthread_mutex_unlock(&g_audio.mtx);
				}
				continue;
			}

			int pause_wake_check = (silence_frames >= 4 && voiced_frames >= 5 &&
			                        last_voiced_f > decoded_wake_voiced_f);
			int mid_wake_check   = (utter_f >= 30 && decoded_wake_voiced_f < 0);

			if (pause_wake_check || mid_wake_check) {
				int64_t end_f = mid_wake_check ? (speech_start_f + 34) : (last_voiced_f + 4);
				if (end_f > scan_f)
					end_f = scan_f;
				int64_t snap_voiced_f = last_voiced_f;
				int n_samp = ring_extract_pcm(speech_start_f, end_f, wake_pcm,
				                              MAX_UTTER_FRAMES * FRAME_SAMPLES);
				if (n_samp >= 2400) {
					char heard[1024];
					char inline_cmd[1024];
					run_whisper_slice(ctx_wake, n_threads, wake_pcm, n_samp,
					                  "en", WAKE_INITIAL_PROMPT, 8, 1, 1, 0,
					                  heard, sizeof(heard), NULL);
					decoded_wake_voiced_f = snap_voiced_f;

					int wmatch = match_wake_prefix(heard, wake_word,
					                               inline_cmd, sizeof(inline_cmd));
					if (wmatch == 0 && utter_f <= 80 &&
					    contains_wake_word(heard, wake_word)) {
						wmatch = 1;
					}
					if (wmatch > 0) {
						/* Strictly two-stage: play beep & reset audio buffer */
						wake_confirmed = 1;
						chime_played = 1;
						play_chime_async(chime_wav);
						waiting_post_wake = 1;
						in_speech = 0;
						consec_voiced = 0;
						voiced_frames = 0;
						silence_frames = 0;
						pthread_mutex_lock(&g_audio.mtx);
						g_audio.in_speech_hint = 0;
						chime_ignore_until_f = g_audio.total_frames + 10;
						pthread_mutex_unlock(&g_audio.mtx);
						wake_end_f = chime_ignore_until_f;
						post_wake_deadline_ms = now_ms() + 7000L;
						write_state_file(state_file, "jarvis-cmd", NULL, NULL);
						notify_osd_async("  Jarvis Listening...",
						                 "Speak your command now.", 6000);
						continue;
					} else if (mid_wake_check || utter_f > 75) {
						non_jarvis_utter = 1;
					}
				}
			}

			if (!wake_confirmed && silence_frames >= 10) {
				in_speech = 0;
				consec_voiced = 0;
				voiced_frames = 0;
				silence_frames = 0;
				pthread_mutex_lock(&g_audio.mtx);
				g_audio.in_speech_hint = 0;
				pthread_mutex_unlock(&g_audio.mtx);
				continue;
			}
		}

		if (wake_confirmed && !waiting_post_wake) {
			int64_t cmd_len_f = scan_f - cmd_start_f;

			if (silence_frames >= 7 && voiced_frames >= 6 &&
			    final_submitted_voiced_f < last_voiced_f) {
				int64_t end_f = last_voiced_f + 8;
				if (end_f > scan_f)
					end_f = scan_f;
				decode_force_submit_final(cmd_start_f, end_f, last_voiced_f,
				                          lang, CMD_INITIAL_PROMPT, 64);
				final_submitted_voiced_f = last_voiced_f;
			} else if (silence_frames < 4 && cmd_len_f >= 14 &&
			           last_voiced_f > decoded_cmd_voiced_f + 8) {
				pthread_mutex_lock(&g_dec.mtx);
				int can_run = (!g_dec.busy && !g_dec.has_job &&
				               (scan_f - g_dec.done_at_frame) >= 15);
				pthread_mutex_unlock(&g_dec.mtx);
				if (can_run) {
					int64_t end_f = last_voiced_f + 4;
					if (end_f > scan_f)
						end_f = scan_f;
					decode_try_submit(cmd_start_f, end_f, last_voiced_f,
					                  lang, CMD_INITIAL_PROMPT, 64, 1);
				}
			}

			if (stop_from_decode ||
			    (silence_frames * FRAME_MS) >= cmd_silence_ms ||
			    (live_cmd[0] != '\0' && post_change_frames >= 55) ||
			    cmd_len_f >= 600) {
				if (final_submitted_voiced_f < last_voiced_f - 1) {
					int64_t end_f = last_voiced_f + 8;
					decode_force_submit_final(cmd_start_f, end_f, last_voiced_f,
					                          lang, CMD_INITIAL_PROMPT, 64);
					final_submitted_voiced_f = last_voiced_f;
				}
				pthread_mutex_lock(&g_dec.mtx);
				while ((g_dec.busy || g_dec.has_job) && running && !g_cancel_req)
					pthread_cond_wait(&g_dec.cond, &g_dec.mtx);
				char final_raw[4096] = {0};
				if (g_dec.has_result) {
					snprintf(final_raw, sizeof(final_raw), "%s", g_dec.result_text);
					g_dec.has_result = 0;
				}
				pthread_mutex_unlock(&g_dec.mtx);

				if (final_raw[0] != '\0') {
					char stripped[2048];
					int wm = match_wake_prefix(final_raw, wake_word,
					                           stripped, sizeof(stripped));
					if (wm == 2 && stripped[0])
						snprintf(live_cmd, sizeof(live_cmd), "%s", stripped);
					else if (wm == 0)
						snprintf(live_cmd, sizeof(live_cmd), "%s", final_raw);
					else
						live_cmd[0] = '\0';
				}
				break;
			}
		}
	}

	if (g_cancel_req) {
		decode_reset_session();
		audio_release(g_preempt_cli >= 0 ? WARM_HOLD_MS : 2000L);
		free(wake_pcm);
		write_all(cli, "CANCELLED\n", 10);
		return;
	}

	audio_release(WARM_HOLD_MS);
	free(wake_pcm);

	if (live_cmd[0] != '\0') {
		if (!chime_played)
			play_chime_async(chime_wav);
		char reply[4200];
		snprintf(reply, sizeof(reply), "CMD|%s\n", live_cmd);
		write_all(cli, reply, strlen(reply));
	} else {
		write_all(cli, "EMPTY\n", 6);
	}
}

static void
send_control_msg(int port, const char *msg)
{
	int s = socket(AF_INET, SOCK_STREAM, 0);
	if (s < 0)
		return;
	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons((uint16_t)port);
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) == 0)
		write_all(s, msg, strlen(msg));
	close(s);
}

static int
run_client_mode(int port, const char *req)
{
	signal(SIGHUP, SIG_IGN);
	signal(SIGPIPE, SIG_IGN);
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_cli_sigint;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	sa.sa_handler = on_cli_sigusr1;
	sigaction(SIGUSR1, &sa, NULL);

	int s = socket(AF_INET, SOCK_STREAM, 0);
	if (s < 0)
		return 3;

	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons((uint16_t)port);
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

	if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		close(s);
		return 3;
	}

	char send_buf[2048];
	snprintf(send_buf, sizeof(send_buf), "%s\n", req);
	write_all(s, send_buf, strlen(send_buf));

	int sent_stop = 0;
	char resp[4096];
	size_t total = 0;

	while (total < sizeof(resp) - 1) {
		if (cli_cancel_req) {
			send_control_msg(port, "CANCEL\n");
			close(s);
			return 130;
		}
		if (cli_stop_req && !sent_stop) {
			send_control_msg(port, "STOP\n");
			sent_stop = 1;
		}

		struct pollfd pfd;
		pfd.fd = s;
		pfd.events = POLLIN;
		pfd.revents = 0;
		int pr = poll(&pfd, 1, 40);
		if (pr < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (pr == 0)
			continue;
		if (pfd.revents & (POLLIN | POLLHUP)) {
			ssize_t n = read(s, resp + total, sizeof(resp) - 1 - total);
			if (n <= 0)
				break;
			total += (size_t)n;
			if (memchr(resp, '\n', total))
				break;
		}
	}
	close(s);
	resp[total] = '\0';
	trim_inplace(resp);

	if (strcmp(resp, "CANCELLED") == 0)
		return 130;
	if (strcmp(resp, "EMPTY") == 0 || strcmp(resp, "TIMEOUT") == 0 || resp[0] == '\0')
		return 2;
	if (strncmp(resp, "OK|", 3) == 0) {
		printf("%s\n", resp + 3);
		return 0;
	}
	if (strncmp(resp, "CMD|", 4) == 0) {
		printf("%s\n", resp + 4);
		return 0;
	}
	printf("%s\n", resp);
	return 0;
}

static void
serve_client_request(int srv, int cli, char *buf,
                     struct whisper_context *ctx_main,
                     struct whisper_context *ctx_wake, int n_threads)
{
	if (strncmp(buf, "GET /", 5) == 0 || strncmp(buf, "PING", 4) == 0) {
		const char *ok = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nOK";
		write_all(cli, ok, strlen(ok));
		close(cli);
		return;
	}
	if (strncmp(buf, "STOP", 4) == 0) {
		g_stop_req = 1;
		pthread_cond_broadcast(&g_audio.cond);
		write_all(cli, "OK\n", 3);
		close(cli);
		return;
	}
	if (strncmp(buf, "CANCEL", 6) == 0) {
		g_cancel_req = 1;
		g_dec.abort_flag = 1;
		pthread_cond_broadcast(&g_audio.cond);
		write_all(cli, "OK\n", 3);
		close(cli);
		return;
	}

	char *nl = strpbrk(buf, "\r\n");
	if (nl)
		*nl = '\0';

	char *save = NULL;
	char *cmd = strtok_r(buf, "|", &save);
	if (!cmd) {
		close(cli);
		return;
	}

	if (strcmp(cmd, "STREAM") == 0) {
		char *mode       = strtok_r(NULL, "|", &save);
		char *lang       = strtok_r(NULL, "|", &save);
		char *thresh_str = strtok_r(NULL, "|", &save);
		char *sil_str    = strtok_r(NULL, "|", &save);
		char *wait_str   = strtok_r(NULL, "|", &save);
		char *state_file = strtok_r(NULL, "|", &save);

		if (mode && lang && thresh_str && sil_str && wait_str) {
			handle_stream(srv, cli, mode, lang,
			              strtod(thresh_str, NULL), atoi(sil_str),
			              atoi(wait_str), state_file ? state_file : "-");
		}
		close(cli);
		return;
	}

	if (strcmp(cmd, "JARVIS_WAIT") == 0) {
		char *lang       = strtok_r(NULL, "|", &save);
		char *thresh_str = strtok_r(NULL, "|", &save);
		char *sil_str    = strtok_r(NULL, "|", &save);
		char *wake_word  = strtok_r(NULL, "|", &save);
		char *state_file = strtok_r(NULL, "|", &save);
		char *chime_wav  = strtok_r(NULL, "|", &save);

		if (lang && thresh_str && sil_str && wake_word) {
			handle_jarvis_wait(srv, cli, ctx_wake, n_threads,
			                   lang, strtod(thresh_str, NULL), atoi(sil_str),
			                   wake_word, state_file ? state_file : "-",
			                   chime_wav ? chime_wav : "-");
		}
		close(cli);
		return;
	}

	if (strcmp(cmd, "TRANSCRIBE") == 0) {
		char *mode     = strtok_r(NULL, "|", &save);
		char *lang     = strtok_r(NULL, "|", &save);
		char *mt_str   = strtok_r(NULL, "|", &save);
		char *prompt   = strtok_r(NULL, "|", &save);
		char *wav_path = strtok_r(NULL, "|", &save);

		if (!mode || !lang || !mt_str || !prompt || !wav_path) {
			close(cli);
			return;
		}

		int is_wake = (strcmp(mode, "wake") == 0);
		int is_cmd  = (strcmp(mode, "cmd") == 0);
		int max_tok = atoi(mt_str);
		if (max_tok <= 0)
			max_tok = is_wake ? 8 : (is_cmd ? 24 : 96);

		float *pcm = NULL;
		int n_samples = 0;
		if (load_wav_f32(wav_path, &pcm, &n_samples) != 0 || n_samples < 1600) {
			free(pcm);
			close(cli);
			return;
		}

		decode_reset_session();
		struct whisper_context *ctx = is_wake ? ctx_wake : ctx_main;
		char out_text[4096];
		run_whisper_slice(ctx, n_threads, pcm, n_samples, lang, prompt,
		                  max_tok, is_wake, 0, 0, out_text, sizeof(out_text), NULL);
		strncat(out_text, "\n", sizeof(out_text) - strlen(out_text) - 1);
		write_all(cli, out_text, strlen(out_text));
		close(cli);
		free(pcm);
		return;
	}

	close(cli);
}

int
main(int argc, char *argv[])
{
	const char *main_model_path = NULL;
	const char *wake_model_path = NULL;
	const char *client_req      = NULL;
	int port = 8178;
	int n_threads = 6;
	int opt;

	while ((opt = getopt(argc, argv, "m:w:p:t:c:h")) != -1) {
		switch (opt) {
		case 'm': main_model_path = optarg; break;
		case 'w': wake_model_path = optarg; break;
		case 'p': port = atoi(optarg); break;
		case 't': n_threads = atoi(optarg); break;
		case 'c': client_req = optarg; break;
		default:
			fprintf(stderr,
				"Usage:\n"
				"  Daemon: %s -m <main-model.bin> [-w <wake-model.bin>] [-p port] [-t threads]\n"
				"  Client: %s [-p port] -c \"<COMMAND|...>\"\n",
				argv[0], argv[0]);
			return 1;
		}
	}

	if (client_req)
		return run_client_mode(port, client_req);

	if (!main_model_path) {
		fprintf(stderr, "dwm-whisperd: -m <main-model.bin> is required\n");
		return 1;
	}

	signal(SIGINT, on_sig);
	signal(SIGTERM, on_sig);
	signal(SIGHUP, SIG_IGN);
	signal(SIGPIPE, SIG_IGN);
	signal(SIGCHLD, SIG_IGN);

	ggml_backend_load_all();
	whisper_log_set(quiet_log, NULL);

	struct whisper_context_params cparams = whisper_context_default_params();
	cparams.use_gpu = true;
	cparams.flash_attn = true;

	struct whisper_context *ctx_main = whisper_init_from_file_with_params(main_model_path, cparams);
	if (!ctx_main) {
		fprintf(stderr, "dwm-whisperd: failed to load main model %s\n", main_model_path);
		return 1;
	}

	struct whisper_context *ctx_wake = NULL;
	if (wake_model_path && *wake_model_path && strcmp(wake_model_path, main_model_path) != 0 &&
	    access(wake_model_path, R_OK) == 0) {
		ctx_wake = whisper_init_from_file_with_params(wake_model_path, cparams);
	}
	if (!ctx_wake)
		ctx_wake = ctx_main;

	g_dec.ctx = ctx_main;
	g_dec.n_threads = n_threads;
	g_dec.pcm = malloc((size_t)MAX_UTTER_FRAMES * FRAME_SAMPLES * sizeof(float));
	if (!g_dec.pcm)
		return 1;

	int srv = socket(AF_INET, SOCK_STREAM, 0);
	if (srv < 0)
		return 1;

	int yes = 1;
	setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons((uint16_t)port);
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

	if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(srv, 16) < 0) {
		close(srv);
		return 1;
	}

	/* Warmup both contexts (128 frames) so threadpool and arena buffers are hot */
	{
		float warm_pcm[3200] = {0};
		struct whisper_full_params wp = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
		wp.n_threads = n_threads;
		wp.single_segment = false;
		wp.no_context = true;
		wp.no_timestamps = false;
		wp.max_tokens = 1;
		wp.audio_ctx = 128;
		wp.temperature_inc = 0.0f;
		wp.language = "en";
		whisper_full(ctx_wake, wp, warm_pcm, 3200);
		if (ctx_main != ctx_wake)
			whisper_full(ctx_main, wp, warm_pcm, 3200);
	}

	pthread_t audio_tid, decode_tid;
	pthread_create(&audio_tid, NULL, audio_thread_fn, NULL);
	pthread_create(&decode_tid, NULL, decode_worker_fn, NULL);

	while (running) {
		int cli = -1;
		char buf[2048];

		if (g_preempt_cli >= 0) {
			cli = g_preempt_cli;
			g_preempt_cli = -1;
			snprintf(buf, sizeof(buf), "%s", g_preempt_buf);
		} else {
			struct sockaddr_in cli_addr;
			socklen_t cli_len = sizeof(cli_addr);
			cli = accept(srv, (struct sockaddr *)&cli_addr, &cli_len);
			if (cli < 0) {
				if (errno == EINTR)
					continue;
				break;
			}
			ssize_t n = read(cli, buf, sizeof(buf) - 1);
			if (n <= 0) {
				close(cli);
				continue;
			}
			buf[n] = '\0';
		}

		serve_client_request(srv, cli, buf, ctx_main, ctx_wake, n_threads);
	}

	running = 0;
	g_dec.abort_flag = 1;
	pthread_cond_broadcast(&g_audio.cond);
	pthread_cond_broadcast(&g_dec.cond);
	pthread_join(audio_tid, NULL);
	pthread_join(decode_tid, NULL);

	close(srv);
	free(g_dec.pcm);
	whisper_free(ctx_main);
	if (ctx_wake != ctx_main)
		whisper_free(ctx_wake);
	return 0;
}
