#!/bin/sh
export PATH="$HOME/.local/bin:$PATH"

# 1. Set display resolution FIRST (before dwm or background tasks run)
if command -v xrandr >/dev/null 2>&1; then
    xrandr --output eDP-1 --mode 1600x900
fi

# 2. Load Xresources (xterm Catppuccin Mocha theme, fonts, padding)
if [ -f "$HOME/.Xresources" ] && command -v xrdb >/dev/null 2>&1; then
    xrdb -merge "$HOME/.Xresources"
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

# 6. Minimalist UTF-8 status bar loop (FontAwesome glyphs + Catppuccin Mocha colors + clickable segments)
# Supports instant refresh on SIGUSR1 (used by dwm-ctl for <20ms volume updates)
# Kill any stale status bar loop from a prior reload before starting a new one
if [ -f "/tmp/dwm-bar-$USER.pid" ]; then
    old_pid=$(cat "/tmp/dwm-bar-$USER.pid" 2>/dev/null)
    if [ -n "$old_pid" ]; then
        kill "$old_pid" 2>/dev/null || true
    fi
fi
(
    my_pid=$(sh -c 'echo $PPID')
    printf '%s\n' "$my_pid" > "/tmp/dwm-bar-$USER.pid"
    trap ':' USR1
    c_sys_ok=$(printf '\020')
    c_mem=$(printf '\023')
    c_cpu=$(printf '\024')
    c_vol=$(printf '\025')
    c_vmu=$(printf '\026')
    c_net=$(printf '\027')
    c_bok=$(printf '\030')
    c_bwn=$(printf '\031')
    c_bcr=$(printf '\032')
    c_dat=$(printf '\033')
    c_voc=$(printf '\034')
    c_ger=$(printf '\035')
    c_tup=$(printf '\036')
    c_tdn=$(printf '\037')
    last_tty_ts=0
    while [ "$(cat "/tmp/dwm-bar-$USER.pid" 2>/dev/null)" = "$my_pid" ]; do
        # tty.pt online state ( neutral when up, red when down; cached 10s so SIGUSR1 stays <20ms)
        now_ts=$(date +%s)
        if [ $(( now_ts - last_tty_ts )) -ge 10 ]; then
            last_tty_ts="$now_ts"
            if [ ! -f "/tmp/dwm-tty-$USER.state" ]; then
                if ping -c 1 -W 1 tty.pt >/dev/null 2>&1; then
                    printf 'up\n' > "/tmp/dwm-tty-$USER.state"
                else
                    printf 'down\n' > "/tmp/dwm-tty-$USER.state"
                fi
            else
                (
                    if ping -c 1 -W 2 tty.pt >/dev/null 2>&1; then
                        printf 'up\n' > "/tmp/dwm-tty-$USER.state.tmp"
                    else
                        printf 'down\n' > "/tmp/dwm-tty-$USER.state.tmp"
                    fi
                    mv -f "/tmp/dwm-tty-$USER.state.tmp" "/tmp/dwm-tty-$USER.state"
                ) &
            fi
        fi
        if [ "$(cat "/tmp/dwm-tty-$USER.state" 2>/dev/null)" = "up" ]; then
            tty_seg="${c_tup}  "
        else
            tty_seg="${c_tdn}  "
        fi

        # Memory (): neutral <70%, amber 70-84%, red >=85%
        mem_info=$(awk '/MemAvailable/ {avail=$2} /MemTotal/ {total=$2} END {used=total-avail; pct=(total>0)?int(used*100/total):0; printf "%.1fG %d", used/1048576, pct}' /proc/meminfo 2>/dev/null)
        mem=${mem_info% *}
        mem_pct=${mem_info#* }
        if [ "${mem_pct:-0}" -ge 85 ]; then
            mem_clr="$c_cpu"
        elif [ "${mem_pct:-0}" -ge 70 ]; then
            mem_clr="$c_mem"
        else
            mem_clr="$c_sys_ok"
        fi

        # CPU load (): neutral <4.0, amber 4.0-9.99, red >=10.0
        load=$(cut -d' ' -f1 /proc/loadavg 2>/dev/null)
        load_int=${load%.*}
        if [ "${load_int:-0}" -ge 10 ]; then
            cpu_clr="$c_cpu"
        elif [ "${load_int:-0}" -ge 4 ]; then
            cpu_clr="$c_mem"
        else
            cpu_clr="$c_sys_ok"
        fi

        # Whisper voice recording / transcribing indicator ( REC /  ...)
        voc_seg=""
        if [ -f "/tmp/dwm-voice-$USER.state" ]; then
            case "$(cat "/tmp/dwm-voice-$USER.state" 2>/dev/null)" in
                rec*)
                    voc_seg="${c_voc}   REC"
                    ;;
                transcribing*)
                    voc_seg="${c_bwn}   ..."
                    ;;
            esac
        fi

        # Audio volume via PipeWire wpctl ( /  / )
        vol_seg=""
        if command -v wpctl >/dev/null 2>&1; then
            vol_out=$(wpctl get-volume @DEFAULT_AUDIO_SINK@ 2>/dev/null)
            if [ -n "$vol_out" ]; then
                vol_pct=$(printf '%s\n' "$vol_out" | awk '{printf "%d", $2 * 100}')
                vol_clr="$c_vol"
                case "$vol_out" in
                    *MUTED*)
                        vol_icon=" ×"
                        vol_clr="$c_vmu"
                        ;;
                    *)
                        if [ "${vol_pct:-0}" -eq 0 ]; then
                            vol_icon=" 0%"
                        elif [ "${vol_pct:-0}" -lt 40 ]; then
                            vol_icon=" ${vol_pct}%"
                        else
                            vol_icon=" ${vol_pct}%"
                        fi
                        ;;
                esac
                vol_seg="${vol_clr}  ${vol_icon}"
            fi
        fi

        # Network state + active SSID / connection name ( Wi-Fi /  Ethernet)
        net_seg=""
        wifi_ssid=$(nmcli -t -f TYPE,STATE,CONNECTION dev 2>/dev/null | awk -F: '$1=="wifi" && $2=="connected" {print $3; exit}')
        if [ -n "$wifi_ssid" ]; then
            net_seg="${c_net}   ${wifi_ssid}"
        else
            for w in /sys/class/net/wl*/operstate; do
                if [ -f "$w" ] && [ "$(cat "$w" 2>/dev/null)" = "up" ]; then
                    net_seg="${c_net}  "
                    break
                fi
            done
        fi
        if [ -z "$net_seg" ]; then
            eth_conn=$(nmcli -t -f TYPE,STATE,CONNECTION dev 2>/dev/null | awk -F: '$1=="ethernet" && $2=="connected" {print $3; exit}')
            if [ -n "$eth_conn" ]; then
                net_seg="${c_net}   ${eth_conn}"
            else
                for e in /sys/class/net/en*/operstate; do
                    if [ -f "$e" ] && [ "$(cat "$e" 2>/dev/null)" = "up" ]; then
                        net_seg="${c_net}  "
                        break
                    fi
                done
            fi
        fi

        # Dynamic Battery: 5 fullness levels (    ) or bolt () when connected to power
        bat_seg=""
        ac_online=$(cat /sys/class/power_supply/AC*/online 2>/dev/null | head -n 1)
        for b in /sys/class/power_supply/BAT*; do
            if [ -f "$b/capacity" ]; then
                cap=$(cat "$b/capacity" 2>/dev/null)
                status=$(cat "$b/status" 2>/dev/null)
                if [ "${cap:-0}" -ge 85 ]; then
                    bat_icon=""
                elif [ "${cap:-0}" -ge 60 ]; then
                    bat_icon=""
                elif [ "${cap:-0}" -ge 35 ]; then
                    bat_icon=""
                elif [ "${cap:-0}" -ge 15 ]; then
                    bat_icon=""
                else
                    bat_icon=""
                fi
                bat_clr="$c_bok"
                case "$status" in
                    Charging|Full|"Not charging")
                        bat_icon=""
                        ;;
                    *)
                        if [ "$ac_online" = "1" ]; then
                            bat_icon=""
                        elif [ "${cap:-0}" -lt 15 ]; then
                            bat_clr="$c_bcr"
                        elif [ "${cap:-0}" -lt 30 ]; then
                            bat_clr="$c_bwn"
                        fi
                        ;;
                esac
                bat_seg="${bat_clr}  ${bat_icon} ${cap}%"
                break
            fi
        done

        xsetroot -name "${tty_seg}${mem_clr}   ${mem}${cpu_clr}   ${load}${voc_seg}${vol_seg}${net_seg}${bat_seg}${c_dat}   $(date '+%d/%m')   $(date '+%H:%M')${c_ger}  "
        sleep 5 &
        wait $!
    done
) &
