# Future Improvement Ideas & Optimization Roadmap for dwm + Jarvis + Auren

This document tracks the implemented milestones and the concrete engineering plan for the next phase of **dwm + Jarvis + Auren** optimizations.

---

## Completed Milestones

1. **[IMPLEMENTED] 3-Layer Jarvis v2 Architecture + ReAct Agent Loop (`1e083bc`, `ce9ad86`)**
   - Deterministic Tier-0 solver (`jarvis_core/solver.py`), Tier-1a local `bge-base` embedding cache, and Tier-1b multi-turn ReAct agent (`jarvis_core/agent.py`) over `axil-auren` (`:4242`).
   - Persistent `UserMemoryStore` (`~/.config/jarvis/memories.json`) and rolling `SessionJournal` (`/tmp/jarvis-$USER.session.json`).

2. **[IMPLEMENTED] 4x–8x Inference & Observation Latency Optimization (`80b57ef`)**
   - `DwmAdapter.observe()` cut from `~170ms` cold / `~135ms` warm to `~19ms` cold / `0.0ms` warm (350ms TTL cache, active-only tmux pane capture, `/proc/<pid>/cmdline` fast path, mtime-cached Firefox sessionstore, zero-subprocess `_get_fast_system_health`).
   - Eliminated `kilo-auto/free` hidden reasoning token exhaustion (`~17.1s` → `~1.9s`).
   - Unified `/home/quirinpa/.local/src/dwm` and `~/.local/{bin,lib}` via symlinks as a single source of truth.

3. **[IMPLEMENTED] Pipelined Clause-Streaming Neural TTS & Removal of `espeak-ng` Fallback (`f1973a8`)**
   - Permanently removed the robotic `spd-say` / `espeak-ng` fallback from `DwmAdapter._speak_tts`.
   - Added `split_tts_segments` + pipelined `pw-cat -p --raw` PCM streaming to `dwm-tts`, cutting Kokoro-82M Time-To-First-Audio (TTFA) from `~11.5s` to `~1.2s` and tuning ONNX Runtime (`intra_op_num_threads=3`, `allow_spinning=0`) for Intel hybrid P/E cores.

---

## Next Phase Implementation Plan (Prioritized)

### Priority 1: True End-to-End LLM → TTS Token-to-Clause Streaming (Target: Sub-1.5s Voice Response)

**Problem**: Even with pipelined clause-streaming inside `dwm-tts`, `JarvisAgent` currently waits for the HTTP response from `axil-auren` to complete in full (`~1.8s–2.2s`) before sending the text to `dwm-tts`.

**Architecture & Implementation Steps**:
1. **Multi-Turn Streaming Socket Protocol in `dwm-tts` (`scripts/dwm-tts`)**:
   - Extend the `dwm-tts` Unix socket server (`/tmp/dwm-tts-$USER.sock`) to accept streaming session packets or newline-delimited `STREAM_CHUNK|<voice>|<clause>` / `STREAM_END` messages over a persistent connection, queueing clauses directly into the active `NeuralTTSEngine` synthesis worker thread while `pw-cat` keeps the single audio stream open.
2. **SSE Streaming Client in `AurenLLMBackend` (`lib/jarvis-adapters/jarvis_adapters/inference.py`)**:
   - Add `_post_chat_messages_stream(messages, on_answer_clause=...)` using `"stream": true` against `http://127.0.0.1:4242/v1/chat/completions`.
   - Incrementally parse the incoming JSON stream: as soon as `"op": "answer_question"` and `"answer": "` are detected in the delta stream, buffer characters until the first clause boundary (`~25–36 chars` at a punctuation mark or conjunction) and immediately dispatch `STREAM_CHUNK` to `dwm-tts`.
3. **Overlap LLM Generation with Kokoro Synthesis (`lib/jarvis-core/jarvis_core/agent.py`)**:
   - Clause 0 begins synthesizing in Kokoro while the LLM is still generating Clause 1+, cutting total end-to-end latency from user silence to spoken voice by ~40–50%.

