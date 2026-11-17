# Future Improvement Ideas for dwm + Jarvis + Auren

This document captures architectural and UX ideas for evolving the **dwm + Jarvis + Auren** voice-driven desktop environment.

---

## 1. Zero-Fork Persistent Daemon for `dwm-auren` & `dwm-act` (Latency: ~150ms → <15ms)
Currently, each transcribed voice command from `dwm-whisperd` spawns `scripts/dwm-auren` (which starts a Python process and loads cached embeddings) and then spawns `scripts/dwm-act` (another Python process).
- **Improvement**: Run a single persistent Unix-socket daemon (`dwm-jarvisd`) in memory with `intents.conf` embeddings pre-loaded and X11/AT-SPI connections warm.
- **Benefit**: Sub-15ms intent classification and action dispatch after Whisper transcription completes, eliminating Python startup overhead.

---

## 2. Local Neural TTS & Subtle Audio Earcons (Eyes-Free Feedback)
Visual feedback via Dunst notifications and the status bar robot icon (`Yellow` / `Red` / `Silver`) requires looking at the top bar.
- **Subtle Two-Tone Earcons**: Play a crisp 30ms synthesized chime when transitioning from `Yellow → Red` (wake word detected) and `Red → Yellow` (command completed).
- **Ultra-Fast Local TTS (`piper` or `espeak-ng`)**: Optionally speak concise answers aloud in ~50ms when answering questions via `dwm-help ask` or reporting system status (battery, network, time).

---

## 3. Short Conversational Follow-Up Window (Multi-Turn Chaining Without Repeating "Jarvis")
Currently, `dwm-whisperd` immediately returns to wake-word standby after executing a command.
- **Improvement**: Keep a configurable 3–4 second follow-up window after an action completes so follow-up commands can be spoken naturally without repeating *"Jarvis"* (e.g. *"Jarvis, open Firefox"* → *"go to github.com"* → *"move window to workspace 2"*).
- **Voice Undo**: Support saying *"Scratch that"* or *"Undo"* within 5 seconds to reverse the last window/workspace state change.

---

## 4. Optional Auto-Resume After Unlocking `slock`
Distinguish between:
1. **Silent / Privacy Mode (`Mod+v`)**: Toggles the Jarvis microphone listener Off (`Silver`) without locking the screen.
2. **Voice Lockdown (`"Jarvis, lockdown"`)**: Turns the Jarvis microphone listener Off (`Silver`) and locks the X11 screen (`slock`), with an optional setting to automatically resume listening once `slock` is unlocked.

---

## 5. Context-Aware Disambiguation & Live X11 Pronouns ("this", "that", "other")
Feed live desktop state (`dwm-msg` client list, active workspace, primary selection, clipboard) into the intent resolver:
- *"Jarvis, search Google for **this**"* → reads the current X11 primary selection (`xclip -o`) and opens the search in Firefox.
- *"Jarvis, move **the other window** to workspace 4"* → targets the unfocused window in the current stack without losing focus on the active window.
- *"Jarvis, pull **Firefox** here"* → locates the workspace where Firefox is open and brings it to the current tag.

---

## 6. Named Voice Macros & Teachable Routines
Build on multi-step clause chaining (`then`, `and then`) with persistent user-defined voice macros:
- *"Jarvis, remember routine demo mode: switch to workspace 2, open terminal, and start recording screen with audio."*
- Later triggered via: *"Jarvis, start demo mode."*

---

## 7. Adaptive Noise Floor & Media Playback Ducking in `dwm-whisperd`
- **Adaptive Noise Floor**: Track an exponential moving average (EMA) of ambient room RMS energy instead of fixed thresholds so wake-word detection stays reliable across quiet rooms and high fan noise.
- **Auto-Duck / Pause During Active Calls or Media**: Detect active WebRTC/meeting microphone streams or loud system playback via PipeWire/ALSA to prevent false wake-word triggers.
