"""Jarvis Orchestrator: Coordinates Solver, Inference, Conversational Context, Multi-Clause Chaining, and WM Adapter."""

import json
import os
import re
import time
from typing import Any, Dict, List, Optional

from .solver import CompositionalSemanticSolver, build_ir
from .ontology import SemanticOntology, clean_phrase
from .ir import IntentIR
from jarvis_adapters.base import Adapter, AdapterResult
from jarvis_adapters.inference import InferenceBackend, LLMBackend, EmbeddingBackend


class Jarvis:
    """Main Jarvis orchestrator."""

    def __init__(
        self,
        adapter: Adapter,
        ontology: Optional[SemanticOntology] = None,
        inference_backends: Optional[List[InferenceBackend]] = None,
        few_shot_path: Optional[str] = None,
        context_path: Optional[str] = None,
    ):
        self.adapter = adapter
        self.ontology = ontology or SemanticOntology()
        self.solver = CompositionalSemanticSolver(self.ontology)
        self.inference_backends = (
            inference_backends if inference_backends is not None else [EmbeddingBackend(), LLMBackend()]
        )
        self.few_shot_path = few_shot_path or os.path.expanduser("~/.config/jarvis/few_shots.json")
        self.few_shots = self._load_few_shots()
        self.dsl_schema = self._load_dsl_schema()
        user = os.environ.get("USER", "user")
        self.context_path = context_path or f"/tmp/jarvis-{user}.context.json"

    def _load_dsl_schema(self) -> Dict[str, Any]:
        candidates = [
            os.path.expanduser("~/.local/lib/jarvis-dsl/dsl_schema.json"),
            os.path.join(os.path.dirname(__file__), "..", "..", "jarvis-dsl", "dsl_schema.json"),
        ]
        for schema_path in candidates:
            try:
                if os.path.exists(schema_path):
                    with open(schema_path, "r") as f:
                        return json.load(f)
            except Exception:
                pass
        return {"properties": {"op": {"enum": []}}}

    def _load_few_shots(self) -> List[Dict[str, Any]]:
        try:
            with open(self.few_shot_path, "r") as f:
                return json.load(f)
        except Exception:
            return []

    def _save_few_shots(self) -> None:
        os.makedirs(os.path.dirname(self.few_shot_path), exist_ok=True)
        # Keep deduplicated recent 150 examples
        dedup = {}
        for shot in self.few_shots:
            k = clean_phrase(shot.get("utterance", ""))
            if k:
                dedup[k] = shot
        unique_list = list(dedup.values())[-150:]
        self.few_shots = unique_list
        with open(self.few_shot_path, "w") as f:
            json.dump(self.few_shots, f)

    def _load_context(self) -> Dict[str, Any]:
        try:
            if os.path.exists(self.context_path):
                with open(self.context_path, "r") as f:
                    ctx = json.load(f)
                # Expire context older than 120 seconds
                if time.time() - ctx.get("ts", 0) <= 120.0:
                    return ctx
        except Exception:
            pass
        return {}

    def _save_context(self, ctx: Dict[str, Any]) -> None:
        try:
            ctx["ts"] = time.time()
            with open(self.context_path, "w") as f:
                json.dump(ctx, f)
        except Exception:
            pass

    def _update_context_from_step(
        self, dsl: Dict[str, Any], live_ontology: Dict[str, Any], ctx: Dict[str, Any]
    ) -> Dict[str, Any]:
        op = dsl.get("op", "")
        target = dsl.get("target", {})
        args = dsl.get("args", {})
        ctx["last_op"] = op

        foc = live_ontology.get("focused", {})
        if foc.get("id"):
            ctx["last_window_id"] = foc.get("id", "")
            ctx["last_window_class"] = foc.get("class", "")
        if target.get("identifier"):
            ctx["last_app"] = target["identifier"]
        if op == "open_terminal":
            ctx["last_app"] = "terminal"
        if op == "spawn_shell_command":
            cmd = target.get("command", "")
            ctx["last_command"] = cmd
            if cmd.startswith("cd "):
                ctx["last_location"] = cmd[3:].strip()
            elif cmd.startswith("ls -la "):
                loc = cmd[len("ls -la "):].strip()
                if loc:
                    ctx["last_location"] = loc
        if op in ("media_play", "search_web") and target.get("query"):
            ctx["last_query"] = target["query"]
        if op in ("workspace_switch", "workspace_move_window") and args.get("workspace"):
            ctx["last_workspace"] = args["workspace"]

        self._save_context(ctx)
        return ctx

    def resolve_anaphora(self, clause: str, ctx: Dict[str, Any]) -> str:
        """Resolve conversational pronouns ('it', 'that', 'there') and direct patterns."""
        c = clause.strip()
        c_low = c.lower()

        # Pattern: "move it/this/other window to workspace N"
        m_move = re.match(
            r"^(?:please\s+)?(?:move|send|put|tag)\s+(it|this|that|this\s+window|the\s+window|window|the\s+other\s+window|other\s+window)\s+(?:to|on|onto)\s+(?:workspace|tag|desktop)\s+([1-9])$",
            c_low,
        )
        if m_move:
            subj = m_move.group(1)
            ws_num = m_move.group(2)
            if "other" in subj:
                return f"__move_other_window_workspace__ {ws_num}"
            return f"__move_window_workspace__ {ws_num}"

        # Pattern: "close the other window" / "kill the other window"
        if c_low in ("close the other window", "close other window", "kill the other window", "kill other window"):
            return "__close_other_window__"

        # Pattern: "close it" / "kill it"
        if c_low in ("close it", "kill it", "close that", "kill that", "quit it"):
            return "close this window"

        # Pattern: "fullscreen it" / "maximize it" / "float it" / "tile it"
        if c_low in ("fullscreen it", "maximize it", "make it fullscreen"):
            return "toggle fullscreen"
        if c_low in ("float it", "make it float", "make it floating"):
            return "float window"
        if c_low in ("tile it", "make it tiled"):
            return "tile window"

        # Pattern: "pause it" / "stop it" / "resume it" / "play it"
        if c_low in ("pause it", "pause that"):
            return "pause the music"
        if c_low in ("play it", "play that", "resume it", "resume that"):
            last_q = ctx.get("last_query", "")
            if last_q and ctx.get("last_op") in ("media_play", "search_web"):
                return f"play {last_q}"
            return "play music"

        # Pattern: "there" -> last_location (e.g. "list files there", "find c files there")
        last_loc = ctx.get("last_location", "")
        if last_loc and re.search(r"\bthere\b", c_low):
            loc_word = os.path.basename(last_loc.rstrip("/")) or last_loc
            c = re.sub(r"\bthere\b", f"in {loc_word}", c, flags=re.IGNORECASE)

        return c

    def split_clauses(self, utterance: str) -> List[str]:
        """Split compound utterance into atomic clauses while respecting 'and press enter' and 'screen and audio'."""
        raw = (utterance or "").strip()
        if not raw:
            return []

        # Protect atomic phrases containing 'and'
        protected = raw
        placeholders = {
            " and press enter": " __AND_PRESS_ENTER__",
            " and hit enter": " __AND_HIT_ENTER__",
            " and hit return": " __AND_HIT_RETURN__",
            "screen and audio": "__SCREEN_AND_AUDIO__",
        }
        for k, v in placeholders.items():
            protected = re.sub(re.escape(k), v, protected, flags=re.IGNORECASE)

        clauses: List[str] = []
        for semi in protected.split(";"):
            semi = semi.strip()
            if not semi:
                continue
            # Split on ", and then ", ", then ", ", and ", " and then ", " then ", or comma followed by verb
            parts = re.split(
                r"\s*(?:,\s*and\s+then\s+|,\s*then\s+|,\s*and\s+|\band\s+then\s+|\bthen\s+|,\s+)\s*",
                semi,
                flags=re.IGNORECASE,
            )
            for p in parts:
                p = p.strip()
                if not p:
                    continue
                # Further split on " and " if both sides start with or contain a known verb head word
                subparts = re.split(r"\s+\band\b\s+", p, flags=re.IGNORECASE)
                if len(subparts) > 1:
                    merged_subs: List[str] = [subparts[0]]
                    for sp in subparts[1:]:
                        sp_words = clean_phrase(sp).split()
                        if sp_words and (
                            sp_words[0] in self.ontology.verb_head_words
                            or sp_words[0] in ("move", "send", "close", "kill", "pause", "play", "switch", "open", "list", "find", "type", "search", "start", "stop", "toggle")
                        ):
                            merged_subs.append(sp)
                        else:
                            merged_subs[-1] = merged_subs[-1] + " and " + sp
                    clauses.extend(merged_subs)
                else:
                    clauses.append(p)

        restored: List[str] = []
        inv_map = {v.strip(): k.strip() for k, v in placeholders.items()}
        for c in clauses:
            for v_tok, orig in inv_map.items():
                c = c.replace(v_tok, orig)
            c = c.strip().strip(",.!?;:")
            if c:
                restored.append(c)

        return restored if restored else [raw]

    def _get_live_ontology(self, ctx: Optional[Dict[str, Any]] = None) -> Dict[str, Any]:
        obs = self.adapter.observe()
        entities = {}
        for c in obs.get("clients", []):
            name = f"{c.get('class', '')} {c.get('title', '')}".strip()
            if name:
                entities.setdefault("ui.window", []).append({
                    "id": c.get("id", ""),
                    "names": [c.get("class", ""), c.get("title", "")],
                })
        for t in obs.get("tabs", []):
            entities.setdefault("ui.tab", []).append({
                "id": t.get("url", ""),
                "names": [t.get("title", ""), t.get("url", "")],
            })
        obs["entities"] = entities
        if ctx:
            obs["context"] = ctx
        return obs

    def _process_single_clause(
        self,
        clause: str,
        active_surface: str,
        ctx: Dict[str, Any],
    ) -> Dict[str, Any]:
        resolved_clause = self.resolve_anaphora(clause, ctx)
        live_ontology = self._get_live_ontology(ctx)

        # Direct synthetic macro for workspace_move_window / close other window
        if resolved_clause.startswith("__move_other_window_workspace__ "):
            ws = int(resolved_clause.split()[-1])
            dsl = {
                "op": "workspace_move_window",
                "target": {"kind": "relative", "relation": "other"},
                "args": {"workspace": ws},
                "verify": {},
            }
            result = self.adapter.execute(dsl)
            self._update_context_from_step(dsl, live_ontology, ctx)
            return {
                "clause": clause,
                "resolved_clause": resolved_clause,
                "ir": {"cap": "workspace.move_window", "score": 0.99, "target_other": True, "args": {"workspace": ws}},
                "dsl": dsl,
                "result": result.__dict__,
                "source": "solver:anaphora",
            }

        if resolved_clause == "__close_other_window__":
            dsl = {
                "op": "window_close",
                "target": {"kind": "relative", "relation": "other"},
                "verify": {},
            }
            result = self.adapter.execute(dsl)
            self._update_context_from_step(dsl, live_ontology, ctx)
            return {
                "clause": clause,
                "resolved_clause": resolved_clause,
                "ir": {"cap": "window.control", "score": 0.99, "target_other": True, "args": {"action": "close"}},
                "dsl": dsl,
                "result": result.__dict__,
                "source": "solver:anaphora",
            }

        if resolved_clause.startswith("__move_window_workspace__ "):
            ws = int(resolved_clause.split()[-1])
            target_id = ctx.get("last_window_id") or live_ontology.get("focused", {}).get("id", "")
            dsl = {
                "op": "workspace_move_window",
                "target": {"identifier": target_id} if target_id else {},
                "args": {"workspace": ws},
                "verify": {},
            }
            result = self.adapter.execute(dsl)
            self._update_context_from_step(dsl, live_ontology, ctx)
            return {
                "clause": clause,
                "resolved_clause": resolved_clause,
                "ir": {"cap": "workspace.move_window", "score": 0.99, "args": {"workspace": ws}},
                "dsl": dsl,
                "result": result.__dict__,
                "source": "solver:anaphora",
            }

        ir = build_ir(resolved_clause, active_surface, live_ontology, self.ontology)
        if ir and ir.get("score", 0) >= 0.85:
            dsl = self._compile_ir_to_dsl(ir, active_surface=active_surface)
            if dsl:
                result = self.adapter.execute(dsl)
                self._maybe_learn(resolved_clause, dsl, result.success)
                self._update_context_from_step(dsl, live_ontology, ctx)
                return {
                    "clause": clause,
                    "resolved_clause": resolved_clause,
                    "ir": ir,
                    "dsl": dsl,
                    "result": result.__dict__,
                    "source": "solver",
                }

        for backend in self.inference_backends:
            dsl = backend.classify_intent(resolved_clause, live_ontology, self.dsl_schema, self.few_shots)
            if dsl and self._validate_dsl(dsl):
                result = self.adapter.execute(dsl)
                self._maybe_learn(resolved_clause, dsl, result.success)
                self._update_context_from_step(dsl, live_ontology, ctx)
                return {
                    "clause": clause,
                    "resolved_clause": resolved_clause,
                    "ir": None,
                    "dsl": dsl,
                    "result": result.__dict__,
                    "source": f"inference:{backend.name}",
                }

        if ir:
            dsl = self._compile_ir_to_dsl(ir, active_surface=active_surface)
            if dsl:
                result = self.adapter.execute(dsl)
                self._update_context_from_step(dsl, live_ontology, ctx)
                return {
                    "clause": clause,
                    "resolved_clause": resolved_clause,
                    "ir": ir,
                    "dsl": dsl,
                    "result": result.__dict__,
                    "source": "solver:low_confidence",
                }

        return {"error": "Could not resolve utterance", "utterance": clause}

    def process(self, utterance: str, active_surface: str = "any") -> Dict[str, Any]:
        ctx = self._load_context()
        clauses = self.split_clauses(utterance)

        if len(clauses) <= 1:
            single_clause = clauses[0] if clauses else utterance
            return self._process_single_clause(single_clause, active_surface, ctx)

        # Compound multi-step execution
        steps = []
        outputs = []
        sources = []
        all_ok = True
        curr_surface = active_surface

        for idx, clause in enumerate(clauses):
            step_res = self._process_single_clause(clause, curr_surface, ctx)
            steps.append(step_res)
            if "error" in step_res:
                all_ok = False
                outputs.append(f"failed({clause})")
            else:
                res_dict = step_res.get("result", {})
                if not res_dict.get("success", False):
                    all_ok = False
                outputs.append(res_dict.get("output") or res_dict.get("error") or step_res["dsl"]["op"])
                sources.append(step_res.get("source", "unknown"))
                # If a step opened a terminal or ran a shell command, subsequent clauses default to terminal surface
                op = step_res.get("dsl", {}).get("op", "")
                if op in ("open_terminal", "spawn_shell_command"):
                    curr_surface = "terminal"
            if idx + 1 < len(clauses):
                time.sleep(0.08)

        if not sources:
            return {"error": "Could not resolve compound utterance", "utterance": utterance, "steps": steps}

        unique_sources = list(dict.fromkeys(sources))
        combined_source = "+".join(unique_sources) if len(unique_sources) > 1 else unique_sources[0]

        return {
            "ir": steps[-1].get("ir"),
            "dsl": steps[-1].get("dsl"),
            "steps": steps,
            "result": {
                "success": all_ok,
                "output": " -> ".join(outputs),
                "error": "" if all_ok else "one or more steps failed",
                "metadata": {"step_count": len(steps)},
            },
            "source": combined_source,
        }

    def _compile_ir_to_dsl(self, ir: Dict[str, Any], active_surface: str = "any") -> Optional[Dict[str, Any]]:
        cap = ir["cap"]
        args = ir.get("args", {})
        surface = ir.get("surface", active_surface)
        explicit_term = ir.get("explicit_terminal", False)
        target_other = ir.get("target_other", False)
        submit_enter = ir.get("submit_enter", False)

        def _with_other(dsl: Dict[str, Any]) -> Dict[str, Any]:
            if target_other:
                dsl.setdefault("target", {})["relation"] = "other"
            return dsl

        if cap == "fs.list":
            loc = args.get("location", ".")
            if loc in ("", "."):
                cmd = "ls -la"
            elif loc == "~":
                cmd = "ls -la ~"
            else:
                cmd = f"ls -la {loc}"
            return _with_other({"op": "spawn_shell_command", "target": {"command": cmd}, "verify": {}})

        if cap == "fs.navigate":
            loc = args.get("location", "~")
            return _with_other({"op": "spawn_shell_command", "target": {"command": f"cd {loc}"}, "verify": {}})

        if cap == "fs.find":
            pat = args.get("pattern", "*")
            loc = args.get("location", ".") or "."
            return _with_other({"op": "spawn_shell_command", "target": {"command": f"find {loc} -name '{pat}'"}, "verify": {}})

        if cap == "fs.metric":
            metric = args.get("metric", "cwd")
            cmd_map = {
                "cwd": "pwd",
                "disk_space": "df -h",
                "largest": "du -sh * 2>/dev/null | sort -h",
                "recent": "ls -lat | head -n 20",
            }
            return _with_other({"op": "spawn_shell_command", "target": {"command": cmd_map.get(metric, "pwd")}, "verify": {}})

        if cap == "git.inspect":
            aspect = args.get("aspect", "status")
            cmd_map = {
                "status": "git status",
                "diff": "git diff",
                "log": "git log -n 10 --oneline",
                "branch": "git branch -a",
            }
            return _with_other({"op": "spawn_shell_command", "target": {"command": cmd_map.get(aspect, "git status")}, "verify": {}})

        if cap == "shell.run":
            prog = args.get("program", "")
            if prog == "tmux_attach":
                cmd = "tmux a || tmux"
            else:
                cmd = prog
            return _with_other({"op": "spawn_shell_command", "target": {"command": cmd}, "verify": {}})

        if cap == "app.launch":
            app = args.get("application", "")
            if app == "terminal":
                return {"op": "open_terminal", "target": {}, "verify": {}}
            return {"op": "open_application", "target": {"identifier": app}, "verify": {}}

        if cap == "window.new":
            kind = args.get("kind", "terminal")
            if kind == "terminal":
                return {"op": "open_terminal", "target": {}, "verify": {}}
            if kind == "tab":
                return _with_other({"op": "press_key", "args": {"key_combo": "ctrl+t"}, "verify": {}})
            if kind == "window":
                if active_surface == "browser":
                    return _with_other({"op": "press_key", "args": {"key_combo": "ctrl+n"}, "verify": {}})
                return {"op": "open_terminal", "target": {}, "verify": {}}
            if kind == "devtools":
                return _with_other({"op": "press_key", "args": {"key_combo": "ctrl+shift+i"}, "verify": {}})
            if kind == "address_bar":
                return _with_other({"op": "press_key", "args": {"key_combo": "ctrl+l"}, "verify": {}})
            cmd_map = {
                "sys_monitor": "dwm-ctl sys-monitor",
                "calendar": "dwm-cal popup",
                "control_center": "dwm-ctl",
                "wifi": "dwm-net wifi",
                "battery": "dwm-ctl battery-info",
                "power_menu": "dwm-ctl power-menu",
                "shortcuts_help": "dwm-help show",
            }
            if kind in cmd_map:
                return {"op": "open_application", "target": {"identifier": cmd_map[kind]}, "verify": {}}
            return None

        if cap == "window.focus_relative":
            relation = args.get("relation", "other")
            return {"op": "focus_relative_window", "target": {"kind": "relative", "relation": relation}, "verify": {}}

        if cap == "workspace.switch":
            ws = args.get("workspace", 1)
            return {"op": "workspace_switch", "target": {}, "args": {"workspace": ws}, "verify": {}}

        if cap == "window.control":
            action = args.get("action", "unknown")
            op_map = {
                "fullscreen": "window_fullscreen",
                "float": "window_float",
                "tile": "window_float",  # toggle floating
                "close": "window_close",
            }
            dsl: Dict[str, Any] = {"op": op_map.get(action, "window_fullscreen"), "target": {}, "verify": {}}
            if action == "close":
                dsl["target"]["kind"] = "window"
            return _with_other(dsl)

        if cap == "edit.clear":
            unit = args.get("unit", "line")
            if active_surface == "browser":
                key_map = {"line": "ctrl+a BackSpace", "word": "ctrl+BackSpace"}
            else:
                key_map = {"line": "ctrl+u", "word": "ctrl+w", "screen": "ctrl+l", "process": "ctrl+c"}
            return _with_other({"op": "press_key", "args": {"key_combo": key_map.get(unit, "ctrl+u")}, "verify": {}})

        if cap == "input.dictate":
            text = args.get("text", "")
            if args.get("submit_enter", False) or submit_enter:
                return _with_other({"op": "type_and_enter", "target": {}, "args": {"text": text}, "verify": {}})
            return _with_other({"op": "type_text", "target": {}, "args": {"text": text}, "verify": {}})

        if cap == "media.play":
            query = args.get("query", "")
            return {"op": "media_play", "target": {"query": query}, "verify": {"audio_playing": True}}

        if cap == "media.control":
            action = args.get("action", "pause")
            op_map = {
                "pause": "media_pause",
                "next_track": "media_next",
                "prev_track": "media_prev",
                "volume_up": "media_volume",
                "volume_down": "media_volume",
                "volume_mute": "media_volume",
            }
            dsl = {"op": op_map.get(action, "media_pause"), "verify": {}}
            if action in ("volume_up", "volume_down", "volume_mute"):
                dsl["args"] = {"action": action}
            return dsl

        if cap == "record.control":
            action = args.get("action", "start")
            target = args.get("target", "both")
            if action == "start":
                return {"op": "record_start", "args": {"mode": target}, "verify": {}}
            return {"op": "record_stop", "verify": {}}

        if cap == "web.search":
            query = args.get("query", "")
            return {"op": "search_web", "target": {"query": query}, "verify": {}}

        return None

    def _validate_dsl(self, dsl: Dict[str, Any]) -> bool:
        if not isinstance(dsl, dict) or not dsl.get("op"):
            return False
        dsl.setdefault("target", {})
        dsl.setdefault("verify", {})
        return dsl["op"] in self.adapter.supported_ops

    def _maybe_learn(self, utterance: str, dsl: Dict[str, Any], success: bool) -> None:
        if not success:
            return
        words = clean_phrase(utterance).split()
        # Do not auto-learn 1-word fragments or runaway repetitive ASR hallucinations
        if len(words) < 2 or len(words) > 12:
            return
        if len(set(words)) * 2 < len(words):
            return
        clean_dsl = {k: v for k, v in dsl.items() if k != "meta"}
        self.few_shots.append({"utterance": utterance.strip(), "dsl": clean_dsl})
        self._save_few_shots()


def create_jarvis(
    wm: str = "dwm",
    ontology_path: Optional[str] = None,
    inference: str = "auto",
) -> Jarvis:
    if wm == "dwm":
        from jarvis_adapters.dwm import DwmAdapter
        adapter = DwmAdapter()
    elif wm == "hyprland":
        from jarvis_adapters.hyprland import HyprlandAdapter
        adapter = HyprlandAdapter()
    elif wm == "sway":
        from jarvis_adapters.sway import SwayAdapter
        adapter = SwayAdapter()
    else:
        raise ValueError(f"Unknown WM: {wm}")

    if inference == "none":
        backends: List[InferenceBackend] = []
    elif inference == "embedding":
        backends = [EmbeddingBackend()]
    elif inference == "llm":
        backends = [LLMBackend()]
    else:
        backends = [EmbeddingBackend(), LLMBackend()]

    return Jarvis(adapter=adapter, ontology=SemanticOntology(ontology_path), inference_backends=backends)
