"""jarvis-core: Universal Compositional Semantic Solver & Cross-Platform Intent IR."""

from .ontology import SemanticOntology
from .solver import CompositionalSemanticSolver, build_ir
from .ir import IntentIR, Capability

__all__ = [
    "SemanticOntology",
    "CompositionalSemanticSolver",
    "IntentIR",
    "Capability",
    "build_ir",
]

__version__ = "0.1.0"