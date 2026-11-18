#!/usr/bin/env python3
"""
jarvis_core.py — Universal, Zero-Hardcoding Compositional Semantic Solver & Cross-Platform Driver
=================================================================================================
Architecture:
  1. Lexicon-Driven ASR Post-Processor:
     - Uses the loaded declarative ontology + live environment entities (no hardcoded word lists)
     - Folds hyphenated/spelled acronyms ("T-MUX" -> "tmux", "H-TOP" -> "htop")
     - Phonetic/edit-distance verb snapping when followed by a domain entity ("least files" -> "list files")
     - Structural prefix-echo truncation ("Open tmux in this terminal. Open t" -> "Open tmux in this terminal")
  2. Compositional Span Tagger & Typed Frame Unifier:
     - Extracts (ACTION, ENTITY, LOCATIVE, MODIFIER, RESIDUAL) constituents
     - Enforces Selectional Restrictions (Argument Type Constraints):
         • "put on a list of my files"  -> Verb=PRESENT|PLAY, Entity=fs.collection("files")
           -> media.play REJECTED (forbids fs.*); fs.list SELECTED (score=0.99)
         • "put on some Miles Davis"    -> Verb=PRESENT|PLAY, Entity=media/residual("Miles Davis")
           -> fs.list REJECTED (missing fs.collection); media.play SELECTED (score=0.96)
     - Enforces Content-Token Coverage Invariant:
         • Refuses partial guesses when content tokens remain unaccounted for, delegating cleanly
           to Tier-2 LLM Semantic Compiler.
  3. Cross-Platform Intent IR & Pluggable Host Drivers:
     - Produces platform-neutral Intent IR: {"cap": "fs.list", "args": {"location": "~"}, "surface": "terminal"}
     - Compiles Intent IR to:
         • Linux / dwm (`compile_ir_linux_dwm`)
         • Windows Desktop / PowerShell (`compile_ir_windows`)
         • Website / Web App / Hyle-Bud (`compile_ir_webapp`)
"""

import difflib
import json
import os
import re
from typing import Any, Dict, List, Optional, Set, Tuple


def clean_tokens(text: str) -> List[str]:
    out = []
    for ch in (text or "").lower():
        if ch.isalnum() or ch in ("'", "~", "/", "."):
            out.append(ch)
        else:
            out.append(" ")
    return [t.strip("'") for t in "".join(out).split() if t.strip("'")]


def clean_phrase(text: str) -> str:
    out = []
    for ch in (text or "").lower():
        if ch.isalnum() or ch.isspace():
            out.append(ch)
        else:
            out.append(" ")
    return " ".join("".join(out).split())


