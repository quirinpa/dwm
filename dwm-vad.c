/* See LICENSE file for copyright and license details.
 * dwm-vad: Real-time PipeWire PCM capture with Voice Activity Detection (VAD),
 * pre-roll ring buffer, and automatic silence endpointing for dwm-voice / Jarvis.
 */
#include <errno.h>
#include <math.h>
#include <pwd.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define SAMPLE_RATE      16000
#define CHANNELS         1
#define BITS_PER_SAMPLE  16
#define BYTES_PER_SAMPLE 2
#define FRAME_MS         20
#define FRAME_SAMPLES    ((SAMPLE_RATE * FRAME_MS) / 1000) /* 320 samples = 640 bytes */
#define FRAME_BYTES      (FRAME_SAMPLES * BYTES_PER_SAMPLE)

static volatile sig_atomic_t stop_requested   = 0;
static volatile sig_atomic_t cancel_requested = 0;

static void
on_sigint(int sig)
{
	(void)sig;
	stop_requested = 1;
}

static void
on_sigusr1(int sig)
{
	(void)sig;
	cancel_requested = 1;
}

static long
now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long)ts.tv_sec * 1000L + (long)(ts.tv_nsec / 1000000L);
}

static void
write_state(const char *path, const char *val)
{
	char tmp[512];
	FILE *fp;

	if (!path || !*path)
		return;
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	fp = fopen(tmp, "wb");
	if (!fp)
		return;
	fprintf(fp, "%s\n", val);
	fclose(fp);
	rename(tmp, path);
}

static void
notify_bar(void)
{
	char ubuf[64] = {0};
	const char *user = getenv("USER");
	char path[256];
	FILE *fp;
	int pid = 0;

	if (!user || !*user) {
		struct passwd pw, *res = NULL;
		char pbuf[512];
		if (getpwuid_r(getuid(), &pw, pbuf, sizeof(pbuf), &res) == 0 && res && res->pw_name && res->pw_name[0]) {
			snprintf(ubuf, sizeof(ubuf), "%s", res->pw_name);
			user = ubuf;
		} else {
			user = "user";
		}
	}
	snprintf(path, sizeof(path), "/tmp/dwm-bar-%s.pid", user);
	fp = fopen(path, "r");
	if (!fp)
		return;
	if (fscanf(fp, "%d", &pid) == 1 && pid > 1)
		kill((pid_t)pid, SIGUSR1);
	fclose(fp);
}

static double
frame_db(const int16_t *samples, size_t n)
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
write_wav(const char *path, const int16_t *pcm, size_t total_samples)
{
	FILE *fp;
	uint32_t data_bytes = (uint32_t)(total_samples * BYTES_PER_SAMPLE);
	uint32_t riff_size  = 36 + data_bytes;
	uint32_t byte_rate  = SAMPLE_RATE * CHANNELS * BYTES_PER_SAMPLE;
	uint16_t block_align = CHANNELS * BYTES_PER_SAMPLE;
	uint32_t fmt_size   = 16;
	uint16_t audio_fmt  = 1; /* PCM */
	uint16_t chans      = CHANNELS;
	uint32_t rate       = SAMPLE_RATE;
	uint16_t bps        = BITS_PER_SAMPLE;

	fp = fopen(path, "wb");
	if (!fp)
		return -1;

	fwrite("RIFF", 1, 4, fp);
	fwrite(&riff_size, 4, 1, fp);
	fwrite("WAVE", 1, 4, fp);
	fwrite("fmt ", 1, 4, fp);
	fwrite(&fmt_size, 4, 1, fp);
	fwrite(&audio_fmt, 2, 1, fp);
	fwrite(&chans, 2, 1, fp);
	fwrite(&rate, 4, 1, fp);
	fwrite(&byte_rate, 4, 1, fp);
	fwrite(&block_align, 2, 1, fp);
	fwrite(&bps, 2, 1, fp);
	fwrite("data", 1, 4, fp);
	fwrite(&data_bytes, 4, 1, fp);
	if (total_samples > 0)
		fwrite(pcm, BYTES_PER_SAMPLE, total_samples, fp);
	fclose(fp);
	return 0;
}

