"""Inference Adapter Interface: Pluggable NLU backends."""

import json
import math
import os
import urllib.request
from abc import ABC, abstractmethod
from typing import Any, Dict, List, Optional


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
    ) -> Optional[Dict[str, Any]]:
        """Classify utterance to validated DSL.

        Args:
            utterance: User's natural language input
            live_ontology: Current desktop state (windows, tabs, apps, audio)
            dsl_schema: JSON Schema of valid DSL operations
            few_shots: Example (utterance -> DSL) pairs for in-context learning

        Returns:
            Validated DSL dict or None if unable to classify confidently
        """
        pass


class EmbeddingBackend(InferenceBackend):
    """Fast bge-base embedding classifier via axil-auren /v1/embeddings."""

    def __init__(
        self,
        endpoint: str = "http://localhost:4242/v1/embeddings",
        model: str = "bge-base",
        threshold: float = 0.86,
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

        # Deduplicate few_shots by normalized utterance (latest wins)
        shot_by_norm: Dict[str, Dict[str, Any]] = {}
        for shot in few_shots:
            s_utt = _clean_phrase(shot.get("utterance", ""))
            s_dsl = shot.get("dsl")
            if s_utt and isinstance(s_dsl, dict) and s_dsl.get("op"):
                shot_by_norm[s_utt] = s_dsl

        if not shot_by_norm:
            return None

        # Exact normalized hit in few_shots (0 ms)
        if u_norm in shot_by_norm:
            dsl = json.loads(json.dumps(shot_by_norm[u_norm]))
            dsl.setdefault("target", {})
            dsl.setdefault("verify", {})
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

            # Guard against parameterized ops where the argument differs
            op = s_dsl.get("op", "")
            if op in ("search_web", "media_play", "type_text", "type_and_enter", "open_application", "open_url"):
                target_arg = (
                    s_dsl.get("target", {}).get("query")
                    or s_dsl.get("target", {}).get("identifier")
                    or s_dsl.get("target", {}).get("url")
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
            dsl = json.loads(json.dumps(best_dsl))
            dsl.setdefault("target", {})
            dsl.setdefault("verify", {})
            dsl["meta"] = {
                "inference_backend": "embedding:bge-base",
                "source_ir_score": round(best_score, 4),
            }
            return dsl

        return None


class AurenLLMBackend(InferenceBackend):
    """Remote LLM backend via axil-auren gateway (routes to cloud models: fast, smart, Gemini, Claude, GPT, etc.)."""

    def __init__(self, model: str = "fast", endpoint: str = "http://localhost:4242/v1/chat/completions"):
        self.model = model
        self.endpoint = endpoint

    @property
    def name(self) -> str:
        return f"auren-llm:{self.model}"

    def _build_prompt(self, utterance: str, live_ontology: Dict[str, Any], dsl_schema: Dict[str, Any], few_shots: List[Dict[str, Any]]) -> str:
        ops = dsl_schema.get("properties", {}).get("op", {}).get("enum", [])
        schema_desc = "\n".join(f"- {op}" for op in ops)

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
            shots_text += f"User: {shot.get('utterance', '')}\nDSL: {json.dumps(shot.get('dsl', {}), separators=(',', ':'))}\n\n"

        ontology_summary = self._summarize_ontology(live_ontology)

        return f"""You are Jarvis, a desktop action engine. Convert the user's utterance into a valid DSL operation JSON object.
Structure: {{"op": "<operation>", "target": {{...}}, "args": {{...}}, "verify": {{...}}}}

AVAILABLE OPERATIONS:
{schema_desc}

CURRENT DESKTOP STATE:
{ontology_summary}

FEW-SHOT EXAMPLES:
{shots_text}

USER: {utterance}

OUTPUT ONLY VALID JSON DSL. No markdown, no explanation."""

    def _summarize_ontology(self, obs: Dict[str, Any]) -> str:
        lines = []
        if obs.get("focused"):
            f = obs["focused"]
            lines.append(f"Focused: {f.get('class', '')} - {f.get('title', '')[:60]}")
        lines.append(f"Windows: {len(obs.get('clients', []))}")
        for c in obs.get("clients", [])[:5]:
            lines.append(f"  - {c.get('class', '')}: {c.get('title', '')[:40]}")
        if obs.get("tabs"):
            lines.append(f"Tabs: {len(obs['tabs'])}")
        audio = obs.get("audio", {})
        lines.append(f"Audio playing: {audio.get('playing', False)}")
        if audio.get("mpris_players"):
            for p in audio["mpris_players"]:
                lines.append(f"  MPRIS: {p.get('bus', '')} [{p.get('status', '')}]")
        if obs.get("context"):
            ctx = obs["context"]
            lines.append(f"Recent context: last_op={ctx.get('last_op', '')}, last_window={ctx.get('last_window_id', '')}, last_location={ctx.get('last_location', '')}")
        return "\n".join(lines)

    def classify_intent(self, utterance: str, live_ontology: Dict[str, Any], dsl_schema: Dict[str, Any], few_shots: List[Dict[str, Any]]) -> Optional[Dict[str, Any]]:
        prompt = self._build_prompt(utterance, live_ontology, dsl_schema, few_shots)

        try:
            req = urllib.request.Request(
                self.endpoint,
                data=json.dumps({
                    "model": self.model,
                    "messages": [
                        {"role": "system", "content": "You output ONLY valid JSON DSL. No markdown, no explanation."},
                        {"role": "user", "content": prompt}
                    ],
                    "temperature": 0.1,
                    "max_tokens": 512,
                }).encode("utf-8"),
                headers={"Content-Type": "application/json"},
                method="POST",
            )
            with urllib.request.urlopen(req, timeout=10.0) as resp:
                data = json.loads(resp.read().decode("utf-8"))
            content = data["choices"][0]["message"]["content"].strip()
            if "```" in content:
                content = content.split("```")[1].split("```")[0]
                if content.startswith("json"):
                    content = content[4:]
            parsed = json.loads(content.strip())
            if isinstance(parsed, dict):
                parsed.setdefault("target", {})
                parsed.setdefault("verify", {})
                return parsed
            return None
        except Exception as e:
            print(f"[AurenLLMBackend] Error: {e}")
            return None


class LLMBackend(AurenLLMBackend):
    """Alias for backwards compatibility."""
    pass
