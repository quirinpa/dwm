#!/usr/bin/env python3
"""
bench-jarvis.py — IRL Latency, Score & Routing Benchmark for dwm-auren / dwm-act
Run with:
    python3 ~/.local/src/dwm/scripts/bench-jarvis.py
"""
import subprocess
import sys
import time

QUERIES = [
    # Category 1: Exact / paraphrased static intents in intents.conf
    ("switch to workspace 2", "Static exemplar"),
    ("turn up the volume", "Paraphrased static"),
    ("make this window full screen", "Paraphrased static"),
    ("open a new browser tab", "Paraphrased static"),
    ("mudar para o ecrã 3", "PT static"),
    ("aumentar o som", "PT static"),
    ("show me my agenda for today", "Paraphrased static"),

    # Category 2: Parameterized / target-specific intents
    ("open whatsapp for web", "Target / Web app"),
    ("put up some lofi dragonball jazz", "Open-ended media"),
    ("switch to the tmux terminal", "Window title"),
    ("go to the duna cowork manager tab", "Browser tab"),
    ("click on Mariana", "UI element"),
    ("type hello how are you and press enter", "Dictation + Enter"),

    # Category 3: Compound multi-step requests
    ("move this window to workspace 4 and switch to workspace 4", "Multi-step WM"),
    ("open youtube and search for dragonball lofi jazz", "Multi-step Web"),
]


def parse_dry_run(out: str):
    s_idx = out.find("score=")
    i_idx = out.find("intent=")
    c_idx = out.find("cmd=")
    status = out[7:s_idx].strip() if out.startswith("status=") and s_idx != -1 else "?"
    score = out[s_idx + 6 : i_idx].strip() if s_idx != -1 and i_idx != -1 else "?"
    intent = out[i_idx + 7 : c_idx].strip() if i_idx != -1 and c_idx != -1 else "?"
    cmd = out[c_idx + 4 :].strip() if c_idx != -1 else out
    return status, score, intent, cmd


def main():
    filter_q = sys.argv[1:]
    cases = [
        (q, cat)
        for q, cat in QUERIES
        if not filter_q or any(f.lower() in q.lower() or f.lower() in cat.lower() for f in filter_q)
    ]

    try:
        ctx = subprocess.check_output(["dwm-act", "context"], text=True, timeout=2.0).strip()
        print("=== Live Desktop Context ===")
        print(ctx)
        print("=" * 128)
    except Exception:
        pass

    print(f"{'CATEGORY':<18} | {'QUERY':<48} | {'MS':>6} | {'SCORE':>5} | {'INTENT':<14} | {'CMD'}")
    print("-" * 128)

    total_ms = 0.0
    for q, cat in cases:
        t0 = time.perf_counter()
        out = subprocess.check_output(["dwm-auren", "dry-run", q], text=True).strip()
        dt = (time.perf_counter() - t0) * 1000.0
        total_ms += dt
        _, score, intent, cmd = parse_dry_run(out)
        extra = ""
        for pfx in ('open "', 'focus "'):
            if cmd.startswith(pfx) and cmd.endswith('"'):
                targ = cmd[len(pfx) : -1]
                try:
                    r_json = subprocess.check_output(["dwm-act", "resolve", targ], text=True, timeout=2.0).strip()
                    import json as _json
                    rj = _json.loads(r_json) if r_json else {}
                    if rj.get("label"):
                        extra = f" -> {rj['label']} ({rj.get('score', 0):.2f})"
                except Exception:
                    pass
        print(f"{cat[:18]:<18} | {q[:48]:<48} | {dt:6.0f} | {score:>5} | {intent[:14]:<14} | {cmd}{extra}")

    print("-" * 128)
    if cases:
        print(f"Total: {total_ms:.0f} ms across {len(cases)} queries (avg {total_ms / len(cases):.0f} ms/query)")


if __name__ == "__main__":
    main()
