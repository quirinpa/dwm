# jarvis-core

Universal Compositional Semantic Solver & Cross-Platform Intent IR.

## Architecture

```
Utterance → Solver → Intent IR → Orchestrator → DSL → WM Adapter → Action
                ↑
         Live Ontology (windows, tabs, apps, audio)
```

## Components

- **SemanticOntology**: Loads declarative `ontology.json` + merges live host entities
- **CompositionalSemanticSolver**: Frame-semantic parser with selectional restrictions
- **Intent IR**: Platform-neutral capability representation
- **Orchestrator**: Compiles IR → DSL, coordinates inference backends, manages few-shot learning

## Configuration

- `~/.config/jarvis/ontology.json` - Declarative verbs, entities, capabilities
- `~/.config/jarvis/few_shots.json` - Learned (utterance → DSL) pairs

## Usage

```python
from jarvis_core.orchestrator import create_jarvis

jarvis = create_jarvis(wm="dwm")
result = jarvis.process("go to the other window")
```

## Extending

1. Add verbs/entities/capabilities to `ontology.json`
2. Add IR→DSL handler in `Orchestrator._compile_ir_to_dsl`
3. Add DSL operation in WM Adapter
