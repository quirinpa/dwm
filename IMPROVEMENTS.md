# Jarvis (`dwm-voice` / `dwm-whisperd` / `dwm-auren` / `dwm-act`) — Architecture & Improvements Plan

This document details the root causes of current failure modes in Jarvis under real-world acoustic conditions and open-ended natural language queries, what can be fixed **immediately with zero extra resource overhead**, and the staged roadmap for making Jarvis noise-immune and semantically sharp.

---

## Executive Summary: What We Can Fix Immediately (Zero-Overhead Wins)

Without loading any heavy LLM or adding background daemons, we can immediately implement six high-impact fixes across [`dwm-whisperd.c`](dwm-whisperd.c), [`scripts/dwm-auren`](scripts/dwm-auren), [`scripts/dwm-act`](scripts/dwm-act), and [`intents.conf`](intents.conf):

1. **Speech-Band Biquad Filter (`130 Hz – 3400 Hz`) + Zero-Crossing Gate in `dwm-whisperd.c` VAD**:
   - Strip laptop cooling fan rumble (`< 120 Hz`), desk thumps, and high-frequency fan hiss (`> 3500 Hz`) *before* computing VAD frame dB (`calc_frame_db`), while keeping raw 16 kHz audio intact for Whisper.
2. **Whisper Token Confidence & `no_speech_prob` Tail Pruning in `dwm-whisperd.c`**:
   - Use `whisper_full_get_token_p()` to drop low-probability hallucinated trailing tokens (`p < 0.30`, such as `"on online"` or `"you"`) caused by background noise at the end of a spoken command, and immediately recalibrate `noise_floor_db` when Whisper flags a slice as `all_silent`.
