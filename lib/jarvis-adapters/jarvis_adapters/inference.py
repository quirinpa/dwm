"""Inference Adapter Interface: Pluggable NLU backends."""

import json
import math
import os
import re
import urllib.request
from abc import ABC, abstractmethod
from typing import Any, Dict, List, Optional, Union


def _clean_phrase(text: str) -> str:
    out = []
    for ch in (text or "").lower():
        if ch.isalnum() or ch.isspace():
            out.append(ch)
        else:
            out.append(" ")
    return " ".join("".join(out).split())


def _normalize_vec(v: List[float]) -> List[float]:
    n = math.sqrt(sum(x * x for x in v))
    return [round(x / n, 6) for x in v] if n > 1e-9 else v


def _dot_vec(a: List[float], b: List[float]) -> float:
    return sum(x * y for x, y in zip(a, b))


def _extract_json_payload(text: str) -> Any:
    """Robustly extract a JSON object or JSON array from LLM output."""
    s = (text or "").strip()
    if not s:
        return None
    if "```" in s:
        blocks = s.split("```")
        for b in blocks[1::2]:
            bc = b.strip()
            if bc.lower().startswith("json"):
                bc = bc[4:].strip()
            try:
                return json.loads(bc)
            except Exception:
                pass
    try:
        return json.loads(s)
    except Exception:
        pass
    # Find outermost { ... } or [ ... ]
    first_brace = s.find("{")
    first_bracket = s.find("[")
    if first_brace != -1 and (first_bracket == -1 or first_brace < first_bracket):
        last_brace = s.rfind("}")
        if last_brace > first_brace:
            try:
                return json.loads(s[first_brace : last_brace + 1])
            except Exception:
                pass
    if first_bracket != -1:
        last_bracket = s.rfind("]")
        if last_bracket > first_bracket:
            try:
                return json.loads(s[first_bracket : last_bracket + 1])
            except Exception:
                pass
    return None


