"""Semantic Ontology: Loads declarative ontology.json + merges live host entities."""

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
                os.environ.get("JARVIS_ONTOLOGY_JSON", ""),
                os.path.expanduser("~/.config/jarvis/ontology.json"),
                os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "ontology.json"),
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
        self.verb_phrases: Dict[str, List[Tuple[str, float]]] = {}
        self.verb_head_words: Set[str] = set()
        self.entity_phrases: Dict[str, List[Tuple[str, str]]] = {}
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
        self.clause_seps = sorted(
            self.raw.get("clause_separators", ["and then", "then", "and"]), key=len, reverse=True
        )
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
                        lst = self.entity_phrases.setdefault(pc, [])
                        if (e_type, eid) not in lst:
                            lst.append((e_type, eid))
                        self.all_vocab_words.update(pc.split())

        self.self_locatives = self.raw.get("self_locatives", {})
        self.capabilities = self.raw.get("capabilities", [])

    def build_live_entity_index(
        self, env_obs: Optional[Dict[str, Any]] = None
    ) -> Dict[str, List[Tuple[str, str]]]:
        """Merge static ontology entities with live environment entities."""
        merged: Dict[str, List[Tuple[str, str]]] = {
            k: list(v) for k, v in self.entity_phrases.items()
        }

        def _add_entity(phrase_clean: str, etype: str, eid: str) -> None:
            if not phrase_clean:
                return
            lst = merged.setdefault(phrase_clean, [])
            if (etype, eid) not in lst:
                lst.append((etype, eid))

        # Dynamically add real $HOME subdirectories
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
                            _add_entity((ec + suffix).strip(), "fs.location", rel)
        except Exception:
            pass

        # Dynamically index discovered .desktop GUI apps from env_obs
        if env_obs and isinstance(env_obs.get("desktop_apps"), dict):
            for app_name, app_exec in env_obs["desktop_apps"].items():
                ac = clean_phrase(app_name)
                if ac and len(ac) >= 3 and ac not in self.verb_phrases:
                    _add_entity(ac, "gui.application", app_exec)

        # Dynamically index live windows and browser tabs
        if env_obs and isinstance(env_obs.get("entities"), dict):
            for e_type, items in env_obs["entities"].items():
                for it in items:
                    eid = it.get("id", "")
                    for nm in it.get("names", []):
                        nc = clean_phrase(nm)
                        if nc and len(nc) >= 2:
                            _add_entity(nc, e_type, eid)
                            for suffix in (" window", " tab"):
                                _add_entity((nc + suffix).strip(), e_type, eid)
        return merged

    def normalize_asr_utterance(
        self, raw_text: str, live_entities: Optional[Dict[str, List[Tuple[str, str]]]] = None
    ) -> str:
        """Vocabulary-driven ASR post-processor."""
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

        # 2. Fold hyphenated or single-letter-prefix tokens
        s = re.sub(
            r"\b([A-Za-z])[\s\-]+([A-Za-z]{2,8})\b",
            lambda m: (m.group(1) + m.group(2)).lower()
            if (m.group(1) + m.group(2)).lower() in vocab
            else m.group(0),
            s,
        )

        # Also fold two-word compound terms in vocab
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
                if joined in vocab and pair_raw not in self.verb_phrases and pair_raw not in (live_entities or {}):
                    folded.append(joined)
                    i += 2
                    continue
            folded.append(tokens[i])
            i += 1

        # 3. Phonetic/edit-distance snap for the leading verb token
        if len(folded) >= 2:
            w0_clean = clean_phrase(folded[0])
            if w0_clean and w0_clean not in self.verb_head_words and w0_clean not in self.stopwords:
                rest_clean = clean_phrase(" ".join(folded[1:]))
                ent_map = live_entities or self.entity_phrases
                has_known_entity = any(ek in rest_clean for ek in ent_map if len(ek) >= 3)
                if has_known_entity:
                    import difflib

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
