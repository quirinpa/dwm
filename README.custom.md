# dwm Environment & Keybindings Guide

This guide documents your custom `dwm` build (`~/.local/src/dwm`), status bar (`~/.dwm/autostart.sh`), and helper scripts (`~/.local/bin/`).

---

## 1. Keybindings Reference (`Mod` = Super / Windows Key)

### Launchers & System Menus
| Shortcut | Action | Command / Script |
|---|---|---|
| `Mod +Shift + Enter` | Open terminal (`ghostty`) | `ghostty` |
| `Mod + p` | Application launcher (`dmenu`) | `dmenu_run` |
| `Mod + Space` | **Toggle Keyboard Layout (`US` ⇄ `PT`)** | `dwm-ctl kbd-toggle` |
| `Mod + n` | **Quick Wi-Fi Switcher** (`dmenu`) | `dwm-net wifi` |
| `Mod + s` | **System Settings Menu** (Wi-Fi, Audio, Bluetooth, VPN, GNOME Settings) | `dwm-net` |
| `Mod + Shift + l` | **Lock screen immediately** | `slock` |
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
| `scripts/autostart.sh` | `~/.dwm/autostart.sh` | Runs automatically when `dwm` starts. Configures `1600x900` resolution, loads `~/.Xresources`, imports DBus/systemd env, sets wallpaper, starts `picom` and `notification-daemon`, and runs the status bar loop. |
| `scripts/dwm-ctl` | `~/.local/bin/dwm-ctl` | Handles `vol-up`, `vol-down`, `vol-mute`, `mic-mute`, `bri-up`, `bri-down`, `power-menu`, and `refresh` (sends `SIGUSR1` to `/tmp/dwm-bar-$USER.pid` for silent `<20ms` status bar updates). |
| `scripts/dwm-net` | `~/.local/bin/dwm-net` | Vertical dropdown network & system settings menu (`Mod+n` for Wi-Fi, `Mod+s` for main settings menu: Wi-Fi, `nmtui`, PipeWire audio sink switcher, Bluetooth, `nm-connection-editor`, GNOME Control Center, and Shortcuts Cheat Sheet). |
| `scripts/dwm-shot` | `~/.local/bin/dwm-shot` | Captures full-screen, rectangular selection, or active window to `~/Pictures/Screenshots/`, copies PNG data to the X11 clipboard via GTK3, and sends a desktop notification with thumbnail. |
| `scripts/dwm-help` | `~/.local/bin/dwm-help` | Opens a centered, floating Catppuccin Mocha GTK cheat-sheet window showing all keyboard shortcuts (`Mod+F1` or `Mod+/`). |
| `scripts/dwm-session` | `~/.local/bin/dwm-session` | Session wrapper launched by the display manager (`dwm.desktop`). |
| `scripts/gnome-control-center` | `~/.local/bin/gnome-control-center` | Wrapper that exports `XDG_CURRENT_DESKTOP=GNOME` before launching `/usr/bin/gnome-control-center` so GNOME Settings works inside `dwm`. |