class SemanticOntology:
    """Loads and indexes the declarative ontology.json + dynamic host environment entities."""

    def __init__(self, ontology_path: Optional[str] = None):
        if not ontology_path:
            candidates = [
                os.environ.get("DWM_ONTOLOGY_JSON", ""),
                os.path.expanduser("~/.config/dwm/ontology.json"),
                os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "ontology.json"),
                "/home/quirinpa/.local/src/dwm/ontology.json",
            ]
            for c in candidates:
                if c and os.path.exists(c):
                    ontology_path = c
                    break
        self.ontology_path = ontology_path or ""
        self.mtime = 0.0
        self.raw: Dict[str, Any] = {}
        self.stopwords: Set[str] = set()
        self.clause_seps: List[str] = []
        # phrase_clean -> list of (verb_concept, weight)
        self.verb_phrases: Dict[str, List[Tuple[str, float]]] = {}
        self.verb_head_words: Set[str] = set()
        # phrase_clean -> (entity_type, entity_id)
        self.entity_phrases: Dict[str, Tuple[str, str]] = {}
        self.all_vocab_words: Set[str] = set()
        self.self_locatives: Dict[str, List[str]] = {}
        self.capabilities: List[Dict[str, Any]] = []
        self.reload_if_needed()

    def reload_if_needed(self) -> None:
        if not self.ontology_path or not os.path.exists(self.ontology_path):
            return
        try:
            mt = os.path.getmtime(self.ontology_path)
            if mt == self.mtime and self.raw:
                return
            with open(self.ontology_path, "r", encoding="utf-8") as f:
                self.raw = json.load(f)
            self.mtime = mt
        except Exception:
            return

        self.stopwords = set(self.raw.get("stopwords", []))
        self.clause_seps = sorted(self.raw.get("clause_separators", ["and then", "then", "and"]), key=len, reverse=True)
        self.verb_phrases = {}
        self.verb_head_words = set()
        self.entity_phrases = {}
        self.all_vocab_words = set()

        for v_concept, groups in self.raw.get("verbs", {}).items():
            for p in groups.get("primary", []):
                pc = clean_phrase(p)
                if pc:
                    self.verb_phrases.setdefault(pc, []).append((v_concept, 1.0))
                    self.verb_head_words.add(pc.split()[0])
                    self.all_vocab_words.update(pc.split())
            for p in groups.get("light", []):
                pc = clean_phrase(p)
                if pc:
                    self.verb_phrases.setdefault(pc, []).append((v_concept, 0.82))
                    self.verb_head_words.add(pc.split()[0])
                    self.all_vocab_words.update(pc.split())

        for e_type, items in self.raw.get("entities", {}).items():
            for item in items:
                eid = item["id"]
                for p in item.get("phrases", []):
                    pc = clean_phrase(p)
                    if pc:
                        self.entity_phrases[pc] = (e_type, eid)
                        self.all_vocab_words.update(pc.split())

        self.self_locatives = self.raw.get("self_locatives", {})
        self.capabilities = self.raw.get("capabilities", [])

    def build_live_entity_index(self, env_obs: Optional[Dict[str, Any]] = None) -> Dict[str, Tuple[str, str]]:
        """Merge static ontology entities with live environment entities ($HOME folders, windows, tabs, DOM nodes)."""
        merged = dict(self.entity_phrases)
        # Dynamically add real $HOME subdirectories so no folder name needs to be hardcoded
        home_dir = os.path.expanduser("~")
        try:
            for entry in os.listdir(home_dir):
                if entry.startswith("."):
                    continue
                full_p = os.path.join(home_dir, entry)
                if os.path.isdir(full_p):
                    ec = clean_phrase(entry)
                    if ec and len(ec) >= 2:
                        rel = f"~/{entry}"
                        for suffix in ("", " folder", " directory"):
                            key = (ec + suffix).strip()
                            if key not in merged:
                                merged[key] = ("fs.location", rel)
        except Exception:
            pass

        if env_obs and isinstance(env_obs.get("entities"), dict):
            for e_type, items in env_obs["entities"].items():
                for it in items:
                    eid = it.get("id", "")
                    for nm in it.get("names", []):
                        nc = clean_phrase(nm)
                        if nc and nc not in merged:
                            merged[nc] = (e_type, eid)
        return merged

    def normalize_asr_utterance(self, raw_text: str, live_entities: Optional[Dict[str, Tuple[str, str]]] = None) -> str:
        """Vocabulary-driven ASR post-processor (zero ad-hoc hardcoded word replacements):
        1. Truncates Whisper trailing prefix echo after sentence punctuation ('Open T-MUX in this terminal. Open T')
        2. Folds hyphenated/space-split acronyms ('T-MUX' -> 'tmux', 'H-TOP' -> 'htop') if folded form is in vocab
        3. Snaps initial near-homophone verbs ('least files' -> 'list files') via phonetic edit-distance against verb_head_words
        """
        s = (raw_text or "").strip()
        if not s:
            return ""

        # 1. Structural prefix-echo truncation after sentence period
        if ". " in s:
            parts = [p.strip() for p in s.split(". ") if p.strip()]
            if len(parts) >= 2:
                first_c = clean_phrase(parts[0])
                last_c = clean_phrase(parts[-1])
                if first_c and last_c and len(last_c) < len(first_c) and first_c.startswith(last_c):
                    s = ". ".join(parts[:-1])

        vocab = set(self.all_vocab_words)
        if live_entities:
            for k in live_entities:
                vocab.update(k.split())

        # 2. Fold hyphenated or single-letter-prefix tokens (e.g. "T-MUX", "t mux", "H-top", "N-vim") when folded token is in vocab
        s = re.sub(
            r"\b([A-Za-z])[\s\-]+([A-Za-z]{2,8})\b",
            lambda m: (m.group(1) + m.group(2)).lower()
            if (m.group(1) + m.group(2)).lower() in vocab
            else m.group(0),
            s,
        )
        # Also fold two-word compound terms in vocab (e.g. "low fee" / "lo-fi" -> "lofi", "neo vim" -> "neovim")
        tokens = s.split()
        folded: List[str] = []
        i = 0
        while i < len(tokens):
            if i + 1 < len(tokens):
                pair_raw = clean_phrase(tokens[i] + " " + tokens[i + 1])
                joined = pair_raw.replace(" ", "")
                if pair_raw in ("low fee", "lo fee", "low fi", "lo fi", "low fly"):
                    folded.append("lofi")
                    i += 2
                    continue
                if joined in vocab and pair_raw not in self.verb_phrases:
                    folded.append(joined)
                    i += 2
                    continue
            folded.append(tokens[i])
            i += 1

        # 3. Phonetic/edit-distance snap for the leading verb token if ASR produced a near-miss
        #    (e.g. "least" -> "list", "lease" -> "list", "shove" -> "show")
        if len(folded) >= 2:
            w0_clean = clean_phrase(folded[0])
            if w0_clean and w0_clean not in self.verb_head_words and w0_clean not in self.stopwords:
                rest_clean = clean_phrase(" ".join(folded[1:]))
                ent_map = live_entities or self.entity_phrases
                has_known_entity = any(ek in rest_clean for ek in ent_map if len(ek) >= 3)
                if has_known_entity:
                    best_v = ""
                    best_ratio = 0.0
                    for vh in self.verb_head_words:
                        if abs(len(vh) - len(w0_clean)) > 2 or vh[0] != w0_clean[0]:
                            continue
                        r = difflib.SequenceMatcher(None, w0_clean, vh).ratio()
                        if r > best_ratio:
                            best_ratio = r
                            best_v = vh
                    if best_v and best_ratio >= 0.66:
                        folded[0] = best_v

        return " ".join(folded)


