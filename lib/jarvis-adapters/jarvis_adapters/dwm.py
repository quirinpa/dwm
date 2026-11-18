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
            "wait",
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
                return f"https://www.youtube.com/watch?v={m.group(1)}"
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
        identifier = dsl.get("target", {}).get("identifier") or dsl.get("target", {}).get("command")
        if not identifier:
            return AdapterResult(False, error="open_application requires target.identifier")
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
        cmd = target.get("command")
        if not cmd:
            return AdapterResult(False, error="spawn_shell_command requires target.command")

        relation = target.get("relation", "")
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

        # Normal case: If already focused on a terminal window, STAY in the current terminal!
        if not self._is_terminal_window(prev_foc):
            # Not currently in a terminal: spawn a fresh terminal on the current workspace
            # so we never hijack an unrelated background terminal session
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
        query = dsl.get("target", {}).get("query", "")
        if query and query.lower() not in ("music", "song", "audio", "track"):
            watch_url = self._resolve_youtube_url(query)
            subprocess.Popen(["firefox", "--new-tab", watch_url], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            self._focus_firefox()
            if self._wait_audio_playing(timeout=3.2):
                return AdapterResult(True, output=f"playing: {query}")
            # Nudge YouTube player with 'k' if autoplay was blocked
            self._clear_modifiers()
            self._xdotool("key", "--clearmodifiers", "k")
            if self._wait_audio_playing(timeout=1.5):
                return AdapterResult(True, output=f"playing: {query}")
            return AdapterResult(True, output=f"opened YouTube video for: {query}")

        if self._mpris_control("Play") and self._wait_audio_playing(timeout=1.2):
            return AdapterResult(True, output="resumed via MPRIS")
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
        seconds = float(dsl.get("args", {}).get("seconds", 1.0))
        time.sleep(seconds)
        return AdapterResult(True, output=f"waited {seconds}s")

    def execute(self, dsl: Dict[str, Any]) -> AdapterResult:
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
            return tabs
        except Exception:
            return []

    def observe(self) -> Dict[str, Any]:
        """Return live desktop observation for inference context."""
        return {
            "focused": self._get_focused(),
            "other": self._get_other_window(),
            "clients": self._get_clients(),
            "tabs": self._get_firefox_open_tabs(),
            "audio": self._get_audio_state(),
            "wm": "dwm",
        }
