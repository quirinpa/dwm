/* See LICENSE file for copyright and license details.
 * dwm-whisperd: Fast resident libwhisper C daemon with dynamic audio_ctx
 * scaling, dual-model support (fast wake-word + full command/dictation),
 * and zero-fallback greedy decoding over a local TCP socket.
 */
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netinet/in.h>
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

#define SAMPLE_RATE 16000

static volatile sig_atomic_t running = 1;

static void
on_sig(int sig)
{
	(void)sig;
	running = 0;
}

static void
quiet_log(enum ggml_log_level level, const char *text, void *user_data)
{
	(void)level;
	(void)text;
	(void)user_data;
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
compute_audio_ctx(int n_samples, int is_wake)
{
	/* Whisper full 30s window = 480000 samples = 1500 audio_ctx frames (320 samples/frame).
	 * Scaling audio_ctx proportionally to actual speech duration avoids running the
	 * Transformer encoder over 28+ seconds of empty zero-padding!
	 */
	int frames = (n_samples / 320) + 28;
	int aligned = ((frames + 31) / 32) * 32;
	int min_ctx = is_wake ? 96 : 128;
	if (aligned < min_ctx)
		aligned = min_ctx;
	if (aligned > 1500)
		aligned = 1500;
	return aligned;
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
	if (strcasecmp(s, "you") == 0 || strcasecmp(s, "thank you.") == 0 ||
	    strcasecmp(s, "thanks for watching!") == 0 || strcasecmp(s, "obrigado.") == 0)
		return 1;
	return 0;
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

static void
trim_inplace(char *s)
{
	char *p = s;
	while (*p && isspace((unsigned char)*p))
		p++;
	if (p != s)
		memmove(s, p, strlen(p) + 1);
	size_t len = strlen(s);
	while (len > 0 && isspace((unsigned char)s[len - 1]))
		s[--len] = '\0';
}

int
main(int argc, char *argv[])
{
	const char *main_model_path = NULL;
	const char *wake_model_path = NULL;
	int port = 8178;
	int n_threads = 6;
	int opt;

	while ((opt = getopt(argc, argv, "m:w:p:t:h")) != -1) {
		switch (opt) {
		case 'm': main_model_path = optarg; break;
		case 'w': wake_model_path = optarg; break;
		case 'p': port = atoi(optarg); break;
		case 't': n_threads = atoi(optarg); break;
		default:
			fprintf(stderr, "Usage: %s -m <main-model.bin> [-w <wake-model.bin>] [-p port] [-t threads]\n", argv[0]);
			return 1;
		}
	}

	if (!main_model_path) {
		fprintf(stderr, "dwm-whisperd: -m <main-model.bin> is required\n");
		return 1;
	}

	signal(SIGINT, on_sig);
	signal(SIGTERM, on_sig);
	signal(SIGPIPE, SIG_IGN);

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

	int srv = socket(AF_INET, SOCK_STREAM, 0);
	if (srv < 0) {
		whisper_free(ctx_main);
		if (ctx_wake != ctx_main)
			whisper_free(ctx_wake);
		return 1;
	}

	int yes = 1;
	setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons((uint16_t)port);
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

	if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(srv, 16) < 0) {
		close(srv);
		whisper_free(ctx_main);
		if (ctx_wake != ctx_main)
			whisper_free(ctx_wake);
		return 1;
	}

	/* Warmup pass (64ms silence) so threadpool and arena allocations are hot */
	{
		float warm_pcm[3200] = {0};
		struct whisper_full_params wp = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
		wp.n_threads = n_threads;
		wp.single_segment = true;
		wp.no_context = true;
		wp.no_timestamps = true;
		wp.max_tokens = 1;
		wp.audio_ctx = 96;
		wp.temperature_inc = 0.0f;
		wp.language = "en";
		whisper_full(ctx_wake, wp, warm_pcm, 3200);
	}

	while (running) {
		struct sockaddr_in cli_addr;
		socklen_t cli_len = sizeof(cli_addr);
		int cli = accept(srv, (struct sockaddr *)&cli_addr, &cli_len);
		if (cli < 0) {
			if (errno == EINTR)
				continue;
			break;
		}

		char buf[2048];
		ssize_t n = read(cli, buf, sizeof(buf) - 1);
		if (n <= 0) {
			close(cli);
			continue;
		}
		buf[n] = '\0';

		/* Protocol:
		 *   PING
		 *   TRANSCRIBE|<mode:wake|cmd|dict>|<lang>|<max_tokens>|<prompt>|<wav_path>
		 * Also responds to HTTP GET / for healthchecks.
		 */
		if (strncmp(buf, "GET /", 5) == 0 || strncmp(buf, "PING", 4) == 0) {
			const char *ok = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nOK";
			write_all(cli, ok, strlen(ok));
			close(cli);
			continue;
		}

		char *nl = strpbrk(buf, "\r\n");
		if (nl)
			*nl = '\0';

		char *save = NULL;
		char *cmd = strtok_r(buf, "|", &save);
		char *mode = strtok_r(NULL, "|", &save);
		char *lang = strtok_r(NULL, "|", &save);
		char *mt_str = strtok_r(NULL, "|", &save);
		char *prompt = strtok_r(NULL, "|", &save);
		char *wav_path = strtok_r(NULL, "|", &save);

		if (!cmd || strcmp(cmd, "TRANSCRIBE") != 0 || !mode || !lang || !mt_str || !prompt || !wav_path) {
			close(cli);
			continue;
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
			continue;
		}

		struct whisper_context *ctx = is_wake ? ctx_wake : ctx_main;
		struct whisper_full_params wparams = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
		wparams.n_threads        = n_threads;
		wparams.single_segment   = true;
		wparams.no_context       = true;
		wparams.no_timestamps    = true;
		wparams.suppress_blank   = true;
		wparams.suppress_nst     = true;
		wparams.temperature      = 0.0f;
		wparams.temperature_inc  = 0.0f; /* NEVER run 5x temperature fallback loops */
		wparams.greedy.best_of   = 1;
		wparams.max_tokens       = max_tok;
		wparams.audio_ctx        = compute_audio_ctx(n_samples, is_wake);

		if (strcmp(lang, "auto") == 0 || !*lang) {
			wparams.language = "auto";
			wparams.detect_language = false;
		} else {
			wparams.language = lang;
		}

		if (*prompt && strcmp(prompt, "-") != 0)
			wparams.initial_prompt = prompt;

		char out_text[4096];
		out_text[0] = '\0';

		if (whisper_full(ctx, wparams, pcm, n_samples) == 0) {
			int n_seg = whisper_full_n_segments(ctx);
			for (int i = 0; i < n_seg; i++) {
				float nsp = whisper_full_get_segment_no_speech_prob(ctx, i);
				if (nsp > 0.82f)
					continue;
				const char *seg = whisper_full_get_segment_text(ctx, i);
				if (!seg)
					continue;
				char seg_clean[1024];
				snprintf(seg_clean, sizeof(seg_clean), "%s", seg);
				trim_inplace(seg_clean);
				if (is_hallucination(seg_clean))
					continue;
				if (out_text[0] != '\0')
					strncat(out_text, " ", sizeof(out_text) - strlen(out_text) - 1);
				strncat(out_text, seg_clean, sizeof(out_text) - strlen(out_text) - 1);
			}
		}

		trim_inplace(out_text);
		strncat(out_text, "\n", sizeof(out_text) - strlen(out_text) - 1);
		write_all(cli, out_text, strlen(out_text));
		close(cli);
		free(pcm);
	}

	close(srv);
	whisper_free(ctx_main);
	if (ctx_wake != ctx_main)
		whisper_free(ctx_wake);
	return 0;
}
