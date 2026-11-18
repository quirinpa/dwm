#!/usr/bin/env python3
"""Unit and integration test suite for the 4-layer reactive dwm-act + dwm-auren engine."""

import importlib.machinery
import importlib.util
import json
import os
import subprocess
import sys
import time
import unittest
from unittest.mock import patch

SCRIPT_PATH = "/home/quirinpa/.local/src/dwm/scripts/dwm-act"
AUREN_PATH = "/home/quirinpa/.local/src/dwm/scripts/dwm-auren"

loader = importlib.machinery.SourceFileLoader("dwm_act", SCRIPT_PATH)
spec = importlib.util.spec_from_loader("dwm_act", loader)
dwm_act = importlib.util.module_from_spec(spec)
loader.exec_module(dwm_act)


class TestLayer1Sensors(unittest.TestCase):
    def test_get_audio_state_parsing(self):
        # Case 1: Active uncorked stream + Playing MPRIS
        def fake_run_playing(args, timeout=4.0, input_bytes=None):
            if args[:2] == ["pactl", "list"]:
                return "Sink Input #42\n\tCorked: no\n\tMedia: Lofi\n"
            if "ListNames" in args[-1]:
                return "(['org.freedesktop.DBus', 'org.mpris.MediaPlayer2.firefox.instance_1'],)"
            if "PlaybackStatus" in args:
                return "(<'Playing'>,)"
            return ""

        with patch.object(dwm_act, "run_cmd", side_effect=fake_run_playing):
            st = dwm_act.get_audio_state()
            self.assertTrue(st["playing"])
            self.assertEqual(st["sink_inputs"], 1)
            self.assertEqual(len(st["mpris_players"]), 1)
            self.assertEqual(st["mpris_players"][0]["status"], "Playing")

        # Case 2: Corked stream + Paused MPRIS
        def fake_run_paused(args, timeout=4.0, input_bytes=None):
            if args[:2] == ["pactl", "list"]:
                return "Sink Input #42\n\tCorked: yes\n"
            if "ListNames" in args[-1]:
                return "(['org.mpris.MediaPlayer2.firefox.instance_1'],)"
            if "PlaybackStatus" in args:
                return "(<'Paused'>,)"
            return ""

        with patch.object(dwm_act, "run_cmd", side_effect=fake_run_paused):
            st = dwm_act.get_audio_state()
            self.assertFalse(st["playing"])
            self.assertEqual(st["sink_inputs"], 0)
            self.assertEqual(st["mpris_players"][0]["status"], "Paused")

    def test_live_observe_snapshot(self):
        obs = dwm_act.observe(include_ocr=False)
        self.assertIn("focused", obs)
        self.assertIn("clients", obs)
        self.assertIn("tabs", obs)
        self.assertIn("audio", obs)
        self.assertIn("ui", obs)
        self.assertIn("playing", obs["audio"])
        self.assertIn("busy", obs["ui"])


class TestLayer2ReactiveWait(unittest.TestCase):
    def test_wait_until_resolves_early(self):
        counter = [0]

        def pred():
            counter[0] += 1
            return "done" if counter[0] >= 3 else None

        t0 = time.time()
        res = dwm_act.wait_until(pred, timeout=2.0, interval=0.02, initial_delay=0.0)
        elapsed = time.time() - t0
        self.assertEqual(res, "done")
        self.assertEqual(counter[0], 3)
        self.assertLess(elapsed, 0.5)

    def test_wait_page_ready_waits_for_title_stability(self):
        titles = [
            {"id": "0x1", "class": "firefox", "title": "New Tab — Mozilla Firefox"},
            {"id": "0x1", "class": "firefox", "title": "Lofi Hip Hop Radio - YouTube — Mozilla Firefox"},
            {"id": "0x1", "class": "firefox", "title": "Lofi Hip Hop Radio - YouTube — Mozilla Firefox"},
        ]
        idx = [0]

        def fake_focused():
            val = titles[min(idx[0], len(titles) - 1)]
            idx[0] += 1
            return val

        with patch.object(dwm_act, "get_focused", side_effect=fake_focused), \
             patch.object(dwm_act, "get_audio_state", return_value={"playing": False, "sink_inputs": 0, "mpris_players": []}), \
             patch.object(dwm_act, "query_atspi_snapshot", return_value={"busy": False}):
            res = dwm_act.wait_page_ready(prev_title="Old Title", target_hint="lofi", timeout=2.0)
            self.assertTrue(res["ready"])
            self.assertIn("Lofi Hip Hop Radio", res["title"])


