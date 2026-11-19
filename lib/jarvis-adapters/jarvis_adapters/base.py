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

    def confirm_action(self, description: str) -> bool:
        """Confirm a potentially destructive action (default True in base/mock)."""
        return True

    def inspect_terminal(self, target_hint: str = "focused", lines: int = 60) -> Dict[str, Any]:
        """Inspect terminal pane/scrollback and cwd."""
        obs = self.observe()
        return {
            "window": obs.get("focused", {}),
            "cwd": obs.get("focused_cwd", "~"),
            "tmux_panes": obs.get("tmux_panes", []),
        }

    def inspect_clipboard(self) -> Dict[str, str]:
        """Inspect X11/Wayland primary selection and clipboard."""
        obs = self.observe()
        return obs.get("clipboard", {"primary_selection": "", "clipboard": ""})

    def inspect_system(self, aspect: str = "all") -> Dict[str, Any]:
        """Inspect system health (CPU, RAM, processes, disk, ports)."""
        obs = self.observe()
        return obs.get("system_health", {"aspect": aspect})

    def run_background_command(self, command: str, cwd: Optional[str] = None, timeout: float = 5.0) -> Dict[str, Any]:
        """Execute a non-interactive probe command and return stdout/stderr."""
        return {"command": command, "cwd": cwd or "~", "returncode": 0, "stdout": "", "stderr": ""}