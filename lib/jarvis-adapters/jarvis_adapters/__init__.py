"""Jarvis WM Adapters: Pluggable execution backends for different window managers."""

from .base import Adapter, AdapterResult, AdapterError
from .dwm import DwmAdapter
from .hyprland import HyprlandAdapter
from .sway import SwayAdapter

__all__ = [
    "Adapter",
    "AdapterResult",
    "AdapterError",
    "DwmAdapter",
    "HyprlandAdapter",
    "SwayAdapter",
]
