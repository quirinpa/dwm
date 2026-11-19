"""JarvisAgent: Multi-turn ReAct Observe-Reason-Act loop, Session Journal, and Persistent User Memory."""

import json
import os
import re
import time
from typing import Any, Dict, List, Optional, Tuple, Union

from jarvis_adapters.base import Adapter, AdapterResult
from jarvis_adapters.inference import AurenLLMBackend, _clean_phrase

OBSERVATION_OPS = {
    "inspect_terminal",
    "inspect_clipboard",
    "inspect_system",
    "run_background_command",
    "recall_facts",
}

DESTRUCTIVE_PATTERNS = (
    r"\brm\s+-[a-zA-Z]*r[a-zA-Z]*f\b",
    r"\brm\s+-[a-zA-Z]*f[a-zA-Z]*r\b",
    r"\bgit\s+reset\s+--hard\b",
    r"\bgit\s+clean\s+-[a-zA-Z]*f\b",
    r"\bmkfs\b",
    r"\bdd\s+if=",
    r"\bkill\s+-9\b",
    r"\bpkill\s+-9\b",
    r"\bkillall\s+-9\b",
    r"\bshutdown\b",
    r"\breboot\b",
    r"\bpoweroff\b",
)


def is_destructive_command(cmd: str) -> bool:
    """Return True if a shell command matches known destructive patterns."""
    if not cmd:
        return False
    for pat in DESTRUCTIVE_PATTERNS:
        if re.search(pat, cmd, flags=re.IGNORECASE):
            return True
    return False


class UserMemoryStore:
    """Persistent long-term memory for user preferences, project paths, and custom workflows."""

    def __init__(self, path: Optional[str] = None) -> None:
        self.path = path or os.path.expanduser("~/.config/jarvis/memories.json")

    def load_all(self) -> Dict[str, str]:
        try:
            if os.path.exists(self.path):
                with open(self.path, "r", encoding="utf-8") as f:
                    data = json.load(f)
                if isinstance(data, dict):
                    return {str(k): str(v) for k, v in data.items()}
        except Exception:
            pass
        return {}

    def remember(self, key: str, value: str) -> Dict[str, str]:
        memories = self.load_all()
        k = (key or "note").strip()
        v = (value or "").strip()
        if k and v:
            memories[k] = v
            os.makedirs(os.path.dirname(self.path), exist_ok=True)
            with open(self.path, "w", encoding="utf-8") as f:
                json.dump(memories, f, indent=2, ensure_ascii=False)
        return memories

    def recall(self, query: str = "") -> Dict[str, str]:
        memories = self.load_all()
        q = (query or "").strip().lower()
        if not q:
            return memories
        q_tokens = set(_clean_phrase(q).split())
        matched = {}
        for k, v in memories.items():
            hay = f"{k} {v}".lower()
            if q in hay or (q_tokens and any(t in hay for t in q_tokens)):
                matched[k] = v
        return matched or memories


class SessionJournal:
    """Rolling multi-turn dialogue and action journal for conversational continuity."""

    def __init__(self, path: Optional[str] = None, ttl_seconds: float = 1800.0) -> None:
        user = os.environ.get("USER", "user")
        self.path = path or f"/tmp/jarvis-{user}.session.json"
        self.ttl_seconds = ttl_seconds

    def load_turns(self) -> List[Dict[str, Any]]:
        try:
            if os.path.exists(self.path):
                with open(self.path, "r", encoding="utf-8") as f:
                    data = json.load(f)
                now = time.time()
                if isinstance(data, list):
                    return [t for t in data if isinstance(t, dict) and now - t.get("ts", 0) <= self.ttl_seconds][-8:]
        except Exception:
            pass
        return []

    def append_turn(self, utterance: str, steps: List[Dict[str, Any]], output: str) -> None:
        turns = self.load_turns()
        ops = [s.get("op", "?") for s in steps if isinstance(s, dict)]
        turns.append({
            "ts": time.time(),
            "utterance": utterance,
            "ops": ",".join(ops),
            "steps": steps,
            "output": output[:400],
        })
        try:
            with open(self.path, "w", encoding="utf-8") as f:
                json.dump(turns[-8:], f, ensure_ascii=False)
        except Exception:
            pass