static void
usage(const char *argv0)
{
	fprintf(stderr,
		"Usage: %s [options] <output.wav>\n"
		"  -t <dB>    Speech threshold in dBFS (default: -38.0, adaptive above noise floor)\n"
		"  -s <ms>    Silence duration to auto-stop after speech (default: 1000, 0 = manual stop)\n"
		"  -w <ms>    Max wait for speech start before timeout (default: 7000, 0 = infinite/wake mode)\n"
		"  -m <ms>    Min speech duration required (default: 220)\n"
		"  -M <ms>    Max total speech duration (default: 30000)\n"
		"  -p <ms>    Pre-roll ring buffer before speech trigger (default: 360)\n"
		"  -S <file>  Optional state file to update on speech start\n"
		"  -A <str>   State string to write when speech becomes active\n",
		argv0);
}

int
main(int argc, char *argv[])
{
	double thresh_db    = -38.0;
	int silence_ms      = 1000;
	int wait_ms         = 7000;
	int min_speech_ms   = 220;
	int max_speech_ms   = 30000;
	int preroll_ms      = 360;
	const char *state_file = NULL;
	const char *active_str = NULL;
	const char *out_wav    = NULL;
	int opt;

	while ((opt = getopt(argc, argv, "t:s:w:m:M:p:S:A:h")) != -1) {
		switch (opt) {
		case 't': thresh_db     = strtod(optarg, NULL); break;
		case 's': silence_ms    = atoi(optarg); break;
		case 'w': wait_ms       = atoi(optarg); break;
		case 'm': min_speech_ms = atoi(optarg); break;
		case 'M': max_speech_ms = atoi(optarg); break;
		case 'p': preroll_ms    = atoi(optarg); break;
		case 'S': state_file    = optarg; break;
		case 'A': active_str    = optarg; break;
		default:
			usage(argv[0]);
			return 3;
		}
	}
	if (optind >= argc) {
		usage(argv[0]);
		return 3;
	}
	out_wav = argv[optind];

	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_sigint;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	sa.sa_handler = on_sigusr1;
	sigaction(SIGUSR1, &sa, NULL);

	int preroll_frames = (preroll_ms / FRAME_MS);
	if (preroll_frames < 2)
		preroll_frames = 2;
	if (preroll_frames > 100)
		preroll_frames = 100;

	int max_frames = (max_speech_ms / FRAME_MS) + preroll_frames + 100;
	if (max_frames < 500)
		max_frames = 500;

	int16_t *ring = calloc((size_t)preroll_frames * FRAME_SAMPLES, sizeof(int16_t));
	int16_t *pcm  = malloc((size_t)max_frames * FRAME_BYTES);
	if (!ring || !pcm) {
		free(ring);
		free(pcm);
		return 3;
	}

	FILE *pw = popen("pw-record --raw --rate=16000 --channels=1 --format=s16 --latency=20ms - 2>/dev/null", "r");
	if (!pw) {
		free(ring);
		free(pcm);
		return 3;
	}

	int ring_idx = 0, ring_filled = 0;
	size_t pcm_samples = 0;
	size_t max_samples = (size_t)max_frames * FRAME_SAMPLES;

	int in_speech = 0;
	int consec_voiced = 0;
	int voiced_frames = 0;
	int silence_frames = 0;
	double noise_floor_db = -52.0;
	int env_frames = 0;

	long t_start = now_ms();
	int16_t frame[FRAME_SAMPLES];

	while (!stop_requested && !cancel_requested) {
		size_t n = fread(frame, BYTES_PER_SAMPLE, FRAME_SAMPLES, pw);
		if (n < FRAME_SAMPLES)
			break;

		double db = frame_db(frame, FRAME_SAMPLES);

		/* Adaptive threshold tracks ambient noise floor while waiting for speech */
		if (!in_speech) {
			if (env_frames < 15) {
				noise_floor_db = 0.75 * noise_floor_db + 0.25 * db;
				env_frames++;
			} else if (db < noise_floor_db + 5.0) {
				noise_floor_db = 0.96 * noise_floor_db + 0.04 * db;
			}
		}

		double eff_start_db = thresh_db;
		if (noise_floor_db + 8.5 > eff_start_db)
			eff_start_db = noise_floor_db + 8.5;
		if (eff_start_db > -16.0)
			eff_start_db = -16.0;
		double eff_keep_db = eff_start_db - 3.5;

		if (!in_speech) {
			/* Store in pre-roll ring buffer */
			memcpy(ring + (size_t)ring_idx * FRAME_SAMPLES, frame, FRAME_BYTES);
			ring_idx = (ring_idx + 1) % preroll_frames;
			if (ring_filled < preroll_frames)
				ring_filled++;

			if (db >= eff_start_db) {
				consec_voiced++;
				if (consec_voiced >= 2) { /* 40ms of solid voice energy triggers speech */
					in_speech = 1;
					voiced_frames = consec_voiced;
					silence_frames = 0;

					/* Flush pre-roll ring buffer into pcm output */
					int start_slot = (ring_filled == preroll_frames) ? ring_idx : 0;
					for (int i = 0; i < ring_filled; i++) {
						int slot = (start_slot + i) % preroll_frames;
						if (pcm_samples + FRAME_SAMPLES <= max_samples) {
							memcpy(pcm + pcm_samples, ring + (size_t)slot * FRAME_SAMPLES, FRAME_BYTES);
							pcm_samples += FRAME_SAMPLES;
						}
					}

					if (state_file && active_str && *active_str) {
						write_state(state_file, active_str);
						notify_bar();
					}
				}
			} else {
				consec_voiced = 0;
				if (wait_ms > 0 && (now_ms() - t_start) >= wait_ms) {
					pclose(pw);
					free(ring);
					free(pcm);
					return 2; /* Timed out waiting for speech */
				}
			}
		} else {
			/* Currently recording speech */
			if (pcm_samples + FRAME_SAMPLES <= max_samples) {
				memcpy(pcm + pcm_samples, frame, FRAME_BYTES);
				pcm_samples += FRAME_SAMPLES;
			}

			if (db >= eff_keep_db) {
				voiced_frames++;
				silence_frames = 0;
			} else {
				silence_frames++;
				if (silence_ms > 0 && (silence_frames * FRAME_MS) >= silence_ms)
					break; /* Auto-endpointing: user stopped speaking! */
			}

			if ((int)((pcm_samples / FRAME_SAMPLES) * FRAME_MS) >= max_speech_ms)
				break;
		}
	}

	pclose(pw);

	if (cancel_requested) {
		unlink(out_wav);
		free(ring);
		free(pcm);
		return 130;
	}

	/* If manually stopped before VAD triggered, flush whatever was in the ring buffer */
	if (!in_speech && stop_requested && ring_filled > 0) {
		int start_slot = (ring_filled == preroll_frames) ? ring_idx : 0;
		for (int i = 0; i < ring_filled; i++) {
			int slot = (start_slot + i) % preroll_frames;
			if (pcm_samples + FRAME_SAMPLES <= max_samples) {
				memcpy(pcm + pcm_samples, ring + (size_t)slot * FRAME_SAMPLES, FRAME_BYTES);
				pcm_samples += FRAME_SAMPLES;
			}
		}
	}

	int total_voiced_ms = voiced_frames * FRAME_MS;
	if (pcm_samples == 0 || (!stop_requested && total_voiced_ms < min_speech_ms)) {
		unlink(out_wav);
		free(ring);
		free(pcm);
		return 2; /* Too short / noise spike only */
	}

	/* Trim excessive trailing silence (keep up to 140ms of natural tail pad) */
	int keep_tail_frames = 140 / FRAME_MS;
	if (silence_frames > keep_tail_frames) {
		size_t trim_samples = (size_t)(silence_frames - keep_tail_frames) * FRAME_SAMPLES;
		if (pcm_samples > trim_samples + (size_t)FRAME_SAMPLES * 5)
			pcm_samples -= trim_samples;
	}

	if (write_wav(out_wav, pcm, pcm_samples) != 0) {
		free(ring);
		free(pcm);
		return 3;
	}

	free(ring);
	free(pcm);
	return 0;
}