def normalize_dsl_dict(raw: Dict[str, Any]) -> Optional[Dict[str, Any]]:
    """Canonicalize arbitrary LLM DSL variations into strict Jarvis DSL schema."""
    if not isinstance(raw, dict):
        return None

    op = str(raw.get("op") or raw.get("operation") or raw.get("action") or "").strip()
    if not op:
        return None

    # Normalize common LLM operation aliases
    op_aliases = {
        "shell": "spawn_shell_command",
        "run_command": "spawn_shell_command",
        "exec_command": "spawn_shell_command",
        "run_shell": "spawn_shell_command",
        "terminal_command": "spawn_shell_command",
        "launch_app": "open_application",
        "open_app": "open_application",
        "start_application": "open_application",
        "switch_workspace": "workspace_switch",
        "goto_workspace": "workspace_switch",
        "move_to_workspace": "workspace_move_window",
        "move_window_to_workspace": "workspace_move_window",
        "close_window": "window_close",
        "kill_window": "window_close",
        "fullscreen": "window_fullscreen",
        "toggle_fullscreen": "window_fullscreen",
        "float": "window_float",
        "toggle_float": "window_float",
        "play_media": "media_play",
        "play_music": "media_play",
        "pause_media": "media_pause",
        "next_track": "media_next",
        "prev_track": "media_prev",
        "previous_track": "media_prev",
        "volume": "media_volume",
        "set_volume": "media_volume",
        "web_search": "search_web",
        "google_search": "search_web",
        "browse_url": "open_url",
        "type": "type_text",
        "dictate": "type_text",
        "key": "press_key",
        "key_press": "press_key",
        "hotkey": "press_key",
        "answer": "answer_question",
        "reply": "answer_question",
        "help": "wm_help",
        "pi_ask": "ask_agent",
        "read_terminal": "inspect_terminal",
        "capture_terminal": "inspect_terminal",
        "read_tmux": "inspect_terminal",
        "terminal_context": "inspect_terminal",
        "read_clipboard": "inspect_clipboard",
        "get_clipboard": "inspect_clipboard",
        "read_selection": "inspect_clipboard",
        "system_health": "inspect_system",
        "check_system": "inspect_system",
        "inspect_system_health": "inspect_system",
        "probe_command": "run_background_command",
        "exec_background": "run_background_command",
        "check_command": "run_background_command",
        "shell_probe": "run_background_command",
        "remember": "remember_fact",
        "save_memory": "remember_fact",
        "save_preference": "remember_fact",
        "learn_preference": "remember_fact",
        "recall": "recall_facts",
        "search_memory": "recall_facts",
        "get_preferences": "recall_facts",
    }
    op = op_aliases.get(op, op)

    target = dict(raw.get("target") or {}) if isinstance(raw.get("target"), dict) else {}
    args = dict(raw.get("args") or {}) if isinstance(raw.get("args"), dict) else {}
    verify = dict(raw.get("verify") or {}) if isinstance(raw.get("verify"), dict) else {}

    # Lift misplaced top-level fields into target / args
    for tk in ("command", "identifier", "url", "query", "relation", "kind", "title_contains", "class_contains", "x", "y", "cwd"):
        if tk in raw and tk not in target:
            target[tk] = raw[tk]
    for ak in ("text", "key_combo", "workspace", "action", "direction", "amount", "button", "mode", "seconds", "answer", "aspect", "key", "value", "fact", "reply", "speak"):
        if ak in raw and ak not in args:
            args[ak] = raw[ak]

    # Cross-heal fields placed in args instead of target or vice-versa
    if op in ("spawn_shell_command", "run_background_command"):
        cmd = target.get("command") or args.get("command") or target.get("query") or args.get("text")
        if cmd:
            target["command"] = str(cmd).strip()
    elif op == "inspect_system":
        asp = args.get("aspect") or target.get("aspect") or target.get("query") or "all"
        args["aspect"] = str(asp).strip()
    elif op == "remember_fact":
        k = args.get("key") or target.get("key") or target.get("identifier") or "note"
        v = args.get("value") or args.get("fact") or args.get("text") or target.get("query") or target.get("value") or ""
        args["key"] = str(k).strip()
        args["value"] = str(v).strip()
    elif op == "recall_facts":
        q = target.get("query") or args.get("query") or args.get("key") or ""
        target["query"] = str(q).strip()
    elif op == "open_application":
        ident = (
            target.get("identifier")
            or target.get("command")
            or target.get("app")
            or args.get("identifier")
            or args.get("app")
            or args.get("command")
        )
        if ident:
            target["identifier"] = str(ident).strip()
    elif op == "open_url":
        url = target.get("url") or args.get("url") or target.get("identifier") or target.get("query")
        if url:
            target["url"] = str(url).strip()
    elif op in ("search_web", "media_play", "wm_help", "ask_agent"):
        q = target.get("query") or args.get("query") or args.get("text") or target.get("identifier") or target.get("command")
        if q is not None:
            target["query"] = str(q).strip()
    elif op in ("type_text", "type_and_enter"):
        txt = args.get("text") or target.get("text") or target.get("query") or target.get("command")
        if txt is not None:
            args["text"] = str(txt)
    elif op == "press_key":
        kc = args.get("key_combo") or target.get("key_combo") or args.get("key") or target.get("key") or target.get("identifier")
        if kc:
            args["key_combo"] = str(kc).strip()
    elif op in ("workspace_switch", "workspace_move_window"):
        ws = args.get("workspace") or target.get("workspace") or target.get("identifier")
        if ws is not None:
            try:
                args["workspace"] = int(re.findall(r"\d+", str(ws))[0])
            except Exception:
                args["workspace"] = 1
    elif op == "focus_relative_window":
        rel = target.get("relation") or args.get("relation") or target.get("identifier") or "other"
        if rel not in ("other", "previous", "next", "master"):
            rel = "other"
        target["relation"] = rel
        target["kind"] = "relative"
    elif op == "focus_window":
        ident = target.get("identifier") or args.get("identifier") or target.get("title_contains") or target.get("class_contains")
        if ident in ("other", "previous", "next", "master"):
            op = "focus_relative_window"
            target = {"kind": "relative", "relation": ident}
        elif ident:
            target["identifier"] = str(ident).strip()
    elif op == "answer_question":
        ans = args.get("answer") or args.get("text") or target.get("answer") or target.get("query")
        if ans:
            args["answer"] = str(ans).strip()

    dsl: Dict[str, Any] = {"op": op, "target": target, "verify": verify}
    if args:
        dsl["args"] = args
    if "meta" in raw and isinstance(raw["meta"], dict):
        dsl["meta"] = raw["meta"]
    return dsl