class JarvisAgent:
    """Multi-turn ReAct desktop agent powered by axil-auren."""

    def __init__(
        self,
        adapter: Adapter,
        llm_backend: AurenLLMBackend,
        memory_store: Optional[UserMemoryStore] = None,
        session_journal: Optional[SessionJournal] = None,
    ) -> None:
        self.adapter = adapter
        self.llm = llm_backend
        self.memory_store = memory_store or UserMemoryStore()
        self.session_journal = session_journal or SessionJournal()
        user = os.environ.get("USER", "user")
        self.status_file = f"/tmp/dwm-acting-{user}.status"

    def _set_status(self, status_text: str) -> None:
        try:
            if status_text:
                with open(self.status_file, "w", encoding="utf-8") as f:
                    f.write(status_text[:48] + "\n")
            elif os.path.exists(self.status_file):
                os.remove(self.status_file)
        except Exception:
            pass

    def _build_agent_system_prompt(self) -> str:
        return (
            "You are Jarvis, Tony Stark's intelligent Linux desktop assistant and systems engineer. "
            "You control a live Linux desktop (dwm, terminals, tmux, Firefox, audio, shell, and coding tools). "
            "You operate in a ReAct Observe-Reason-Act loop: you can either call an OBSERVATION tool first to inspect "
            "live terminal panes, clipboard, system health, or command output, OR immediately emit one or more ACTION DSL objects (or a JSON array of DSL objects) to fulfill the user's intent. "
            "When using `answer_question`, keep your spoken reply crisp, natural, and concise (1 to 2 short sentences, under 180 characters, like Iron Man's Jarvis) — begin with a short natural clause so voice synthesis starts speaking immediately, and never recite long lists of capabilities unless asked. "
            "Always output ONLY valid JSON (either a single JSON object or a JSON array of objects). No markdown prose outside JSON."
        )

    def _build_initial_user_prompt(
        self,
        utterance: str,
        live_ontology: Dict[str, Any],
        few_shots: List[Dict[str, Any]],
    ) -> str:
        tools_doc = """=== OBSERVATION & MEMORY TOOLS (Use when you need to inspect state before acting or answering) ===
- inspect_terminal: {"op": "inspect_terminal", "target": {"relation": "focused|other"}}  (Reads live tmux pane output, terminal scrollback, running command, and cwd)
- inspect_clipboard: {"op": "inspect_clipboard", "target": {}}  (Reads X11 highlighted primary selection and clipboard text)
- inspect_system: {"op": "inspect_system", "args": {"aspect": "all|memory|cpu|disk|ports"}}  (Reads free RAM, top CPU/RAM processes, disk space, and listening ports)
- run_background_command: {"op": "run_background_command", "target": {"command": "<read_only_or_diagnostic_shell_cmd>"}}  (Runs a command silently in the background and returns stdout/stderr to you so you can inspect files, logs, git, processes, etc.)
- remember_fact: {"op": "remember_fact", "args": {"key": "<short_topic_or_trigger>", "value": "<preference_or_workflow_to_remember>"}}  (Persists a user preference, path, or custom workflow to permanent memory)
- recall_facts: {"op": "recall_facts", "target": {"query": "<topic>"}}  (Searches permanent user memory)

=== DESKTOP & SHELL ACTION TOOLS ===
- open_terminal: {"op": "open_terminal", "target": {}}
- open_application: {"op": "open_application", "target": {"identifier": "<app_binary>"}}  (GUI desktop apps: firefox, gnome-calculator, code, discord, pavucontrol, dwm-cal popup, dwm-ctl sys-monitor, etc.)
- spawn_shell_command: {"op": "spawn_shell_command", "target": {"command": "<shell_cmd>"}}  (Types and executes ANY shell command in an interactive terminal window; add "relation": "other" in target if user says "in the other window". Examples: "nvim ~/.vimrc", "git log -n 5", "ps aux | grep python", "df -h", "make -j", "bitwise")
- focus_window: {"op": "focus_window", "target": {"identifier": "<window_class_or_title>"}}
- focus_relative_window: {"op": "focus_relative_window", "target": {"kind": "relative", "relation": "other|next|previous|master"}}
- open_url: {"op": "open_url", "target": {"url": "https://..."}}
- search_web: {"op": "search_web", "target": {"query": "<web_search_query>"}}  (ONLY for internet/Google browser searches)
- type_text: {"op": "type_text", "target": {}, "args": {"text": "<text_to_type>"}}
- type_and_enter: {"op": "type_and_enter", "target": {}, "args": {"text": "<text_to_type>"}}
- press_key: {"op": "press_key", "target": {}, "args": {"key_combo": "<xdotool_key_combo>"}}  (e.g. "ctrl+c", "ctrl+t", "ctrl+w", "ctrl+u", "Return")
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
- answer_question: {"op": "answer_question", "target": {}, "args": {"answer": "<concise Jarvis reply>"}}  (Use to answer questions, report findings after inspecting terminal/system, or confirm preferences)
- wm_help: {"op": "wm_help", "target": {"query": "<dwm_or_shortcut_question>"}}
- ask_agent: {"op": "ask_agent", "target": {"query": "<coding_agent_task>"}}  (Delegates deep code edits/refactoring to the background Pi coding agent)"""

        obs_enriched = dict(live_ontology)
        obs_enriched["memories"] = self.memory_store.load_all()
        obs_enriched["session_turns"] = self.session_journal.load_turns()
        state_summary = self.llm._summarize_ontology(obs_enriched)

        return f"""{tools_doc}

DECISION GUIDELINES:
1. If the user asks you to diagnose an error in their terminal, check why a build/test failed, or read what's on their terminal screen: if `Live Terminal / Tmux Panes` below is empty or you need deeper scrollback/another window, call `inspect_terminal` first; once you see the error output, answer with `answer_question` or run the fix.
2. If the user asks about clipboard / highlighted text ("what is this I selected?", "open the URL in my clipboard"), call `inspect_clipboard` first.
3. If the user asks a quick question about free RAM, available disk space, or CPU load ("how much memory/disk space is left?"), answer IMMEDIATELY in 1 turn with `answer_question` using the `Live System Health (RAM / Disk / CPU)` line below! Only call `inspect_system` if you need top CPU/RAM process tables or listening network ports (`ss -tulpn`).
4. If the user asks you to remember a preference or workflow ("remember that...", "whenever I say..."), include `remember_fact` (and execute any requested actions). If they invoke a remembered workflow later, expand it into a JSON array `[step1, step2, ...]` of all the actions in that workflow!
5. If the user refers to a previous turn or something visible on screen ("open that file", "edit the first one", "actually move it to workspace 5 instead", "do that again", "what did you just do?"), inspect the `Live Terminal / Tmux Panes`, `Recent Conversation Turns`, and `Recent action context` below (or call `inspect_terminal` if needed) to resolve the exact filename, path, host, or window they mean!
6. For any local or remote shell command, SSH command (e.g. `ssh tty.pt`, or `ssh tty.pt 'df -h'`), CLI tool (nvim, git, grep, find, make, htop, bitwise, ping, tar, etc.), or file edit in a terminal, use `spawn_shell_command`. If the focused terminal is ALREADY inside an active `ssh` session (`Focused terminal active foreground command: ssh ...`), simply run the command directly in that terminal (`{{"op": "spawn_shell_command", "target": {{"command": "df -h"}}}}`).

CURRENT DESKTOP STATE, MEMORIES & CONVERSATION HISTORY:
{state_summary}

USER REQUEST: {utterance}

OUTPUT ONLY VALID JSON (a single DSL object or a JSON array of DSL objects):"""

    def _execute_step(self, dsl: Dict[str, Any]) -> AdapterResult:
        op = dsl.get("op", "")
        if op == "remember_fact":
            args = dsl.get("args", {})
            k = args.get("key", "note")
            v = args.get("value", "")
            self.memory_store.remember(k, v)
            return AdapterResult(True, output=f"Remembered '{k}': {v}")
        if op == "recall_facts":
            q = dsl.get("target", {}).get("query", "")
            facts = self.memory_store.recall(q)
            return AdapterResult(True, output=json.dumps(facts, ensure_ascii=False), metadata=facts)

        if op in ("spawn_shell_command", "run_background_command"):
            cmd = dsl.get("target", {}).get("command", "")
            if is_destructive_command(cmd):
                self._set_status(f"Confirm: {cmd[:28]}")
                if not self.adapter.confirm_action(cmd):
                    return AdapterResult(False, error=f"Cancelled destructive command: {cmd}")

        self._set_status(f"Jarvis: {op}")
        res = self.adapter.execute(dsl)

        # If the LLM attached an inline conversational reply alongside an action, notify the user
        inline_reply = (dsl.get("args") or {}).get("reply")
        if inline_reply and op != "answer_question":
            self.adapter.execute({
                "op": "answer_question",
                "target": {},
                "args": {"answer": str(inline_reply), "speak": bool((dsl.get("args") or {}).get("speak", False))},
            })
        return res

    def run(
        self,
        utterance: str,
        live_ontology: Dict[str, Any],
        few_shots: List[Dict[str, Any]],
        max_steps: int = 4,
    ) -> Optional[Dict[str, Any]]:
        """Run the multi-turn ReAct agent loop for an utterance."""
        messages: List[Dict[str, str]] = [
            {"role": "system", "content": self._build_agent_system_prompt()},
            {"role": "user", "content": self._build_initial_user_prompt(utterance, live_ontology, few_shots)},
        ]

        executed_dsls: List[Dict[str, Any]] = []
        executed_results: List[AdapterResult] = []
        trace_steps: List[Dict[str, Any]] = []

        try:
            for step_idx in range(max_steps):
                self._set_status(f"Jarvis thinking ({step_idx + 1})...")
                turn_out = self.llm.chat_agent_turn(messages)
                if not turn_out:
                    break

                step_list = turn_out if isinstance(turn_out, list) else [turn_out]
                valid_steps = [
                    d for d in step_list
                    if isinstance(d, dict) and (d.get("op") in self.adapter.supported_ops or d.get("op") in ("remember_fact", "recall_facts"))
                ]
                if not valid_steps:
                    break

                messages.append({"role": "assistant", "content": json.dumps(valid_steps, ensure_ascii=False)})

                has_observation = any(d.get("op") in OBSERVATION_OPS for d in valid_steps)
                has_final_action = any(d.get("op") not in OBSERVATION_OPS for d in valid_steps)

                obs_feedback_lines: List[str] = []
                for dsl in valid_steps:
                    dsl.setdefault("target", {})
                    dsl.setdefault("verify", {})
                    res = self._execute_step(dsl)
                    executed_dsls.append(dsl)
                    executed_results.append(res)
                    trace_steps.append({
                        "dsl": dsl,
                        "result": res.__dict__,
                        "source": f"agent:{self.llm.name}:step{step_idx + 1}",
                    })
                    if dsl.get("op") in OBSERVATION_OPS:
                        obs_feedback_lines.append(f"OBSERVATION ({dsl['op']}): {res.output or res.error}")

                # If this turn performed action(s) and didn't solely request an intermediate observation, we are done
                if has_final_action or not has_observation:
                    break

                # If we are at the last allowed step, synthesize a final answer_question from the observation
                if step_idx + 1 >= max_steps:
                    break

                # Otherwise feed the observation back into the conversation for the next ReAct step
                messages.append({
                    "role": "user",
                    "content": (
                        "\n".join(obs_feedback_lines)
                        + "\n\nBased on the observation above, output the next action DSL (or `answer_question` with a concise answer for the user). Output ONLY valid JSON:"
                    ),
                })
        finally:
            self._set_status("")

        if not executed_dsls:
            return None

        all_ok = all(r.success for r in executed_results)
        visible_pairs = [
            (d, r) for d, r in zip(executed_dsls, executed_results) if d.get("op") not in OBSERVATION_OPS
        ]
        if not visible_pairs:
            visible_pairs = list(zip(executed_dsls, executed_results))
        outputs = [r.output or r.error or d["op"] for d, r in visible_pairs]
        combined_output = " -> ".join(outputs)

        # Record turn in session journal
        self.session_journal.append_turn(utterance, executed_dsls, combined_output)

        return {
            "ir": None,
            "dsl": executed_dsls[-1],
            "steps": trace_steps,
            "result": {
                "success": all_ok,
                "output": combined_output,
                "error": "" if all_ok else (executed_results[-1].error or "one or more agent steps failed"),
                "metadata": {"agent_steps": len(trace_steps)},
            },
            "source": f"agent:{self.llm.name}",
        }
