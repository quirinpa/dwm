# dwm Environment & Keybindings Guide

This guide documents your custom `dwm` build (`~/.local/src/dwm`), status bar (`~/.dwm/autostart.sh`), and helper scripts (`~/.local/bin/`).

---

## 1. Keybindings Reference (`Mod` = Super / Windows Key)

### Launchers & System Menus
| Shortcut | Action | Command / Script |
|---|---|---|
| `Mod +Shift + Enter` | Open terminal (`st`) | `st` |
| `Mod + p` | Application launcher (`dmenu`) | `dmenu_run` |
| `Mod + Space` | **Toggle Keyboard Layout (`US` ⇄ `PT`)** | `dwm-ctl kbd-toggle` |
| `Mod + v` | **Toggle Jarvis Listener On (`Yellow`) ⇄ Off (`Silver`)** (without locking screen) | `jarvis toggle` |
| `Mod + Shift + v` | **Jarvis Settings & ren Menu** (`dmenu`) | `jarvis menu` |
| `Mod + n` | **Quick Wi-Fi Switcher** (`dmenu`) | `dwm-net wifi` |
| `Mod + s` or `Mod + Shift + l` | **Lock screen immediately** | `slock` |
| `Mod + Ctrl + s` | **System Settings Menu** (Wi-Fi, Audio, Bluetooth, VPN, GNOME Settings) | `dwm-net` |
| `Mod + Escape` | **Session / Power Menu** (Lock, Hot-Restart, Logout, Suspend, Reboot, Poweroff) | `dwm-ctl power-menu` |
| `Mod + F1` or `Mod + /` | **Toggle Floating Keyboard Shortcuts Window** (closes with `Esc`, `q`, or second press) | `dwm-help` |

### Hardware Controls (Volume & Brightness)
*(Updates the status bar silently in `<20ms` via `SIGUSR1` — no popup notifications)*
| Shortcut | Fallback Shortcut | Action | Command |
|---|---|---|---|
| `XF86AudioRaiseVolume` | `Mod + F12` | Volume `+5%` (up to 150%) | `dwm-ctl vol-up` |
| `XF86AudioLowerVolume` | `Mod + F11` | Volume `-5%` | `dwm-ctl vol-down` |
| `XF86AudioMute` | `Mod + F10` | Toggle speaker mute | `dwm-ctl vol-mute` |
| `XF86AudioMicMute` | — | Toggle microphone mute | `dwm-ctl mic-mute` |
| `XF86MonBrightnessUp` | `Mod + F6` | Screen brightness `+5%` (rootless via `systemd-logind`) | `dwm-ctl bri-up` |
| `XF86MonBrightnessDown` | `Mod + F5` | Screen brightness `-5%` | `dwm-ctl bri-down` |

### Screenshots (`dwm-shot`)
*(Saves PNG to `~/Pictures/Screenshots/` AND copies `image/png` directly to your clipboard for `Ctrl+V` pasting)*
| Shortcut | Action | Command |
|---|---|---|
| `Print` | Full-screen screenshot | `dwm-shot full` |
| `Shift + Print` or `Mod + Shift + s` | Interactive area/rectangle selection | `dwm-shot select` |
| `Mod + Print` | Active window screenshot | `dwm-shot window` |

### Window & Layout Management
| Shortcut | Action |
|---|---|
| `Mod + j` / `Mod + k` | Focus next / previous window in stack |
| `Mod + Shift + j` / `Mod + Shift + k` | **Move / swap focused window down / up in the stack (`movestack`)** |
| `Mod + Enter` | Promote focused window to Master area (`zoom`) |
| `Mod + h` / `Mod + l` | Decrease / increase Master area width (`mfact`) |
| `Mod + i` / `Mod + d` | Increase / decrease number of windows in Master area |
| `Mod + Shift + c` | Close focused window |
| `Mod + t` | Tile layout (`◫`) |
| `Mod + f` | Floating layout (`◰`) |
| `Mod + m` | Monocle (full-screen deck) layout (`▣`) |
| `Mod + Shift + f` | **Toggle true 100% borderless fullscreen over the top bar (`togglefullscreen`)** |
| `Mod + Ctrl + Space` | Toggle between current and previous layout |
| `Mod + Shift + Space` | Toggle floating state of focused window |
| `Mod + b` | Toggle top status bar visibility |
| `Mod + -` / `Mod + =` | Decrease / increase window gaps (`±2px`) |
| `Mod + Shift + =` | Reset window gaps to `0px` |