class InferenceBackend(ABC):
    """Abstract base for NLU inference backends."""

    @property
    @abstractmethod
    def name(self) -> str:
        """Backend identifier (e.g., 'auren-llm:fast', 'embedding')."""
        pass

    @abstractmethod
    def classify_intent(
        self,
        utterance: str,
        live_ontology: Dict[str, Any],
        dsl_schema: Dict[str, Any],
        few_shots: List[Dict[str, Any]],
    ) -> Optional[Union[Dict[str, Any], List[Dict[str, Any]]]]:
        """Classify utterance to validated DSL (or list of DSL steps)."""
        pass


_FILLER_WORDS = {
    "please",
    "jarvis",
    "hey",
    "ok",
    "okay",
    "can",
    "could",
    "would",
    "you",
    "the",
    "a",
    "an",
    "my",
    "some",
    "me",
    "for",
    "now",
    "right",
    "up",
    "to",
    "in",
    "on",
    "at",
    "of",
    "with",
    "and",
    "then",
    "this",
    "that",
}

_INTERROGATIVE_HEADS = {
    "what",
    "whats",
    "why",
    "how",
    "who",
    "when",
    "where",
    "which",
    "did",
    "does",
    "do",
    "is",
    "are",
    "explain",
}


class EmbeddingBackend(InferenceBackend):
    """Fast bge-base embedding classifier via axil-auren /v1/embeddings."""

    def __init__(
        self,
        endpoint: str = "http://localhost:4242/v1/embeddings",
        model: str = "bge-base",
        threshold: float = 0.92,
        cache_path: Optional[str] = None,
    ):
        self.endpoint = endpoint
        self.model = model
        self.threshold = threshold
        self.cache_path = cache_path or os.path.expanduser("~/.cache/jarvis/embeddings.json")
        self.vec_map: Dict[str, List[float]] = self._load_cache()

    @property
    def name(self) -> str:
        return "embedding"

    def _load_cache(self) -> Dict[str, List[float]]:
        try:
            if os.path.exists(self.cache_path):
                with open(self.cache_path, "r", encoding="utf-8") as f:
                    data = json.load(f)
                if isinstance(data, dict) and isinstance(data.get("vec_map"), dict):
                    return data["vec_map"]
        except Exception:
            pass
        return {}

    def _save_cache(self) -> None:
        try:
            os.makedirs(os.path.dirname(self.cache_path), exist_ok=True)
            with open(self.cache_path, "w", encoding="utf-8") as f:
                json.dump({"engine": self.model, "vec_map": self.vec_map}, f)
        except Exception:
            pass

    def _ensure_embedded(self, texts: List[str]) -> bool:
        missing = [t for t in dict.fromkeys(texts) if t and t not in self.vec_map]
        if not missing:
            return True
        try:
            req = urllib.request.Request(
                self.endpoint,
                data=json.dumps({"model": self.model, "input": missing}).encode("utf-8"),
                headers={"Content-Type": "application/json"},
                method="POST",
            )
            with urllib.request.urlopen(req, timeout=2.5) as resp:
                edata = json.loads(resp.read().decode("utf-8")).get("data", [])
            for t_str, item in zip(missing, edata):
                emb = item.get("embedding")
                if emb:
                    self.vec_map[t_str] = _normalize_vec(emb)
            self._save_cache()
            return True
        except Exception:
            return False

    def classify_intent(
        self,
        utterance: str,
        live_ontology: Dict[str, Any],
        dsl_schema: Dict[str, Any],
        few_shots: List[Dict[str, Any]],
    ) -> Optional[Dict[str, Any]]:
        if not few_shots:
            return None

        u_norm = _clean_phrase(utterance)
        if not u_norm:
            return None

        u_words = u_norm.split()
        while u_words and u_words[0] in ("please", "jarvis", "hey", "ok", "okay"):
            u_words.pop(0)
        # Never use static embedding lookup for questions/interrogatives — defer to LLM
        if (u_words and u_words[0] in _INTERROGATIVE_HEADS) or utterance.strip().endswith("?"):
            return None

        # Deduplicate few_shots by normalized utterance (latest wins)
        shot_by_norm: Dict[str, Dict[str, Any]] = {}
        for shot in few_shots:
            s_utt = _clean_phrase(shot.get("utterance", ""))
            s_dsl = shot.get("dsl")
            if s_utt and isinstance(s_dsl, dict) and s_dsl.get("op"):
                if s_dsl.get("op") in ("wait", "answer_question", "wm_help", "ask_agent", "type_text", "type_and_enter"):
                    continue
                shot_by_norm[s_utt] = s_dsl

        if not shot_by_norm:
            return None

        # Exact normalized hit in few_shots (0 ms)
        if u_norm in shot_by_norm:
            dsl = normalize_dsl_dict(json.loads(json.dumps(shot_by_norm[u_norm])))
            if dsl:
                dsl["meta"] = {"inference_backend": "embedding:exact", "source_ir_score": 1.0}
                return dsl

        all_texts = [u_norm] + list(shot_by_norm.keys())
        self._ensure_embedded(all_texts)

        q_vec = self.vec_map.get(u_norm)
        if not q_vec:
            return None

        best_score = -1.0
        best_dsl: Optional[Dict[str, Any]] = None

        u_tokens = set(u_norm.split())
        u_content = u_tokens - _FILLER_WORDS
        u_nums = {t for t in u_tokens if t.isdigit()}

        for s_norm, s_dsl in shot_by_norm.items():
            s_vec = self.vec_map.get(s_norm)
            if not s_vec:
                continue
            # Guard against mismatched numeric arguments (e.g. "workspace 2" vs "workspace 5")
            s_tokens = set(s_norm.split())
            s_nums = {t for t in s_tokens if t.isdigit()}
            if u_nums != s_nums:
                continue

            # Guard against novel content words not present in the cached few-shot utterance
            s_content = s_tokens - _FILLER_WORDS
            if u_content - s_content:
                continue

            # Guard against parameterized ops where the argument differs
            op = s_dsl.get("op", "")
            if op in (
                "search_web",
                "media_play",
                "type_text",
                "type_and_enter",
                "open_application",
                "open_url",
                "spawn_shell_command",
                "focus_window",
            ):
                target_arg = (
                    s_dsl.get("target", {}).get("query")
                    or s_dsl.get("target", {}).get("identifier")
                    or s_dsl.get("target", {}).get("url")
                    or s_dsl.get("target", {}).get("command")
                    or s_dsl.get("args", {}).get("text")
                    or ""
                )
                arg_tokens = set(_clean_phrase(str(target_arg)).split())
                if arg_tokens and not arg_tokens.issubset(u_tokens):
                    continue

            cos = _dot_vec(q_vec, s_vec)
            if cos > best_score:
                best_score = cos
                best_dsl = s_dsl

        if best_dsl is not None and best_score >= self.threshold:
            dsl = normalize_dsl_dict(json.loads(json.dumps(best_dsl)))
            if dsl:
                dsl["meta"] = {
                    "inference_backend": "embedding:bge-base",
                    "source_ir_score": round(best_score, 4),
                }
                return dsl

        return None


