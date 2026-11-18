"""Base Adapter Interface: All WM adapters implement this."""

from abc import ABC, abstractmethod
from dataclasses import dataclass
from typing import Any, Dict, List, Optional


@dataclass
class AdapterResult:
    """Result of executing a DSL operation."""
    success: bool
    output: str = ""
    error: str = ""
    metadata: Dict[str, Any] = None

    def __post_init__(self):
        if self.metadata is None:
            self.metadata = {}


class AdapterError(Exception):
    """Adapter execution error."""
    pass


class Adapter(ABC):
    """Abstract base class for Window Manager adapters.

    Each adapter implements the execution of DSL operations for a specific WM.
    """

    @property
    @abstractmethod
    def name(self) -> str:
        """Unique adapter name (e.g., 'dwm', 'hyprland', 'windows')."""
        pass

    @property
    @abstractmethod
    def supported_ops(self) -> List[str]:
        """List of DSL op codes this adapter supports."""
        pass

    @abstractmethod
    def execute(self, dsl: Dict[str, Any]) -> AdapterResult:
        """Execute a single DSL operation."""
        pass

    @abstractmethod
    def observe(self) -> Dict[str, Any]:
        """Return live desktop observation (windows, tabs, audio, etc.)."""
        pass

    def can_execute(self, op: str) -> bool:
        return op in self.supported_ops

    def validate_dsl(self, dsl: Dict[str, Any]) -> bool:
        """Validate DSL against schema (optional, can be overridden)."""
        return True