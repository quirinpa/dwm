"""Compositional Semantic Solver: Utterance + Host Observation -> Platform-Neutral Intent IR."""

import re
from typing import Any, Dict, List, Optional, Set, Tuple

from .ontology import SemanticOntology, clean_phrase


_DICTATION_VERBS = {"type", "dictate", "write", "input"}

_DEICTIC_PRONOUNS = {
    "this",
    "current",
    "active",
    "window",
}


class CompositionalSemanticSolver:
    """Universal Frame-Semantic Solver."""

    def __init__(self, ontology: Optional[SemanticOntology] = None):
        self.ont = ontology or SemanticOntology()

    def strip_locative_modifiers(
        self, clause: str, active_surface: str = "any"
    ) -> Tuple[str, str, bool, bool, bool]:
        """Strip trailing self/other/submit locative phrases."""
        self.ont.reload_if_needed()
        c = clause.strip().strip(",.!?;:")
        eff_surface = active_surface
        explicit_term = False
        target_other = False
        submit_enter = False

        changed = True
        while changed:
            changed = False
            c_low = c.lower()
            for tail in self.ont.self_locatives.get("submit", []):
                if c_low.endswith(" " + tail):
                    c = c[: len(c) - len(tail)].strip().strip(",.!?;:")
                    submit_enter = True
                    changed = True
                    break
            if changed:
                continue
            for tail in self.ont.self_locatives.get("other", []):
                if c_low.endswith(" " + tail):
                    c = c[: len(c) - len(tail)].strip().strip(",.!?;:")
                    target_other = True
                    changed = True
                    break
            if changed:
                continue
            for tail in self.ont.self_locatives.get("terminal", []):
                if c_low.endswith(" " + tail):
                    c = c[: len(c) - len(tail)].strip().strip(",.!?;:")
                    eff_surface = "terminal"
                    explicit_term = True
                    changed = True
                    break
            if changed:
                continue
            for tail in self.ont.self_locatives.get("any", []):
                if c_low.endswith(" " + tail):
                    c = c[: len(c) - len(tail)].strip().strip(",.!?;:")
                    changed = True
                    break

        return (c if c else clause.strip()), eff_surface, explicit_term, target_other, submit_enter

    def extract_constituents(
        self, clause: str, live_entities: Dict[str, List[Tuple[str, str]]]
    ) -> Dict[str, Any]:
        """Bottom-up constituent span tagger with multi-type entity and verb-first resolution."""
        words = clean_phrase(clause).split()
        n = len(words)
        consumed = [False] * n
        verbs: Dict[str, float] = {}
        entities: Dict[str, List[str]] = {}
        modifiers: Set[str] = set()
        lead_verb_span = ""
        lead_verb_end = 0

        # Skip leading polite stopwords ("please", "jarvis", "can", "you", etc.) to find the verb head
        v_start = 0
        while (
            v_start < n
            and words[v_start] in self.ont.stopwords
            and " ".join(words[v_start : min(n, v_start + 3)]) not in self.ont.verb_phrases
        ):
            v_start += 1

        # 1. Tag leading Verb phrase first
        for L in range(min(6, n - v_start), 0, -1):
            span = " ".join(words[v_start : v_start + L])
            if span in self.ont.verb_phrases:
                for v_concept, w in self.ont.verb_phrases[span]:
                    verbs[v_concept] = max(verbs.get(v_concept, 0.0), w)
                lead_verb_span = span
                lead_verb_end = v_start + L
                # Only mark consumed if this verb phrase is not simultaneously a self-contained entity phrase
                if span not in live_entities:
                    for k in range(v_start, v_start + L):
                        consumed[k] = True
                break

        # 2. Tag longest matching Entity spans (up to 6 words) across the clause
        for L in range(min(6, n), 0, -1):
            for i in range(0, n - L + 1):
                span = " ".join(words[i : i + L])
                if span not in live_entities:
                    continue
                if any(consumed[i : i + L]):
                    # Allow multi-word entity phrases that subsume the leading verb (e.g. "turn up the volume", "lower volume")
                    if not (i == v_start and L > (lead_verb_end - v_start) and all(not consumed[k] for k in range(lead_verb_end, i + L))):
                        continue
                # Do not let a 1-word entity consume the leading verb of a multi-word command
                if (
                    i == v_start
                    and L == 1
                    and span in self.ont.verb_phrases
                    and n > v_start + 1
                    and span == lead_verb_span
                ):
                    continue
                for etype, eid in live_entities[span]:
                    lst = entities.setdefault(etype, [])
                    if eid not in lst:
                        lst.append(eid)
                for k in range(i, i + L):
                    consumed[k] = True

        # 3. Tag secondary / interior Verb spans if not yet tagged
        for L in range(min(5, n), 0, -1):
            for i in range(0, n - L + 1):
                if any(consumed[i : i + L]):
                    continue
                span = " ".join(words[i : i + L])
                if span in self.ont.verb_phrases:
                    # Do not let a secondary "google" token (e.g. "google.com" or "... on google") override another leading verb
                    if span == "google" and i > v_start and verbs:
                        continue
                    for v_concept, w in self.ont.verb_phrases[span]:
                        verbs[v_concept] = max(verbs.get(v_concept, 0.0), w)
                    for k in range(i, i + L):
                        consumed[k] = True

        # 4. Modifiers and implicit verb fallback for verbless noun utterances
        for i, w in enumerate(words):
            if w in ("new", "hidden", "all", "silent", "fullscreen", "floating"):
                modifiers.add(w)
            if not consumed[i] and w in self.ont.verb_phrases and not verbs:
                for v_concept, vw in self.ont.verb_phrases[w]:
                    verbs[v_concept] = max(verbs.get(v_concept, 0.0), vw)
                consumed[i] = True

        # Also promote self-contained entity phrases that carry inherent verb semantics
        if "media.object" in entities and entities["media.object"][0] in (
            "next_track",
            "prev_track",
            "volume_up",
            "volume_down",
            "volume_mute",
        ):
            verbs["MEDIA_ADJUST"] = max(verbs.get("MEDIA_ADJUST", 0.0), 0.95)
        if "ui.window_action" in entities and ("OPEN_LAUNCH" not in verbs or verbs.get("OPEN_LAUNCH", 0.0) < 0.90):
            verbs["WINDOW_CONTROL"] = max(verbs.get("WINDOW_CONTROL", 0.0), 0.95)

        if not verbs and entities:
            if "ui.workspace" in entities:
                verbs["WORKSPACE_SWITCH"] = 0.92
            elif "ui.window_action" in entities:
                verbs["WINDOW_CONTROL"] = 0.92
            elif "shell.program" in entities or "ui.container_kind" in entities or "gui.application" in entities:
                verbs["OPEN_LAUNCH"] = 0.90
            elif "web.shortcut" in entities:
                verbs["OPEN_LAUNCH"] = 0.90
            elif "git.object" in entities or "fs.collection" in entities or "fs.metric" in entities:
                verbs["PRESENT"] = 0.90
            elif "ui.relative_window" in entities or "ui.window" in entities:
                verbs["FOCUS_NAV"] = 0.90
            elif "fs.location" in entities:
                verbs["FOCUS_NAV"] = 0.88

        residual_tokens = [
            words[i]
            for i in range(n)
            if not consumed[i] and words[i] not in self.ont.stopwords and words[i] not in _DEICTIC_PRONOUNS
        ]
        open_arg_tokens = words[lead_verb_end:] if lead_verb_end > 0 else residual_tokens
        content_tokens = [w for w in words if w not in self.ont.stopwords]
        total_content = max(1, len(content_tokens))
        bound_content = max(0, total_content - len(residual_tokens))
        coverage = bound_content / float(total_content)

        return {
            "verbs": verbs,
            "entities": entities,
            "modifiers": modifiers,
            "residual_tokens": residual_tokens,
            "residual_text": " ".join(residual_tokens),
            "open_arg_text": " ".join(open_arg_tokens),
            "lead_verb_span": lead_verb_span,
            "coverage": coverage,
            "words": words,
        }

    def _extract_after_verb(self, raw_stripped: str, lead_verb_span: str, strip_preps: Tuple[str, ...] = ()) -> str:
        """Cleanly extract the open argument substring following the matched verb phrase."""
        q = raw_stripped.strip()
        q = re.sub(
            r"^(?:(?:please|jarvis|hey|ok|okay|can\s+you|could\s+you|would\s+you|let's|lets)\s+)+",
            "",
            q,
            flags=re.IGNORECASE,
        ).strip()

        candidate_vps = sorted(self.ont.verb_phrases.keys(), key=len, reverse=True)
        if lead_verb_span:
            candidate_vps = [lead_verb_span] + [vp for vp in candidate_vps if vp != lead_verb_span]

        q_low = q.lower()
        for vp in candidate_vps:
            if q_low.startswith(vp + " "):
                q = q[len(vp) :].strip()
                break
            vp_pattern = r"^" + r"\s+".join(re.escape(w) for w in vp.split()) + r"\b\s*"
            m = re.match(vp_pattern, q_low)
            if m:
                q = q[m.end() :].strip()
                break

        if strip_preps:
            changed = True
            while changed:
                changed = False
                for p in strip_preps:
                    if q.lower().startswith(p):
                        q = q[len(p) :].strip()
                        changed = True
                        break
        return q.strip()

    def solve_clause(
        self,
        raw_clause: str,
        active_surface: str = "any",
        env_obs: Optional[Dict[str, Any]] = None,
    ) -> Optional[Dict[str, Any]]:
        """Resolve a single clause to a Canonical Cross-Platform Intent IR dict."""
        self.ont.reload_if_needed()
        live_entities = self.ont.build_live_entity_index(env_obs)
        norm_clause = self.ont.normalize_asr_utterance(raw_clause, live_entities)
        if not norm_clause:
            return None

        stripped, eff_surface, explicit_term, target_other, submit_enter = self.strip_locative_modifiers(
            norm_clause, active_surface=active_surface
        )
        new_terminal = False
        m_new_term = re.search(
            r"\s+\b(?:in|on|using|inside|into)\s+(?:a\s+)?new\s+(?:terminal|console|shell)(?:\s+window)?\b\s*$",
            stripped,
            flags=re.IGNORECASE,
        )
        if m_new_term:
            stripped = stripped[: m_new_term.start()].strip().strip(",.!?;:")
            new_terminal = True
            eff_surface = "terminal"
            explicit_term = True

        parsed = self.extract_constituents(stripped, live_entities)
        verbs = parsed["verbs"]
        entities = parsed["entities"]
        modifiers = parsed["modifiers"]
        residual = parsed["residual_tokens"]
        lead_verb_span = parsed["lead_verb_span"]

        # Guard: Never hijack help questions or conversational interrogatives
        if "HELP_ASK" in verbs:
            return None

        raw_words = clean_phrase(raw_clause).split()
        while raw_words and raw_words[0] in ("please", "jarvis", "hey", "ok", "okay"):
            raw_words.pop(0)
        if raw_words and raw_words[0] in (
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
        ):
            if not (
                not residual
                and ("fs.metric" in entities or "fs.collection" in entities or "git.object" in entities)
            ):
                return None

        # Direct URL pattern in clause (e.g. "open https://...", "go to github.com/torvalds")
        # Require lead_verb_span (leading OPEN_LAUNCH or FOCUS_NAV verb) and zero unexplained non-URL residual tokens!
        m_url = re.search(
            r"\b(https?://[^\s]+|[a-zA-Z0-9-]+\.(?:com|org|net|io|dev|ai|edu|gov|pt)(?:/[^\s]*)?)\b", stripped
        )
        if m_url and lead_verb_span and ("OPEN_LAUNCH" in verbs or "FOCUS_NAV" in verbs):
            url_val = m_url.group(1)
            url_words = set(clean_phrase(url_val).split())
            non_url_residual = [t for t in residual if t not in url_words]
            if (
                not non_url_residual
                and "PRESENT" not in verbs
                and "shell.program" not in entities
                and "SEARCH_WEB" not in verbs
            ):
                return {
                    "cap": "web.open",
                    "score": 0.96,
                    "surface": "any",
                    "explicit_terminal": False,
                    "target_other": target_other,
                    "submit_enter": submit_enter,
                    "args": {"url": url_val},
                }

        # Guard: Explicit dictation only when clause begins with a dictation verb ('type', 'dictate', 'write', 'input')
        is_explicit_dictation = bool(raw_words and raw_words[0] in _DICTATION_VERBS)
        if is_explicit_dictation and "edit.unit" not in entities:
            ir_dict = self._build_intent_ir(
                "input.dictate",
                0.96,
                verbs,
                entities,
                modifiers,
                residual,
                stripped,
                lead_verb_span,
                eff_surface,
                explicit_term,
                target_other,
                submit_enter,
            )
            if ir_dict:
                ir_dict["target_other"] = target_other
                ir_dict.setdefault("submit_enter", submit_enter)
                return ir_dict

        # Fast-path explicit coding agent delegation ("ask pi to ...", "tell the agent to ...")
        if "AGENT_ASK" in verbs:
            ir_agent = self._build_intent_ir(
                "agent.ask",
                0.96,
                verbs,
                entities,
                modifiers,
                residual,
                stripped,
                lead_verb_span,
                eff_surface,
                explicit_term,
                target_other,
                submit_enter,
            )
            if ir_agent:
                ir_agent["target_other"] = target_other
                return ir_agent

        best_cap: Optional[Dict[str, Any]] = None
        best_score = 0.0

        for cap in self.ont.capabilities:
            cid = cap["id"]
            allowed_verbs = cap.get("verbs", [])
            v_score = max((verbs.get(v, 0.0) for v in allowed_verbs), default=0.0)
            if v_score <= 0.0:
                continue

            req_ents = set(cap.get("required_entities", []))
            forbid_ents = set(cap.get("forbid_entities", [])) - req_ents
            if any(fe in entities for fe in forbid_ents):
                blocked = False
                for fe in forbid_ents:
                    if fe in entities:
                        if not req_ents or fe not in req_ents:
                            # Allow multi-tagged entity overlaps (e.g. "terminal" is both ui.container_kind and gui.application;
                            # "firefox" is both gui.application and ui.window; "window" is in "make this window full screen")
                            if fe in ("ui.container_kind", "gui.application", "ui.window", "fs.location") and (
                                req_ents & {"ui.container_kind", "gui.application", "ui.window", "ui.window_action", "media.object"}
                                or cid in ("window.control", "media.play", "media.control")
                            ):
                                if fe == "fs.location" and "media.object" not in entities:
                                    blocked = True
                                    break
                                continue
                            blocked = True
                            break
                if blocked:
                    continue

            # Required entity types
            if req_ents and not all(re_t in entities for re_t in req_ents):
                if cid == "fs.list" and "fs.location" in entities and "PRESENT" in verbs and "FOCUS_NAV" not in verbs:
                    pass
                elif (
                    cid == "workspace.switch"
                    and "WORKSPACE_SWITCH" in verbs
                    and any(t.isdigit() and 1 <= int(t) <= 9 for t in residual)
                ):
                    pass
                elif (
                    cid == "workspace.move_window"
                    and "WORKSPACE_MOVE" in verbs
                    and any(t.isdigit() and 1 <= int(t) <= 9 for t in residual)
                ):
                    pass
                elif cid == "ui.scroll" and "SCROLL_NAV" in verbs:
                    pass
                else:
                    continue

            # Required_any for media.play
            req_any = cap.get("required_any", [])
            if req_any:
                ok_any = False
                if "media.object" in req_any and "media.object" in entities:
                    ok_any = True
                if (
                    "residual_with_play_verb" in req_any
                    and (residual or "media.object" in entities or lead_verb_span in ("press play", "hit play", "resume", "unpause", "continue playing"))
                    and verbs.get("PLAY_STREAM", 0.0) >= 0.80
                ):
                    ok_any = True
                if not ok_any:
                    continue

            # Content-Token Coverage Invariant
            if cap.get("forbid_residual", False) and residual:
                if cid in ("workspace.switch", "workspace.move_window") and all(
                    t.isdigit() and 1 <= int(t) <= 9 for t in residual
                ):
                    pass
                elif cid == "ui.scroll" and all(t.isdigit() for t in residual):
                    pass
                elif cid == "window.control" and residual == ["it"]:
                    pass
                else:
                    continue

            if cid == "input.dictate" and not is_explicit_dictation:
                continue

            # Compute joint score = VerbFit * Coverage * ArgTypeFit * ContextFit
            # Penalize any unexplained residual tokens for closed-arg capabilities so partial matches drop in confidence
            cap_surface = cap.get("surface", "any")
            ctx_bonus = 0.04 if (cap_surface == eff_surface or explicit_term) else 0.0
            req_bonus = 0.03 if req_ents else 0.0
            is_open_arg_cap = cid in ("media.play", "web.search", "input.dictate", "agent.ask")
            eff_res_count = 0 if (is_open_arg_cap or (cid == "window.control" and residual == ["it"])) else len(residual)
            residual_penalty = 0.18 * eff_res_count
            lead_penalty = 0.0 if (lead_verb_span or not verbs or "ui.window_action" in entities or "media.object" in entities) else 0.14
            score = min(0.99, max(0.0, 0.55 * v_score + 0.41 + ctx_bonus + req_bonus - residual_penalty - lead_penalty))

            # Disambiguation: open/launch GUI application vs focus live window
            if cid == "app.launch" and "OPEN_LAUNCH" in verbs and "FOCUS_NAV" not in verbs:
                score = 0.99
            if cid == "window.focus" and "OPEN_LAUNCH" in verbs and "gui.application" in entities:
                continue

            # Disambiguation between fs.list and fs.navigate
            if cid == "fs.navigate" and "fs.location" in entities:
                if verbs.get("FOCUS_NAV", 0.0) >= 0.90 and "fs.collection" not in entities:
                    score = 0.99
                elif "PRESENT" in verbs and "OPEN_LAUNCH" not in verbs:
                    continue

            # Disambiguation: workspace.move_window beats workspace.switch when WORKSPACE_MOVE is active
            if cid == "workspace.move_window" and verbs.get("WORKSPACE_MOVE", 0.0) >= 0.80:
                score = 0.99
            if cid == "workspace.switch" and verbs.get("WORKSPACE_MOVE", 0.0) >= 0.80:
                continue

            # Disambiguation: window.control beats window.new when ui.window_action or WINDOW_CONTROL is active
            if cid == "window.control" and (
                "ui.window_action" in entities or verbs.get("WINDOW_CONTROL", 0.0) >= 0.80
            ):
                score = 0.99

            # Disambiguation: media.control vs media.play
            if cid == "media.control":
                m_objs = entities.get("media.object", [])
                if "PAUSE_STOP" in verbs and not residual:
                    score = 0.99
                elif (
                    m_objs
                    and m_objs[0] in ("next_track", "prev_track", "volume_up", "volume_down", "volume_mute")
                    and not residual
                ):
                    score = 0.99
                else:
                    continue

            if score > best_score:
                best_score = score
                best_cap = cap

        if not best_cap or best_score < 0.80:
            return None

        # Detect "other window" on any capability where ui.relative_window == ["other"] or phrase contains "other window"
        if best_cap["id"] != "window.focus_relative":
            if "other" in entities.get("ui.relative_window", []) or re.search(
                r"\bother\s+(?:window|terminal)\b", stripped, flags=re.IGNORECASE
            ):
                target_other = True

        ir_res = self._build_intent_ir(
            best_cap["id"],
            best_score,
            verbs,
            entities,
            modifiers,
            residual,
            stripped,
            lead_verb_span,
            eff_surface,
            explicit_term,
            target_other,
            submit_enter,
        )
        if ir_res is not None:
            ir_res["target_other"] = target_other
            if new_terminal:
                ir_res["new_terminal"] = True
            ir_res.setdefault("submit_enter", submit_enter)
        return ir_res

    def _build_intent_ir(
        self,
        cid: str,
        score: float,
        verbs: Dict[str, float],
        entities: Dict[str, List[str]],
        modifiers: Set[str],
        residual: List[str],
        raw_stripped: str,
        lead_verb_span: str,
        eff_surface: str,
        explicit_term: bool,
        target_other: bool,
        submit_enter: bool,
    ) -> Optional[Dict[str, Any]]:
        loc = (entities.get("fs.location") or ["."])[0]

        if cid == "fs.list":
            coll = (entities.get("fs.collection") or ["files"])[0]
            return {
                "cap": "fs.list",
                "score": score,
                "surface": "terminal",
                "explicit_terminal": explicit_term,
                "args": {"location": loc, "hidden": True, "collection": coll},
            }

        if cid == "fs.metric":
            metric = entities["fs.metric"][0]
            return {
                "cap": "fs.metric",
                "score": score,
                "surface": "terminal",
                "explicit_terminal": explicit_term,
                "args": {"metric": metric, "location": loc},
            }

        if cid == "fs.find":
            pattern = entities["fs.file_type"][0]
            return {
                "cap": "fs.find",
                "score": score,
                "surface": "terminal",
                "explicit_terminal": explicit_term,
                "args": {"pattern": pattern, "location": loc},
            }

        if cid == "fs.navigate":
            target_dir = entities["fs.location"][0]
            return {
                "cap": "fs.navigate",
                "score": score,
                "surface": "terminal",
                "explicit_terminal": explicit_term,
                "args": {"location": target_dir},
            }

        if cid == "git.inspect":
            gobj = entities["git.object"][0]
            return {
                "cap": "git.inspect",
                "score": score,
                "surface": "terminal",
                "explicit_terminal": explicit_term,
                "args": {"aspect": gobj, "location": loc},
            }

        if cid == "shell.run":
            prog = entities["shell.program"][0]
            return {
                "cap": "shell.run",
                "score": score,
                "surface": "terminal",
                "explicit_terminal": explicit_term,
                "args": {"program": prog},
            }

        if cid == "edit.clear":
            unit = entities["edit.unit"][0]
            return {
                "cap": "edit.clear",
                "score": score,
                "surface": eff_surface,
                "explicit_terminal": explicit_term,
                "args": {"unit": unit},
            }

        if cid == "window.new":
            kind = entities["ui.container_kind"][0]
            if kind in (
                "terminal",
                "window",
                "tab",
                "devtools",
                "address_bar",
                "sys_monitor",
                "calendar",
                "control_center",
                "wifi",
                "battery",
                "power_menu",
                "shortcuts_help",
            ):
                if kind in ("window", "tab") and "new" not in modifiers and "OPEN_LAUNCH" not in verbs:
                    return None
                return {
                    "cap": "window.new",
                    "score": score,
                    "surface": eff_surface,
                    "explicit_terminal": explicit_term,
                    "args": {"kind": kind, "new": ("new" in modifiers)},
                }
            return None

        if cid == "window.focus":
            win_id = entities["ui.window"][0]
            return {
                "cap": "window.focus",
                "score": score,
                "surface": "any",
                "explicit_terminal": False,
                "args": {"identifier": win_id},
            }

        if cid == "window.focus_relative":
            rel = entities["ui.relative_window"][0]
            return {
                "cap": "window.focus_relative",
                "score": score,
                "surface": "any",
                "explicit_terminal": False,
                "args": {"relation": rel},
            }

        if cid in ("workspace.switch", "workspace.move_window"):
            ws = 1
            if "ui.workspace" in entities:
                ws = int(entities["ui.workspace"][0])
            else:
                for token in residual:
                    if token.isdigit() and 1 <= int(token) <= 9:
                        ws = int(token)
                        break
                nums = re.findall(r"\b([1-9])\b", raw_stripped)
                if nums:
                    ws = int(nums[0])
            return {
                "cap": cid,
                "score": score,
                "surface": "any",
                "explicit_terminal": False,
                "args": {"workspace": ws},
            }

        if cid == "window.control":
            action = "unknown"
            if "ui.window_action" in entities:
                action = entities["ui.window_action"][0]
            else:
                rl = raw_stripped.lower()
                if "fullscreen" in rl or "full screen" in rl or "maximize" in rl:
                    action = "fullscreen"
                elif "unfloat" in rl or "tile" in rl:
                    action = "tile"
                elif "float" in rl:
                    action = "float"
                elif "close" in rl or "kill" in rl or "quit" in rl:
                    action = "close"
            if action == "unknown":
                return None
            kind = "tab" if ("tab" in entities.get("ui.container_kind", []) or re.search(r"\btab\b", raw_stripped, flags=re.IGNORECASE)) else "window"
            return {
                "cap": "window.control",
                "score": score,
                "surface": "any",
                "explicit_terminal": False,
                "args": {"action": action, "kind": kind},
            }

        if cid == "ui.scroll":
            if "ui.scroll_direction" in entities:
                direction = entities["ui.scroll_direction"][0]
            else:
                rl = raw_stripped.lower()
                if "up" in rl:
                    direction = "up"
                elif "left" in rl:
                    direction = "left"
                elif "right" in rl:
                    direction = "right"
                else:
                    direction = "down"
            amount = 5
            for t in residual:
                if t.isdigit():
                    amount = max(1, min(20, int(t)))
            return {
                "cap": "ui.scroll",
                "score": score,
                "surface": eff_surface,
                "explicit_terminal": False,
                "args": {"direction": direction, "amount": amount},
            }

        if cid == "input.dictate":
            q_clean = self._extract_after_verb(raw_stripped, lead_verb_span, strip_preps=("in ", "out "))
            submit_enter_flag = submit_enter or q_clean.lower().endswith(
                (" and press enter", " and hit enter", " and hit return", " then press enter", " then hit enter")
            )
            if submit_enter_flag:
                for suffix in (
                    " and press enter",
                    " and hit enter",
                    " and hit return",
                    " then press enter",
                    " then hit enter",
                ):
                    if q_clean.lower().endswith(suffix):
                        q_clean = q_clean[: -len(suffix)].strip()
                        break
            if not q_clean and not submit_enter_flag:
                return None
            return {
                "cap": "input.dictate",
                "score": score,
                "surface": eff_surface,
                "explicit_terminal": explicit_term,
                "args": {"text": q_clean, "submit_enter": submit_enter_flag},
            }

        if cid == "agent.ask":
            q_clean = self._extract_after_verb(raw_stripped, lead_verb_span, strip_preps=("to ", "for ", "about "))
            if not q_clean:
                return None
            return {
                "cap": "agent.ask",
                "score": score,
                "surface": "any",
                "explicit_terminal": False,
                "args": {"query": q_clean},
            }

        if cid == "web.open":
            url = entities["web.shortcut"][0]
            return {
                "cap": "web.open",
                "score": score,
                "surface": "any",
                "explicit_terminal": False,
                "args": {"url": url},
            }

        if cid == "app.launch":
            app = entities["gui.application"][0]
            return {
                "cap": "app.launch",
                "score": score,
                "surface": "any",
                "explicit_terminal": False,
                "args": {"application": app},
            }

        if cid == "record.control":
            robj = entities["record.object"][0]
            op = "stop" if "PAUSE_STOP" in verbs else "start"
            return {
                "cap": "record.control",
                "score": score,
                "surface": "any",
                "explicit_terminal": False,
                "args": {"target": robj, "action": op},
            }

        if cid == "web.search":
            q_clean = self._extract_after_verb(raw_stripped, lead_verb_span, strip_preps=("for ", "about ", "on "))
            if not q_clean:
                return None
            return {
                "cap": "web.search",
                "score": score,
                "surface": "any",
                "explicit_terminal": False,
                "args": {"query": q_clean},
            }

        if cid == "media.control":
            m_objs = entities.get("media.object", [])
            if "PAUSE_STOP" in verbs and m_objs and not residual:
                return {
                    "cap": "media.control",
                    "score": score,
                    "surface": "any",
                    "explicit_terminal": False,
                    "args": {"action": "pause"},
                }
            if m_objs and m_objs[0] in ("next_track", "prev_track", "volume_up", "volume_down", "volume_mute"):
                return {
                    "cap": "media.control",
                    "score": score,
                    "surface": "any",
                    "explicit_terminal": False,
                    "args": {"action": m_objs[0]},
                }
            return None

        if cid == "media.play":
            m_objs = entities.get("media.object", [])
            if "PAUSE_STOP" in verbs and m_objs == ["music"] and not residual:
                return {
                    "cap": "media.control",
                    "score": score,
                    "surface": "any",
                    "explicit_terminal": False,
                    "args": {"action": "pause"},
                }
            if (
                m_objs
                and m_objs[0] in ("next_track", "prev_track", "volume_up", "volume_down", "volume_mute")
                and not residual
            ):
                return {
                    "cap": "media.control",
                    "score": score,
                    "surface": "any",
                    "explicit_terminal": False,
                    "args": {"action": m_objs[0]},
                }
            q_clean = self._extract_after_verb(
                raw_stripped,
                lead_verb_span,
                strip_preps=("a bit of ", "a little ", "some ", "the ", "a ", "an "),
            )
            if not q_clean:
                if "media.object" in entities or lead_verb_span in ("press play", "hit play", "resume", "unpause", "continue playing"):
                    q_clean = "music"
                else:
                    return None
            return {
                "cap": "media.play",
                "score": min(score, 0.96),
                "surface": "any",
                "explicit_terminal": False,
                "args": {"query": q_clean},
            }

        return None


def build_ir(
    utterance: str,
    active_surface: str = "any",
    env_obs: Optional[Dict[str, Any]] = None,
    ontology: Optional[SemanticOntology] = None,
) -> Optional[Dict[str, Any]]:
    """Convenience function: single clause -> Intent IR."""
    solver = CompositionalSemanticSolver(ontology)
    return solver.solve_clause(utterance, active_surface, env_obs)