class AurenLLMBackend(InferenceBackend):
    """Remote LLM backend via axil-auren gateway (routes to cloud models: kilo-auto/free, fast, smart, etc.)."""

    def __init__(
        self,
        model: Optional[str] = None,
        endpoint: str = "http://localhost:4242/v1/chat/completions",
    ):
        self.model = model or os.environ.get("JARVIS_LLM_MODEL", "kilo-auto/free")
        self.endpoint = endpoint

    @property
    def name(self) -> str:
        return f"auren-llm:{self.model}"

    def _build_prompt(
        self,
        utterance: str,
        live_ontology: Dict[str, Any],
        dsl_schema: Dict[str, Any],
        few_shots: List[Dict[str, Any]],
    ) -> str:
        schema_desc = """- open_terminal: {"op": "open_terminal", "target": {}}
- open_application: {"op": "open_application", "target": {"identifier": "<app_binary>"}}  (GUI desktop apps: firefox, gnome-calculator, code, discord, pavucontrol, etc.)
- spawn_shell_command: {"op": "spawn_shell_command", "target": {"command": "<shell_cmd>"}}  (Run ANY shell command, pipeline, git command, file inspection, compiler, script, or CLI tool in a terminal; add "relation": "other" in target if user says "in the other window". Examples: "nvim ~/.vimrc", "git log -n 5", "ps aux | grep python", "df -h", "make -j", "cargo test", "grep -rn 'TODO' .")
- focus_window: {"op": "focus_window", "target": {"identifier": "<window_class_or_title>"}}
- focus_relative_window: {"op": "focus_relative_window", "target": {"kind": "relative", "relation": "other|next|previous|master"}}
- open_url: {"op": "open_url", "target": {"url": "https://..."}}
- search_web: {"op": "search_web", "target": {"query": "<web_search_query>"}}  (ONLY for internet/Google searches, never for local files or local shell actions)
- type_text: {"op": "type_text", "target": {}, "args": {"text": "<text_to_type>"}}  (Add "relation": "other" in target if "in the other window")
- type_and_enter: {"op": "type_and_enter", "target": {}, "args": {"text": "<text_to_type>"}}
- press_key: {"op": "press_key", "target": {}, "args": {"key_combo": "<xdotool_key_combo>"}}  (Keyboard shortcuts, e.g. "ctrl+c", "ctrl+t", "ctrl+w", "ctrl+r", "alt+Left", "Escape", "Return")
- click: {"op": "click", "target": {"x": 500, "y": 500}, "args": {"button": 1}}
- scroll: {"op": "scroll", "target": {}, "args": {"direction": "up|down|left|right", "amount": 5}}
- workspace_switch: {"op": "workspace_switch", "target": {}, "args": {"workspace": 1}}
- workspace_move_window: {"op": "workspace_move_window", "target": {}, "args": {"workspace": 1}}  (Add "relation": "other" in target if "move the other window")
- window_fullscreen: {"op": "window_fullscreen", "target": {}}
- window_float: {"op": "window_float", "target": {}}
- window_close: {"op": "window_close", "target": {"kind": "window"}}  (Add "relation": "other" in target if "close the other window")
- media_play: {"op": "media_play", "target": {"query": "<song_or_artist>"}}
- media_pause: {"op": "media_pause", "target": {}}
- media_next: {"op": "media_next", "target": {}}
- media_prev: {"op": "media_prev", "target": {}}
- media_volume: {"op": "media_volume", "target": {}, "args": {"action": "volume_up|volume_down|volume_mute"}}
- record_start: {"op": "record_start", "target": {}, "args": {"mode": "screen|mic|both"}}
- record_stop: {"op": "record_stop", "target": {}}
- answer_question: {"op": "answer_question", "target": {}, "args": {"answer": "<concise helpful answer>"}}  (Use when the user asks a conversational or factual question)
- wm_help: {"op": "wm_help", "target": {"query": "<dwm_or_shortcut_question>"}}  (Use when the user asks how to use window manager shortcuts/keybindings)
- ask_agent: {"op": "ask_agent", "target": {"query": "<coding_agent_task>"}}  (Use when the user explicitly asks Pi / the coding agent to perform a coding task)"""

        shots_text = ""
        seen = set()
        unique_shots = []
        for shot in reversed(few_shots):
            u_k = _clean_phrase(shot.get("utterance", ""))
            if u_k and u_k not in seen:
                seen.add(u_k)
                unique_shots.append(shot)
            if len(unique_shots) >= 10:
                break
        for shot in reversed(unique_shots):
            shots_text += (
                f"User: {shot.get('utterance', '')}\n"
                f"DSL: {json.dumps(shot.get('dsl', {}), separators=(',', ':'))}\n\n"
            )

        ontology_summary = self._summarize_ontology(live_ontology)

        return f"""You are Jarvis, an intelligent Linux desktop action and reasoning engine. Convert ANY user utterance — even informal, unusual, or complex requests — into a valid JSON DSL object.

AVAILABLE OPERATIONS AND EXACT SCHEMAS:
{schema_desc}

RULES:
1. For ANY local shell task, system inspection, file operation, process management, compiler/build command, git command, or CLI program (nvim, vim, grep, find, htop, bitwise, make, curl, ping, etc.), ALWAYS synthesize the exact Linux command using `spawn_shell_command` with `"target": {{"command": "<shell_cmd>"}}`.
2. For launching GUI desktop applications (e.g. calculator -> `gnome-calculator`, browser -> `firefox`, volume control -> `pavucontrol`), use `open_application` with `"target": {{"identifier": "<executable>"}}`.
3. If the user says "in the other window" or "the other window", include `"relation": "other"` inside `"target"`.
4. If the user asks a question (e.g. "What's this that you did?", "What is..."), use `answer_question` with `"args": {{"answer": "..."}}` using the RECENT CONTEXT and CURRENT DESKTOP STATE below.
5. Never fail or refuse: map every request to the best matching DSL operation.

CURRENT DESKTOP STATE & LIVE CONTEXT:
{ontology_summary}

FEW-SHOT EXAMPLES:
{shots_text}
USER: {utterance}

OUTPUT ONLY VALID JSON DSL. No markdown, no explanation."""

    def _summarize_ontology(self, obs: Dict[str, Any]) -> str:
        lines = []
        if obs.get("focused"):
            f = obs["focused"]
            lines.append(
                f"Focused window: class={f.get('class', '')}, title={f.get('title', '')[:60]}, id={f.get('id', '')}"
            )
        if obs.get("focused_cwd"):
            lines.append(f"Focused terminal working directory (cwd): {obs['focused_cwd']}")
        if obs.get("focused_process"):
            lines.append(f"Focused terminal active foreground command: {obs['focused_process']}")
        if obs.get("system_health"):
            lines.append(f"Live System Health (RAM / Disk / CPU): {obs['system_health']}")
        if obs.get("terminal_panes"):
            lines.append("Live Terminal / Tmux Panes (recent visible output & active process):")
            for p in obs["terminal_panes"][:3]:
                tail_lines = [ln for ln in (p.get("tail") or "").splitlines() if ln.strip()][-12:]
                tail_preview = " | ".join(tail_lines)
                lines.append(
                    f"  - Pane {p.get('pane', '')} [active={p.get('active', False)}, cmd={p.get('command', '')}, cwd={p.get('cwd', '')}]: {tail_preview[:360]}"
                )
        if obs.get("other") and obs["other"].get("id"):
            o = obs["other"]
            lines.append(
                f"Other window: class={o.get('class', '')}, title={o.get('title', '')[:60]}, id={o.get('id', '')}"
            )
        lines.append(f"Open windows ({len(obs.get('clients', []))}):")
        for c in obs.get("clients", [])[:8]:
            lines.append(
                f"  - [{c.get('id', '')}] {c.get('class', '')}: {c.get('title', '')[:50]} "
                f"(tag={c.get('tag', 1)}, visible={c.get('visible', True)})"
            )
        if obs.get("tabs"):
            lines.append(f"Browser tabs ({len(obs['tabs'])}):")
            for t in obs["tabs"][:5]:
                lines.append(f"  - {t.get('title', '')[:50]} ({t.get('url', '')[:60]})")
        if obs.get("user_configs"):
            lines.append("Existing user config paths: " + ", ".join(obs["user_configs"]))
        if obs.get("desktop_apps"):
            app_pairs = [f"{k} -> {v}" for k, v in list(obs["desktop_apps"].items())[:25]]
            lines.append("Installed GUI applications: " + "; ".join(app_pairs))
        audio = obs.get("audio", {})
        lines.append(f"Audio playing: {audio.get('playing', False)}")
        if audio.get("mpris_players"):
            for p in audio["mpris_players"]:
                lines.append(f"  MPRIS: {p.get('bus', '')} [{p.get('status', '')}]")
        if obs.get("context"):
            ctx = obs["context"]
            lines.append(
                f"Recent action context: last_op={ctx.get('last_op', '')}, "
                f"last_command={ctx.get('last_command', '')}, "
                f"last_app={ctx.get('last_app', '')}, "
                f"last_query={ctx.get('last_query', '')}, "
                f"last_location={ctx.get('last_location', '')}, "
                f"last_window={ctx.get('last_window_id', '')} ({ctx.get('last_window_class', '')})"
            )
        if obs.get("memories"):
            mem_items = [f"{k}: {v}" for k, v in list(obs["memories"].items())[:15]]
            lines.append("Learned User Preferences & Memories: " + " | ".join(mem_items))
        if obs.get("session_turns"):
            lines.append("Recent Conversation Turns:")
            for t in obs["session_turns"][-4:]:
                lines.append(f"  User: {t.get('utterance', '')} -> Ops: {t.get('ops', '')} (Out: {str(t.get('output', ''))[:100]})")
        return "\n".join(lines)

    def _post_chat_messages(
        self,
        messages: List[Dict[str, str]],
        max_tokens: int = 512,
    ) -> Optional[Union[Dict[str, Any], List[Dict[str, Any]]]]:
        """Send chat completion request to axil-auren with fallback routing and parse JSON DSL output."""
        candidate_models = [self.model]
        for fallback_m in ("kilo-auto/free", "fast", "gpt-oss-120b"):
            if fallback_m not in candidate_models:
                candidate_models.append(fallback_m)

        for cur_model in candidate_models:
            try:
                payload: Dict[str, Any] = {
                    "model": cur_model,
                    "messages": messages,
                    "temperature": 0.0,
                    "max_tokens": max_tokens,
                }
                # Only pass reasoning_effort for models that need explicit low effort (avoid triggering 640-token reasoning loops on Kilo free models)
                if cur_model not in ("kilo-auto/free",) and not cur_model.startswith("kilo/"):
                    payload["reasoning_effort"] = "low"

                req = urllib.request.Request(
                    self.endpoint,
                    data=json.dumps(payload).encode("utf-8"),
                    headers={"Content-Type": "application/json"},
                    method="POST",
                )
                with urllib.request.urlopen(req, timeout=16.0) as resp:
                    raw_body = resp.read().decode("utf-8").strip()
                if not raw_body:
                    continue
                data = json.loads(raw_body)
                msg = (data.get("choices") or [{}])[0].get("message") or {}
                content = (msg.get("content") or msg.get("reasoning_content") or "").strip()
                if not content:
                    continue
                parsed = _extract_json_payload(content)
                if isinstance(parsed, dict):
                    res = normalize_dsl_dict(parsed)
                    if res:
                        return res
                if isinstance(parsed, list):
                    norm_list = [normalize_dsl_dict(item) for item in parsed if isinstance(item, dict)]
                    valid_list = [d for d in norm_list if d is not None]
                    if valid_list:
                        return valid_list
            except Exception as e:
                print(f"[AurenLLMBackend:{cur_model}] Error: {e}")
                continue
        return None

    def chat_agent_turn(
        self,
        messages: List[Dict[str, str]],
        max_tokens: int = 512,
    ) -> Optional[Union[Dict[str, Any], List[Dict[str, Any]]]]:
        """Execute a single turn in the Jarvis ReAct agent loop and parse the resulting JSON tool/DSL step(s)."""
        return self._post_chat_messages(messages, max_tokens=max_tokens)

    def classify_intent(
        self,
        utterance: str,
        live_ontology: Dict[str, Any],
        dsl_schema: Dict[str, Any],
        few_shots: List[Dict[str, Any]],
    ) -> Optional[Union[Dict[str, Any], List[Dict[str, Any]]]]:
        prompt = self._build_prompt(utterance, live_ontology, dsl_schema, few_shots)
        return self._post_chat_messages(
            [
                {"role": "system", "content": "You output ONLY valid JSON DSL. No markdown, no explanation."},
                {"role": "user", "content": prompt},
            ],
            max_tokens=512,
        )


class LLMBackend(AurenLLMBackend):
    """Alias for backwards compatibility."""

    pass
