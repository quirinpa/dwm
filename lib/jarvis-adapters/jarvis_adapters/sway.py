"""Sway Adapter: Executes DSL via swaymsg, wtype, playerctl, wpctl."""

import json
import subprocess
import time
import urllib.parse
from typing import Any, Dict, List

from .base import Adapter, AdapterResult


class SwayAdapter(Adapter):
    """Sway / i3-compatible window manager adapter."""

    @property
    def name(self) -> str:
        return "sway"

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

    def _swaymsg(self, *args: str) -> str:
        return self._run(["swaymsg"] + list(args))

    def _walk_nodes(self, node: Dict[str, Any], out: List[Dict[str, Any]], ws_num: int = 1) -> None:
        if node.get("type") == "workspace":
            ws_num = node.get("num", ws_num)
        if node.get("pid") and (node.get("name") or node.get("app_id")):
            cls = node.get("app_id") or node.get("window_properties", {}).get("class", "")
            out.append({
                "id": str(node.get("id", "")),
                "class": cls,
                "title": node.get("name", ""),
                "tag": ws_num,
                "visible": node.get("visible", True),
                "focused": node.get("focused", False),
            })
        for child in node.get("nodes", []) + node.get("floating_nodes", []):
            self._walk_nodes(child, out, ws_num)

    def _get_clients(self) -> List[Dict[str, Any]]:
        raw = self._swaymsg("-t", "get_tree")
        clients: List[Dict[str, Any]] = []
        try:
            tree = json.loads(raw) if raw else {}
            self._walk_nodes(tree, clients)
        except Exception:
            pass
        return clients

    def _get_focused(self) -> Dict[str, Any]:
        for c in self._get_clients():
            if c.get("focused"):
                return {"id": c["id"], "class": c["class"], "title": c["title"]}
        return {}

    def _op_focus_window(self, dsl: Dict[str, Any]) -> AdapterResult:
        target = dsl.get("target", {})
        ident = target.get("identifier") or target.get("title_contains") or target.get("class_contains")
        if not ident:
            return AdapterResult(False, error="focus_window requires target identifier")
        if ident.isdigit():
            self._swaymsg(f"[con_id={ident}] focus")
            return AdapterResult(True, output=f"focused con_id={ident}")
        for c in self._get_clients():
            if ident.lower() in c["class"].lower() or ident.lower() in c["title"].lower():
                self._swaymsg(f"[con_id={c['id']}] focus")
                return AdapterResult(True, output=f"focused {c['class']}: {c['title'][:40]}")
        return AdapterResult(False, error=f"Window not found: {ident}")

    def _op_focus_relative_window(self, dsl: Dict[str, Any]) -> AdapterResult:
        rel = dsl.get("target", {}).get("relation", "other")
        if rel in ("other", "previous"):
            self._swaymsg("focus", "prev")
        else:
            self._swaymsg("focus", "next")
        return AdapterResult(True, output=f"focused {rel} window")

    def _op_open_terminal(self, dsl: Dict[str, Any]) -> AdapterResult:
        cmd = dsl.get("target", {}).get("command", "foot")
        self._swaymsg("exec", cmd)
        return AdapterResult(True, output=f"opened terminal: {cmd}")

    def _op_open_application(self, dsl: Dict[str, Any]) -> AdapterResult:
        ident = dsl.get("target", {}).get("identifier") or dsl.get("target", {}).get("command")
        if not ident:
            return AdapterResult(False, error="open_application requires target.identifier")
        self._swaymsg("exec", ident)
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
        self._swaymsg("workspace", "number", str(ws))
        return AdapterResult(True, output=f"switched to workspace {ws}")

    def _op_workspace_move_window(self, dsl: Dict[str, Any]) -> AdapterResult:
        ws = dsl.get("args", {}).get("workspace", 1)
        ident = dsl.get("target", {}).get("identifier")
        if ident and ident.isdigit():
            self._swaymsg(f"[con_id={ident}] move container to workspace number {ws}")
        else:
            self._swaymsg("move", "container", "to", "workspace", "number", str(ws))
        return AdapterResult(True, output=f"moved window to workspace {ws}")

    def _op_window_fullscreen(self, dsl: Dict[str, Any]) -> AdapterResult:
        self._swaymsg("fullscreen", "toggle")
        return AdapterResult(True, output="toggled fullscreen")

    def _op_window_float(self, dsl: Dict[str, Any]) -> AdapterResult:
        self._swaymsg("floating", "toggle")
        return AdapterResult(True, output="toggled floating")

    def _op_window_close(self, dsl: Dict[str, Any]) -> AdapterResult:
        self._swaymsg("kill")
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
            return AdapterResult(False, error=f"Unsupported op on sway: {op}")
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
            "wm": "sway",
        }
