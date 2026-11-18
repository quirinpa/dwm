"""Hyprland Adapter: Executes DSL via hyprctl, wtype, playerctl, pactl."""

import json
import subprocess
import time
import urllib.parse
from typing import Any, Dict, List

from .base import Adapter, AdapterResult


class HyprlandAdapter(Adapter):
    """Hyprland Wayland compositor adapter."""

    @property
    def name(self) -> str:
        return "hyprland"

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

    def _run(self, args: List[str], timeout: float = 3.0) -> str:
        try:
            res = subprocess.run(
                args,
                stdout=subprocess.PIPE,
                stderr=subprocess.DEVNULL,
                timeout=timeout,
                check=False,
            )
            return res.stdout.decode("utf-8", errors="ignore").strip()
        except Exception:
            return ""

    def _hyprctl(self, *args: str) -> str:
        return self._run(["hyprctl"] + list(args))

    def _get_clients(self) -> List[Dict[str, Any]]:
        out = self._hyprctl("clients", "-j")
        try:
            raw = json.loads(out) if out else []
        except Exception:
            raw = []
        clients = []
        for c in raw:
            ws = c.get("workspace", {}).get("id", 1)
            clients.append({
                "id": c.get("address", ""),
                "class": c.get("class", ""),
                "title": c.get("title", ""),
                "tag": ws,
                "visible": not c.get("hidden", False),
            })
        return clients

    def _get_focused(self) -> Dict[str, Any]:
        out = self._hyprctl("activewindow", "-j")
        try:
            c = json.loads(out) if out else {}
            if c and c.get("address"):
                return {
                    "id": c.get("address", ""),
                    "class": c.get("class", ""),
                    "title": c.get("title", ""),
                }
        except Exception:
            pass
        return {}

    def _op_focus_window(self, dsl: Dict[str, Any]) -> AdapterResult:
        target = dsl.get("target", {})
        ident = target.get("identifier") or target.get("title_contains") or target.get("class_contains")
        if not ident:
            return AdapterResult(False, error="focus_window requires target identifier")
        if ident.startswith("0x"):
            self._hyprctl("dispatch", "focuswindow", f"address:{ident}")
            return AdapterResult(True, output=f"focused {ident}")
        for c in self._get_clients():
            if ident.lower() in c["class"].lower() or ident.lower() in c["title"].lower():
                self._hyprctl("dispatch", "focuswindow", f"address:{c['id']}")
                return AdapterResult(True, output=f"focused {c['class']}: {c['title'][:40]}")
        return AdapterResult(False, error=f"Window not found: {ident}")

    def _op_focus_relative_window(self, dsl: Dict[str, Any]) -> AdapterResult:
        rel = dsl.get("target", {}).get("relation", "other")
        if rel in ("other", "previous"):
            self._hyprctl("dispatch", "cyclenext", "prev")
        elif rel == "master":
            self._hyprctl("dispatch", "layoutmsg", "swapwithmaster")
        else:
            self._hyprctl("dispatch", "cyclenext")
        return AdapterResult(True, output=f"focused {rel} window")

    def _op_open_terminal(self, dsl: Dict[str, Any]) -> AdapterResult:
        cmd = dsl.get("target", {}).get("command", "kitty")
        self._hyprctl("dispatch", "exec", cmd)
        return AdapterResult(True, output=f"opened terminal: {cmd}")

    def _op_open_application(self, dsl: Dict[str, Any]) -> AdapterResult:
        ident = dsl.get("target", {}).get("identifier") or dsl.get("target", {}).get("command")
        if not ident:
            return AdapterResult(False, error="open_application requires target.identifier")
        self._hyprctl("dispatch", "exec", ident)
        return AdapterResult(True, output=f"launched {ident}")

    def _op_open_url(self, dsl: Dict[str, Any]) -> AdapterResult:
        url = dsl.get("target", {}).get("url", "")
        if not url:
            return AdapterResult(False, error="open_url requires target.url")
        if not url.startswith(("http://", "https://")):
            url = "https://" + url
        subprocess.Popen(["xdg-open", url], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        return AdapterResult(True, output=f"opened URL: {url}")

    def _op_search_web(self, dsl: Dict[str, Any]) -> AdapterResult:
        query = dsl.get("target", {}).get("query", "")
        if not query:
            return AdapterResult(False, error="search_web requires target.query")
        url = f"https://www.google.com/search?q={urllib.parse.quote_plus(query)}"
        return self._op_open_url({"target": {"url": url}})

    def _op_spawn_shell_command(self, dsl: Dict[str, Any]) -> AdapterResult:
        cmd = dsl.get("target", {}).get("command", "")
        if not cmd:
            return AdapterResult(False, error="spawn_shell_command requires target.command")
        self._run(["wtype", cmd])
        return AdapterResult(True, output=f"typed command: {cmd}")

    def _op_type_text(self, dsl: Dict[str, Any]) -> AdapterResult:
        text = dsl.get("args", {}).get("text", "")
        if not text:
            return AdapterResult(False, error="type_text requires args.text")
        self._run(["wtype", text])
        return AdapterResult(True, output=f"typed: {text[:40]}")

    def _op_type_and_enter(self, dsl: Dict[str, Any]) -> AdapterResult:
        text = dsl.get("args", {}).get("text", "")
        if text:
            self._run(["wtype", text, "-k", "Return"])
        else:
            self._run(["wtype", "-k", "Return"])
        return AdapterResult(True, output=f"typed+enter: {text[:40]}")

    def _op_press_key(self, dsl: Dict[str, Any]) -> AdapterResult:
        combo = dsl.get("args", {}).get("key_combo", "Return")
        self._run(["wtype", "-k", combo])
        return AdapterResult(True, output=f"key: {combo}")

    def _op_workspace_switch(self, dsl: Dict[str, Any]) -> AdapterResult:
        ws = dsl.get("args", {}).get("workspace", 1)
        self._hyprctl("dispatch", "workspace", str(ws))
        return AdapterResult(True, output=f"switched to workspace {ws}")

    def _op_workspace_move_window(self, dsl: Dict[str, Any]) -> AdapterResult:
        ws = dsl.get("args", {}).get("workspace", 1)
        ident = dsl.get("target", {}).get("identifier")
        if ident:
            self._hyprctl("dispatch", "movetoworkspacesilent", f"{ws},address:{ident}")
        else:
            self._hyprctl("dispatch", "movetoworkspacesilent", str(ws))
        return AdapterResult(True, output=f"moved window to workspace {ws}")

    def _op_window_fullscreen(self, dsl: Dict[str, Any]) -> AdapterResult:
        self._hyprctl("dispatch", "fullscreen", "0")
        return AdapterResult(True, output="toggled fullscreen")

    def _op_window_float(self, dsl: Dict[str, Any]) -> AdapterResult:
        self._hyprctl("dispatch", "togglefloating")
        return AdapterResult(True, output="toggled floating")

    def _op_window_close(self, dsl: Dict[str, Any]) -> AdapterResult:
        self._hyprctl("dispatch", "killactive")
        return AdapterResult(True, output="closed active window")

    def _op_media_play(self, dsl: Dict[str, Any]) -> AdapterResult:
        query = dsl.get("target", {}).get("query", "")
        if query:
            url = f"https://www.youtube.com/results?search_query={urllib.parse.quote_plus(query)}"
            return self._op_open_url({"target": {"url": url}})
        self._run(["playerctl", "play"])
        return AdapterResult(True, output="resumed via playerctl")

    def _op_media_pause(self, dsl: Dict[str, Any]) -> AdapterResult:
        self._run(["playerctl", "pause"])
        return AdapterResult(True, output="paused via playerctl")

    def _op_media_next(self, dsl: Dict[str, Any]) -> AdapterResult:
        self._run(["playerctl", "next"])
        return AdapterResult(True, output="next track")

    def _op_media_prev(self, dsl: Dict[str, Any]) -> AdapterResult:
        self._run(["playerctl", "previous"])
        return AdapterResult(True, output="previous track")

    def _op_media_volume(self, dsl: Dict[str, Any]) -> AdapterResult:
        action = dsl.get("args", {}).get("action", "volume_up")
        if action == "volume_up":
            self._run(["wpctl", "set-volume", "@DEFAULT_AUDIO_SINK@", "5%+"])
        elif action == "volume_down":
            self._run(["wpctl", "set-volume", "@DEFAULT_AUDIO_SINK@", "5%-"])
        else:
            self._run(["wpctl", "set-mute", "@DEFAULT_AUDIO_SINK@", "toggle"])
        return AdapterResult(True, output=f"volume: {action}")

    def _op_wait(self, dsl: Dict[str, Any]) -> AdapterResult:
        seconds = dsl.get("args", {}).get("seconds", 1.0)
        time.sleep(seconds)
        return AdapterResult(True, output=f"waited {seconds}s")

    def execute(self, dsl: Dict[str, Any]) -> AdapterResult:
        op = dsl.get("op")
        if not op:
            return AdapterResult(False, error="DSL missing 'op' field")
        handler = getattr(self, f"_op_{op}", None)
        if not handler:
            return AdapterResult(False, error=f"Unsupported op on hyprland: {op}")
        try:
            return handler(dsl)
        except Exception as e:
            return AdapterResult(False, error=f"Handler error: {e}")

    def observe(self) -> Dict[str, Any]:
        return {
            "focused": self._get_focused(),
            "clients": self._get_clients(),
            "tabs": [],
            "audio": {"playing": self._run(["playerctl", "status"]).lower() == "playing", "mpris_players": []},
            "wm": "hyprland",
        }