class TestLayer3GoalPrimitives(unittest.TestCase):
    def test_normalize_and_classify_media(self):
        self.assertEqual(dwm_act.normalize_media_query("low fee Dragon Ball music"), "lofi Dragon Ball music")
        self.assertTrue(dwm_act.is_media_query("low fee Dragon Ball music"))
        self.assertTrue(dwm_act.is_media_query("lofi hip hop beats"))
        self.assertTrue(dwm_act.is_media_query("miles davis jazz"))
        self.assertTrue(dwm_act.is_media_query("miles davis"))
        self.assertFalse(dwm_act.is_media_query("github"))
        self.assertFalse(dwm_act.is_media_query("firefox"))
        self.assertFalse(dwm_act.is_media_query("new terminal"))

    def test_score_yt_video_renderer(self):
        q_tok = {"miles", "davis"}
        short_vr = {
            "videoId": "short123456",
            "title": {"runs": [{"text": "Miles Davis Interview Explains Jazz"}]},
            "ownerText": {"runs": [{"text": "Random Clips"}]},
            "lengthText": {"simpleText": "0:48"},
            "navigationEndpoint": {"commandMetadata": {"webCommandMetadata": {"url": "/shorts/short123456"}}},
        }
        album_vr = {
            "videoId": "album123456",
            "title": {"runs": [{"text": "Miles Davis - Kind of Blue (Full Album)"}]},
            "ownerText": {"runs": [{"text": "Miles Davis - Topic"}]},
            "lengthText": {"simpleText": "45:44"},
            "navigationEndpoint": {"commandMetadata": {"webCommandMetadata": {"url": "/watch?v=album123456"}}},
        }
        s_short, _ = dwm_act._score_yt_video_renderer(short_vr, q_tok, 0)
        s_album, vid_album = dwm_act._score_yt_video_renderer(album_vr, q_tok, 1)
        self.assertEqual(vid_album, "album123456")
        self.assertGreater(s_album, 80.0)
        self.assertLess(s_short, 0.0)

    def test_ensure_playback_escalation_mpris(self):
        # Simulate audio initially paused with an MPRIS player, then playing after mpris_control("Play")
        mpris_called = [False]

        def fake_audio():
            return {
                "playing": mpris_called[0],
                "sink_inputs": 1 if mpris_called[0] else 0,
                "mpris_players": [{"bus": "org.mpris.MediaPlayer2.firefox", "status": "Playing" if mpris_called[0] else "Paused"}],
            }

        def fake_mpris(action="Play", target_bus=None):
            if action == "Play":
                mpris_called[0] = True
                return True
            return False

        with patch.object(dwm_act, "get_audio_state", side_effect=fake_audio), \
             patch.object(dwm_act, "mpris_control", side_effect=fake_mpris), \
             patch.object(dwm_act, "wait_audio_playing", side_effect=lambda timeout=1.0: mpris_called[0]):
            ok = dwm_act.ensure_playback_started(is_youtube=True)
            self.assertTrue(ok)
            self.assertTrue(mpris_called[0])

    def test_ensure_playback_escalation_key_k(self):
        # Simulate no MPRIS player yet (fresh YouTube tab), starts playing after 'k' key press
        key_k_sent = [False]

        def fake_run(args, timeout=4.0, input_bytes=None):
            if args[:2] == ["xdotool", "key"] and args[-1] == "k":
                key_k_sent[0] = True
            return ""

        with patch.object(dwm_act, "get_audio_state", return_value={"playing": False, "sink_inputs": 0, "mpris_players": []}), \
             patch.object(dwm_act, "run_cmd", side_effect=fake_run), \
             patch.object(dwm_act, "_focus_firefox_window", return_value=True), \
             patch.object(dwm_act, "wait_audio_playing", side_effect=lambda timeout=1.0: key_k_sent[0]):
            ok = dwm_act.ensure_playback_started(is_youtube=True)
            self.assertTrue(ok)
            self.assertTrue(key_k_sent[0])

    def test_do_focus_delegates_unmatched_media_to_do_play(self):
        with patch.object(dwm_act, "resolve_semantic_target", return_value={"kind": "place", "score": 0.45, "label": "weak"}), \
             patch.object(dwm_act, "do_play", return_value="play:https://www.youtube.com/watch?v=test[playing]") as mock_play:
            res = dwm_act.do_focus("low fee Dragon Ball music", allow_open=True)
            mock_play.assert_called_once_with("low fee Dragon Ball music")
            self.assertIn("[playing]", res)


