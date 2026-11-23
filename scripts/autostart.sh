#!/bin/sh
export PATH="$HOME/.local/bin:$PATH"
export GNOME_ACCESSIBILITY=1
export QT_ACCESSIBILITY=1
export QT_LINUX_ACCESSIBILITY_ALWAYS_ON=1

# 1. Set display resolution FIRST (before dwm or background tasks run)
if command -v xrandr >/dev/null 2>&1; then
    out="${DWM_OUTPUT:-$(xrdb -query 2>/dev/null | sed -n 's/^dwm\.output:[[:space:]]*//p' | tail -n 1)}"
    mode="${DWM_MODE:-$(xrdb -query 2>/dev/null | sed -n 's/^dwm\.mode:[[:space:]]*//p' | tail -n 1)}"
    out="${out:-eDP-1}"
    mode="${mode:-1600x900}"
    if xrandr --query 2>/dev/null | awk -v o="$out" -v m="$mode" '
        $1 == o && $2 == "connected" { in_out = 1; next }
        /^[^[:space:]]/ { in_out = 0 }
        in_out && $1 == m { found = 1; exit }
        END { exit(found ? 0 : 1) }
    '; then
        xrandr --output "$out" --mode "$mode"
    fi
fi

# 2. Load Xresources (xterm & dwm Catppuccin Mocha theme, fonts, padding, bar/auren prefs)
if [ -f "$HOME/.Xresources" ] && command -v xrdb >/dev/null 2>&1; then
    xrdb -nocpp -merge "$HOME/.Xresources"
    command -v dwm-msg >/dev/null 2>&1 && dwm-msg reload 2>/dev/null || true
fi

# 3. Import X11 environment into systemd/DBus synchronously (and sync any live tmux server),
# then enable AT-SPI2 accessibility bus and ensure portal is healthy in background
if command -v dbus-update-activation-environment >/dev/null 2>&1; then
    dbus-update-activation-environment --systemd DISPLAY XAUTHORITY XDG_CURRENT_DESKTOP GNOME_ACCESSIBILITY QT_ACCESSIBILITY
fi
if command -v tmux >/dev/null 2>&1 && tmux info >/dev/null 2>&1; then
    [ -n "${DISPLAY:-}" ] && tmux set-environment -g DISPLAY "$DISPLAY" >/dev/null 2>&1 || true
    [ -n "${XAUTHORITY:-}" ] && tmux set-environment -g XAUTHORITY "$XAUTHORITY" >/dev/null 2>&1 || true
fi
(
    if command -v gdbus >/dev/null 2>&1; then
        gdbus call --session --dest org.a11y.Bus --object-path /org/a11y/bus --method org.freedesktop.DBus.Properties.Set org.a11y.Status IsEnabled "<true>" >/dev/null 2>&1 || true
        gdbus call --session --dest org.a11y.Bus --object-path /org/a11y/bus --method org.freedesktop.DBus.Properties.Set org.a11y.Status ScreenReaderEnabled "<true>" >/dev/null 2>&1 || true
    fi
    for ff_dir in "$HOME"/snap/firefox/common/.mozilla/firefox/*.default* "$HOME"/.mozilla/firefox/*.default*; do
        if [ -d "$ff_dir" ] && ! grep -q "accessibility.force_disabled" "$ff_dir/user.js" 2>/dev/null; then
            printf 'user_pref("accessibility.force_disabled", -1);\n' >> "$ff_dir/user.js"
        fi
    done
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
dwm-bar &

# 7. Ensure clean microphone capture gain (avoid +12dB internal boost fan noise),
# ensure systemd jarvisd has X11 DISPLAY/XAUTHORITY, and start/warm Jarvis listener
(
    if command -v amixer >/dev/null 2>&1; then
        amixer -c 0 sset 'Internal Mic Boost' 0 >/dev/null 2>&1 || true
        amixer -c 0 sset 'Capture' 42 >/dev/null 2>&1 || true
    fi
    if command -v systemctl >/dev/null 2>&1 && systemctl --user is-active --quiet jarvisd.service 2>/dev/null; then
        jpid=$(systemctl --user show -p MainPID --value jarvisd.service 2>/dev/null || true)
        need_restart=0
        if [ -n "$jpid" ] && [ "$jpid" != "0" ] && [ -r "/proc/$jpid/environ" ]; then
            if [ -n "${DISPLAY:-}" ] && ! tr '\0' '\n' < "/proc/$jpid/environ" 2>/dev/null | grep -qx "DISPLAY=$DISPLAY"; then
                need_restart=1
            elif [ -n "${XAUTHORITY:-}" ] && ! tr '\0' '\n' < "/proc/$jpid/environ" 2>/dev/null | grep -qx "XAUTHORITY=$XAUTHORITY"; then
                need_restart=1
            fi
        fi
        if [ "$need_restart" = "1" ]; then
            systemctl --user restart jarvisd.service >/dev/null 2>&1 || true
        fi
        i=0
        while [ "$i" -lt 40 ]; do
            [ "$(jarvis status 2>/dev/null)" = "on" ] && break
            sleep 0.25
            i=$(( i + 1 ))
        done
    fi
    jarvis start >/dev/null 2>&1
    jarvis warmup >/dev/null 2>&1
) &
