#!/bin/sh
export PATH="$HOME/.local/bin:$PATH"

# 1. Set display resolution FIRST (before dwm or background tasks run)
if command -v xrandr >/dev/null 2>&1; then
    xrandr --output eDP-1 --mode 1600x900
fi

# 2. Load Xresources (xterm & dwm Catppuccin Mocha theme, fonts, padding, bar/auren prefs)
if [ -f "$HOME/.Xresources" ] && command -v xrdb >/dev/null 2>&1; then
    xrdb -nocpp -merge "$HOME/.Xresources"
    command -v dwm-msg >/dev/null 2>&1 && dwm-msg reload 2>/dev/null || true
fi

# 3. Import X11 environment into systemd/DBus and ensure portal is healthy in background
(
    if command -v dbus-update-activation-environment >/dev/null 2>&1; then
        dbus-update-activation-environment --systemd DISPLAY XAUTHORITY XDG_CURRENT_DESKTOP
    fi
    if command -v systemctl >/dev/null 2>&1; then
        systemctl --user reset-failed xdg-desktop-portal-gtk.service xdg-desktop-portal.service 2>/dev/null
        systemctl --user start xdg-desktop-portal-gtk.service 2>/dev/null
    fi
) &

# 4. Restore wallpaper via ~/.fehbg (or fallback)
if [ -x "$HOME/.fehbg" ]; then
    "$HOME/.fehbg" &
elif command -v feh >/dev/null 2>&1 && [ -f "$HOME/snap/firefox/common/Firefox_wallpaper.png" ]; then
    feh --bg-fill "$HOME/snap/firefox/common/Firefox_wallpaper.png" &
else
    xsetroot -solid "#1e1e2e"
fi

# 5. Start compositor if installed (tear-free rendering, shadows, transparency)
if command -v picom >/dev/null 2>&1; then
    pkill -x picom 2>/dev/null
    picom --backend glx -b
fi

# 5b. Start desktop notification daemon (dunst with Catppuccin Mocha theme)
if command -v dunst >/dev/null 2>&1; then
    pkill -f '/usr/lib/notification-daemon/[n]otification-daemon' 2>/dev/null || true
    pkill -x dunst 2>/dev/null || true
    dunst &
elif [ -x /usr/lib/notification-daemon/notification-daemon ]; then
    pkill -f '/usr/lib/notification-daemon/[n]otification-daemon' 2>/dev/null || true
    /usr/lib/notification-daemon/notification-daemon &
fi

# 6. Start pure-C event-driven status bar daemon (dwm-bar)
if [ -f "/tmp/dwm-bar-$USER.pid" ]; then
    old_pid=$(cat "/tmp/dwm-bar-$USER.pid" 2>/dev/null)
    if [ -n "$old_pid" ]; then
        kill "$old_pid" 2>/dev/null || true
    fi
fi
pkill -x dwm-bar 2>/dev/null || true
/home/quirinpa/.local/bin/dwm-bar &

# 7. Start warm whisper-server & Jarvis wake-word listener (if enabled in ~/.Xresources)
(
    jarvis_auto=$(xrdb -query 2>/dev/null | awk '$1 == "dwm.voice.jarvis_autostart:" {print $2}')
    if [ "$jarvis_auto" = "true" ] || [ "$jarvis_auto" = "1" ]; then
        /home/quirinpa/.local/bin/dwm-voice jarvis-start >/dev/null 2>&1 &
    else
        /home/quirinpa/.local/bin/dwm-voice server-start >/dev/null 2>&1 &
    fi
) &