class TestLayer4AurenRouter(unittest.TestCase):
    def _dry_run(self, phrase: str) -> str:
        out = subprocess.check_output([AUREN_PATH, "dry-run", phrase], text=True, timeout=6.0).strip()
        return out

    def test_natural_media_phrases(self):
        r1 = self._dry_run("Open up some low fee Dragon Ball music.")
        self.assertIn("intent=play_target", r1)
        self.assertIn('cmd=play "lofi Dragon Ball music"', r1)

        r2 = self._dry_run("put up some lofi")
        self.assertIn("intent=play_target", r2)
        self.assertIn('cmd=play "lofi"', r2)

        r3 = self._dry_run("put on some jazz")
        self.assertIn("intent=play_target", r3)
        self.assertIn('cmd=play "jazz"', r3)

        r3b = self._dry_run("open up some miles davis")
        self.assertIn("intent=play_target", r3b)
        self.assertIn('cmd=play "miles davis"', r3b)

        r3c = self._dry_run("open up a new terminal")
        self.assertIn("intent=open_terminal", r3c)
        self.assertIn("cmd=sh st", r3c)

        r4 = self._dry_run("pause the music")
        self.assertIn("intent=media_pause", r4)
        self.assertIn("cmd=media pause", r4)

    def test_desktop_and_chained_intents(self):
        r5 = self._dry_run("switch to whatsapp")
        self.assertIn("status=ACT", r5)
        self.assertIn('focus "whatsapp"', r5)

        r6 = self._dry_run("switch to workspace 2 and toggle fullscreen")
        self.assertIn("status=ACT", r6)
        self.assertIn("view 2 ; wm fullscreen", r6)

        r6b = self._dry_run("open a new tab, press enter, press")
        self.assertIn("status=ACT", r6b)
        self.assertIn("key ctrl+t ; key Return", r6b)
        self.assertNotIn('open "new tab', r6b)

        r6c = self._dry_run("type in hello world")
        self.assertIn("intent=type_text", r6c)
        self.assertIn('cmd=type "hello world"', r6c)

        r6d = self._dry_run("type out hello world and press enter")
        self.assertIn("intent=type_enter", r6d)
        self.assertIn('cmd=type_enter "hello world"', r6d)

        r6e = self._dry_run("write down buy milk")
        self.assertIn("intent=type_text", r6e)
        self.assertIn('cmd=type "buy milk"', r6e)

    def test_context_scoped_intents(self):
        env_term = dict(os.environ, DWM_AUREN_CONTEXT_OVERRIDE="terminal")
        out_term = subprocess.check_output(
            [AUREN_PATH, "dry-run", "clear the line"], text=True, env=env_term, timeout=6.0
        ).strip()
        self.assertIn("intent=clear_line@terminal", out_term)
        self.assertIn("cmd=key ctrl+u", out_term)

        env_browser = dict(os.environ, DWM_AUREN_CONTEXT_OVERRIDE="browser")
        out_browser = subprocess.check_output(
            [AUREN_PATH, "dry-run", "clear the line"], text=True, env=env_browser, timeout=6.0
        ).strip()
        self.assertIn("intent=clear_line@browser", out_browser)
        self.assertIn("cmd=key ctrl+a BackSpace", out_browser)

        out_word_term = subprocess.check_output(
            [AUREN_PATH, "dry-run", "delete previous word"], text=True, env=env_term, timeout=6.0
        ).strip()
        self.assertIn("intent=del_word@terminal", out_word_term)
        self.assertIn("cmd=key ctrl+w", out_word_term)

        out_word_browser = subprocess.check_output(
            [AUREN_PATH, "dry-run", "delete previous word"], text=True, env=env_browser, timeout=6.0
        ).strip()
        self.assertIn("intent=del_word@browser", out_word_browser)
        self.assertIn("cmd=key ctrl+BackSpace", out_word_browser)

        out_ls_home = subprocess.check_output(
            [AUREN_PATH, "dry-run", "list the files in my home directory"], text=True, env=env_term, timeout=6.0
        ).strip()
        self.assertIn("intent=ls_home@terminal", out_ls_home)
        self.assertIn('cmd=type_enter "ls -la ~"', out_ls_home)

        out_asr_ls = subprocess.check_output(
            [AUREN_PATH, "dry-run", "least files in this terminal"], text=True, env=env_term, timeout=6.0
        ).strip()
        self.assertIn("intent=ls_cwd@terminal", out_asr_ls)
        self.assertIn('cmd=type_enter "ls -la"', out_asr_ls)

        out_asr_tmux = subprocess.check_output(
            [AUREN_PATH, "dry-run", "Open T-MUX in this terminal. Open T"], text=True, env=env_term, timeout=6.0
        ).strip()
        self.assertIn("intent=open_tmux@terminal", out_asr_tmux)
        self.assertIn('cmd=type_enter "tmux"', out_asr_tmux)

        out_win_term = subprocess.check_output(
            [AUREN_PATH, "dry-run", "Open up a new window."], text=True, env=env_term, timeout=6.0
        ).strip()
        self.assertIn("intent=new_win@terminal", out_win_term)
        self.assertIn("cmd=sh st", out_win_term)

        out_win_browser = subprocess.check_output(
            [AUREN_PATH, "dry-run", "Open up a new window."], text=True, env=env_browser, timeout=6.0
        ).strip()
        self.assertIn("intent=new_win@browser", out_win_browser)
        self.assertIn("cmd=key ctrl+n", out_win_browser)

        out_find = subprocess.check_output(
            [AUREN_PATH, "dry-run", "find all python files in my home directory"], text=True, env=env_term, timeout=6.0
        ).strip()
        self.assertIn("intent=find_ext@terminal", out_find)
        self.assertIn('cmd=type_enter "find ~ -name \'*.py\'"', out_find)

        out_chain = subprocess.check_output(
            [AUREN_PATH, "dry-run", "open up a new terminal and list the files in my home directory"],
            text=True, env=env_browser, timeout=6.0
        ).strip()
        self.assertIn("status=ACT", out_chain)
        self.assertIn('cmd=sh st ; type_enter "ls -la ~"', out_chain)

        self.assertEqual(dwm_act._unquote('"find . -name \'*.c\'"'), "find . -name '*.c'")

    def test_learned_intent_persistence_and_undo_eviction(self):
        learned_path = os.path.expanduser("~/.config/dwm/intents.learned.conf")
        last_learned_json = f"/tmp/dwm-last-learned-{os.environ.get('USER', 'user')}.json"
        os.makedirs(os.path.dirname(learned_path), exist_ok=True)
        entry = "learned_unit_test_transpose@terminal | dwm-act key ctrl+t | unit test transpose chars\n"
        with open(learned_path, "a", encoding="utf-8") as f:
            f.write(entry)
        with open(last_learned_json, "w", encoding="utf-8") as f:
            json.dump({"ts": time.time(), "id": "learned_unit_test_transpose@terminal", "exemplar": "unit test transpose chars"}, f)

        env_term = dict(os.environ, DWM_AUREN_CONTEXT_OVERRIDE="terminal")
        out_learned = subprocess.check_output(
            [AUREN_PATH, "dry-run", "unit test transpose chars"], text=True, env=env_term, timeout=6.0
        ).strip()
        self.assertIn("intent=learned_unit_test_transpose@terminal", out_learned)
        self.assertIn("cmd=key ctrl+t", out_learned)

        undo_res = dwm_act.do_undo()
        self.assertIn("undo:unlearned", undo_res)
        if os.path.exists(learned_path):
            with open(learned_path, "r", encoding="utf-8") as f:
                self.assertNotIn("learned_unit_test_transpose@terminal", f.read())

    def test_recording_and_demo_chains(self):
        r7 = self._dry_run("start recording my screen and audio")
        self.assertIn("intent=record_start", r7)
        self.assertIn("cmd=record start", r7)

        r8 = self._dry_run("stop recording")
        self.assertIn("intent=record_stop", r8)
        self.assertIn("cmd=record stop", r8)

        r9 = self._dry_run("record my screen and audio, switch to workspace 2, play some low fee Dragon Ball music, wait 5 seconds, and stop recording")
        self.assertIn("status=ACT", r9)
        self.assertIn('record start ; view 2 ; play "lofi Dragon Ball music" ; wait_sec "5" ; record stop', r9)

    def test_live_record_start_and_stop_lifecycle(self):
        start_res = dwm_act.do_record("start both", in_chain=False)
        self.assertIn("record:start(both)", start_res)
        self.assertIn("[recording]", start_res)
        st = dwm_act.get_recording_state()
        self.assertTrue(st["recording"])
        rec_path = st["path"]
        self.assertTrue(rec_path.endswith(".mp4"))
        time.sleep(0.8)
        stop_res = dwm_act.do_record("stop", in_chain=False)
        self.assertIn("record:stop", stop_res)
        st_after = dwm_act.get_recording_state()
        self.assertFalse(st_after["recording"])
        self.assertTrue(os.path.exists(rec_path))
        self.assertGreater(os.path.getsize(rec_path), 1024)
        try:
            os.unlink(rec_path)
        except Exception:
            pass


if __name__ == "__main__":
    unittest.main(verbosity=2)