3. **Post-Wake Speaker Auto-Ducking + Optional PipeWire WebRTC Echo Cancellation**:
   - When `"Jarvis"` wakes (`waiting_post_wake = 1`), temporarily duck active speaker volume (via `wpctl`) and restore it as soon as the command finishes (or wire PipeWire's installed `libpipewire-module-echo-cancel` WebRTC AEC source) so speaker audio never drowns out your voice or holds VAD open.
4. **Split Lightweight Embedding Inference (`bge-base`, `13 ms`, `0 MB` extra RAM) from Heavy LLM Inference (`qwen2.5-1.5b`, `1.1 GB`)**:
   - `axil-auren` (`:4242`) is *already* running in RAM (`113 MB`) with `bge-base-en-v1.5.gguf`. Use it strictly for single-query `13 ms` vector disambiguation in `dwm-act` (multi-prototype centroids including `ARTIST_BAND_PROTO`) without ever invoking `/v1/systemone` (`qwen2.5-1.5b`).
5. **Partitive Media Grammar (`"open [up] some {arg}"` → `play_target`)**:
   - Route `"open up some <X>"` / `"open some <X>"` directly to `play_target` and strip the partitive `"some "` determiner (`0 ms` lexical rule).
6. **Smart `ytInitialData` Music Scorer in `resolve_youtube_url()`**:
   - Parse video metadata (title, duration, channel badge) from the single YouTube search HTML response to reject Shorts (`< 90s`), interviews (*"Explains Why..."*), reactions, and karaoke, while boosting official audio, full albums, mixes, and `"- Topic"` channels.

---

## Track 1: Acoustic & Noise Immunity (`dwm-whisperd.c` & PipeWire)

### Problem Analysis

Jarvis currently gets confused by three distinct categories of sound:

| Noise Source | Acoustic Signature | How It Breaks Current VAD / Whisper |
| :--- | :--- | :--- |
| **1. Computer Speaker Output** (YouTube, music, notifications) | `-25 dB` to `-12 dB` directly in the speech band (`200–3500 Hz`) | `pw-record` captures raw `alsa_input` with no Acoustic Echo Cancellation (AEC). Speaker playback crosses `eff_start_db` (false wakes) or holds `db >= eff_keep_db` during a command so `silence_frames` never reaches `500 ms`. |
| **2. Heavy Computer Load** (Laptop cooling fan spin-up + CPU contention) | Broadband chassis vibration (`60–140 Hz` rumble) + air turbulence (`3800–7500 Hz` hiss) raising RMS by `+10..+15 dB` | `calc_frame_db()` integrates full `0–8000 Hz` energy. When the fan ramps up during a build/task, unweighted RMS jumps above `eff_keep_db` faster than `noise_floor_db` adapts (`0.992`), locking `in_speech = 1`. Meanwhile, competing processes starve `dwm-whisperd`'s 6 worker threads. |
| **3. Outside / Room Noise** (Door slam, traffic, cough, distant talk) | Short impulse spikes or non-vocal background energy at the tail of an utterance | Keep-threshold holds the utterance window open across trailing room noise; Whisper decodes the trailing noise with low token probabilities into hallucinated suffixes (`"on online"`, `"thank you"`, `"miles"`). |

---

### Improvement 1.1: Voice-Band Biquad Filter (`130 Hz – 3400 Hz`) in `calc_frame_db()`

- **Where**: [`dwm-whisperd.c`](dwm-whisperd.c) (`calc_frame_db()` and `audio_thread_fn()`).
- **Mechanism**:
  - Maintain a 2nd-order high-pass filter (`fc = 130 Hz`, removes fan chassis rumble, AC hum, and desk thumps) cascaded with a 2nd-order low-pass filter (`fc = 3400 Hz`, removes fan air hiss) used **exclusively for computing `db` and zero-crossing rate (ZCR)** for the VAD state machine (the ring buffer `g_audio.pcm` continues to store unmodified PCM for Whisper).
  - Compute the frame's **Zero-Crossing Rate (ZCR)**: human voiced speech concentrates energy in `150–2800 Hz` (`ZCR` between `0.02` and `0.38`), whereas pure white/fan hiss has high ZCR (`> 0.45`) and low-frequency rumble has near-zero ZCR (`< 0.015`).
  - Frames whose energy lies outside the speech ZCR envelope are penalized by `-6 dB` in the VAD detector so steady fan hiss cannot trigger `consec_voiced >= 2` or reset `silence_frames = 0`.
- **Cost**: ~12 floating-point multiply-adds per sample (`< 0.01%` CPU).

### Improvement 1.2: Whisper Token Probability Pruning & Fast Noise-Floor Recalibration

- **Where**: [`dwm-whisperd.c`](dwm-whisperd.c) (`run_whisper_slice()` and `handle_jarvis_wait()`).
- **Mechanism**:
  1. **Per-Token Confidence Pruning**:
     - Inside `run_whisper_slice()`, inspect tokens in each segment via `whisper_full_n_tokens(ctx, i)` and `whisper_full_get_token_data(ctx, i, j)`.
     - If trailing word tokens at the end of a segment have probability `t.p < 0.28f` (classic Whisper noise-tail hallucination such as `"on online"` after `"open a new terminal"`), strip those trailing low-confidence tokens before assembling `out_text`.
     - Require average segment token probability `avg_p >= 0.40f` (in addition to `no_speech_prob < nsp_limit`) before accepting a segment.
  2. **Instant Noise-Floor Recalibration on Silent Decode**:
     - When `run_whisper_slice()` returns `all_silent == 1` (Whisper confirmed the captured audio was noise, not speech), immediately step `g_audio.noise_floor_db` toward the recent frame average (`0.50 * noise_floor_db + 0.50 * recent_db`) and reset `in_speech = 0`. This guarantees that if your laptop fan suddenly spins up, Jarvis adapts to the new fan baseline within a single decode cycle (`~150 ms`).

### Improvement 1.3: Speaker Bleed Mitigation (Post-Wake Auto-Ducking + PipeWire WebRTC AEC)

- **Where**: [`dwm-whisperd.c`](dwm-whisperd.c) / [`scripts/dwm-voice`](scripts/dwm-voice).
- **Two Complementary Layers**:
  1. **Immediate Layer — Automatic Speaker Ducking During Command Capture**:
     - When Jarvis confirms the wake word (`waiting_post_wake = 1`) and speaker audio is currently playing, temporarily duck `@DEFAULT_AUDIO_SINK@` volume by `65%` (or save and attenuate via ALSA/PipeWire) for the `2–4s` command capture window, then restore the exact volume as soon as the command endpoint triggers (unless the command itself was a volume/mute command).
     - This immediately solves the problem of trying to talk over Miles Davis or a YouTube video playing through the laptop speakers.
  2. **PipeWire DSP Layer — `libpipewire-module-echo-cancel` (`webrtc`)**:
     - Your system already has `/usr/lib/x86_64-linux-gnu/pipewire-0.3/libpipewire-module-echo-cancel.so` and `/usr/lib/x86_64-linux-gnu/spa-0.2/aec/libspa-aec-webrtc.so`.
     - We can add an opt-in PipeWire drop-in (`~/.config/pipewire/pipewire.conf.d/99-jarvis-aec.conf` or runtime `pactl load-module module-echo-cancel aec_method=webrtc`) that exposes a noise-suppressed, echo-cancelled virtual mic source (`jarvis_aec_source`) and point `pw-record --target=jarvis_aec_source` at it when present.

---

## Track 2: Ultra-Lightweight Semantic Inference (`dwm-auren` & `dwm-act`)

### Problem Analysis: Why Inference Felt Heavy vs. What Actually Costs Resources

On this machine, `axil-auren` (`PID 5816` on `http://localhost:4242`) hosts two completely different endpoints:

1. **`POST /v1/embeddings` (`bge-base-en-v1.5.gguf`, `113 MB` RAM)**:
   - **Already loaded in RAM** for `pi-mm`.
   - Latency for 1 short string: **`13.8 ms`**.
   - CPU cost: single encoder forward pass (~10 ms on AVX-VNNI).
2. **`POST /v1/systemone` / `POST /v1/chat/completions` (`qwen2.5-1.5b-instruct-q4_k_m.gguf`, `1.1 GB` RAM)**:
   - Autoregressive LLM token generation.
   - Latency: **`800 ms – 2500 ms`**, high CPU utilization across all cores, 1.1 GB memory footprint.

Previously, enabling `dwm.auren.inference: true` turned on both bulk exemplar embedding checks and slow fallback paths, so turning it `false` disabled even the `13 ms` `bge-base` target classifier in `dwm-act`.

---

### Improvement 2.1: Split `embed_fast` (`bge-base`, Always-On, `13 ms`) from `llm_infer` (Off by Default)

- **Where**: [`scripts/dwm-act`](scripts/dwm-act) and [`scripts/dwm-auren`](scripts/dwm-auren).
- **Mechanism**:
  - Keep full-sentence intent routing in `dwm-auren` on the `0–2 ms` lexical/frame fast-path whenever a static command or `{arg}` template matches (`score >= 0.84`).
  - Only when an extracted `{arg}` (like `"miles davis"` in `open "miles davis"`) has **low lexical confidence (`< 0.72`)** against open windows, tabs, `.desktop` apps, and `places.sqlite`, call the already-running `localhost:4242/v1/embeddings` (`bge-base`) on `{arg}` alone (`13 ms` total, cached in `~/.cache/dwm-auren-embeddings.json` so repeated queries take `0.1 ms`).
  - Never load or call the 1.1 GB `qwen2.5-1.5b` model in the voice loop.

### Improvement 2.2: Multi-Prototype Semantic Centroids for Music / Artist / App Disambiguation

- **Where**: [`scripts/dwm-act`](scripts/dwm-act) (`is_media_query()`).
- **Why `"miles davis"` missed `0.56`**:
  - A single prototype string (`MEDIA_PROTO = "music song audio video..."`) has cosine similarity `0.544` with `"miles davis"` (vs `0.479` for `WEB_PROTO`), just under the `0.56` cutoff because `"miles davis"` is a proper name (jazz musician/trumpeter/composer), not a generic media noun.
- **Mechanism**:
  - Replace the single `MEDIA_PROTO` string with three pre-embedded media anchor prototypes (cached once in `~/.cache/dwm-auren-embeddings.json`):
    - `MEDIA_ARTIST_PROTO`: `"famous musician singer band composer jazz rock pop classical artist discography songs album"`
    - `MEDIA_TRACK_PROTO`: `"music song audio video lofi soundtrack playlist stream listen watch concert live mix"`
    - `WEB_TOOL_PROTO`: `"website webpage documentation software repository dashboard portal service tool account login"`
  - Take `sim_m = max(cos(q, MEDIA_ARTIST_PROTO), cos(q, MEDIA_TRACK_PROTO))`.
  - Classify as media when `sim_m >= 0.52 and (sim_m - sim_w) >= 0.035`.

### Improvement 2.3: Partitive Grammar Rule (`"open [up] some {arg}"` → `play_target`)

- **Where**: [`intents.conf`](intents.conf) and [`scripts/dwm-auren`](scripts/dwm-auren) (`promote_intent()`).
- **Mechanism**:
  - Add `"open up some {arg}"`, `"open some {arg}"`, `"put on some {arg}"`, `"put up some {arg}"`, `"play some {arg}"` directly to `play_target` in `intents.conf`.
  - Strip leading partitive determiners (`"some "`, `"a bit of "`, `"a little "`) from `{arg}` in `promote_intent()` so `"open up some miles davis"` immediately extracts `play "miles davis"` in `< 1 ms` even without an embedding call.

---

## Track 3: Deterministic Media & YouTube Resolution (`dwm-act`)

### Problem Analysis: Why `resolve_youtube_url()` Opened Random Videos

In [`scripts/dwm-act`](scripts/dwm-act#L1424-L1464), `resolve_youtube_url(query)` currently does:
```python
marker = '"videoId":"'
idx = html.find(marker)
```
This grabs the **very first `videoId`** anywhere in YouTube's raw JSON blob. For an artist query like `"miles davis"`, the first `videoId` is often:
- A 45-second YouTube Short (*"Miles Davis Explains Why Great Art Doesn't Require Suffering"*),
- A documentary/interview snippet, or
- An unrelated song sharing a single word (`"500 Miles Karaoke"` when Whisper clipped `"Miles Davis"` to `"Miles"`).

Furthermore, when non-media queries like `"new terminal"` accidentally fell through to `resolve_youtube_url()`, their bogus results were permanently saved in `/tmp/dwm-yt-cache-$USER.json`.

---

### Improvement 3.1: `ytInitialData` Candidate Parser & Music Ranking Scorer

- **Where**: [`scripts/dwm-act`](scripts/dwm-act) (`resolve_youtube_url()`).
- **Mechanism**:
  1. **Query Augmentation for Bare Artist / Short Queries**:
     - If the query has `<= 3` words and does not already contain a media qualifier (`"jazz"`, `"lofi"`, `"song"`, `"album"`, `"live"`, `"mix"`, `"playlist"`, `"official"`, `"audio"`, `"video"`), search YouTube with `f"{q_clean} music"` so YouTube's own ranker surfaces music tracks/albums instead of biography/interview clips.
  2. **Parse Top 8 `videoRenderer` Objects from `ytInitialData`** (zero extra HTTP requests):
     - Extract `videoId`, `title`, `lengthText` (duration in seconds), and owner/channel badges from the `ytInitialData` JSON embedded in the search HTML.
     - **Score each candidate**:
       - **Hard reject / heavy penalty (`-100`)**: Shorts (`reelWatchEndpoint` or duration `< 75s`), titles containing `"interview"`, `"explains"`, `"reaction"`, `"review"`, `"tutorial"`, `"karaoke"`, `"lesson"`, `"podcast"`, `"trailer"`.
       - **Boost (`+25` to `+50`)**: Duration between `150s` (2.5 min) and `7200s` (2 hours), titles/channels containing `"Official Audio"`, `"- Topic"`, `"Full Album"`, `"Mix"`, `"Live"`, `"Playlist"`, or exact token overlap with the requested artist/title.
  3. **Cache Hygiene**:
     - Purge polluted entries (`"new terminal"`, `"new term online"`, `"miles"`, `"miles davis on online"`) from `/tmp/dwm-yt-cache-$USER.json` and only cache queries that pass validation.

---

## Track 4: Cloud / External Micro-Inference Option (Future / Optional)

If you ever want full natural-language slot/intent understanding for complex, messy sentences that neither `intents.conf` templates nor `bge-base` embeddings cover — **without spending 1.1 GB of local RAM**:

- **Option A — Local `bge-base` (`13 ms`, `113 MB` shared with `pi-mm`) [Recommended Default]**:
  - Covers 99% of desktop WM, app, tab, and artist/media commands in `< 20 ms` completely offline.
- **Option B — Stateless Cloud Micro-Router Fallback (Groq Llama-3.1-8B-Instant or Gemini 2.5 Flash-Lite)**:
  - Triggered **only** when `dwm-auren` returns `LOW` confidence (`< 0.68`).
  - Sends a 150-token prompt containing the user utterance + open window/tab titles and receives a single-line `dwm-act` DSL command (`play "miles davis kind of blue"`, `focus "tmux"`, etc.) in `~140 ms` with **0 MB local RAM and 0% local CPU**.

---

## Implementation Order

| Step | Target Files | Change | Verification |
| :--- | :--- | :--- | :--- |
| **Step 1** | [`dwm-whisperd.c`](dwm-whisperd.c) | Add `130–3400 Hz` speech-band biquad filter + ZCR gate in `calc_frame_db()`; add per-token probability tail pruning (`t.p < 0.28`) & instant noise-floor recalibration on `all_silent` in `run_whisper_slice()`; add optional post-wake speaker ducking. | `make && make install`, test under fan/speaker noise and verify no `"on online"` tail hallucinations. |
| **Step 2** | [`intents.conf`](intents.conf), [`scripts/dwm-auren`](scripts/dwm-auren) | Add `"open [up] some {arg}"` to `play_target`, strip leading `"some "` in `promote_intent()`, and enable fast single-target `bge-base` check when lexical score is low. | `dwm-auren dry-run "open up some miles davis"` → `intent=play_target cmd=play "miles davis"`. |
| **Step 3** | [`scripts/dwm-act`](scripts/dwm-act) | Upgrade `is_media_query()` with multi-prototype centroids (`MEDIA_ARTIST_PROTO`, `MEDIA_TRACK_PROTO`) + `localhost:4242/v1/embeddings` fallback; upgrade `resolve_youtube_url()` with `ytInitialData` duration/interview/Shorts filtering and clean `/tmp/dwm-yt-cache-$USER.json`. | `dwm-act resolve-play "miles davis"` returns a real Miles Davis music track/album (`> 2.5 min`), not an interview clip or karaoke. |
| **Step 4** | [`scripts/bench-jarvis.py`](scripts/bench-jarvis.py), [`tests/test_reactive_act.py`](tests/test_reactive_act.py) | Run full benchmark and unit test suite; commit and push. | All unit tests pass; average query latency remains `< 25 ms`. |
