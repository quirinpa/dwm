"""DWM Adapter: Self-contained closed-loop execution via dwm-msg, xdotool, pactl, MPRIS."""

import glob
import json
import os
import re
import subprocess
import time
import urllib.parse
import urllib.request
from typing import Any, Dict, List, Optional, Tuple

from .base import Adapter, AdapterResult

TERMINAL_CLASSES = ("st-256color", "st", "xterm", "alacritty", "kitty", "foot", "wezterm", "urxvt")


def _decompress_mozlz4(path: str) -> bytes:
    with open(path, "rb") as f:
        data = f.read()
    if data[:8] != b"mozLz40\x00":
        raise ValueError("Invalid mozlz4 magic")
    src = data[8:]
    out = bytearray()
    ip = 0
    n = len(src)
    while ip < n:
        token = src[ip]
        ip += 1
        lit_len = token >> 4
        if lit_len == 15:
            while ip < n:
                s = src[ip]
                ip += 1
                lit_len += s
                if s != 255:
                    break
        out.extend(src[ip : ip + lit_len])
        ip += lit_len
        if ip >= n:
            break
        offset = src[ip] | (src[ip + 1] << 8)
        ip += 2
        if offset == 0:
            break
        match_len = (token & 0x0F) + 4
        if match_len == 19:
            while ip < n:
                s = src[ip]
                ip += 1
                match_len += s
                if s != 255:
                    break
        start = len(out) - offset
        for i in range(match_len):
            out.append(out[start + i])
    return bytes(out)