---

### Priority 2: Instant Voice Interruption / "Barge-In" & Cancellation

**Problem**: When Jarvis is speaking a multi-sentence explanation, the user cannot easily cut him off mid-sentence without waiting for `dwm-tts` playback to finish.

**Architecture & Implementation Steps**:
1. **Non-Blocking / Asynchronous Daemon Dispatch in `dwm-tts` (`scripts/dwm-tts`)**:
   - Move `engine.stream_speak()` into a dedicated playback worker thread inside `dwm-tts daemon` with a shared `cancel_event = threading.Event()` and an active `self.current_player: Optional[subprocess.Popen]` reference so the Unix socket accept loop never blocks during playback.
   - Support an instant `INTERRUPT` (or `CANCEL`) command on `/tmp/dwm-tts-$USER.sock` (and automatically interrupt any previous speech if a new `SPEAK` request arrives).
   - When `INTERRUPT` is received: set `cancel_event`, drain `pcm_queue`, terminate `self.current_player` (`pw-cat`/`aplay`) immediately (`< 10ms`), and unmute the microphone (`set_mic_mute(False)`).
2. **Hotkey, CLI & Wake-Word Hook (`scripts/dwm-voice`, `scripts/dwm-tts`, `lib/jarvis-adapters/jarvis_adapters/dwm.py`)**:
   - Add `dwm-tts interrupt` (or `dwm-tts stop-speech`).
   - Wire `dwm-voice` (`Mod+v` or `jarvis_stop_quiet`) and any new command execution in `jarvis` to send `INTERRUPT` to `dwm-tts` first.
   - Add a Tier-0 fast reflex in `solver.py` for `"stop talking"`, `"be quiet"`, `"shut up"`, `"quiet"`, `"cancel"` → executes `tts_interrupt`.

---

### Priority 3: Speaker-Playback Guard Against Follow-Up Window Hallucinations

**Problem**: During the 8-second post-command follow-up window (`next_followup=8000`), `dwm-whisperd` listens without requiring the `"Jarvis"` wake word. If music or a YouTube video is playing over the speakers (e.g., right after `"Please open some jazz music"`), Whisper transcribes the song lyrics/audio from the speakers as a phantom voice command.

**Architecture & Implementation Steps**:
1. **Suppress Wake-Free Follow-Up When Media Audio is Playing (`scripts/dwm-voice`)**:
   - In `jarvis_loop()` (`scripts/dwm-voice` lines 284–291), extend the `is_speaker_playing` check (via `playerctl status` and PipeWire/PulseAudio active sink-input check) to **all** follow-up modes (`next_followup > 0`), not just `3600000`.
   - Specifically: if `media_play` just executed or `is_speaker_playing` returns true, set `next_followup=0` so Jarvis immediately returns to strict `"Jarvis"` wake-word standby (`Yellow` icon) instead of recording the music in the room.
2. **Garbage / Lyric Hallucination Filter (`scripts/dwm-voice` / `orchestrator.py`)**:
   - Reject low-coherence transcriptions during follow-up windows before invoking the LLM agent if they neither match a Tier-0/Tier-1a intent nor contain an imperative verb or question structure.

---

### Priority 4: Smarter Media Playback & Tab Reuse (`DwmAdapter._op_media_play`)

**Problem**: Saying `"play some jazz music"` followed by `"play some classical music"` opens a brand-new Firefox tab each time and steals window focus even when working in a terminal.

**Architecture & Implementation Steps**:
1. **Existing YouTube Tab Reuse or Background Audio Player**:
   - Check `_get_firefox_open_tabs()` for an existing YouTube tab; or provide an optional lightweight background audio/mpv stream mode when the user says `"play <music> in the background"`.
2. **Preserve Terminal Focus When Requested**:
   - If the user is actively typing in a terminal or says `"play ... in the background"`, start playback without pulling X11 focus away from the current workspace/window.
