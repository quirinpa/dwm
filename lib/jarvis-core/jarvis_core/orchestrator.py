"""Jarvis Orchestrator: Coordinates Solver, Inference, Conversational Context, Multi-Clause Chaining, and WM Adapter."""

import json
import os
import re
import time
from typing import Any, Dict, List, Optional

from .solver import CompositionalSemanticSolver, build_ir
from .ontology import SemanticOntology, clean_phrase
from .ir import IntentIR
from .agent import JarvisAgent, SessionJournal, UserMemoryStore
from jarvis_adapters.base import Adapter, AdapterResult
from jarvis_adapters.inference import AurenLLMBackend, InferenceBackend, LLMBackend, EmbeddingBackend

_AGENT_REASONING_PATTERNS = (
    r"\b(?:what|whats|why|how|who|where|which|explain|diagnose)\b",
    r"\b(?:hello|hi|hey|greetings|good\s+morning|good\s+afternoon|good\s+evening|thanks|thank\s+you)\b",
    r"\b(?:are\s+you\s+there|can\s+you\s+hear|talk\s+to\s+me|talk\s+again|speak\s+to\s+me|you\s+awake|you\s+listening)\b",
    r"\b(?:error|errors|failing|failed|broken|crash|crashing|bug|traceback|exception)\b",
    r"\b(?:eating|hogging|lagging|slow|ram|memory|cpu|port|ports|process|processes)\b",
    r"\b(?:clipboard|selected|highlighted|selection)\b",
    r"\b(?:remember|whenever|preference|preferences|workflow|habit)\b",
    r"\b(?:actually|instead|undo|repeat\s+that|do\s+that\s+again|what\s+did\s+you)\b",
    r"\b(?:my\s+eyes|killing\s+me|i\s+am\s+bored|im\s+bored|set\s+up\s+my|setup\s+my)\b",
)