class DwmAdapter(Adapter):
    """dwm window manager adapter."""

    def __init__(self) -> None:
        self._desktop_apps_cache: Optional[Dict[str, str]] = None
        self._obs_cache: Optional[Dict[str, Any]] = None
        self._obs_cache_ts: float = 0.0
        self._tabs_cache: List[Dict[str, Any]] = []
        self._tabs_mtime: float = 0.0

    @property
    def name(self) -> str:
        return "dwm"

    @property
    def supported_ops(self) -> List[str]:
        return [
            "focus_window",
            "focus_relative_window",
            "open_terminal",
            "open_application",
            "open_url",
            "search_web",
            "spawn_shell_command",
            "type_text",
            "type_and_enter",
            "press_key",
            "click",
            "scroll",
            "workspace_switch",
            "workspace_move_window",
            "window_fullscreen",
            "window_float",
            "window_close",
            "media_play",
            "media_pause",
            "media_next",
            "media_prev",
            "media_volume",
            "record_start",
            "record_stop",
            "answer_question",
            "wm_help",
            "ask_agent",
            "wait",
            "inspect_terminal",
            "inspect_clipboard",
            "inspect_system",
            "run_background_command",
            "remember_fact",
            "recall_facts",
        ]

    def _run(self, args: List[str], timeout: float = 3.0, input_bytes: Optional[bytes] = None) -> str:
        try:
            res = subprocess.run(
                args,
                input=input_bytes,
                stdout=subprocess.PIPE,
                stderr=subprocess.DEVNULL,
                timeout=timeout,
                check=False,
            )
            return res.stdout.decode("utf-8", errors="ignore").strip()
        except Exception:
            return ""

    def _dwm_msg(self, *args: str) -> str:
        return self._run(["dwm-msg"] + list(args))

    def _xdotool(self, *args: str) -> str:
        return self._run(["xdotool"] + list(args))

    def _clear_modifiers(self) -> None:
        self._xdotool("keyup", "Super_L", "Super_R", "Shift_L", "Shift_R", "Control_L", "Control_R", "Alt_L", "Alt_R")

    def _is_terminal_window(self, win: Dict[str, Any]) -> bool:
        cls = (win.get("class") or "").lower()
        return any(tc in cls for tc in TERMINAL_CLASSES)

    def _is_idle_shell_terminal(self, win: Dict[str, Any]) -> bool:
        """Return True only if win is a terminal sitting at an idle interactive shell prompt (not running a TUI/app like pi, nvim, htop)."""
        if not self._is_terminal_window(win):
            return False
        wid = (win.get("id") or "").strip()
        if not wid:
            return False
        try:
            out = self._run(["xprop", "-id", wid, "_NET_WM_PID"], timeout=0.4)
            m = re.search(r"=\s*(\d+)", out)
            if not m:
                return False
            wpid = m.group(1)
            sh_pids = self._run(["pgrep", "-P", wpid], timeout=0.3).split()
            if not sh_pids:
                return False
            sh_pid = sh_pids[-1]
            sub_pids = self._run(["pgrep", "-P", sh_pid], timeout=0.3).split()
            if not sub_pids:
                # Interactive shell has no foreground child process -> idle prompt!
                return True

            # Check if any child of the shell is a tmux client
            clients_out = self._run(
                ["tmux", "list-clients", "-F", "#{client_pid}\t#{pane_current_command}\t#{pane_pid}"],
                timeout=0.4,
            )
            for line in clients_out.splitlines():
                parts = line.split("\t")
                if len(parts) >= 3 and parts[0].strip() in sub_pids:
                    pane_cmd = parts[1].strip().lower()
                    pane_pid = parts[2].strip()
                    if pane_cmd not in ("bash", "zsh", "sh", "fish", "dash", "-bash", "-zsh", "-sh"):
                        return False
                    pane_children = self._run(["pgrep", "-P", pane_pid], timeout=0.3).split()
                    return len(pane_children) == 0

            # Foreground process is running directly inside the shell (e.g. nvim, ssh, python, pi)
            return False
        except Exception:
            return False

    def _is_interactive_remote_session(self, win: Dict[str, Any]) -> bool:
        """Return True if win is a terminal currently running an interactive ssh session."""
        if not self._is_terminal_window(win):
            return False
        wid = (win.get("id") or "").strip()
        if not wid:
            return False
        try:
            out = self._run(["xprop", "-id", wid, "_NET_WM_PID"], timeout=0.3)
            m = re.search(r"=\s*(\d+)", out)
            if not m:
                return False
            wpid = m.group(1)
            sh_pids = self._run(["pgrep", "-P", wpid], timeout=0.25).split()
            if not sh_pids:
                return False
            sh_pid = sh_pids[-1]
            sub_pids = self._run(["pgrep", "-P", sh_pid], timeout=0.25).split()
            for sp in sub_pids:
                comm = self._run(["ps", "-p", sp, "-o", "comm="], timeout=0.25).strip().lower()
                if comm == "ssh":
                    return True
        except Exception:
            pass
        return False

    def _get_other_window(self) -> Dict[str, Any]:
        out = self._dwm_msg("other")
        parts = out.split("\t")
        if len(parts) >= 3 and parts[0].strip():
            return {
                "id": parts[0].strip(),
                "class": parts[1].strip(),
                "title": parts[2].strip(),
            }
        return {}

    def _focus_firefox(self) -> bool:
        # Prefer visible Firefox on current workspace first
        clients = self._get_clients()
        for c in clients:
            if c.get("visible") and "firefox" in c["class"].lower():
                self._dwm_msg("focus-win", c["id"])
                return self._wait_window_focused(c["id"], timeout=1.2)
        for c in clients:
            if "firefox" in c["class"].lower():
                self._dwm_msg("focus-win", c["id"])
                return self._wait_window_focused(c["id"], timeout=1.2)
        return False

    def _get_clients(self) -> List[Dict[str, Any]]:
        out = self._dwm_msg("clients")
        clients = []
        for line in out.splitlines():
            parts = line.split("\t")
            if len(parts) >= 3:
                tag_num = int(parts[3].strip()) if (len(parts) >= 4 and parts[3].strip().isdigit()) else 1
                vis = (parts[4].strip() == "1") if len(parts) >= 5 else True
                clients.append({
                    "id": parts[0].strip(),
                    "class": parts[1].strip(),
                    "title": parts[2].strip(),
                    "tag": tag_num,
                    "visible": vis,
                })
        return clients

    def _get_focused(self) -> Dict[str, Any]:
        out = self._dwm_msg("focused")
        parts = out.split("\t")
        if len(parts) >= 3 and parts[0].strip():
            return {"id": parts[0].strip(), "class": parts[1].strip(), "title": parts[2].strip()}
        return {}

    def _get_win_geometry(self, wid: Optional[str] = None) -> Tuple[int, int, int, int]:
        if not wid:
            foc = self._get_focused()
            wid = foc.get("id") if foc else ""
        if wid:
            out = self._run(["xwininfo", "-id", wid], timeout=1.2)
            mx = re.search(r"Absolute upper-left X:\s+(-?\d+)", out)
            my = re.search(r"Absolute upper-left Y:\s+(-?\d+)", out)
            mw = re.search(r"Width:\s+(\d+)", out)
            mh = re.search(r"Height:\s+(\d+)", out)
            if mx and my and mw and mh:
                return int(mx.group(1)), int(my.group(1)), int(mw.group(1)), int(mh.group(1))
        return 0, 0, 1920, 1080

    def _get_mouse_location(self) -> Tuple[int, int]:
        out = self._xdotool("getmouselocation", "--shell")
        x, y = 0, 0
        for line in out.splitlines():
            if line.startswith("X="):
                x = int(line[2:].strip())
            elif line.startswith("Y="):
                y = int(line[2:].strip())
        return x, y

    def _wait_window_focused(self, target_id_or_class: str, timeout: float = 2.0) -> bool:
        t_low = (target_id_or_class or "").lower().strip()
        if not t_low:
            return False
        deadline = time.time() + timeout
        while time.time() < deadline:
            foc = self._get_focused()
            if foc and (foc["id"].lower() == t_low or t_low in foc["class"].lower() or t_low in foc["title"].lower()):
                return True
            time.sleep(0.05)
        return False

    def _get_audio_state(self) -> Dict[str, Any]:
        sink_out = self._run(["pactl", "list", "sink-inputs"], timeout=1.5)
        uncorked = 0
        for block in sink_out.split("Sink Input #"):
            if not block.strip():
                continue
            block_low = block.lower()
            if "corked: no" in block:
                if not any(
                    h in block_low
                    for h in (
                        "webrtc voiceengine",
                        "discord",
                        "pw-play",
                        "paplay",
                        "aplay",
                        "canberra",
                        "speech-dispatcher",
                        "espeak",
                        "piper",
                        "sox",
                    )
                ):
                    uncorked += 1

        mpris_players = []
        names_out = self._run(
            [
                "gdbus",
                "call",
                "--session",
                "--dest",
                "org.freedesktop.DBus",
                "--object-path",
                "/org/freedesktop/DBus",
                "--method",
                "org.freedesktop.DBus.ListNames",
            ],
            timeout=1.2,
        )
        for token in names_out.replace("'", " ").replace(",", " ").replace("[", " ").replace("]", " ").split():
            if token.startswith("org.mpris.MediaPlayer2."):
                st_out = self._run(
                    [
                        "gdbus",
                        "call",
                        "--session",
                        "--dest",
                        token,
                        "--object-path",
                        "/org/mpris/MediaPlayer2",
                        "--method",
                        "org.freedesktop.DBus.Properties.Get",
                        "org.mpris.MediaPlayer2.Player",
                        "PlaybackStatus",
                    ],
                    timeout=1.0,
                )
                status = "Unknown"
                for s_cand in ("Playing", "Paused", "Stopped"):
                    if s_cand in st_out:
                        status = s_cand
                        break
                mpris_players.append({"bus": token, "status": status})

        mpris_playing = any(p["status"] == "Playing" for p in mpris_players)
        return {"playing": (uncorked > 0) or mpris_playing, "sink_inputs": uncorked, "mpris_players": mpris_players}

    def _mpris_control(self, action: str = "Play") -> bool:
        state = self._get_audio_state()
        ok = False
        for p in state.get("mpris_players", []):
            out = self._run(
                [
                    "gdbus",
                    "call",
                    "--session",
                    "--dest",
                    p["bus"],
                    "--object-path",
                    "/org/mpris/MediaPlayer2",
                    "--method",
                    f"org.mpris.MediaPlayer2.Player.{action}",
                ],
                timeout=1.2,
            )
            if out != "":
                ok = True
        return ok

    def _wait_audio_playing(self, timeout: float = 2.2) -> bool:
        deadline = time.time() + timeout
        while time.time() < deadline:
            if self._get_audio_state().get("playing"):
                return True
            time.sleep(0.15)
        return False

    def _resolve_youtube_url(self, query: str) -> str:
        q_enc = urllib.parse.quote_plus(query.strip())
        search_url = f"https://www.youtube.com/results?search_query={q_enc}"
        try:
            req = urllib.request.Request(
                search_url,
                headers={
                    "User-Agent": "Mozilla/5.0 (X11; Linux x86_64; rv:128.0) Gecko/20100101 Firefox/128.0",
                    "Accept-Language": "en-US,en;q=0.9",
                },
            )
            with urllib.request.urlopen(req, timeout=3.5) as resp:
                html = resp.read().decode("utf-8", errors="ignore")
            m = re.search(r'"videoRenderer":\{"videoId":"([a-zA-Z0-9_-]{11})"', html)
            if not m:
                m = re.search(r'/watch\?v=([a-zA-Z0-9_-]{11})', html)
            if m:
                return f"https://www.youtube.com/watch?v={m.group(1)}&autoplay=1"
        except Exception:
            pass
        return search_url

    # --- DSL Operation Handlers ---

    def _op_focus_window(self, dsl: Dict[str, Any]) -> AdapterResult:
        target = dsl.get("target", {})
        if target.get("relation") in ("other", "previous", "next", "master"):
            return self._op_focus_relative_window(dsl)

        identifier = target.get("identifier") or target.get("title_contains") or target.get("class_contains")
        if not identifier:
            return AdapterResult(False, error="focus_window requires target.identifier/title_contains/class_contains")

        if identifier.startswith("0x"):
            self._dwm_msg("focus-win", identifier)
            ok = self._wait_window_focused(identifier, timeout=1.2)
            return AdapterResult(ok, output=f"focused window {identifier}")

        clients = self._get_clients()
        # 1. Prefer visible client on the current workspace first
        for c in clients:
            if c.get("visible") and (
                identifier.lower() in c["class"].lower() or identifier.lower() in c["title"].lower()
            ):
                self._dwm_msg("focus-win", c["id"])
                ok = self._wait_window_focused(c["id"], timeout=1.2)
                return AdapterResult(ok, output=f"focused {c['class']}: {c['title'][:40]}")

        # 2. Fallback to any workspace
        for c in clients:
            if identifier.lower() in c["class"].lower() or identifier.lower() in c["title"].lower():
                self._dwm_msg("focus-win", c["id"])
                ok = self._wait_window_focused(c["id"], timeout=1.2)
                return AdapterResult(ok, output=f"focused {c['class']}: {c['title'][:40]}")

        return AdapterResult(False, error=f"Window not found: {identifier}")

    def _op_focus_relative_window(self, dsl: Dict[str, Any]) -> AdapterResult:
        relation = dsl.get("target", {}).get("relation", "other")
        if relation == "other":
            other = self._get_other_window()
            if other and other.get("id"):
                self._dwm_msg("focus-win", other["id"])
                ok = self._wait_window_focused(other["id"], timeout=1.0)
                return AdapterResult(ok, output=f"focused other window: {other['class']}")
            return AdapterResult(False, error="No other window available")
        elif relation == "previous":
            self._dwm_msg("focusstack", "prev")
            return AdapterResult(True, output="focused previous window")
        elif relation == "next":
            self._dwm_msg("focusstack", "next")
            return AdapterResult(True, output="focused next window")
        elif relation == "master":
            self._dwm_msg("zoom")
            return AdapterResult(True, output="swapped with master")
        return AdapterResult(False, error=f"Unknown relation: {relation}")

    def _op_open_terminal(self, dsl: Dict[str, Any]) -> AdapterResult:
        cmd = dsl.get("target", {}).get("command", "st")
        prev_foc = self._get_focused()
        prev_id = (prev_foc.get("id") or "").lower()
        term_cls = "xterm" if cmd.startswith("xterm") else "st-256color"

        proc = subprocess.Popen(
            ["/bin/sh", "-c", cmd],
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            start_new_session=True,
            close_fds=True,
        )

        # Wait specifically for a NEW terminal window ID (!= prev_id) to become focused
        focused_new = False
        deadline = time.time() + 1.5
        time.sleep(0.03)
        while time.time() < deadline:
            foc = self._get_focused()
            if foc:
                cid = (foc.get("id") or "").lower()
                ccls = (foc.get("class") or "").lower()
                if term_cls in ccls and (not prev_id or cid != prev_id):
                    focused_new = True
                    break
            time.sleep(0.035)

        if not focused_new:
            self._wait_window_focused(term_cls, timeout=0.6)

        # Wait briefly for interactive shell (.bashrc/.zshrc) subshells to settle before returning
        t_end = time.time() + 0.65
        idle_hits = 0
        time.sleep(0.06)
        while time.time() < t_end:
            try:
                r1 = subprocess.run(
                    ["pgrep", "-P", str(proc.pid)],
                    stdout=subprocess.PIPE,
                    stderr=subprocess.DEVNULL,
                    text=True,
                    timeout=0.15,
                )
                pids = r1.stdout.split()
                if pids:
                    sh_pid = pids[-1]
                    r2 = subprocess.run(
                        ["pgrep", "-P", sh_pid],
                        stdout=subprocess.DEVNULL,
                        stderr=subprocess.DEVNULL,
                        timeout=0.15,
                    )
                    if r2.returncode != 0:
                        idle_hits += 1
                        if idle_hits >= 2:
                            break
                    else:
                        idle_hits = 0
            except Exception:
                break
            time.sleep(0.035)

        return AdapterResult(True, output=f"opened terminal: {cmd}")

    def _op_open_application(self, dsl: Dict[str, Any]) -> AdapterResult:
        target = dsl.get("target", {}) or {}
        args = dsl.get("args", {}) or {}
        identifier = (
            target.get("identifier")
            or target.get("command")
            or target.get("app")
            or target.get("name")
            or args.get("identifier")
            or args.get("app")
            or args.get("command")
            or args.get("name")
            or ""
        ).strip()
        if not identifier:
            return AdapterResult(False, error="open_application requires target.identifier")

        # Resolve human application names via discovered .desktop entries (e.g. "calculator" -> "gnome-calculator")
        apps = self._get_desktop_apps()
        id_low = identifier.lower()
        if id_low in apps:
            identifier = apps[id_low]
        elif id_low.startswith("a ") and id_low[2:] in apps:
            identifier = apps[id_low[2:]]
        elif id_low.startswith("the ") and id_low[4:] in apps:
            identifier = apps[id_low[4:]]

        if identifier in ("terminal", "st", "xterm"):
            return self._op_open_terminal({"target": {"command": "st" if identifier == "terminal" else identifier}})

        subprocess.Popen(
            ["/bin/sh", "-c", identifier],
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            start_new_session=True,
            close_fds=True,
        )
        base_bin = identifier.split()[0]
        deadline = time.time() + 1.2
        while time.time() < deadline:
            for c in self._get_clients():
                if base_bin.lower() in c["class"].lower():
                    return AdapterResult(True, output=f"launched {identifier}")
            time.sleep(0.08)
        return AdapterResult(True, output=f"launched {identifier}")

    def _op_open_url(self, dsl: Dict[str, Any]) -> AdapterResult:
        url = dsl.get("target", {}).get("url")
        if not url:
            return AdapterResult(False, error="open_url requires target.url")
        if not url.startswith(("http://", "https://")):
            url = "https://" + url
        subprocess.Popen(["firefox", "--new-tab", url], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self._focus_firefox()
        return AdapterResult(True, output=f"opened URL: {url}")

    def _op_search_web(self, dsl: Dict[str, Any]) -> AdapterResult:
        query = dsl.get("target", {}).get("query", "")
        if not query:
            return AdapterResult(False, error="search_web requires target.query")
        url = f"https://www.google.com/search?q={urllib.parse.quote_plus(query)}"
        return self._op_open_url({"target": {"url": url}})

    def _op_spawn_shell_command(self, dsl: Dict[str, Any]) -> AdapterResult:
        target = dsl.get("target", {})
        args = dsl.get("args", {})
        cmd = target.get("command")
        if not cmd:
            return AdapterResult(False, error="spawn_shell_command requires target.command")

        relation = target.get("relation", "")
        new_term = bool(target.get("new_terminal") or args.get("new_terminal"))
        prev_foc = self._get_focused()

        if relation == "other":
            # Explicitly asked to run in the other window
            other = self._get_other_window()
            if not other or not other.get("id"):
                return AdapterResult(False, error="No other window to run command in")
            self._dwm_msg("focus-win", other["id"])
            self._wait_window_focused(other["id"], timeout=0.8)
            self._clear_modifiers()
            self._xdotool("type", "--clearmodifiers", "--delay", "2", "--", cmd)
            time.sleep(0.02)
            self._xdotool("key", "--clearmodifiers", "Return")
            time.sleep(0.05)
            if prev_foc and prev_foc.get("id") and prev_foc["id"] != other["id"]:
                self._dwm_msg("focus-win", prev_foc["id"])
                self._wait_window_focused(prev_foc["id"], timeout=0.4)
            return AdapterResult(True, output=f"ran in other window ({other['class']}): {cmd}")

        # Normal case: If already focused on an idle shell terminal (or an active interactive SSH shell session)
        # and not explicitly requesting a new terminal, type into it!
        # Otherwise (focused on a non-terminal OR a terminal busy running a local TUI/app like pi/nvim/htop), spawn a fresh terminal!
        if new_term or not (self._is_idle_shell_terminal(prev_foc) or self._is_interactive_remote_session(prev_foc)):
            self._op_open_terminal({"target": {"command": "st"}})

        self._clear_modifiers()
        self._xdotool("type", "--clearmodifiers", "--delay", "2", "--", cmd)
        time.sleep(0.02)
        self._xdotool("key", "--clearmodifiers", "Return")
        time.sleep(0.04)
        return AdapterResult(True, output=f"ran command: {cmd}")

    def _op_type_text(self, dsl: Dict[str, Any]) -> AdapterResult:
        text = dsl.get("args", {}).get("text", "")
        if not text:
            return AdapterResult(False, error="type_text requires args.text")
        relation = dsl.get("target", {}).get("relation", "")
        prev_foc = self._get_focused()

        if relation == "other":
            other = self._get_other_window()
            if not other or not other.get("id"):
                return AdapterResult(False, error="No other window to type into")
            self._dwm_msg("focus-win", other["id"])
            self._wait_window_focused(other["id"], timeout=0.8)

        self._clear_modifiers()
        self._xdotool("type", "--clearmodifiers", "--delay", "2", "--", text)
        time.sleep(0.03)

        if relation == "other" and prev_foc and prev_foc.get("id"):
            self._dwm_msg("focus-win", prev_foc["id"])
            self._wait_window_focused(prev_foc["id"], timeout=0.4)
        return AdapterResult(True, output=f"typed: {text[:40]}")

    def _op_type_and_enter(self, dsl: Dict[str, Any]) -> AdapterResult:
        text = dsl.get("args", {}).get("text", "")
        relation = dsl.get("target", {}).get("relation", "")
        prev_foc = self._get_focused()

        if relation == "other":
            other = self._get_other_window()
            if not other or not other.get("id"):
                return AdapterResult(False, error="No other window to type into")
            self._dwm_msg("focus-win", other["id"])
            self._wait_window_focused(other["id"], timeout=0.8)

        self._clear_modifiers()
        if text:
            self._xdotool("type", "--clearmodifiers", "--delay", "2", "--", text)
            time.sleep(0.03)
        self._xdotool("key", "--clearmodifiers", "Return")
        time.sleep(0.04)

        if relation == "other" and prev_foc and prev_foc.get("id"):
            self._dwm_msg("focus-win", prev_foc["id"])
            self._wait_window_focused(prev_foc["id"], timeout=0.4)
        return AdapterResult(True, output=f"typed+enter: {text[:40]}")

    def _op_press_key(self, dsl: Dict[str, Any]) -> AdapterResult:
        combo = dsl.get("args", {}).get("key_combo", "Return")
        relation = dsl.get("target", {}).get("relation", "")
        prev_foc = self._get_focused()

        if relation == "other":
            other = self._get_other_window()
            if other and other.get("id"):
                self._dwm_msg("focus-win", other["id"])
                self._wait_window_focused(other["id"], timeout=0.8)

        self._clear_modifiers()
        self._xdotool("key", "--clearmodifiers", *combo.split())
        time.sleep(0.05)

        if relation == "other" and prev_foc and prev_foc.get("id"):
            self._dwm_msg("focus-win", prev_foc["id"])
            self._wait_window_focused(prev_foc["id"], timeout=0.4)
        return AdapterResult(True, output=f"key: {combo}")

    def _op_click(self, dsl: Dict[str, Any]) -> AdapterResult:
        target = dsl.get("target", {})
        x = target.get("x")
        y = target.get("y")
        if x is None or y is None:
            wx, wy, ww, wh = self._get_win_geometry()
            x, y = wx + ww // 2, wy + wh // 2
        button = dsl.get("args", {}).get("button", 1)
        self._xdotool("mousemove", "--sync", str(x), str(y), "click", str(button))
        return AdapterResult(True, output=f"click at ({x},{y}) button {button}")

    def _op_scroll(self, dsl: Dict[str, Any]) -> AdapterResult:
        direction = (dsl.get("args", {}).get("direction") or "down").lower()
        amount = max(1, min(20, int(dsl.get("args", {}).get("amount", 5))))
        btn = {"up": "4", "down": "5", "left": "6", "right": "7"}.get(direction, "5")
        wx, wy, ww, wh = self._get_win_geometry()
        mx, my = self._get_mouse_location()
        if not (wx <= mx <= wx + ww and wy <= my <= wy + wh):
            self._xdotool("mousemove", "--sync", str(wx + ww // 2), str(wy + wh // 2))
        self._xdotool("click", "--repeat", str(amount), "--delay", "30", btn)
        time.sleep(0.05)
        return AdapterResult(True, output=f"scrolled {direction} x{amount}")

    def _op_workspace_switch(self, dsl: Dict[str, Any]) -> AdapterResult:
        ws = int(dsl.get("args", {}).get("workspace", 1))
        if 1 <= ws <= 9:
            self._dwm_msg("view", str(ws))
            return AdapterResult(True, output=f"switched to workspace {ws}")
        return AdapterResult(False, error="workspace must be 1-9")

    def _op_workspace_move_window(self, dsl: Dict[str, Any]) -> AdapterResult:
        ws = int(dsl.get("args", {}).get("workspace", 1))
        target_dict = dsl.get("target", {})
        target = target_dict.get("identifier")
        if not target and target_dict.get("relation") == "other":
            other = self._get_other_window()
            target = other.get("id", "")
        if not target:
            foc = self._get_focused()
            target = foc.get("id", "")
        if target and 1 <= ws <= 9:
            self._dwm_msg("tag-win", target, str(ws))
            return AdapterResult(True, output=f"moved window {target} to workspace {ws}")
        return AdapterResult(False, error="invalid window or workspace")

    def _op_window_fullscreen(self, dsl: Dict[str, Any]) -> AdapterResult:
        self._dwm_msg("togglefullscreen")
        return AdapterResult(True, output="toggled fullscreen")

    def _op_window_float(self, dsl: Dict[str, Any]) -> AdapterResult:
        self._dwm_msg("togglefloating")
        return AdapterResult(True, output="toggled floating")

    def _op_window_close(self, dsl: Dict[str, Any]) -> AdapterResult:
        target_dict = dsl.get("target", {})
        kind = (target_dict.get("kind") or "").lower()
        if kind == "tab":
            self._clear_modifiers()
            self._xdotool("key", "--clearmodifiers", "ctrl+w")
            return AdapterResult(True, output="closed browser tab")
        target = target_dict.get("identifier")
        if not target and target_dict.get("relation") == "other":
            other = self._get_other_window()
            target = other.get("id", "")
            if target:
                self._dwm_msg("close-win", target)
                return AdapterResult(True, output=f"closed other window ({other.get('class', '')})")
        if not target:
            foc = self._get_focused()
            target = foc.get("id", "")
        if target:
            self._dwm_msg("killclient", target)
            return AdapterResult(True, output="closed window")
        return AdapterResult(False, error="no window to close")

    def _op_media_play(self, dsl: Dict[str, Any]) -> AdapterResult:
        query = (dsl.get("target", {}).get("query") or "").strip()
        q_clean = re.sub(r"[^a-z0-9\s]", "", query.lower()).strip()
        resume_phrases = {
            "",
            "music",
            "song",
            "audio",
            "track",
            "video",
            "it",
            "this",
            "that",
            "play",
            "press play",
            "hit play",
            "resume",
            "unpause",
            "continue",
            "start playing",
            "keep playing",
        }
        if query and q_clean not in resume_phrases:
            # Pause any existing MPRIS player before launching a new YouTube track
            self._mpris_control("Pause")
            watch_url = self._resolve_youtube_url(query)
            subprocess.Popen(["firefox", "--new-tab", watch_url], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            self._focus_firefox()
            if self._wait_audio_playing(timeout=3.5):
                return AdapterResult(True, output=f"playing: {query}")
            # Wait briefly for YouTube DOM to finish hydrating if tab just opened, then try MPRIS Play or click player center
            time.sleep(0.8)
            if self._mpris_control("Play") and self._wait_audio_playing(timeout=1.5):
                return AdapterResult(True, output=f"playing: {query}")
            wx, wy, ww, wh = self._get_win_geometry()
            self._clear_modifiers()
            self._xdotool("mousemove", "--sync", str(wx + ww // 2), str(wy + int(wh * 0.45)), "click", "1")
            if self._wait_audio_playing(timeout=1.8):
                return AdapterResult(True, output=f"playing: {query}")
            return AdapterResult(True, output=f"playing on YouTube: {query}")

        if self._mpris_control("Play") and self._wait_audio_playing(timeout=1.2):
            return AdapterResult(True, output="resumed via MPRIS")
        foc = self._get_focused()
        if foc and "firefox" in (foc.get("class") or "").lower():
            wx, wy, ww, wh = self._get_win_geometry(foc.get("id"))
            self._clear_modifiers()
            self._xdotool("mousemove", "--sync", str(wx + ww // 2), str(wy + int(wh * 0.45)), "click", "1")
            if self._wait_audio_playing(timeout=1.2):
                return AdapterResult(True, output="toggled browser video playback")
        self._clear_modifiers()
        self._xdotool("key", "--clearmodifiers", "XF86AudioPlay")
        return AdapterResult(True, output="toggled media play")

    def _op_media_pause(self, dsl: Dict[str, Any]) -> AdapterResult:
        if not self._mpris_control("Pause"):
            self._clear_modifiers()
            self._xdotool("key", "--clearmodifiers", "XF86AudioPlay")
        return AdapterResult(True, output="paused media")

    def _op_media_next(self, dsl: Dict[str, Any]) -> AdapterResult:
        self._mpris_control("Next")
        return AdapterResult(True, output="next track")

    def _op_media_prev(self, dsl: Dict[str, Any]) -> AdapterResult:
        self._mpris_control("Previous")
        return AdapterResult(True, output="previous track")

    def _op_media_volume(self, dsl: Dict[str, Any]) -> AdapterResult:
        action = dsl.get("args", {}).get("action", "volume_up")
        vol_map = {
            "volume_up": "dwm-ctl vol-up",
            "volume_down": "dwm-ctl vol-down",
            "volume_mute": "dwm-ctl vol-mute",
            "mute": "dwm-ctl vol-mute",
        }
        cmd = vol_map.get(action)
        if cmd:
            self._run(cmd.split())
            return AdapterResult(True, output=f"volume: {action}")
        return AdapterResult(False, error=f"unknown volume action: {action}")

    def _op_record_start(self, dsl: Dict[str, Any]) -> AdapterResult:
        mode = dsl.get("args", {}).get("mode", "both")
        out = self._run(["dwm-rec", "start", mode], timeout=4.0)
        return AdapterResult(True, output=f"recording started ({mode}): {out}")

    def _op_record_stop(self, dsl: Dict[str, Any]) -> AdapterResult:
        out = self._run(["dwm-rec", "stop"], timeout=6.0)
        return AdapterResult(True, output=f"recording stopped: {out}")

    def _op_wait(self, dsl: Dict[str, Any]) -> AdapterResult:
        seconds = float(dsl.get("args", {}).get("seconds", dsl.get("args", {}).get("duration", 1.0)))
        time.sleep(max(0.0, min(seconds, 10.0)))
        return AdapterResult(True, output=f"waited {seconds}s")

    def _is_tts_enabled(self) -> bool:
        if os.environ.get("JARVIS_NO_TTS", "0") == "1":
            return False
        env_tts = os.environ.get("JARVIS_TTS")
        if env_tts is not None:
            return env_tts != "0"
        tts_conf = os.path.expanduser("~/.config/dwm-voice/tts_enabled")
        try:
            if os.path.exists(tts_conf):
                with open(tts_conf, "r", encoding="utf-8") as f:
                    return f.read().strip() not in ("0", "false", "off", "no")
        except Exception:
            pass
        return True

    def _op_answer_question(self, dsl: Dict[str, Any]) -> AdapterResult:
        args = dsl.get("args", {}) or {}
        target = dsl.get("target", {}) or {}
        answer = (
            args.get("answer")
            or args.get("text")
            or target.get("answer")
            or target.get("query")
            or ""
        ).strip()
        if not answer:
            return AdapterResult(False, error="answer_question requires args.answer")
        speak = self._is_tts_enabled()
        try:
            subprocess.Popen(
                [
                    "notify-send",
                    "-t",
                    "8000",
                    "-h",
                    "string:x-dunst-stack-tag:jarvis-answer",
                    "  Jarvis",
                    answer,
                ],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
        except Exception:
            pass
        if speak:
            self._speak_tts(answer)
        return AdapterResult(True, output=answer)

    def _speak_tts(self, text: str) -> None:
        if os.environ.get("JARVIS_NO_TTS", "0") == "1":
            return
        clean_text = re.sub(r"[`*_#\[\]]", "", (text or "").strip())
        clean_text = " ".join(clean_text.split())
        if not clean_text:
            return
        if len(clean_text) > 260:
            cut = clean_text[:260].rfind(". ")
            clean_text = (clean_text[: cut + 1] if cut > 60 else clean_text[:260]).strip()

        # Primary: Human-grade neural TTS daemon (Kokoro-82M / Piper ONNX) with automatic mic mute protection
        dwm_tts_bin = os.path.expanduser("~/.local/bin/dwm-tts")
        if os.path.exists(dwm_tts_bin):
            try:
                res = subprocess.run(
                    [dwm_tts_bin, "speak", clean_text],
                    stdin=subprocess.DEVNULL,
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL,
                    timeout=25.0,
                    check=False,
                )
                if res.returncode == 0:
                    return
            except Exception:
                pass

        # Fallback: spd-say with mic mute guard
        try:
            self._run(["wpctl", "set-mute", "@DEFAULT_AUDIO_SOURCE@", "1"], timeout=0.4)
            self._run(["spd-say", "-C"], timeout=0.4)
            subprocess.run(
                ["spd-say", "-o", "espeak-ng", "-r", "12", "-w", clean_text],
                stdin=subprocess.DEVNULL,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                timeout=12.0,
                check=False,
            )
            time.sleep(0.25)
        except Exception:
            pass
        finally:
            self._run(["wpctl", "set-mute", "@DEFAULT_AUDIO_SOURCE@", "0"], timeout=0.4)

    def confirm_action(self, description: str) -> bool:
        """Ask the user for confirmation via dmenu before running a destructive command."""
        if os.environ.get("JARVIS_AUTO_CONFIRM", "0") == "1" or not os.environ.get("DISPLAY"):
            return True
        menu_input = b"Yes, execute\nNo, cancel\n"
        try:
            proc = subprocess.run(
                [
                    "dmenu",
                    "-i",
                    "-l",
                    "2",
                    "-p",
                    f" Confirm ({description[:48]}):",
                    "-fn",
                    "Ubuntu Sans:style=Medium:size=12:antialias=true:autohint=true",
                    "-nb",
                    "#1e1e2e",
                    "-nf",
                    "#cdd6f4",
                    "-sb",
                    "#f38ba8",
                    "-sf",
                    "#11111b",
                ],
                input=menu_input,
                stdout=subprocess.PIPE,
                stderr=subprocess.DEVNULL,
                timeout=15.0,
                check=False,
            )
            choice = proc.stdout.decode("utf-8", errors="ignore").strip().lower()
            return choice.startswith("yes")
        except Exception:
            return False

    def inspect_terminal(
        self,
        target_hint: str = "focused",
        lines: int = 60,
        active_only: bool = False,
        foc: Optional[Dict[str, Any]] = None,
        other: Optional[Dict[str, Any]] = None,
    ) -> Dict[str, Any]:
        """Capture live terminal scrollback/tmux pane content, running process, and cwd."""
        if foc is None:
            foc = self._get_focused()
        if other is None:
            other = self._get_other_window()
        win = other if target_hint == "other" else foc
        if not self._is_terminal_window(win) and self._is_terminal_window(other):
            win = other
        cwd = self._get_focused_cwd(win) if win else os.path.expanduser("~")

        fg_process = ""
        try:
            wid = (win.get("id") or "").strip() if win else ""
            if wid:
                out = self._run(["xprop", "-id", wid, "_NET_WM_PID"], timeout=0.3)
                m = re.search(r"=\s*(\d+)", out)
                if m:
                    wpid = m.group(1)
                    sh_pids = self._run(["pgrep", "-P", wpid], timeout=0.25).split()
                    if sh_pids:
                        sh_pid = sh_pids[-1]
                        sub_pids = self._run(["pgrep", "-P", sh_pid], timeout=0.25).split()
                        target_p = sub_pids[-1] if sub_pids else sh_pid
                        if os.path.exists(f"/proc/{target_p}/cmdline"):
                            with open(f"/proc/{target_p}/cmdline", "rb") as cf:
                                fg_process = cf.read().replace(b"\x00", b" ").decode("utf-8", errors="ignore").strip()
                        if not fg_process:
                            fg_process = self._run(["ps", "-p", target_p, "-o", "args="], timeout=0.25).strip()
        except Exception:
            pass

        tmux_panes: List[Dict[str, str]] = []
        try:
            sess_out = self._run(
                ["tmux", "list-panes", "-a", "-F", "#{session_name}:#{window_index}.#{pane_index}\t#{pane_current_command}\t#{pane_current_path}\t#{pane_active}\t#{window_active}"],
                timeout=0.5,
            )
            for line in sess_out.splitlines():
                parts = line.split("\t")
                if len(parts) >= 3:
                    pane_id, p_cmd, p_path = parts[0], parts[1], parts[2]
                    is_active = (parts[3] == "1") if len(parts) >= 4 else False
                    win_active = (parts[4] == "1") if len(parts) >= 5 else True
                    if active_only and not (is_active and win_active):
                        continue
                    cap = self._run(["tmux", "capture-pane", "-p", "-t", pane_id, "-S", f"-{max(10, min(lines, 120))}"], timeout=0.4)
                    non_empty = [ln for ln in cap.splitlines() if ln.strip()]
                    tmux_panes.append({
                        "pane": pane_id,
                        "command": p_cmd,
                        "cwd": p_path,
                        "active": is_active,
                        "tail": "\n".join(non_empty[-lines:]),
                    })
                    if active_only and len(tmux_panes) >= 2:
                        break
        except Exception:
            pass

        return {
            "window": win,
            "cwd": cwd,
            "foreground_process": fg_process,
            "tmux_panes": tmux_panes[:4],
        }

    def inspect_clipboard(self) -> Dict[str, str]:
        """Read X11 primary selection and clipboard contents."""
        primary = self._run(["xclip", "-o", "-selection", "primary"], timeout=0.8)
        if not primary:
            primary = self._run(["xsel", "-op"], timeout=0.8)
        clipboard = self._run(["xclip", "-o", "-selection", "clipboard"], timeout=0.8)
        if not clipboard:
            clipboard = self._run(["xsel", "-ob"], timeout=0.8)
        return {
            "primary_selection": (primary or "")[:1500],
            "clipboard": (clipboard or "")[:1500],
        }

    def inspect_system(self, aspect: str = "all") -> Dict[str, Any]:
        """Inspect CPU, memory, top processes, disk usage, and listening ports."""
        info: Dict[str, Any] = {}
        if aspect in ("all", "memory", "ram", "processes", "cpu"):
            info["memory"] = self._run(["free", "-h"], timeout=1.2)
            info["top_cpu"] = self._run(["ps", "-eo", "pid,comm,%cpu,%mem", "--sort=-%cpu"], timeout=1.2).splitlines()[:8]
            info["top_mem"] = self._run(["ps", "-eo", "pid,comm,%cpu,%mem", "--sort=-%mem"], timeout=1.2).splitlines()[:8]
        if aspect in ("all", "disk", "storage"):
            info["disk"] = self._run(["df", "-h", "/", os.path.expanduser("~")], timeout=1.2)
        if aspect in ("all", "ports", "network"):
            ports_out = self._run(["ss", "-tulpn"], timeout=1.5)
            info["listening_ports"] = ports_out.splitlines()[:15]
        return info

    def run_background_command(self, command: str, cwd: Optional[str] = None, timeout: float = 5.0) -> Dict[str, Any]:
        """Run a non-interactive shell probe/command and capture its stdout/stderr for the agent loop."""
        work_dir = os.path.expanduser(cwd) if cwd else self._get_focused_cwd(self._get_focused())
        if not os.path.isdir(work_dir):
            work_dir = os.path.expanduser("~")
        try:
            proc = subprocess.run(
                ["/bin/sh", "-c", command],
                cwd=work_dir,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                timeout=max(0.5, min(timeout, 15.0)),
                check=False,
            )
            stdout = proc.stdout.decode("utf-8", errors="ignore").strip()
            stderr = proc.stderr.decode("utf-8", errors="ignore").strip()
            return {
                "command": command,
                "cwd": work_dir,
                "returncode": proc.returncode,
                "stdout": stdout[-2000:] if len(stdout) > 2000 else stdout,
                "stderr": stderr[-1000:] if len(stderr) > 1000 else stderr,
            }
        except subprocess.TimeoutExpired:
            return {"command": command, "cwd": work_dir, "returncode": -1, "stdout": "", "stderr": "Command timed out"}
        except Exception as e:
            return {"command": command, "cwd": work_dir, "returncode": -1, "stdout": "", "stderr": str(e)}

    def _op_inspect_terminal(self, dsl: Dict[str, Any]) -> AdapterResult:
        target_hint = dsl.get("target", {}).get("relation") or dsl.get("target", {}).get("identifier") or "focused"
        data = self.inspect_terminal(target_hint=str(target_hint))
        return AdapterResult(True, output=json.dumps(data, ensure_ascii=False), metadata=data)

    def _op_inspect_clipboard(self, dsl: Dict[str, Any]) -> AdapterResult:
        data = self.inspect_clipboard()
        return AdapterResult(True, output=json.dumps(data, ensure_ascii=False), metadata=data)

    def _op_inspect_system(self, dsl: Dict[str, Any]) -> AdapterResult:
        aspect = dsl.get("args", {}).get("aspect") or dsl.get("target", {}).get("query") or "all"
        data = self.inspect_system(aspect=str(aspect))
        return AdapterResult(True, output=json.dumps(data, ensure_ascii=False), metadata=data)

    def _op_run_background_command(self, dsl: Dict[str, Any]) -> AdapterResult:
        cmd = dsl.get("target", {}).get("command") or dsl.get("args", {}).get("command") or ""
        if not cmd:
            return AdapterResult(False, error="run_background_command requires target.command")
        cwd = dsl.get("target", {}).get("cwd") or dsl.get("args", {}).get("cwd")
        data = self.run_background_command(cmd, cwd=cwd)
        ok = data.get("returncode", -1) == 0
        summary = data.get("stdout") or data.get("stderr") or f"exit {data.get('returncode')}"
        return AdapterResult(ok, output=summary, error="" if ok else data.get("stderr", ""), metadata=data)

    def _op_wm_help(self, dsl: Dict[str, Any]) -> AdapterResult:
        target = dsl.get("target", {}) or {}
        args = dsl.get("args", {}) or {}
        query = (target.get("query") or args.get("query") or args.get("text") or "").strip()
        if not query:
            subprocess.Popen(["dwm-help", "show"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            return AdapterResult(True, output="opened dwm-help")
        out = self._run(["dwm-help", "ask", query], timeout=4.0)
        return AdapterResult(True, output=out or f"dwm-help: {query}")

    def _op_ask_agent(self, dsl: Dict[str, Any]) -> AdapterResult:
        target = dsl.get("target", {}) or {}
        args = dsl.get("args", {}) or {}
        query = (target.get("query") or args.get("query") or args.get("text") or "").strip()
        if not query:
            return AdapterResult(False, error="ask_agent requires target.query")
        subprocess.Popen(
            ["dwm-jarvis-pi", "send", query],
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            start_new_session=True,
        )
        return AdapterResult(True, output=f"sent to Pi agent: {query}")

    def execute(self, dsl: Dict[str, Any]) -> AdapterResult:
        self._obs_cache_ts = 0.0
        op = dsl.get("op")
        if not op:
            return AdapterResult(False, error="DSL missing 'op' field")
        handler = getattr(self, f"_op_{op}", None)
        if not handler:
            return AdapterResult(False, error=f"Unsupported op: {op}")
        try:
            return handler(dsl)
        except Exception as e:
            return AdapterResult(False, error=f"Handler error: {e}")

    def _get_firefox_open_tabs(self) -> List[Dict[str, Any]]:
        patterns = [
            os.path.expanduser("~/snap/firefox/common/.mozilla/firefox/*.default*/sessionstore-backups/recovery.jsonlz4"),
            os.path.expanduser("~/.mozilla/firefox/*.default*/sessionstore-backups/recovery.jsonlz4"),
        ]
        paths = []
        for pat in patterns:
            paths.extend(glob.glob(pat))
        if not paths:
            return []
        paths.sort(key=lambda p: os.path.getmtime(p), reverse=True)
        try:
            mt = os.path.getmtime(paths[0])
            if mt == self._tabs_mtime and self._tabs_cache:
                return self._tabs_cache
            raw = _decompress_mozlz4(paths[0])
            j = json.loads(raw.decode("utf-8", errors="ignore"))
            tabs = []
            for w in j.get("windows", []):
                for idx, t in enumerate(w.get("tabs", []), 1):
                    entries = t.get("entries", [])
                    if entries:
                        cur = entries[min(t.get("index", len(entries)) - 1, len(entries) - 1)]
                        tabs.append({
                            "index": idx,
                            "title": cur.get("title", ""),
                            "url": cur.get("url", ""),
                        })
            self._tabs_mtime = mt
            self._tabs_cache = tabs
            return tabs
        except Exception:
            return []

    def _get_focused_cwd(self, foc: Dict[str, Any]) -> str:
        wid = foc.get("id", "")
        if not wid:
            return os.path.expanduser("~")
        try:
            out = self._run(["xprop", "-id", wid, "_NET_WM_PID"], timeout=0.4)
            m = re.search(r"=\s*(\d+)", out)
            if m:
                wpid = m.group(1)
                ch = self._run(["pgrep", "-P", wpid], timeout=0.3).split()
                target_pid = ch[-1] if ch else wpid
                cwd_link = f"/proc/{target_pid}/cwd"
                if os.path.exists(cwd_link):
                    return os.path.realpath(cwd_link)
        except Exception:
            pass
        return os.path.expanduser("~")

    def _get_desktop_apps(self) -> Dict[str, str]:
        if self._desktop_apps_cache is not None:
            return self._desktop_apps_cache
        apps: Dict[str, str] = {}
        search_dirs = [
            os.path.expanduser("~/.local/share/applications"),
            "/usr/local/share/applications",
            "/usr/share/applications",
            "/var/lib/snapd/desktop/applications",
        ]
        for d in search_dirs:
            if not os.path.isdir(d):
                continue
            for entry in sorted(os.listdir(d)):
                if not entry.endswith(".desktop"):
                    continue
                fpath = os.path.join(d, entry)
                try:
                    name = ""
                    generic = ""
                    exec_cmd = ""
                    nodisplay = False
                    in_desktop_entry = False
                    with open(fpath, "r", encoding="utf-8", errors="ignore") as f:
                        for line in f:
                            s = line.strip()
                            if s.startswith("["):
                                in_desktop_entry = (s == "[Desktop Entry]")
                                continue
                            if not in_desktop_entry:
                                continue
                            if s.startswith("NoDisplay=true") or s.startswith("Hidden=true"):
                                nodisplay = True
                                break
                            if s.startswith("Name=") and not name:
                                name = s[5:].strip()
                            elif s.startswith("GenericName=") and not generic:
                                generic = s[12:].strip()
                            elif s.startswith("Exec=") and not exec_cmd:
                                raw_exec = s[5:].strip()
                                cleaned_parts = [
                                    p for p in raw_exec.split()
                                    if not p.startswith("%") and not p.startswith("env")
                                ]
                                if cleaned_parts:
                                    exec_cmd = " ".join(cleaned_parts)
                    if not nodisplay and name and exec_cmd:
                        apps.setdefault(name.lower(), exec_cmd)
                        if generic:
                            apps.setdefault(generic.lower(), exec_cmd)
                except Exception:
                    pass
        self._desktop_apps_cache = apps
        return apps

    def _get_user_configs(self) -> List[str]:
        candidates = [
            "~/.vimrc",
            "~/.config/nvim/init.lua",
            "~/.config/nvim/init.vim",
            "~/.bashrc",
            "~/.zshrc",
            "~/.tmux.conf",
            "~/.Xresources",
            "~/.local/src/dwm/config.h",
            "~/.config/jarvis/ontology.json",
        ]
        existing = []
        for c in candidates:
            if os.path.exists(os.path.expanduser(c)):
                existing.append(c)
        return existing

    def _get_fast_system_health(self) -> str:
        """Zero-subprocess system health snapshot (<0.5ms) via /proc and os.statvfs."""
        parts: List[str] = []
        try:
            st = os.statvfs("/")
            total_gb = (st.f_blocks * st.f_frsize) / (1024 ** 3)
            free_gb = (st.f_bavail * st.f_frsize) / (1024 ** 3)
            used_pct = int(round((1.0 - (free_gb / max(total_gb, 0.1))) * 100))
            parts.append(f"Disk /: {free_gb:.1f} GB free / {total_gb:.0f} GB ({used_pct}% used)")
        except Exception:
            pass
        try:
            mem_total_kb = 0
            mem_avail_kb = 0
            with open("/proc/meminfo", "r", encoding="utf-8") as mf:
                for line in mf:
                    if line.startswith("MemTotal:"):
                        mem_total_kb = int(line.split()[1])
                    elif line.startswith("MemAvailable:"):
                        mem_avail_kb = int(line.split()[1])
                    if mem_total_kb and mem_avail_kb:
                        break
            if mem_total_kb:
                tot_g = mem_total_kb / (1024 ** 2)
                avail_g = mem_avail_kb / (1024 ** 2)
                used_g = tot_g - avail_g
                parts.append(f"RAM: {used_g:.1f} GB used / {tot_g:.1f} GB ({avail_g:.1f} GB free)")
        except Exception:
            pass
        try:
            with open("/proc/loadavg", "r", encoding="utf-8") as lf:
                ld = lf.read().split()[:3]
                parts.append(f"CPU Load: {' '.join(ld)}")
        except Exception:
            pass
        return " | ".join(parts)

    def observe(self) -> Dict[str, Any]:
        """Return live desktop observation for inference context (cached for 350ms across pipeline phases)."""
        now = time.time()
        if self._obs_cache is not None and (now - self._obs_cache_ts) < 0.35:
            return dict(self._obs_cache)
        foc = self._get_focused()
        other = self._get_other_window()
        term_snapshot = self.inspect_terminal(
            target_hint="focused",
            lines=20,
            active_only=True,
            foc=foc,
            other=other,
        )
        obs = {
            "focused": foc,
            "focused_cwd": term_snapshot.get("cwd") or self._get_focused_cwd(foc),
            "focused_process": term_snapshot.get("foreground_process", ""),
            "terminal_panes": term_snapshot.get("tmux_panes", []),
            "system_health": self._get_fast_system_health(),
            "other": other,
            "clients": self._get_clients(),
            "tabs": self._get_firefox_open_tabs(),
            "desktop_apps": self._get_desktop_apps(),
            "user_configs": self._get_user_configs(),
            "wm": "dwm",
        }
        self._obs_cache = obs
        self._obs_cache_ts = time.time()
        return dict(obs)