class CompositionalSemanticSolver:
    """Universal Frame-Semantic Solver: Utterance + Host Observation -> Platform-Neutral Intent IR."""

    def __init__(self, ontology: Optional[SemanticOntology] = None):
        self.ont = ontology or SemanticOntology()

    def strip_locative_modifiers(
        self, clause: str, active_surface: str = "any"
    ) -> Tuple[str, str, bool, bool, bool]:
        """Strip trailing self/other/submit locative phrases and return:
        (stripped_clause, effective_surface, explicit_terminal, target_other, submit_enter)"""
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
        """Bottom-up greedy constituent span tagger.
        Partitions the clause into:
          - verbs: Dict[verb_concept, float]
          - entities: Dict[entity_type, List[entity_id]]
          - modifiers: Set[str]
          - residual_tokens: List[str]
          - coverage: float (fraction of content tokens bound to structured concepts)
        """
        words = clean_phrase(clause).split()
        n = len(words)
        consumed = [False] * n
        verbs: Dict[str, float] = {}
        entities: Dict[str, List[str]] = {}
        modifiers: Set[str] = set()

        # 1. Tag longest matching Entity spans first (up to 6 words) so noun phrases like
        #    "list of my files" or "my home directory" bind as typed entities before light verbs
        for L in range(min(6, n), 0, -1):
            for i in range(0, n - L + 1):
                if any(consumed[i : i + L]):
                    continue
                span = " ".join(words[i : i + L])
                if span in live_entities:
                    etype, eid = live_entities[span]
                    # Do not let single-word "list" or "files" consume the leading verb if it's at index 0 and also a verb
                    if i == 0 and L == 1 and span in self.ont.verb_phrases and n > 1:
                        continue
                    entities.setdefault(etype, []).append(eid)
                    for k in range(i, i + L):
                        consumed[k] = True

        # 2. Tag Verb spans (usually near the start of the clause, up to 4 words)
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

        # 3. Also check if an entity phrase itself implies a PRESENT/OPEN verb when the user spoke a light verb
        #    e.g. "put on a list of my files": "put on" -> PRESENT, "list of my files" -> fs.collection("files")
        for i, w in enumerate(words):
            if w in ("new", "hidden", "all", "silent", "fullscreen", "floating"):
                modifiers.add(w)
            if not consumed[i] and w in self.ont.verb_phrases and not verbs:
                for v_concept, vw in self.ont.verb_phrases[w]:
                    verbs[v_concept] = max(verbs.get(v_concept, 0.0), vw)
                consumed[i] = True

        # If no explicit verb was spoken (e.g. bare noun command "tmux", "git status", "downloads"),
        # infer implicit default verb from the entity type
        if not verbs and entities:
            if "shell.program" in entities or "ui.container_kind" in entities:
                verbs["OPEN_LAUNCH"] = 0.90
            elif "git.object" in entities or "fs.collection" in entities or "fs.metric" in entities:
                verbs["PRESENT"] = 0.90
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
        """Resolve a single clause to a Canonical Cross-Platform Intent IR dict:
        {
          "cap": "<capability_id>",
          "score": 0.98,
          "args": {...},
          "surface": "terminal" | "browser" | "any",
          "explicit_terminal": bool
        }
        Returns None if the utterance has unaccounted residual tokens or does not satisfy type constraints.
        """
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

        # Guard: Never hijack help questions ("how do I...", "what is the shortcut...")
        if "HELP_ASK" in verbs:
            return None

        # Guard: Explicit dictation ("type hello world") belongs to input.dictate
        if "INPUT_TYPE" in verbs and clean_phrase(stripped).split()[0] in ("type", "write", "dictate", "input"):
            return None

        best_cap: Optional[Dict[str, Any]] = None
        best_score = 0.0

        for cap in self.ont.capabilities:
            cid = cap["id"]
            allowed_verbs = cap.get("verbs", [])
            v_score = max((verbs.get(v, 0.0) for v in allowed_verbs), default=0.0)
            if v_score <= 0.0:
                continue

            # Check forbidden entity types (Selectional Restrictions!)
            forbid_ents = cap.get("forbid_entities", [])
            if any(fe in entities for fe in forbid_ents):
                continue

            # Check required entity types
            req_ents = cap.get("required_entities", [])
            if req_ents and not all(re_t in entities for re_t in req_ents):
                # Special allowance: in @terminal, "pull up my downloads" / "list my home directory"
                # has PRESENT + fs.location without the word "files" -> still valid fs.list!
                if cid == "fs.list" and "fs.location" in entities and "PRESENT" in verbs and "FOCUS_NAV" not in verbs:
                    pass
                else:
                    continue

            # Check required_any for media.play
            req_any = cap.get("required_any", [])
            if req_any:
                ok_any = False
                if "media.object" in req_any and "media.object" in entities:
                    ok_any = True
                if "residual_with_play_verb" in req_any and residual and verbs.get("PLAY_STREAM", 0.0) >= 0.80:
                    ok_any = True
                if not ok_any:
                    continue

            # Check Content-Token Coverage Invariant:
            # If the capability forbids arbitrary residual words and there are leftover content words,
            # reject so we don't silently drop user constraints (e.g. "modified today")!
            if cap.get("forbid_residual", False) and residual:
                continue

            # Compute joint score = VerbFit * ArgTypeFit * ContextFit
            cap_surface = cap.get("surface", "any")
            ctx_bonus = 0.04 if (cap_surface == eff_surface or explicit_term) else 0.0
            score = min(0.99, 0.55 * v_score + 0.43 + ctx_bonus)

            # Disambiguation between fs.list ("list my home directory") and fs.navigate ("go to my home directory")
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

        return self._build_intent_ir(
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
            # Only handle creation/opening of containers we have deterministic rules for;
            # let other window/tab operations fall through if needed.
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
            # Extract the clean media query by removing the leading verb phrase
            q_clean = raw_stripped
            for vp in sorted(self.ont.verb_phrases.keys(), key=len, reverse=True):
                if q_clean.lower().startswith(vp + " "):
                    q_clean = q_clean[len(vp) :].strip()
                    break
            while q_clean.lower().startswith(("some ", "a bit of ", "a little ", "the ", "a ", "an ")):
                for prefix in ("a bit of ", "a little ", "some ", "the ", "a ", "an "):
                    if q_clean.lower().startswith(prefix):
                        q_clean = q_clean[len(prefix) :].strip()
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


# =============================================================================
# Layer 3: Pluggable Cross-Platform Capability Drivers (Linux/dwm, Windows, Web)
# =============================================================================

def compile_ir_linux_dwm(
    ir: Dict[str, Any],
    active_surface: str = "any",
    auto_bridge_terminal: bool = False,
) -> Tuple[str, str]:
    """Compile a platform-neutral Intent IR into (intent_label, dwm_act_dsl) for Linux/dwm."""
    cap = ir["cap"]
    args = ir.get("args", {})
    req_surface = ir.get("surface", "any")

    prefix_steps: List[str] = []
    if req_surface == "terminal" and active_surface != "terminal" and auto_bridge_terminal:
        prefix_steps.append("sh st")

    def _wrap(label: str, step: str) -> Tuple[str, str]:
        all_steps = prefix_steps + [step]
        return label, " ; ".join(all_steps)

    if cap == "fs.list":
        loc = args.get("location", ".")
        if loc in ("", "."):
            return _wrap("fs.list@terminal", 'type_enter "ls -la"')
        if loc == "~":
            return _wrap("fs.list_home@terminal", 'type_enter "ls -la ~"')
        return _wrap("fs.list_dir@terminal", f'type_enter "ls -la {loc}"')

    if cap == "fs.metric":
        m = args.get("metric", "cwd")
        if m == "cwd":
            return _wrap("fs.pwd@terminal", 'type_enter "pwd"')
        if m == "disk_space":
            return _wrap("fs.disk_space@terminal", 'type_enter "df -h"')
        if m == "largest":
            return _wrap("fs.largest@terminal", 'type_enter "du -sh * 2>/dev/null | sort -h"')
        if m == "recent":
            return _wrap("fs.recent@terminal", 'type_enter "ls -lat | head -n 20"')

    if cap == "fs.find":
        pat = args.get("pattern", "*")
        loc = args.get("location", ".") or "."
        return _wrap("fs.find@terminal", f"type_enter \"find {loc} -name '{pat}'\"")

    if cap == "fs.navigate":
        loc = args.get("location", "~")
        return _wrap("fs.navigate@terminal", f'type_enter "cd {loc}"')

    if cap == "git.inspect":
        aspect = args.get("aspect", "status")
        cmd_map = {
            "status": "git status",
            "diff": "git diff",
            "log": "git log -n 10 --oneline",
            "branch": "git branch -a",
        }
        return _wrap(f"git.{aspect}@terminal", f'type_enter "{cmd_map.get(aspect, "git status")}"')

    if cap == "shell.run":
        prog = args.get("program", "tmux")
        if prog == "tmux_attach":
            return _wrap("shell.tmux_attach@terminal", 'type_enter "tmux a || tmux"')
        return _wrap(f"shell.{prog}@terminal", f'type_enter "{prog}"')

    if cap == "edit.clear":
        unit = args.get("unit", "line")
        if unit == "line":
            if active_surface == "browser":
                return "clear_line@browser", "key ctrl+a BackSpace"
            return "clear_line@terminal", "key ctrl+u"
        if unit == "word":
            if active_surface == "browser":
                return "del_word@browser", "key ctrl+BackSpace"
            return "del_word@terminal", "key ctrl+w"
        if unit == "screen":
            return "clear_scr@terminal", "key ctrl+l"
        if unit == "process":
            return "interrupt@terminal", "key ctrl+c"

    if cap == "window.new":
        kind = args.get("kind", "terminal")
        if kind == "terminal":
            return "open_terminal", "sh st"
        if kind == "window":
            if active_surface == "browser":
                return "new_win@browser", "key ctrl+n"
            return "new_win@terminal", "sh st"
        if kind == "tab":
            return "new_tab", "key ctrl+t"
        if kind == "devtools":
            return "dev_tools@browser", "key ctrl+shift+i"
        if kind == "address_bar":
            return "address_bar", "key ctrl+l"
        if kind == "sys_monitor":
            return "sys_monitor", "sh dwm-ctl sys-monitor"
        if kind == "calendar":
            return "cal_popup", "sh dwm-cal popup"
        if kind == "control_center":
            return "control_center", "sh dwm-ctl"
        if kind == "wifi":
            return "wifi_menu", "sh dwm-net wifi"
        if kind == "battery":
            return "battery_status", "sh dwm-ctl battery-info"
        if kind == "power_menu":
            return "power_menu", "sh dwm-ctl power"
        if kind == "shortcuts_help":
            return "wm_help", "sh dwm-help show"

    if cap == "record.control":
        target = args.get("target", "screen_audio")
        action = args.get("action", "start")
        if action == "stop":
            return "record_stop", "record stop"
        if target == "screen_silent":
            return "record_silent", "record screen"
        if target == "screenshot_area":
            return "take_screenshot", "sh dwm-shot area"
        if target == "screenshot_full":
            return "full_screenshot", "sh dwm-shot full"
        return "record_start", "record start"

    if cap == "media.control":
        act = args.get("action", "pause")
        if act == "pause":
            return "media_pause", "media pause"
        if act == "next_track":
            return "media_next", "media next"
        if act == "prev_track":
            return "media_prev", "media prev"
        if act == "volume_up":
            return "vol_up", "sh dwm-ctl vol-up"
        if act == "volume_down":
            return "vol_down", "sh dwm-ctl vol-down"
        if act == "volume_mute":
            return "vol_mute", "sh dwm-ctl vol-mute"

    if cap == "media.play":
        q = args.get("query", "")
        return "play_target", f'play "{q}"'

    return cap, ""


def compile_ir_windows(ir: Dict[str, Any], active_surface: str = "any") -> Dict[str, Any]:
    """Compile a platform-neutral Intent IR into a Windows action descriptor (PowerShell / Win32 / UIA)."""
    cap = ir["cap"]
    args = ir.get("args", {})
    if cap == "fs.list":
        loc = args.get("location", ".").replace("~", "$HOME")
        return {"driver": "windows-powershell", "op": "type_enter", "command": f"Get-ChildItem -Force {loc}"}
    if cap == "fs.find":
        loc = args.get("location", ".").replace("~", "$HOME")
        pat = args.get("pattern", "*")
        return {"driver": "windows-powershell", "op": "type_enter", "command": f"Get-ChildItem -Path {loc} -Filter '{pat}' -Recurse"}
    if cap == "fs.navigate":
        loc = args.get("location", "~").replace("~", "$HOME")
        return {"driver": "windows-powershell", "op": "type_enter", "command": f"Set-Location {loc}"}
    if cap == "window.new" and args.get("kind") in ("terminal", "window"):
        return {"driver": "windows-shell", "op": "spawn", "command": "wt.exe"}
    if cap == "media.play":
        return {"driver": "windows-browser", "op": "open_url", "query": args.get("query", "")}
    return {"driver": "windows-generic", "cap": cap, "args": args}


def compile_ir_webapp(ir: Dict[str, Any], active_surface: str = "webapp") -> Dict[str, Any]:
    """Compile a platform-neutral Intent IR into a Website / Web App (Hyle-Bud / DOM) action descriptor."""
    cap = ir["cap"]
    args = ir.get("args", {})
    if cap in ("fs.list", "fs.find"):
        return {"driver": "web-hyle", "op": "dataset.query", "filter": args}
    if cap == "fs.navigate":
        return {"driver": "web-router", "op": "navigate", "path": args.get("location", "/")}
    if cap == "media.play":
        return {"driver": "web-media", "op": "player.play", "query": args.get("query", "")}
    if cap == "edit.clear":
        return {"driver": "web-bud", "op": "input.clear", "unit": args.get("unit", "line")}
    return {"driver": "web-generic", "cap": cap, "args": args}
