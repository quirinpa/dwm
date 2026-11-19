# Future Improvement Ideas & Optimization Roadmap for dwm + Jarvis + Auren

This document tracks the implemented milestones and architecture for **dwm + Jarvis + Auren**.

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

4. **[IMPLEMENTED] Priority 1: True End-to-End LLM → TTS Token-to-Clause Streaming**
   - Extended `dwm-tts` Unix socket server (`/tmp/dwm-tts-$USER.sock`) with multi-chunk streaming sessions (`STREAM_START`, `STREAM_CHUNK`, `STREAM_END`) feeding `SpeechSession` synthesis/playback workers over a single open `pw-cat -p --raw` stream.
   - Added SSE streaming (`"stream": true`) and incremental JSON `"answer"` clause extraction (`_extract_streaming_answer_prefix` + `_pop_ready_clause`) in `AurenLLMBackend._post_chat_messages`, overlapping LLM token generation with Kokoro-82M synthesis.

5. **[IMPLEMENTED] Priority 2: Instant Voice Interruption ("Barge-In") & Cancellation (`0.8ms` Socket Latency)**
   - Moved `dwm-tts` synthesis and playback into non-blocking `SpeechSession` threads with `cancel_event` and tracked `current_player` (`pw-cat` Popen handle).
   - Added `INTERRUPT` socket command (`0.8ms` direct socket preemption, `dwm-tts interrupt` CLI), automatic barge-in on every new `Jarvis.process()` call and `dwm-voice` query/stop hook, plus Tier-0 voice reflexes (`"stop talking"`, `"be quiet"`, `"shut up"`, `"hush"`) → `tts_interrupt`.

6. **[IMPLEMENTED] Priority 3: Speaker-Playback Guard Against Follow-Up Window Hallucinations**
   - Enhanced `is_speaker_playing()` in `scripts/dwm-voice` to inspect both `playerctl status` and active uncorked PipeWire/PulseAudio media sink-inputs (`pactl list sink-inputs`), suppressing wake-free follow-up windows (`next_followup=0`) whenever music or video is playing over speakers.

7. **[IMPLEMENTED] Priority 4: Smarter Media Playback, YouTube Tab Reuse & Terminal Focus Preservation**
   - Updated `DwmAdapter._op_media_play` to detect and replace existing YouTube tabs in Firefox instead of accumulating duplicate tabs, and preserve active terminal window focus (or when `"in the background"` is requested).
