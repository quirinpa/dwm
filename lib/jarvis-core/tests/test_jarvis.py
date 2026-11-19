#!/usr/bin/env python3
"""Comprehensive Test Suite for Jarvis v2 (Compositional Solver, Embeddings, Auren LLM, Multi-Clause & Random Unstructured Commands)."""

import json
import os
import sys
import tempfile
import unittest
from typing import Any, Dict, List, Optional

sys.path.insert(0, "/home/quirinpa/.local/lib/jarvis-core")
sys.path.insert(0, "/home/quirinpa/.local/lib/jarvis-adapters")

from jarvis_core.ontology import SemanticOntology
from jarvis_core.solver import CompositionalSemanticSolver, build_ir
from jarvis_core.orchestrator import Jarvis
from jarvis_core.agent import is_destructive_command
from jarvis_adapters.base import Adapter, AdapterResult
from jarvis_adapters.inference import EmbeddingBackend, AurenLLMBackend, normalize_dsl_dict, _extract_json_payload


class MockAdapter(Adapter):
    """Side-effect-free mock WM adapter that records executed DSL operations."""

    def __init__(self) -> None:
        self.executed: List[Dict[str, Any]] = []
        self.mock_obs: Dict[str, Any] = {
            "focused": {"id": "0x1001", "class": "st-256color", "title": "quirinpa@host:~/site"},
            "focused_cwd": "/home/quirinpa/site",
            "other": {"id": "0x2002", "class": "firefox", "title": "GitHub - Mozilla Firefox"},
            "clients": [
                {"id": "0x1001", "class": "st-256color", "title": "tmux: main", "tag": 1, "visible": True},
                {"id": "0x2002", "class": "firefox", "title": "GitHub - Mozilla Firefox", "tag": 1, "visible": True},
                {"id": "0x3003", "class": "discord", "title": "Discord", "tag": 2, "visible": False},
            ],
            "tabs": [
                {"index": 1, "title": "Duna Cowork Manager", "url": "https://duna.example.com"},
                {"index": 2, "title": "YouTube", "url": "https://www.youtube.com"},
            ],
            "audio": {"playing": False, "sink_inputs": 0, "mpris_players": []},
            "desktop_apps": {
                "calculator": "gnome-calculator",
                "firefox web browser": "firefox",
                "pulseaudio volume control": "pavucontrol",
                "visual studio code": "code",
                "files": "thunar",
            },
            "user_configs": [
                "~/.vimrc",
                "~/.config/nvim/init.lua",
                "~/.bashrc",
                "~/.tmux.conf",
                "~/.Xresources",
                "~/.local/src/dwm/config.h",
            ],
            "wm": "dwm",
        }

    @property
    def name(self) -> str:
        return "mock-dwm"

    @property
    def supported_ops(self) -> List[str]:
        return [
            "focus_window",
            "focus_relative_window",
            "open_terminal",
            "open_application",
            "open_url",
            "search_web",
            "spawn_shell_command",
            "type_text",
            "type_and_enter",
            "press_key",
            "click",
            "scroll",
            "workspace_switch",
            "workspace_move_window",
            "window_fullscreen",
            "window_float",
            "window_close",
            "media_play",
            "media_pause",
            "media_next",
            "media_prev",
            "media_volume",
            "record_start",
            "record_stop",
            "answer_question",
            "wm_help",
            "ask_agent",
            "wait",
            "inspect_terminal",
            "inspect_clipboard",
            "inspect_system",
            "run_background_command",
            "remember_fact",
            "recall_facts",
        ]

    def confirm_action(self, description: str) -> bool:
        return getattr(self, "allow_confirm", True)

    def execute(self, dsl: Dict[str, Any]) -> AdapterResult:
        self.executed.append(dsl)
        op = dsl.get("op", "")
        if op not in self.supported_ops:
            return AdapterResult(False, error=f"Unsupported op: {op}")
        if op == "inspect_terminal":
            data = {
                "window": self.mock_obs["focused"],
                "cwd": self.mock_obs["focused_cwd"],
                "tmux_panes": [
                    {
                        "pane": "main:0.0",
                        "command": "make",
                        "cwd": "/home/quirinpa/site",
                        "active": True,
                        "tail": "cc -Wall -Werror -c src/audio.c\nsrc/audio.c:88:12: error: expected ';' after expression 'pitch_init()'\nmake: *** [Makefile:42: src/audio.o] Error 1",
                    }
                ],
            }
            return AdapterResult(True, output=json.dumps(data), metadata=data)
        if op == "inspect_clipboard":
            data = {
                "primary_selection": "https://news.ycombinator.com",
                "clipboard": "https://news.ycombinator.com",
            }
            return AdapterResult(True, output=json.dumps(data), metadata=data)
        if op == "inspect_system":
            data = {
                "memory": "Mem: 32Gi total, 28Gi used, 4.0Gi free",
                "top_mem": ["PID COMMAND %CPU %MEM", "4812 blender 92.1 64.5", "2010 firefox 8.2 12.1"],
            }
            return AdapterResult(True, output=json.dumps(data), metadata=data)
        if op == "run_background_command":
            cmd = dsl.get("target", {}).get("command", "")
            return AdapterResult(True, output=f"probe-output({cmd}): ok")
        if op == "answer_question":
            return AdapterResult(True, output=dsl.get("args", {}).get("answer", "answered"))
        return AdapterResult(True, output=f"mock-executed:{op}")

    def observe(self) -> Dict[str, Any]:
        return dict(self.mock_obs)


