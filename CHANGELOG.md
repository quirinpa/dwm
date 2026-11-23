# Changelog

All notable changes to this custom `dwm` build (`~/.local/src/dwm`) are documented here.

## [2026-11-23]

### Changed
- **Portability & Zero Hardcoded User Paths (`dwm.c`, `config.def.h`, `config.h`, `dwm-bar.c`, `mods/dwm-ren/dwm-ren.c`, `scripts/*`)**:
  - Prepended `$HOME/.local/bin` to `PATH` automatically inside `dwm.c` `setup()` and `dwm-bar.c` `main()`, replacing hardcoded `/home/quirinpa/.local/bin/...` paths across keybindings, `statusclick()`, `dwm-cal` refresh/sync, `autostart.sh`, `dwm-auren`, `dwm-ctl`, `dwm-net`, and `dwm-cal`.
  - Replaced hardcoded `"quirinpa"` username fallbacks in `dwm-bar.c` and `mods/dwm-ren/dwm-ren.c` with POSIX `getpwuid(getuid())->pw_name` fallback.
  - Guarded `xrandr` mode configuration in `scripts/autostart.sh` and `scripts/dwm-session` via `DWM_OUTPUT` / `DWM_MODE` (or Xresources `dwm.output` / `dwm.mode`, defaulting to `eDP-1` `1600x900` only when connected and listed by `xrandr --query`).
- **Status Bar Cleanup & Docs (`dwm-bar.c`, `README.custom.md`, `CHANGELOG.md`)**:
  - Removed legacy `/tmp/dwm-acting-*.pid`, `/tmp/dwm-voice-*.state`, and `/tmp/dwm-jarvis-*.pid` polling fallbacks from `seg_voice()`, reading canonical `/tmp/jarvis-*` and `/tmp/dwm-ren-*` state files directly.
  - Removed obsolete empty `mods/tts/` directory and updated `README.custom.md` to reflect `ren-dwm.so`, `dwm-ren.so`, `ren-reflex.so`, and `jarvis`.

## [2026-11-22]

### Changed
- Split monolithic `mod_dwm.c` into decoupled `libxylem` modules: `mods/dwm-ren/dwm-ren.so` (`dwm` IPC + state publisher) and `mods/ren-dwm/ren-dwm.so` (`ren` workspace/window tools).
- Retired obsolete `dwm-jarvis.so` module and synced `X11` `DISPLAY`/`XAUTHORITY` before `jarvisd` startup in `autostart.sh`.
- Extended `dwm-bar` voice segment (`seg_voice`) with distinct status colors for `tts-speaking` (green) and `processing` (cyan/blue).

## [2026-11-19]

### Changed
- Decoupled voice daemon (`jarvisd`), TTS (`jarvis-tts.c`), and `ren` runtime into `external/libjarvis` and `external/axil-auren`, keeping `dwm` focused on X11 tiling, `dwm-bar`, and lightweight helper scripts.