### Workspaces (Tags `1`–`9`)
*(Only occupied tags or the currently active tag are shown in the top bar)*
| Shortcut | Action |
|---|---|
| `Mod + [1..9]` | Switch to workspace `1..9` |
| `Mod + Shift + [1..9]` | Move focused window to workspace `1..9` |
| `Mod + Ctrl + [1..9]` | View multiple workspaces simultaneously |
| `Mod + Ctrl + Shift + [1..9]` | Pin focused window across multiple workspaces |
| `Mod + Tab` | Toggle back to previously viewed workspace |
| `Mod + 0` | View all workspaces at once |
| `Mod + Shift + 0` | Make focused window sticky on all workspaces |

### Session Control
| Shortcut | Action |
|---|---|
| `Mod + Shift + r` | **Hot-restart `dwm` in-place** (re-executes `~/.local/bin/dwm` without closing open apps) |
| `Mod + Shift + q` | Quit `dwm` immediately |

### Clickable Status Bar Modules
| Status Segment | Left Click | Right Click | Scroll Up / Down |
|---|---|---|---|
| **Memory / CPU** (`` / ``) | Open floating `top` monitor | — | — |
| **Volume** (` 40%`) | Open Audio Menu (Mute, `±10%`, Output Sinks, `alsamixer`) | Toggle mute | Volume `+5%` / `-5%` |
| **Network** (` SSID`) | Open Wi-Fi Dropdown (`dwm-net wifi`) | Open System Settings Menu (`dwm-net`) | — |
| **Battery** (` 55%`) | Show Battery Status popup (Watts, time remaining, health) | Show Battery Status popup | Brightness `+5%` / `-5%` |
| **Keyboard** (` US` / ` PT`) | Toggle layout (`US` ⇄ `PT`) | Open Keyboard Layout Dropdown (`dmenu`) | Toggle layout (`US` ⇄ `PT`) |
| **Date / Time** (`` / ``) | Show Calendar popup | Show Calendar popup | — |
| **Gear (``)** | **Open System & Session Menu (Shortcuts Panel, Settings, Lock, Restart, Logout, Suspend, Reboot, Poweroff)** | Same | — |

---

## 2. Helper Scripts & Architecture

All custom scripts are tracked inside `scripts/` in the `dwm` Git repository (`~/.local/src/dwm/scripts/`) and installed automatically via `make -C ~/.local/src/dwm install`:

