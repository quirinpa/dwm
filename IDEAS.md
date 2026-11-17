# Future Improvement Ideas for dwm + Jarvis + Auren

This document captures architectural and UX ideas for evolving the **dwm + Jarvis + Auren** voice-driven desktop environment.

---

## 1. [IMPLEMENTED] Zero-Fork Persistent Daemon for `dwm-auren` & `dwm-act` (Latency: ~150ms → ~13ms)
- **Implemented**: `dwm-auren daemon` listens on `/tmp/dwm-auren-$USER.sock` with `intents.conf`, cached embeddings (`~/.cache/dwm-auren-embeddings.json`), and `dwm-act` dynamically imported in-process.
- **Result**: ~13ms average intent classification and action dispatch over Unix socket (`EVAL` / `ACT_EXEC`), eliminating Python and subprocess startup overhead.

---

## 2. [IMPLEMENTED: Earcons] Local Neural TTS & Subtle Audio Earcons (Eyes-Free Feedback)
Visual feedback via Dunst notifications and the status bar robot icon (`Yellow` / `Red` / `Silver`) requires looking at the top bar.
- **Subtle Two-Tone Earcons (Implemented)**: Synthesizes and plays a crisp ascending wake chime (`587 Hz → 880 Hz`, `/tmp/dwm-voice-chime-$USER.wav`) when transitioning from `Yellow → Red` and a soft descending completion chime (`659 Hz → 523 Hz`, `/tmp/dwm-voice-done-$USER.wav`) when an action completes.
- **Ultra-Fast Local TTS (`piper` or `espeak-ng`)**: Optionally speak concise answers aloud in ~50ms when answering questions via `dwm-help ask` or reporting system status (battery, network, time).

---

## 3. [IMPLEMENTED] Short Conversational Follow-Up Window & Voice Undo ("Scratch that")
- **Conversational Follow-Up Window (Implemented)**: `dwm-whisperd` accepts an 8th parameter `followup_ms` (default `3800` ms via `dwm.voice.followup_ms`) in `JARVIS_WAIT`. After executing a command, Jarvis stays in active command-listening mode (`Red` icon) for 3.8 seconds so follow-up commands can be spoken naturally without repeating *"Jarvis"*, then transitions back to wake-word standby (`Yellow` icon) automatically if silent.
- **Voice Undo (Implemented)**: `dwm-act` snapshots window tags, active workspace, and focused window to `/tmp/dwm-undo-$USER.json` before mutating actions; saying *"Scratch that"* or *"Undo"* restores the prior window/workspace state.

---

## 4. [IMPLEMENTED] Separate Privacy Toggle (`Mod+v`) vs. Voice Lockdown (`"Jarvis, lockdown"`) + `Mod+s` Lock
1. **Silent / Privacy Mode (`Mod+v`)**: Toggles the Jarvis microphone listener Off (`Silver`) and On (`Yellow`) without locking the screen.
2. **Immediate Screen Lock (`Mod+s` / `Mod+Shift+l`)**: Locks the X11 screen via `slock`.
3. **Voice Lockdown (`"Jarvis, lockdown"`)**: Turns the Jarvis microphone listener Off (`Silver`) and locks the X11 screen (`slock`).

---

## 5. [IMPLEMENTED] Context-Aware Disambiguation & Live X11 Pronouns ("this", "that", "other")
Live desktop state (`_DWM_CUR_TAG`, `_DWM_CLIENT_TAGS`, `dwm-msg other`, `xsel`/`xclip` primary & clipboard selections) is wired into `dwm`, `dwm-msg`, `dwm-act`, and `dwm-auren`:
- *"Jarvis, search Google for **this**"* / *"ask Pi to explain **this**"* → reads the current X11 primary/clipboard selection and opens the search in Firefox or sends it to Pi.
- *"Jarvis, move **the other window** to workspace 4"* / *"close **the other window**"* / *"focus **the other window**"* → targets the unfocused window on the current workspace (`dwm-msg other` + `tag-win` / `close-win` / `focus-win`) without losing focus on the active window.
- *"Jarvis, pull **Firefox** here"* → locates the workspace where the target window is open and pulls it onto the current workspace (`dwm-msg pull-win`).

---

## 6. Named Voice Macros & Teachable Routines
Build on multi-step clause chaining (`then`, `and then`) with persistent user-defined voice macros:
- *"Jarvis, remember routine demo mode: switch to workspace 2, open terminal, and start recording screen with audio."*
- Later triggered via: *"Jarvis, start demo mode."*

---

## 7. Adaptive Noise Floor & Media Playback Ducking in `dwm-whisperd`
- **Adaptive Noise Floor**: Track an exponential moving average (EMA) of ambient room RMS energy instead of fixed thresholds so wake-word detection stays reliable across quiet rooms and high fan noise.
- **Auto-Duck / Pause During Active Calls or Media**: Detect active WebRTC/meeting microphone streams or loud system playback via PipeWire/ALSA to prevent false wake-word triggers.
