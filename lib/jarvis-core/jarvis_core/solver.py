"""Compositional Semantic Solver: Utterance + Host Observation -> Platform-Neutral Intent IR."""

import difflib
import re
from typing import Any, Dict, List, Optional, Set, Tuple

from .ontology import SemanticOntology, clean_phrase


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
        self, clause: str, live_entities: Dict[str, Tuple[str, str]]
    ) -> Dict[str, Any]:
        """Bottom-up greedy constituent span tagger."""
        words = clean_phrase(clause).split()
        n = len(words)
        consumed = [False] * n
        verbs: Dict[str, float] = {}
        entities: Dict[str, List[str]] = {}
        modifiers: Set[str] = set()

        # 1. Tag longest matching Entity spans first (up to 6 words)
        for L in range(min(6, n), 0, -1):
            for i in range(0, n - L + 1):
                if any(consumed[i : i + L]):
                    continue
                span = " ".join(words[i : i + L])
                if span in live_entities:
                    etype, eid = live_entities[span]
                    if i == 0 and L == 1 and span in self.ont.verb_phrases and n > 1:
                        continue
                    entities.setdefault(etype, []).append(eid)
                    for k in range(i, i + L):
                        consumed[k] = True

        # 2. Tag Verb spans (usually near the start, up to 4 words)
        for L in range(min(4, n), 0, -1):
            for i in range(0, min(4, n - L + 1)):
                if any(consumed[i : i + L]):
                    continue
                span = " ".join(words[i : i + L])
                if span in self.ont.verb_phrases:
                    for v_concept, w in self.ont.verb_phrases[span]:
                        verbs[v_concept] = max(verbs.get(v_concept, 0.0), w)
                    for k in range(i, i + L):
                        consumed[k] = True

        # 3. Light verb / implicit verb fallback
        for i, w in enumerate(words):
            if w in ("new", "hidden", "all", "silent", "fullscreen", "floating"):
                modifiers.add(w)
            if not consumed[i] and w in self.ont.verb_phrases and not verbs:
                for v_concept, vw in self.ont.verb_phrases[w]:
                    verbs[v_concept] = max(verbs.get(v_concept, 0.0), vw)
                consumed[i] = True

        if not verbs and entities:
            if "shell.program" in entities or "ui.container_kind" in entities:
                verbs["OPEN_LAUNCH"] = 0.90
            elif "git.object" in entities or "fs.collection" in entities or "fs.metric" in entities:
                verbs["PRESENT"] = 0.90
            elif "ui.relative_window" in entities:
                verbs["FOCUS_NAV"] = 0.90
            elif "fs.location" in entities:
                verbs["FOCUS_NAV"] = 0.88

        residual_tokens = [
            words[i]
            for i in range(n)
            if not consumed[i] and words[i] not in self.ont.stopwords
        ]
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
            "coverage": coverage,
            "words": words,
        }

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
        parsed = self.extract_constituents(stripped, live_entities)
        verbs = parsed["verbs"]
        entities = parsed["entities"]
        modifiers = parsed["modifiers"]
        residual = parsed["residual_tokens"]

        # Guard: Never hijack help questions
        if "HELP_ASK" in verbs:
            return None

        # Guard: Explicit dictation handled by input.dictate capability
        pass

        best_cap: Optional[Dict[str, Any]] = None
        best_score = 0.0

        for cap in self.ont.capabilities:
            cid = cap["id"]
            allowed_verbs = cap.get("verbs", [])
            v_score = max((verbs.get(v, 0.0) for v in allowed_verbs), default=0.0)
            if v_score <= 0.0:
                continue

            # Selectional Restrictions: forbidden entity types
            forbid_ents = cap.get("forbid_entities", [])
            if any(fe in entities for fe in forbid_ents):
                continue

            # Required entity types
            req_ents = cap.get("required_entities", [])
            if req_ents and not all(re_t in entities for re_t in req_ents):
                if cid == "fs.list" and "fs.location" in entities and "PRESENT" in verbs and "FOCUS_NAV" not in verbs:
                    pass
                else:
                    continue

            # Required_any for media.play
            req_any = cap.get("required_any", [])
            if req_any:
                ok_any = False
                if "media.object" in req_any and "media.object" in entities:
                    ok_any = True
                if "residual_with_play_verb" in req_any and residual and verbs.get("PLAY_STREAM", 0.0) >= 0.80:
                    ok_any = True
                if not ok_any:
                    continue

            # Content-Token Coverage Invariant
            if cap.get("forbid_residual", False) and residual:
                continue

            # Compute joint score = VerbFit * ArgTypeFit * ContextFit
            cap_surface = cap.get("surface", "any")
            ctx_bonus = 0.04 if (cap_surface == eff_surface or explicit_term) else 0.0
            score = min(0.99, 0.55 * v_score + 0.43 + ctx_bonus)

            # Disambiguation between fs.list and fs.navigate
            if cid == "fs.navigate" and "fs.location" in entities:
                if verbs.get("FOCUS_NAV", 0.0) >= 0.90:
                    score = 0.99
                elif "PRESENT" in verbs and "OPEN_LAUNCH" not in verbs:
                    continue

            if score > best_score:
                best_score = score
                best_cap = cap

        if not best_cap or best_score < 0.80:
            return None

        # Also detect "in the other window" or "other window" on window.control
        if best_cap["id"] == "window.control" and entities.get("ui.relative_window") == ["other"]:
            target_other = True

        ir_res = self._build_intent_ir(
            best_cap["id"],
            best_score,
            verbs,
            entities,
            modifiers,
            residual,
            stripped,
            eff_surface,
            explicit_term,
            target_other,
            submit_enter,
        )
        if ir_res is not None:
            ir_res["target_other"] = target_other
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
            if kind in ("terminal", "window", "tab", "devtools", "address_bar", "sys_monitor", "calendar", "control_center", "wifi", "battery", "power_menu", "shortcuts_help"):
                if kind == "window" and "new" not in modifiers and "OPEN_LAUNCH" not in verbs:
                    return None
                if kind == "tab" and "new" not in modifiers and "OPEN_LAUNCH" not in verbs:
                    return None
                return {
                    "cap": "window.new",
                    "score": score,
                    "surface": eff_surface,
                    "explicit_terminal": explicit_term,
                    "args": {"kind": kind, "new": ("new" in modifiers)},
                }
            return None

        if cid == "window.focus_relative":
            rel = entities["ui.relative_window"][0]
            return {
                "cap": "window.focus_relative",
                "score": score,
                "surface": "any",
                "explicit_terminal": False,
                "args": {"relation": rel},
            }

        if cid == "workspace.switch":
            ws = 1
            for token in residual:
                if token.isdigit() and 1 <= int(token) <= 9:
                    ws = int(token)
                    break
            nums = re.findall(r"\b([1-9])\b", raw_stripped)
            if nums:
                ws = int(nums[0])
            return {
                "cap": "workspace.switch",
                "score": score,
                "surface": "any",
                "explicit_terminal": False,
                "args": {"workspace": ws},
            }

        if cid == "window.control":
            action = "unknown"
            rl = raw_stripped.lower()
            if "fullscreen" in rl or "maximize" in rl:
                action = "fullscreen"
            elif "float" in rl:
                action = "float"
            elif "tile" in rl:
                action = "tile"
            elif "close" in rl or "kill" in rl:
                action = "close"
            return {
                "cap": "window.control",
                "score": score,
                "surface": "any",
                "explicit_terminal": False,
                "args": {"action": action},
            }


        if cid == "input.dictate":
            q_clean = raw_stripped
            for vp in sorted(self.ont.verb_phrases.keys(), key=len, reverse=True):
                if q_clean.lower().startswith(vp + " "):
                    q_clean = q_clean[len(vp):].strip()
                    break
            while q_clean.lower().startswith(("the ", "a ", "an ", "some ")):
                for prefix in ("the ", "a ", "an ", "some "):
                    if q_clean.lower().startswith(prefix):
                        q_clean = q_clean[len(prefix):].strip()
                        break
            submit_enter_flag = submit_enter or q_clean.lower().endswith((" and press enter", " and hit enter", " and hit return", " then press enter", " then hit enter"))
            if submit_enter_flag:
                for suffix in (" and press enter", " and hit enter", " and hit return", " then press enter", " then hit enter"):
                    if q_clean.lower().endswith(suffix):
                        q_clean = q_clean[:-len(suffix)].strip()
                        break
            return {
                "cap": "input.dictate",
                "score": score,
                "surface": eff_surface,
                "explicit_terminal": explicit_term,
                "args": {"text": q_clean, "submit_enter": submit_enter_flag},
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
            q_clean = raw_stripped
            for vp in sorted(self.ont.verb_phrases.keys(), key=len, reverse=True):
                if q_clean.lower().startswith(vp + " "):
                    q_clean = q_clean[len(vp):].strip()
                    break
            while q_clean.lower().startswith(("for ", "about ", "on ")):
                for prefix in ("for ", "about ", "on "):
                    if q_clean.lower().startswith(prefix):
                        q_clean = q_clean[len(prefix):].strip()
                        break
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
            if m_objs in (["next_track"], ["prev_track"], ["volume_up"], ["volume_down"], ["volume_mute"]):
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
            if m_objs in (["next_track"], ["prev_track"], ["volume_up"], ["volume_down"], ["volume_mute"]):
                return {
                    "cap": "media.control",
                    "score": score,
                    "surface": "any",
                    "explicit_terminal": False,
                    "args": {"action": m_objs[0]},
                }
            # Extract clean media query
            q_clean = raw_stripped
            for vp in sorted(self.ont.verb_phrases.keys(), key=len, reverse=True):
                if q_clean.lower().startswith(vp + " "):
                    q_clean = q_clean[len(vp):].strip()
                    break
            while q_clean.lower().startswith(("some ", "a bit of ", "a little ", "the ", "a ", "an ")):
                for prefix in ("a bit of ", "a little ", "some ", "the ", "a ", "an "):
                    if q_clean.lower().startswith(prefix):
                        q_clean = q_clean[len(prefix):].strip()
                        break
            if not q_clean:
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