| Source Path (`~/.local/src/dwm/`) | Installed Path | Purpose |
|---|---|---|
| `dwm.c`, `config.h` | `~/.local/bin/dwm` | Custom `dwm` window manager binary. Build & install everything with `make -C ~/.local/src/dwm install`, then press `Mod+Shift+r`. |
| `scripts/autostart.sh` | `~/.dwm/autostart.sh` | Runs automatically when `dwm` starts. Applies guarded `xrandr` mode (`DWM_OUTPUT`/`DWM_MODE` or `dwm.output`/`dwm.mode` from Xresources, defaulting to `eDP-1` `1600x900` if connected), loads `~/.Xresources`, imports DBus/systemd env, sets wallpaper, starts `picom` and `dunst`, and runs `dwm-bar`. |
| `scripts/dwm-ctl` | `~/.local/bin/dwm-ctl` | Handles `vol-up`, `vol-down`, `vol-mute`, `mic-mute`, `bri-up`, `bri-down`, `kbd-toggle`, `power-menu`, `status-click`, and `refresh` (sends `SIGUSR1` to `/tmp/dwm-bar-$USER.pid` for silent `<20ms` status bar updates). |
| `scripts/dwm-net` | `~/.local/bin/dwm-net` | Vertical dropdown network & system settings menu (`Mod+n` for Wi-Fi, `Mod+Ctrl+s` for main settings menu: Wi-Fi, `nmtui`, PipeWire audio sink switcher, Bluetooth, `nm-connection-editor`, GNOME Control Center, and Shortcuts Cheat Sheet). |
| `scripts/dwm-shot` | `~/.local/bin/dwm-shot` | Captures full-screen, rectangular selection, or active window to `~/Pictures/Screenshots/`, copies PNG data to the X11 clipboard via GTK3, and sends a desktop notification with thumbnail. |
| `scripts/dwm-help` | `~/.local/bin/dwm-help` | Opens a centered, floating Catppuccin Mocha GTK cheat-sheet window showing all keyboard & voice shortcuts (`Mod+F1` or `Mod+/`). |
| `scripts/dwm-voice` | `~/.local/bin/dwm-voice` | Compatibility wrapper delegating voice controls and menus to `jarvis` (`~/.local/bin/jarvis`). |
| `scripts/dwm-jarvis` | `~/.local/bin/dwm-jarvis` | Persistent `tmux`-backed `ren --listen /tmp/ren-jarvis-$USER.sock` session launcher managed automatically by `jarvisd`. |
| `scripts/dwm-auren` | `~/.local/bin/dwm-auren` | Compatibility CLI wrapper delegating to `jarvis` (`dmenu`, `query`, `clear-learned`, `edit-config`). |
| `mods/dwm-ren/dwm-ren.c` | `~/.local/lib/dwm/dwm-ren.so` | `dwm` `libxylem` module syncing focused client / tag state to `/tmp/dwm-ren-state-$USER.txt` and forwarding `_DWM_CMD` opcode `100` to `ren`. |
| `mods/ren-dwm/ren-dwm.c` | `~/.local/lib/ren/ren-dwm.so` | `ren` `libxylem` module exposing `dwm` workspace and window manipulation tools via `_DWM_CMD`. |
| `scripts/dwm-session` | `~/.local/bin/dwm-session` | Session wrapper launched by the display manager (`dwm.desktop`). |
| `scripts/gnome-control-center` | `~/.local/bin/gnome-control-center` | Wrapper that exports `XDG_CURRENT_DESKTOP=GNOME` before launching `/usr/bin/gnome-control-center` so GNOME Settings works inside `dwm`. |

---

## 3. Jarvis Voice Assistant & `ren` Integration

### Interactive Help & Menus
- **Floating Cheat Sheet (`Mod + F1` or `Mod + /`)**: Toggles the graphical shortcuts & Jarvis reference panel (`dwm-help`). You can also say ***"Jarvis, show shortcuts"***.
- **Jarvis Control Menu (`Mod + Shift + v` or Right-Click `` in top bar)**:
  - Toggle listener On (`Yellow`) ⇄ Off (`Silver`) via `jarvis toggle`
  - Open/restart/doctor the **Jarvis Daemon (`jarvisd`) & `ren` Brain Session**
  - Switch recognition language (`auto`, `pt-pt`, `en-gb`, `en-us`, etc. — single `lang = ...` line in `~/.jarvisrc`) or Whisper model
- **Semantic Text Bar (`Mod + a`)**: Type any natural-language Jarvis command (`jarvis dmenu`) instead of speaking it.

### Unified `ren` Executive Brain
All spoken or typed commands are routed directly to `ren --listen /tmp/ren-jarvis-$USER.sock` running inside the `jarvis` `tmux` session:
1. **Tier 0 Reflex (`< 1 ms`)**: High-confidence deterministic EN/PT desktop, media, window, browser tab, and workspace commands execute instantaneously via `ren-reflex.so` dispatching into `ren-dwm.so`, `ren-desktop.so`, `ren-firefox.so`, and `ren-jarvis.so`.
2. **Tier 1 ReAct Agent (`ren`)**: Open-ended, multi-step, GUI, browser accessibility, or coding tasks execute via `ren` (`axil-auren`) with conversation continuity and `ren-mm.so` (`libmm`) episodic memory (`recall`, `record`).
3. **Zero-Config Setup**: Run `jarvis doctor` to verify daemon health, PipeWire `jarvis_aec_source`, Whisper model, TTS backend, and loaded `ren-*.so` plugins.