class TestJarvisPipeline(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.ontology = SemanticOntology("/home/quirinpa/.config/jarvis/ontology.json")
        cls.solver = CompositionalSemanticSolver(cls.ontology)

    def _make_jarvis(self, use_llm: bool = True) -> Tuple[Jarvis, MockAdapter]:
        adapter = MockAdapter()
        tmp_few = tempfile.NamedTemporaryFile(suffix=".json", delete=False)
        tmp_few.write(b"[]")
        tmp_few.close()
        tmp_ctx = tempfile.NamedTemporaryFile(suffix=".json", delete=False)
        tmp_ctx.close()
        os.unlink(tmp_ctx.name)
        tmp_mem = tempfile.NamedTemporaryFile(suffix=".json", delete=False)
        tmp_mem.write(b"{}")
        tmp_mem.close()
        tmp_sess = tempfile.NamedTemporaryFile(suffix=".json", delete=False)
        tmp_sess.write(b"[]")
        tmp_sess.close()
        backends = [EmbeddingBackend(), AurenLLMBackend()] if use_llm else []
        j = Jarvis(
            adapter=adapter,
            ontology=self.ontology,
            inference_backends=backends,
            few_shot_path=tmp_few.name,
            context_path=tmp_ctx.name,
            memory_path=tmp_mem.name,
            session_path=tmp_sess.name,
        )
        return j, adapter

    def test_01_standard_structured_commands(self) -> None:
        """All canonical desktop, shell, file, git, window, workspace, media, and dictation commands resolve deterministically."""
        j, adapter = self._make_jarvis(use_llm=False)

        cases = [
            # Terminal & GUI apps
            ("open a new terminal", "open_terminal", None),
            ("Please open a terminal", "open_terminal", None),
            ("open firefox", "open_application", lambda d: d["target"]["identifier"] == "firefox"),
            ("launch calculator", "open_application", lambda d: d["target"]["identifier"] == "gnome-calculator"),
            ("open discord", "open_application", lambda d: d["target"]["identifier"] == "discord"),
            ("open volume control", "open_application", lambda d: d["target"]["identifier"] == "pavucontrol"),
            ("show me my agenda for today", "open_application", lambda d: d["target"]["identifier"] == "dwm-cal popup"),
            ("open system monitor", "open_application", lambda d: d["target"]["identifier"] == "dwm-ctl sys-monitor"),
            ("open wifi settings", "open_application", lambda d: d["target"]["identifier"] == "dwm-net wifi"),
            ("open power menu", "open_application", lambda d: d["target"]["identifier"] == "dwm-ctl power-menu"),
            # Shell tools & configs
            ("run htop", "spawn_shell_command", lambda d: d["target"]["command"] == "htop"),
            ("open btop", "spawn_shell_command", lambda d: d["target"]["command"] == "btop"),
            ("open lazygit", "spawn_shell_command", lambda d: d["target"]["command"] == "lazygit"),
            ("Can you open the bit calculator on this terminal", "spawn_shell_command", lambda d: d["target"]["command"] == "bitwise"),
            ("open my vim configuration", "spawn_shell_command", lambda d: d["target"]["command"] == "nvim ~/.vimrc"),
            ("open my neovim config", "spawn_shell_command", lambda d: d["target"]["command"] == "nvim ~/.config/nvim/init.lua"),
            ("open the Makefile", "spawn_shell_command", lambda d: d["target"]["command"] == "nvim Makefile"),
            ("open dwm config", "spawn_shell_command", lambda d: d["target"]["command"] == "nvim ~/.local/src/dwm/config.h"),
            # Filesystem & metrics
            ("list files", "spawn_shell_command", lambda d: d["target"]["command"] == "ls -la"),
            ("list my files in the other window", "spawn_shell_command", lambda d: d["target"]["command"] == "ls -la" and d["target"].get("relation") == "other"),
            ("list files in home", "spawn_shell_command", lambda d: d["target"]["command"] == "ls -la ~"),
            ("go to site folder", "spawn_shell_command", lambda d: d["target"]["command"] == "cd ~/site"),
            ("navigate to downloads directory", "spawn_shell_command", lambda d: d["target"]["command"] == "cd ~/Downloads"),
            ("go to parent directory", "spawn_shell_command", lambda d: d["target"]["command"] == "cd .."),
            ("find all python files", "spawn_shell_command", lambda d: d["target"]["command"] == "find . -name '*.py'"),
            ("find c files in site", "spawn_shell_command", lambda d: d["target"]["command"] == "find ~/site -name '*.c'"),
            ("show disk space", "spawn_shell_command", lambda d: d["target"]["command"] == "df -h"),
            ("show largest files", "spawn_shell_command", lambda d: "du -sh" in d["target"]["command"]),
            # Git
            ("show git status", "spawn_shell_command", lambda d: d["target"]["command"] == "git status"),
            ("check git diff", "spawn_shell_command", lambda d: d["target"]["command"] == "git diff"),
            ("show recent commits", "spawn_shell_command", lambda d: "git log" in d["target"]["command"]),
            ("show git branches", "spawn_shell_command", lambda d: "git branch" in d["target"]["command"]),
            # Workspaces & windows
            ("switch to workspace 2", "workspace_switch", lambda d: d["args"]["workspace"] == 2),
            ("go to workspace 5", "workspace_switch", lambda d: d["args"]["workspace"] == 5),
            ("switch to the third workspace", "workspace_switch", lambda d: d["args"]["workspace"] == 3),
            ("move this window to workspace 4", "workspace_move_window", lambda d: d["args"]["workspace"] == 4),
            ("send the other window to workspace 7", "workspace_move_window", lambda d: d["args"]["workspace"] == 7 and d["target"].get("relation") == "other"),
            ("go to the other window", "focus_relative_window", lambda d: d["target"]["relation"] == "other"),
            ("next window", "focus_relative_window", lambda d: d["target"]["relation"] == "next"),
            ("previous window", "focus_relative_window", lambda d: d["target"]["relation"] == "previous"),
            ("toggle fullscreen", "window_fullscreen", None),
            ("make this window full screen", "window_fullscreen", None),
            ("float this window", "window_float", None),
            ("close this window", "window_close", lambda d: d["target"].get("kind") == "window"),
            ("close this tab on Firefox", "window_close", lambda d: d["target"].get("kind") == "tab"),
            ("close the other window", "window_close", lambda d: d["target"].get("relation") == "other"),
            ("close it", "window_close", None),
            ("fullscreen it", "window_fullscreen", None),
            # Media & volume
            ("play some lofi music", "media_play", lambda d: "lofi" in d["target"]["query"]),
            ("open up some jazz music for me please", "media_play", lambda d: "jazz" in d["target"]["query"]),
            ("press play", "media_play", None),
            ("put up some lofi dragonball jazz", "media_play", lambda d: "dragonball" in d["target"]["query"]),
            ("let's listen to some miles davis", "media_play", lambda d: "miles davis" in d["target"]["query"]),
            ("pause the music", "media_pause", None),
            ("next song", "media_next", None),
            ("skip track", "media_next", None),
            ("previous track", "media_prev", None),
            ("turn up the volume", "media_volume", lambda d: d["args"]["action"] == "volume_up"),
            ("lower volume", "media_volume", lambda d: d["args"]["action"] == "volume_down"),
            ("mute audio", "media_volume", lambda d: d["args"]["action"] == "volume_mute"),
            # Web & URLs
            ("open whatsapp for web", "open_url", lambda d: "whatsapp" in d["target"]["url"]),
            ("open github.com/torvalds", "open_url", lambda d: "github.com/torvalds" in d["target"]["url"]),
            ("search for python asyncio", "search_web", lambda d: d["target"]["query"] == "python asyncio"),
            ("google rust borrow checker", "search_web", lambda d: d["target"]["query"] == "rust borrow checker"),
            # Dictation & editing
            ("type hello world and press enter", "type_and_enter", lambda d: d["args"]["text"] == "hello world"),
            ("type in list files", "type_text", lambda d: d["args"]["text"] == "list files"),
            ("open up bashrc on a new terminal", "spawn_shell_command", lambda d: d["target"]["command"] == "nvim ~/.bashrc" and d["target"].get("new_terminal") is True),
            ("type git status in the other window", "type_text", lambda d: d["args"]["text"] == "git status" and d["target"].get("relation") == "other"),
            ("clear the line", "press_key", lambda d: d["args"]["key_combo"] == "ctrl+u"),
            ("delete last word", "press_key", lambda d: d["args"]["key_combo"] == "ctrl+w"),
            # Scroll & Recording & Agent
            ("scroll down", "scroll", lambda d: d["args"]["direction"] == "down"),
            ("scroll up 8", "scroll", lambda d: d["args"]["direction"] == "up" and d["args"]["amount"] == 8),
            ("start recording screen and audio", "record_start", lambda d: d["args"]["mode"] == "both"),
            ("stop recording", "record_stop", None),
            ("ask pi to refactor the parser", "ask_agent", lambda d: "refactor the parser" in d["target"]["query"]),
        ]

        for utt, expected_op, check_fn in cases:
            with self.subTest(utterance=utt):
                res = j.process(utt, active_surface="terminal")
                self.assertNotIn("error", res, f"Failed to resolve '{utt}': {res}")
                dsl = res["dsl"]
                self.assertEqual(dsl["op"], expected_op, f"Utterance '{utt}' got op={dsl['op']} instead of {expected_op} (dsl={dsl})")
                if check_fn:
                    self.assertTrue(check_fn(dsl), f"Check failed for '{utt}': dsl={dsl}")

    def test_02_multi_clause_and_conversational_anaphora(self) -> None:
        """Multi-clause chaining and conversational context ('there', 'it', 'other window') work without manual hacks."""
        j, adapter = self._make_jarvis(use_llm=False)

        # 1. Compound WM chaining
        r1 = j.process("move this window to workspace 4 and switch to workspace 4")
        self.assertNotIn("error", r1)
        self.assertEqual(len(r1["steps"]), 2)
        self.assertEqual(r1["steps"][0]["dsl"]["op"], "workspace_move_window")
        self.assertEqual(r1["steps"][0]["dsl"]["args"]["workspace"], 4)
        self.assertEqual(r1["steps"][1]["dsl"]["op"], "workspace_switch")
        self.assertEqual(r1["steps"][1]["dsl"]["args"]["workspace"], 4)

        # 2. Compound navigation + spatial anaphora ("there")
        r2 = j.process("go to site folder and then find c files there")
        self.assertNotIn("error", r2)
        self.assertEqual(len(r2["steps"]), 2)
        self.assertEqual(r2["steps"][0]["dsl"]["target"]["command"], "cd ~/site")
        self.assertEqual(r2["steps"][1]["dsl"]["target"]["command"], "find ~/site -name '*.c'")

        # 3. Dictation containing 'and' is NOT split into broken clauses
        r3 = j.process("type bread and butter and press enter")
        self.assertNotIn("error", r3)
        self.assertEqual(r3["dsl"]["op"], "type_and_enter")
        self.assertEqual(r3["dsl"]["args"]["text"], "bread and butter")

    def test_03_dsl_normalizer_resilience(self) -> None:
        """normalize_dsl_dict and _extract_json_payload heal arbitrary malformed LLM outputs."""
        raw_text = "Sure! Here is the DSL:\n```json\n{\"action\": \"run_command\", \"command\": \"grep -rn 'foo' .\"}\n```\nHope that helps!"
        extracted = _extract_json_payload(raw_text)
        norm = normalize_dsl_dict(extracted)
        self.assertIsNotNone(norm)
        self.assertEqual(norm["op"], "spawn_shell_command")
        self.assertEqual(norm["target"]["command"], "grep -rn 'foo' .")

        # Workspace string healing
        norm_ws = normalize_dsl_dict({"op": "goto_workspace", "workspace": "Workspace 6"})
        self.assertEqual(norm_ws["op"], "workspace_switch")
        self.assertEqual(norm_ws["args"]["workspace"], 6)

        # Relative focus healing
        norm_rel = normalize_dsl_dict({"op": "focus_window", "identifier": "other"})
        self.assertEqual(norm_rel["op"], "focus_relative_window")
        self.assertEqual(norm_rel["target"]["relation"], "other")

    def test_04_random_unstructured_commands_end_to_end(self) -> None:
        """Random, informal, out-of-distribution commands that do NOT fit static ontology templates must all succeed via full pipeline."""
        j, adapter = self._make_jarvis(use_llm=True)

        random_commands = [
            # Random system / shell tasks that don't match static ontology templates
            ("yo jarvis could you check how much memory my machine has left right now", ("spawn_shell_command", "open_application", "inspect_system", "answer_question")),
            ("find out which process is hogging port 8080", ("spawn_shell_command", "inspect_system", "run_background_command", "answer_question")),
            ("count how many lines of c code are in the current directory", ("spawn_shell_command", "run_background_command", "answer_question")),
            ("show me the last 15 lines of the auren server log", ("spawn_shell_command", "run_background_command", "answer_question")),
            ("edit my gitconfig in neovim", ("spawn_shell_command",)),
            ("run make test-fast in the other window", ("spawn_shell_command",)),
            ("check if google.com is reachable with ping", ("spawn_shell_command", "run_background_command", "answer_question")),
            ("compress the logs folder into a tar gz archive", ("spawn_shell_command",)),
            # Random window / desktop phrasing
            ("yo toss that other window over to desktop 6", ("workspace_move_window",)),
            ("nuke the other window please", ("window_close",)),
            ("flip back to whichever window I was looking at before this one", ("focus_relative_window",)),
            ("pop open a calculator real quick", ("open_application",)),
            # Random media / browser / questions
            ("crank the sound up a notch", ("media_volume",)),
            ("shush the audio for a second", ("media_volume", "media_pause")),
            ("throw on some chill synthwave beats for coding", ("media_play",)),
            ("look up the linux kernel epoll man page on google", ("search_web", "open_url")),
            ("what directory am I currently working in?", ("answer_question", "spawn_shell_command")),
            ("what shortcut do I press to split or tile windows in dwm?", ("wm_help", "answer_question")),
            ("tell pi to inspect the memory recall latency", ("ask_agent",)),
        ]

        for utt, allowed_ops in random_commands:
            with self.subTest(utterance=utt):
                res = j.process(utt, active_surface="terminal")
                self.assertNotIn("error", res, f"Unstructured command failed: '{utt}' -> {res}")
                self.assertTrue(res["result"]["success"], f"Execution not successful for '{utt}': {res}")
                op = res["dsl"]["op"]
                self.assertIn(
                    op,
                    allowed_ops,
                    f"Command '{utt}' produced op='{op}' (expected one of {allowed_ops}), dsl={res['dsl']}",
                )
                print(f"  [PASS] {utt:<68} -> {op:<22} ({res['source']}): {json.dumps(res['dsl'].get('target') or res['dsl'].get('args'))}")

    def test_05_agentic_react_loop_and_memory(self) -> None:
        """Multi-turn ReAct observation loop (terminal diagnosis, clipboard action), persistent user memory, and conversational follow-ups."""
        j, adapter = self._make_jarvis(use_llm=True)

        # 1. Multi-step ReAct Terminal Diagnosis:
        # Jarvis must call inspect_terminal first, see src/audio.c:88:12 error, and answer/act on it.
        adapter.executed.clear()
        r_term = j.process("Jarvis, look at the compiler error in my terminal and tell me what failed")
        self.assertNotIn("error", r_term, f"Terminal diagnosis failed: {r_term}")
        executed_ops = [d["op"] for d in adapter.executed]
        self.assertIn("inspect_terminal", executed_ops, f"Expected inspect_terminal in ReAct trace, got {executed_ops}")
        self.assertTrue(len(executed_ops) >= 2, f"Expected multi-step observe->answer trace, got {executed_ops}")
        print(f"  [PASS] ReAct Terminal Diagnosis -> steps={executed_ops}, output={r_term['result']['output'][:120]}")

        # 2. Multi-step ReAct Clipboard Inspection -> Browser Action:
        adapter.executed.clear()
        r_clip = j.process("Jarvis, check what URL is in my clipboard and open it in the browser")
        self.assertNotIn("error", r_clip, f"Clipboard action failed: {r_clip}")
        clip_ops = [d["op"] for d in adapter.executed]
        self.assertIn("inspect_clipboard", clip_ops, f"Expected inspect_clipboard in trace, got {clip_ops}")
        self.assertIn("open_url", clip_ops, f"Expected open_url after inspect_clipboard, got {clip_ops}")
        url_step = [d for d in adapter.executed if d["op"] == "open_url"][-1]
        self.assertIn("ycombinator.com", url_step["target"].get("url", ""))
        print(f"  [PASS] ReAct Clipboard -> Browser -> steps={clip_ops}, url={url_step['target'].get('url')}")

        # 3. Persistent User Workflow Memory (Teach Jarvis a habit, then invoke it):
        adapter.executed.clear()
        r_mem1 = j.process(
            "Jarvis, remember my preference for 'audio engine setup': switch to workspace 3 and play some Miles Davis jazz"
        )
        self.assertNotIn("error", r_mem1)
        memories = j.memory_store.load_all()
        self.assertTrue(len(memories) >= 1, f"Expected memory to be persisted, got {memories}")

        adapter.executed.clear()
        r_mem2 = j.process("Jarvis, run my audio engine setup please")
        self.assertNotIn("error", r_mem2, f"Invoking remembered workflow failed: {r_mem2}")
        wf_ops = [d["op"] for d in adapter.executed]
        self.assertIn("workspace_switch", wf_ops, f"Expected workspace_switch in workflow execution, got {wf_ops}")
        self.assertIn("media_play", wf_ops, f"Expected media_play in workflow execution, got {wf_ops}")
        print(f"  [PASS] Learned Workflow Memory -> memories={memories}, executed={wf_ops}")

        # 4. Conversational Follow-up Across Turns ("Actually move it to workspace 6 instead"):
        adapter.executed.clear()
        r_turn1 = j.process("move this window to workspace 3")
        self.assertEqual(r_turn1["dsl"]["op"], "workspace_move_window")
        self.assertEqual(r_turn1["dsl"]["args"]["workspace"], 3)

        r_turn2 = j.process("actually move it to workspace 6 instead")
        self.assertNotIn("error", r_turn2, f"Follow-up correction failed: {r_turn2}")
        self.assertEqual(r_turn2["dsl"]["op"], "workspace_move_window")
        self.assertEqual(r_turn2["dsl"]["args"]["workspace"], 6)
        print(f"  [PASS] Conversational Follow-up -> turn1=ws3, turn2={r_turn2['dsl']}")

        # 4b. Contextual "open that file" resolution after terminal diagnosis & SSH to domain host:
        adapter.executed.clear()
        adapter.mock_obs["terminal_panes"] = [
            {
                "pane": "main:0.0",
                "command": "bash",
                "cwd": "/home/quirinpa/site",
                "active": True,
                "tail": "cc -Wall -Werror -c src/audio.c\nsrc/audio.c:88:12: error: expected ';' after expression 'pitch_init()'\nmake: *** [Makefile:42: src/audio.o] Error 1",
            }
        ]
        r_file = j.process("open that file in neovim")
        self.assertNotIn("error", r_file, f"Contextual 'open that file' failed: {r_file}")
        file_cmd = (r_file["dsl"].get("target") or {}).get("command", "")
        self.assertEqual(r_file["dsl"]["op"], "spawn_shell_command")
        self.assertIn("audio.c", file_cmd, f"Expected audio.c in command, got {r_file['dsl']}")
        print(f"  [PASS] Contextual 'open that file' -> {r_file['dsl']}")

        adapter.executed.clear()
        r_ssh = j.process("ssh to tty.pt and list the available disk space")
        self.assertNotIn("error", r_ssh, f"SSH command failed: {r_ssh}")
        ssh_ops = [d["op"] for d in adapter.executed]
        self.assertNotIn("open_url", ssh_ops, f"SSH to tty.pt must NOT be hijacked by open_url! got {adapter.executed}")
        self.assertIn("spawn_shell_command", ssh_ops)
        print(f"  [PASS] SSH to tty.pt (not hijacked by open_url) -> {adapter.executed}")

        # 5. Destructive Command Guardrail:
        self.assertTrue(is_destructive_command("rm -rf ~/important"))
        self.assertTrue(is_destructive_command("git reset --hard HEAD~3"))
        self.assertFalse(is_destructive_command("git status"))
        adapter.allow_confirm = False
        blocked_res = j.agent._execute_step({"op": "spawn_shell_command", "target": {"command": "rm -rf /tmp/test_dir"}})
        self.assertFalse(blocked_res.success)
        self.assertIn("Cancelled destructive command", blocked_res.error)
        adapter.allow_confirm = True
        print("  [PASS] Destructive Command Safety Guardrail -> blocked unconfirmed 'rm -rf'")


if __name__ == "__main__":
    unittest.main(verbosity=2)