class Jarvis:
    """Main Jarvis orchestrator."""

    def __init__(
        self,
        adapter: Adapter,
        ontology: Optional[SemanticOntology] = None,
        inference_backends: Optional[List[InferenceBackend]] = None,
        few_shot_path: Optional[str] = None,
        context_path: Optional[str] = None,
        memory_path: Optional[str] = None,
        session_path: Optional[str] = None,
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
        self.memory_store = UserMemoryStore(memory_path)
        self.session_journal = SessionJournal(session_path)
        self.llm_backend: Optional[AurenLLMBackend] = next(
            (b for b in self.inference_backends if isinstance(b, AurenLLMBackend)),
            None,
        )
        self.agent: Optional[JarvisAgent] = (
            JarvisAgent(
                adapter=self.adapter,
                llm_backend=self.llm_backend,
                memory_store=self.memory_store,
                session_journal=self.session_journal,
            )
            if self.llm_backend is not None
            else None
        )

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
                loc = cmd[len("ls -la ") :].strip()
                if loc:
                    ctx["last_location"] = loc
        if op in ("media_play", "search_web") and target.get("query"):
            ctx["last_query"] = target["query"]
        if op in ("workspace_switch", "workspace_move_window") and args.get("workspace"):
            ctx["last_workspace"] = args["workspace"]

        self._save_context(ctx)
        return ctx

    def resolve_anaphora(self, clause: str, ctx: Dict[str, Any]) -> str:
        """Resolve conversational pronouns ('it', 'that', 'there') using recent context."""
        c = clause.strip()
        c_low = c.lower()

        # Deictic pronoun resolution for media ("play it", "resume it", "pause it")
        if c_low in ("pause it", "pause that"):
            return "pause the music"
        if c_low in ("play it", "play that", "resume it", "resume that"):
            last_q = ctx.get("last_query", "")
            if last_q and ctx.get("last_op") in ("media_play", "search_web"):
                return f"play {last_q}"
            return "play music"

        # Deictic spatial pronoun: "there" -> last_location (e.g. "list files there", "find c files there")
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

        # If the entire utterance starts with an explicit dictation or agent-ask verb, keep it as a single clause
        # unless it has an explicit "and then" separator.
        raw_clean_words = clean_phrase(raw).split()
        while raw_clean_words and raw_clean_words[0] in ("please", "jarvis", "hey", "ok", "okay"):
            raw_clean_words.pop(0)
        if raw_clean_words and raw_clean_words[0] in ("type", "dictate", "write", "input", "ask", "tell"):
            if not re.search(r"\b(?:and\s+then|then)\b", raw, flags=re.IGNORECASE):
                return [raw]

        # Protect atomic phrases containing 'and'
        protected = raw
        placeholders = {
            " and press enter": " __AND_PRESS_ENTER__",
            " and hit enter": " __AND_HIT_ENTER__",
            " and hit return": " __AND_HIT_RETURN__",
            " and press return": " __AND_PRESS_RETURN__",
            "screen and audio": "__SCREEN_AND_AUDIO__",
        }
        for k, v in placeholders.items():
            protected = re.sub(re.escape(k), v, protected, flags=re.IGNORECASE)

        clauses: List[str] = []
        for semi in protected.split(";"):
            semi = semi.strip()
            if not semi:
                continue
            # Split on ", and then ", ", then ", ", and ", " and then ", " then ", " after that ", or comma
            parts = re.split(
                r"\s*(?:,\s*and\s+then\s+|,\s*then\s+|,\s*and\s+|\band\s+then\s+|\bafter\s+that\s+|\bfollowed\s+by\s+|\bthen\s+|,\s+)\s*",
                semi,
                flags=re.IGNORECASE,
            )
            for p in parts:
                p = p.strip()
                if not p:
                    continue
                # Further split on " and " if the right-hand side begins with a known verb head word
                subparts = re.split(r"\s+\band\b\s+", p, flags=re.IGNORECASE)
                if len(subparts) > 1:
                    merged_subs: List[str] = [subparts[0]]
                    for sp in subparts[1:]:
                        sp_words = clean_phrase(sp).split()
                        # Skip polite prefixes
                        while sp_words and sp_words[0] in ("please", "also", "then"):
                            sp_words.pop(0)
                        if sp_words and sp_words[0] in self.ontology.verb_head_words:
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
                names = [n for n in (c.get("class", ""), c.get("title", "")) if n]
                entities.setdefault("ui.window", []).append({
                    "id": c.get("id", ""),
                    "names": names,
                })
        for t in obs.get("tabs", []):
            names = [n for n in (t.get("title", ""), t.get("url", "")) if n]
            entities.setdefault("ui.tab", []).append({
                "id": t.get("url", ""),
                "names": names,
            })
        obs["entities"] = entities
        if ctx:
            obs["context"] = ctx
        obs["memories"] = self.memory_store.load_all()
        obs["session_turns"] = self.session_journal.load_turns()
        return obs

    def _requires_agent_reasoning(self, utterance: str) -> bool:
        """Return True if the utterance is conversational, diagnostic, memory-related, or situational."""
        if self.agent is None:
            return False
        raw = (utterance or "").strip()
        if not raw:
            return False
        if raw.endswith("?"):
            return True
        u_clean = clean_phrase(raw)
        for pat in _AGENT_REASONING_PATTERNS:
            if re.search(pat, u_clean, flags=re.IGNORECASE):
                return True
        # Also route to agent if utterance references a user-defined workflow/memory key
        for mem_key in self.memory_store.load_all().keys():
            mk_clean = clean_phrase(mem_key)
            if mk_clean and len(mk_clean) >= 3 and mk_clean in u_clean:
                return True
        return False

    def _process_single_clause(
        self,
        clause: str,
        active_surface: str,
        ctx: Dict[str, Any],
    ) -> Dict[str, Any]:
        t_start = time.time()
        resolved_clause = self.resolve_anaphora(clause, ctx)
        t_obs0 = time.time()
        live_ontology = self._get_live_ontology(ctx)
        observe_ms = round((time.time() - t_obs0) * 1000, 1)
        force_agent = self._requires_agent_reasoning(resolved_clause)

        # Tier 0 Fast Reflexes (skipped when utterance requires conversational/diagnostic agent reasoning)
        t_sol0 = time.time()
        ir = None if force_agent else build_ir(resolved_clause, active_surface, live_ontology, self.ontology)
        solver_ms = round((time.time() - t_sol0) * 1000, 1)
        tier0_threshold = 0.90 if self.agent is not None else 0.85
        if ir and ir.get("score", 0) >= tier0_threshold:
            dsl = self._compile_ir_to_dsl(ir, active_surface=active_surface, ctx=ctx, live_ontology=live_ontology)
            if dsl:
                t_ex0 = time.time()
                result = self.adapter.execute(dsl)
                exec_ms = round((time.time() - t_ex0) * 1000, 1)
                self._maybe_learn(resolved_clause, dsl, result.success)
                self._update_context_from_step(dsl, live_ontology, ctx)
                self.session_journal.append_turn(clause, [dsl], result.output or result.error)
                return {
                    "clause": clause,
                    "resolved_clause": resolved_clause,
                    "ir": ir,
                    "dsl": dsl,
                    "result": result.__dict__,
                    "source": "solver",
                    "timing_ms": {
                        "observe_ms": observe_ms,
                        "solver_ms": solver_ms,
                        "execute_ms": exec_ms,
                        "total_ms": round((time.time() - t_start) * 1000, 1),
                    },
                }

        # Tier 1a: Embedding few-shot cache (only for non-conversational/non-agentic commands)
        if not force_agent:
            for backend in self.inference_backends:
                if isinstance(backend, AurenLLMBackend):
                    continue
                t_emb0 = time.time()
                dsl_or_list = backend.classify_intent(resolved_clause, live_ontology, self.dsl_schema, self.few_shots)
                emb_ms = round((time.time() - t_emb0) * 1000, 1)
                if isinstance(dsl_or_list, dict) and self._validate_dsl(dsl_or_list):
                    t_ex0 = time.time()
                    result = self.adapter.execute(dsl_or_list)
                    exec_ms = round((time.time() - t_ex0) * 1000, 1)
                    self._maybe_learn(resolved_clause, dsl_or_list, result.success)
                    self._update_context_from_step(dsl_or_list, live_ontology, ctx)
                    self.session_journal.append_turn(clause, [dsl_or_list], result.output or result.error)
                    return {
                        "clause": clause,
                        "resolved_clause": resolved_clause,
                        "ir": None,
                        "dsl": dsl_or_list,
                        "result": result.__dict__,
                        "source": f"inference:{backend.name}",
                        "timing_ms": {
                            "observe_ms": observe_ms,
                            "solver_ms": solver_ms,
                            "embedding_ms": emb_ms,
                            "execute_ms": exec_ms,
                            "total_ms": round((time.time() - t_start) * 1000, 1),
                        },
                    }

        # Tier 1b: Multi-turn ReAct Agent Loop via axil-auren
        if self.agent is not None:
            t_ag0 = time.time()
            agent_res = self.agent.run(resolved_clause, live_ontology, self.few_shots)
            agent_ms = round((time.time() - t_ag0) * 1000, 1)
            if agent_res and agent_res.get("dsl"):
                for step in agent_res.get("steps", []):
                    s_dsl = step.get("dsl")
                    if isinstance(s_dsl, dict):
                        self._update_context_from_step(s_dsl, live_ontology, ctx)
                if len(agent_res.get("steps", [])) == 1:
                    self._maybe_learn(resolved_clause, agent_res["dsl"], agent_res["result"]["success"])
                agent_res["clause"] = clause
                agent_res["resolved_clause"] = resolved_clause
                agent_res["timing_ms"] = {
                    "observe_ms": observe_ms,
                    "solver_ms": solver_ms,
                    "agent_ms": agent_ms,
                    "total_ms": round((time.time() - t_start) * 1000, 1),
                }
                return agent_res

        # Offline / fallback solver
        if ir is None:
            ir = build_ir(resolved_clause, active_surface, live_ontology, self.ontology)
        if ir:
            dsl = self._compile_ir_to_dsl(ir, active_surface=active_surface, ctx=ctx, live_ontology=live_ontology)
            if dsl:
                result = self.adapter.execute(dsl)
                self._update_context_from_step(dsl, live_ontology, ctx)
                self.session_journal.append_turn(clause, [dsl], result.output or result.error)
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

        # If the full utterance is conversational, diagnostic, or invokes a remembered workflow,
        # send the entire utterance directly to the ReAct agent without naive clause splitting.
        if self._requires_agent_reasoning(utterance):
            return self._process_single_clause(utterance, active_surface, ctx)

        clauses = self.split_clauses(utterance)

        if len(clauses) <= 1:
            single_clause = clauses[0] if clauses else utterance
            return self._process_single_clause(single_clause, active_surface, ctx)

        # Before splitting a multi-clause utterance into independent steps, check if ALL clauses
        # have high-confidence Tier-0 solver matches. If ANY clause is low-confidence / unknown
        # (e.g. "ssh to tty.pt and list the available disk space"), send the ENTIRE utterance
        # as a whole to the ReAct LLM agent so cross-clause context is never lost!
        if self.agent is not None:
            live_ont = self._get_live_ontology(ctx)
            all_tier0 = True
            for c in clauses:
                rc = self.resolve_anaphora(c, ctx)
                if self._requires_agent_reasoning(rc):
                    all_tier0 = False
                    break
                c_ir = build_ir(rc, active_surface, live_ont, self.ontology)
                if not c_ir or c_ir.get("score", 0) < 0.90:
                    all_tier0 = False
                    break
            if not all_tier0:
                return self._process_single_clause(utterance, active_surface, ctx)

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

    def _compile_ir_to_dsl(
        self,
        ir: Dict[str, Any],
        active_surface: str = "any",
        ctx: Optional[Dict[str, Any]] = None,
        live_ontology: Optional[Dict[str, Any]] = None,
    ) -> Optional[Dict[str, Any]]:
        cap = ir["cap"]
        args = ir.get("args", {})
        target_other = ir.get("target_other", False)
        new_terminal = ir.get("new_terminal", False)
        submit_enter = ir.get("submit_enter", False)

        def _with_other(dsl: Dict[str, Any]) -> Dict[str, Any]:
            if target_other:
                dsl.setdefault("target", {})["relation"] = "other"
            if new_terminal:
                dsl.setdefault("target", {})["new_terminal"] = True
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
            return _with_other(
                {"op": "spawn_shell_command", "target": {"command": f"find {loc} -name '{pat}'"}, "verify": {}}
            )

        if cap == "fs.metric":
            metric = args.get("metric", "cwd")
            cmd_map = {
                "cwd": "pwd",
                "disk_space": "df -h",
                "largest": "du -sh * 2>/dev/null | sort -h",
                "recent": "ls -lat | head -n 20",
            }
            return _with_other(
                {"op": "spawn_shell_command", "target": {"command": cmd_map.get(metric, "pwd")}, "verify": {}}
            )

        if cap == "git.inspect":
            aspect = args.get("aspect", "status")
            cmd_map = {
                "status": "git status",
                "diff": "git diff",
                "log": "git log -n 10 --oneline",
                "branch": "git branch -a",
            }
            return _with_other(
                {"op": "spawn_shell_command", "target": {"command": cmd_map.get(aspect, "git status")}, "verify": {}}
            )

        if cap == "shell.run":
            prog = args.get("program", "")
            cmd = "tmux a || tmux" if prog == "tmux_attach" else prog
            return _with_other({"op": "spawn_shell_command", "target": {"command": cmd}, "verify": {}})

        if cap == "app.launch":
            app = args.get("application", "")
            if app == "terminal":
                return {"op": "open_terminal", "target": {}, "verify": {}}
            return {"op": "open_application", "target": {"identifier": app}, "verify": {}}

        if cap == "web.open":
            url = args.get("url", "")
            return {"op": "open_url", "target": {"url": url}, "verify": {}}

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

        if cap == "window.focus":
            ident = args.get("identifier", "")
            return {"op": "focus_window", "target": {"identifier": ident}, "verify": {}}

        if cap == "window.focus_relative":
            relation = args.get("relation", "other")
            return {"op": "focus_relative_window", "target": {"kind": "relative", "relation": relation}, "verify": {}}

        if cap == "workspace.switch":
            ws = args.get("workspace", 1)
            return {"op": "workspace_switch", "target": {}, "args": {"workspace": ws}, "verify": {}}

        if cap == "workspace.move_window":
            ws = args.get("workspace", 1)
            if target_other:
                return {
                    "op": "workspace_move_window",
                    "target": {"kind": "relative", "relation": "other"},
                    "args": {"workspace": ws},
                    "verify": {},
                }
            target_id = (ctx or {}).get("last_window_id") or (live_ontology or {}).get("focused", {}).get("id", "")
            return {
                "op": "workspace_move_window",
                "target": {"identifier": target_id} if target_id else {},
                "args": {"workspace": ws},
                "verify": {},
            }

        if cap == "window.control":
            action = args.get("action", "unknown")
            op_map = {
                "fullscreen": "window_fullscreen",
                "float": "window_float",
                "tile": "window_float",
                "close": "window_close",
            }
            dsl: Dict[str, Any] = {"op": op_map.get(action, "window_fullscreen"), "target": {}, "verify": {}}
            if action == "close":
                dsl["target"]["kind"] = args.get("kind", "window")
            return _with_other(dsl)

        if cap == "ui.scroll":
            direction = args.get("direction", "down")
            amount = args.get("amount", 5)
            return _with_other(
                {"op": "scroll", "target": {}, "args": {"direction": direction, "amount": amount}, "verify": {}}
            )

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

        if cap == "agent.ask":
            query = args.get("query", "")
            return {"op": "ask_agent", "target": {"query": query}, "verify": {}}

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
        op = dsl.get("op", "")
        # Never auto-learn ephemeral, context-dependent, observation, or free-form dictation ops
        if op in (
            "wait",
            "answer_question",
            "wm_help",
            "ask_agent",
            "type_text",
            "type_and_enter",
            "inspect_terminal",
            "inspect_clipboard",
            "inspect_system",
            "run_background_command",
            "remember_fact",
            "recall_facts",
        ):
            return
        target = dsl.get("target", {})
        if op == "media_play" and not target.get("query"):
            return
        u_clean = clean_phrase(utterance)
        if op == "search_web" and clean_phrase(target.get("query", "")) == u_clean:
            return
        words = u_clean.split()
        # Do not auto-learn 1-word fragments or runaway repetitive ASR hallucinations
        if len(words) < 2 or len(words) > 12:
            return
        if words[0] in (
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
        ) or utterance.strip().endswith("?"):
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
