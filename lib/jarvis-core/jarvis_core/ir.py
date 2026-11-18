"""Intent IR: Platform-neutral capability representation."""

from dataclasses import dataclass, field
from typing import Any, Dict, List, Optional


@dataclass
class IntentIR:
    """Platform-neutral Intent Intermediate Representation."""
    cap: str
    score: float
    surface: str = "any"  # "terminal", "browser", "any"
    explicit_terminal: bool = False
    args: Dict[str, Any] = field(default_factory=dict)
    target_other: bool = False
    submit_enter: bool = False

    def to_dict(self) -> Dict[str, Any]:
        return {
            "cap": self.cap,
            "score": self.score,
            "surface": self.surface,
            "explicit_terminal": self.explicit_terminal,
            "args": self.args,
            "target_other": self.target_other,
            "submit_enter": self.submit_enter,
        }

    @classmethod
    def from_dict(cls, d: Dict[str, Any]) -> "IntentIR":
        return cls(
            cap=d["cap"],
            score=d["score"],
            surface=d.get("surface", "any"),
            explicit_terminal=d.get("explicit_terminal", False),
            args=d.get("args", {}),
            target_other=d.get("target_other", False),
            submit_enter=d.get("submit_enter", False),
        )


@dataclass
class Capability:
    """Declarative capability definition from ontology."""
    id: str
    verbs: List[str]
    required_entities: List[str] = field(default_factory=list)
    forbid_entities: List[str] = field(default_factory=list)
    required_any: List[str] = field(default_factory=list)
    forbid_residual: bool = False
    surface: str = "any"

    @classmethod
    def from_dict(cls, d: Dict[str, Any]) -> "Capability":
        return cls(
            id=d["id"],
            verbs=d.get("verbs", []),
            required_entities=d.get("required_entities", []),
            forbid_entities=d.get("forbid_entities", []),
            required_any=d.get("required_any", []),
            forbid_residual=d.get("forbid_residual", False),
            surface=d.get("surface", "any"),
        )