import sys
import threading
import time
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))

import server  # noqa: E402


class DeviceCommandQueueFlowControlTest(unittest.TestCase):
    def test_full_speech_queue_keeps_speech_but_allows_control_command(self):
        queue = server.DeviceCommandQueue(4)
        speak = {"cmd_id": "speak-1", "type": "speak", "priority": 50, "discardable": False}
        stop = {"cmd_id": "stop-1", "type": "stop", "priority": 100, "discardable": False}
        queue.put(speak)
        queue.put(stop)

        self.assertEqual(queue.get_nowait(allow_speak=False)["cmd_id"], "stop-1")
        self.assertEqual(queue.qsize(), 1)
        self.assertEqual(queue.get_nowait()["cmd_id"], "speak-1")

    def test_discard_removes_database_leased_duplicate(self):
        queue = server.DeviceCommandQueue(4)
        queue.put({"cmd_id": "speak-1", "type": "speak"})

        self.assertTrue(queue.discard("speak-1"))
        self.assertEqual(queue.qsize(), 0)
        self.assertFalse(queue.discard("speak-1"))

    def test_pending_dialogue_defers_find_owner_but_allows_other_control(self):
        queue = server.DeviceCommandQueue(4)
        queue.put({"cmd_id": "find-1", "type": "find_owner", "priority": 85})
        queue.put({"cmd_id": "face-1", "type": "face", "priority": 65})

        self.assertEqual(queue.get_nowait(allow_find_owner=False)["cmd_id"], "face-1")
        self.assertEqual(queue.qsize(), 1)
        self.assertEqual(queue.get_nowait()["cmd_id"], "find-1")

    def test_long_poll_rechecks_dialogue_before_returning_find_owner(self):
        queue = server.DeviceCommandQueue(4)
        state = {"dialogue_pending": False}
        predicate_checked = threading.Event()
        result = []

        def allow_find_owner():
            predicate_checked.set()
            return not state["dialogue_pending"]

        worker = threading.Thread(
            target=lambda: result.append(queue.get(timeout=1, allow_find_owner=allow_find_owner))
        )
        worker.start()
        self.assertTrue(predicate_checked.wait(1))
        state["dialogue_pending"] = True
        queue.put({"cmd_id": "find-1", "type": "find_owner", "priority": 85})
        queue.put({"cmd_id": "face-1", "type": "face", "priority": 65})
        worker.join(1)

        self.assertFalse(worker.is_alive())
        self.assertEqual(result[0]["cmd_id"], "face-1")
        self.assertEqual(queue.get_nowait()["cmd_id"], "find-1")


class MorrowEventContentTest(unittest.TestCase):
    def test_speech_recognition_uses_recognized_text_as_content(self):
        content = server.build_morrow_event_content(
            "44:1b:f6:e4:83:8c",
            "speech_recognition",
            {
                "text": "帮我看一下今天日程",
                "task_id": "task-123",
                "timestamp": "2026-06-11T17:30:00+08:00",
                "user_id": "ou_requester",
            },
        )

        self.assertEqual(content, "帮我看一下今天日程")
        self.assertNotIn("{", content)

    def test_non_touch_device_event_uses_plain_text_summary(self):
        content = server.build_morrow_event_content(
            "robot-001",
            "button_press",
            {
                "name": "side_button",
                "timestamp": "2026-06-11T17:31:00+08:00",
            },
        )

        self.assertEqual(content, "小派设备事件：设备 robot-001，事件类型 button_press，事件名称 side_button。")
        self.assertNotIn("{", content)

    def test_morrow_event_content_is_plain_text(self):
        content = server.build_morrow_event_content(
            "robot-001",
            "speech_recognition",
            {"text": "你好", "task_id": "task-456"},
        )

        self.assertEqual(content, "你好")
        self.assertNotIn("\n", content)


class CommandPayloadTest(unittest.TestCase):
    def test_global_speaker_volume_is_distinct_from_tts_synthesis_volume(self):
        payload = {"text": "你好", "volume": 80, "speaker_volume": 70}

        server.apply_global_speaker_volume("speak", payload, 10)

        self.assertEqual(payload["volume"], 80)
        self.assertEqual(payload["speaker_volume"], 10)

    def test_global_speaker_volume_reaches_sequence_and_find_owner_speech(self):
        sequence = [
            {"type": "face", "expression": "happy"},
            {"type": "speak", "text": "完成了"},
            {"type": "find_owner", "reply": "该活动一下了"},
        ]

        server.apply_global_speaker_volume("sequence", sequence, 10)

        self.assertNotIn("speaker_volume", sequence[0])
        self.assertEqual(sequence[1]["speaker_volume"], 10)
        self.assertEqual(sequence[2]["speaker_volume"], 10)

    def test_volume_command_updates_server_global_and_becomes_absolute(self):
        fake_server = type("FakeServer", (), {"speaker_volume": 10})()
        payload = {"direction": "up", "step": 10}

        result = server.normalize_server_volume_command(fake_server, payload)

        self.assertEqual(result, 20)
        self.assertEqual(fake_server.speaker_volume, 20)
        self.assertEqual(payload, {"mode": "set", "value": 20})

    def test_sequence_volume_change_updates_global_before_later_speech(self):
        fake_server = type("FakeServer", (), {"speaker_volume": 10})()
        payload = [
            {"type": "speak", "text": "先用原音量"},
            {"type": "volume", "direction": "up", "step": 10},
            {"type": "speak", "text": "再用新音量"},
        ]

        server.prepare_server_command_audio(fake_server, "sequence", payload)

        self.assertEqual(payload[0]["speaker_volume"], 10)
        self.assertEqual(payload[1], {"type": "volume", "mode": "set", "value": 20})
        self.assertEqual(payload[2]["speaker_volume"], 20)
        self.assertEqual(fake_server.speaker_volume, 20)

    def test_sequence_query_speak_step_pauses_listener(self):
        payload = server.command_payload_from_query(
            "sequence",
            {"expression": ["calm"], "text": ["在的。"]},
        )

        self.assertEqual(payload[0], {"type": "face", "expression": "calm"})
        self.assertEqual(payload[1], {"type": "speak", "text": "在的。", "pause_listener": True})

    def test_speak_query_preserves_tts_voice_options(self):
        payload = server.command_payload_from_query(
            "speak",
            {
                "text": ["你好，我是知妙。"],
                "voice": ["zhimiao_emo"],
                "speech_rate": ["-80"],
                "pitch_rate": ["20"],
            },
        )

        self.assertEqual(payload["text"], "你好，我是知妙。")
        self.assertEqual(payload["voice"], "zhimiao_emo")
        self.assertEqual(payload["speech_rate"], -80)
        self.assertEqual(payload["pitch_rate"], 20)

    def test_speak_query_payload_has_only_speech_fields(self):
        payload = server.command_payload_from_query(
            "speak",
            {"text": ["保持静态表情说话。"]},
        )

        self.assertEqual(payload, {"text": "保持静态表情说话。"})

    def test_sequence_query_speak_step_has_only_speech_fields(self):
        payload = server.command_payload_from_query(
            "sequence",
            {"expression": ["thinking"], "text": ["我想一下。"]},
        )

        self.assertEqual(payload[0], {"type": "face", "expression": "thinking"})
        self.assertEqual(
            payload[1],
            {"type": "speak", "text": "我想一下。", "pause_listener": True},
        )

    def test_speech_text_normalizes_inline_markdown_table(self):
        text = (
            "你今天（2026年6月16日 周二）有 **2 个日程**： "
            "| 时间 | 内容 | |------|------| "
            "| 10:00 - 11:00 | 汇报上周工作进展 | "
            "| 17:00 - 18:00 | 跟老板开会 |"
        )

        self.assertEqual(
            server.normalize_speech_text_for_voice(text),
            "你今天（2026年6月16日 周二）有 2 个日程：10:00 - 11:00，汇报上周工作进展；17:00 - 18:00，跟老板开会。",
        )

    def test_sequence_speech_payload_is_normalized_before_queue(self):
        payload = [
            {
                "type": "speak",
                "text": "**2026-06-16 周二** 10:00 - 11:00 汇报上周工作进展",
            },
            {"type": "face", "expression": "calm"},
        ]

        server.normalize_command_speech_payload("sequence", payload)

        self.assertEqual(payload[0]["text"], "2026-06-16 周二 10:00 - 11:00 汇报上周工作进展")

    def test_state_query_defaults_to_waiting(self):
        payload = server.command_payload_from_query("state", {})

        self.assertEqual(payload, {"state": "waiting"})

    def test_find_owner_query_can_disable_reply(self):
        payload = server.command_payload_from_query("find_owner", {"speak": ["false"]})

        self.assertEqual(payload["reply"], "")
        self.assertFalse(payload["speak"])

    def test_sedentary_audio_is_cached_event_not_head_touch_event(self):
        self.assertIn("sedentary_reminder_stretch", server.EVENT_AUDIO_TEXT)
        self.assertIn("sedentary_reminder_stretch", server.PREWARM_EVENT_AUDIO_NAMES)
        self.assertNotIn("sedentary_reminder_stretch", server.HEAD_TOUCH_EVENT_TEXT)


class MorrowWaitingStateTest(unittest.TestCase):
    def test_waiting_keeps_default_calm_face_until_reply_pcm_starts(self):
        handler = object.__new__(server.Handler)
        calls = []
        handler._send_device_state_command = (
            lambda device_id, state, reason="": calls.append((device_id, state, reason)) or ["state-command"]
        )
        handler._enqueue_command = lambda *_args, **_kwargs: self.fail("waiting must not enqueue a face command")

        queued = handler._enter_morrow_waiting("dev1", "speech_recognition")

        self.assertEqual(queued, ["state-command"])
        self.assertEqual(calls, [("dev1", "waiting", "morrow:speech_recognition")])

    def test_production_path_submits_global_morrow_coordinator(self):
        class FakeCoordinator:
            def __init__(self):
                self.submitted = []

            def submit(self, content, device_id, source):
                self.submitted.append((content, device_id, source))
                return type("Outcome", (), {"request_id": "req-global"})()

        coordinator = FakeCoordinator()
        handler = object.__new__(server.Handler)
        handler.server = type(
            "FakeServer",
            (),
            {"morrow_coordinator": coordinator},
        )()
        handler._enter_morrow_waiting = lambda _device_id, _event_type: ["waiting"]
        handler._log_info = lambda _message: None
        handler._log_error = lambda _message: None

        result = handler._send_morrow_event("dev1", "speech_recognition", {"text": "今天日程"})

        self.assertEqual(coordinator.submitted, [("今天日程", "dev1", "voice")])
        self.assertTrue(result["morrow_submitted"])
        self.assertEqual(result["morrow_request_id"], "req-global")


class CommandRoutingTest(unittest.TestCase):
    def make_handler(self):
        class FakeRealtimeManager:
            def __init__(self):
                self.sent = []

            def first_device_id(self):
                return "44:1b:f6:e4:83:8c"

            def has_device(self, device_id):
                return device_id == "44:1b:f6:e4:83:8c"

            def enqueue_command(self, device_id, command):
                self.sent.append((device_id, command))
                return True

            def set_device_state(self, device_id, state):
                self.sent.append((device_id, {"type": "state", "payload": {"state": state}}))
                return True

        class FakeServer:
            command_queue_max_size = 24

            def __init__(self):
                self.device_lock = threading.Lock()
                self.device_order = ["44:1b:f6:e4:83:8c"]
                self.last_seen = {}
                self.last_ack = {}
                self.device_queues = {}
                self.realtime_manager = FakeRealtimeManager()

        handler = object.__new__(server.Handler)
        handler.server = FakeServer()
        handler._log_info = lambda _msg: None
        handler._log_debug = lambda _msg: None
        return handler

    def test_http_online_device_uses_http_command_queue_before_realtime(self):
        handler = self.make_handler()
        device_id = "44:1b:f6:e4:83:8c"
        handler.server.last_seen[device_id] = time.time()
        command = server.make_command("face", {"expression": "shy"})

        self.assertTrue(handler._enqueue_command(device_id, command))

        self.assertEqual(handler.server.realtime_manager.sent, [])
        self.assertEqual(handler._queue_for(device_id).get_nowait()["cmd_id"], command["cmd_id"])

    def test_realtime_connected_device_still_uses_http_command_queue(self):
        handler = self.make_handler()
        device_id = "44:1b:f6:e4:83:8c"
        command = server.make_command("face", {"expression": "shy"})

        self.assertTrue(handler._enqueue_command(device_id, command))

        self.assertEqual(handler.server.realtime_manager.sent, [])
        self.assertEqual(handler._queue_for(device_id).get_nowait()["cmd_id"], command["cmd_id"])

    def test_http_online_device_state_uses_http_command_queue_before_realtime(self):
        handler = self.make_handler()
        device_id = "44:1b:f6:e4:83:8c"
        handler.server.last_seen[device_id] = time.time()

        queued = handler._send_device_state_command(device_id, "waiting", reason="test")

        self.assertEqual(len(queued), 1)
        self.assertEqual(handler.server.realtime_manager.sent, [])
        command = handler._queue_for(device_id).get_nowait()
        self.assertEqual(command["type"], "state")
        self.assertEqual(command["payload"]["state"], "waiting")


class DeviceEventForwardingTest(unittest.TestCase):
    def make_handler(self, morrow_enabled=True):
        handler = object.__new__(server.Handler)
        sent_bodies = []
        enqueued_commands = []
        forwarded_events = []

        handler._send_json = lambda body, status=server.HTTPStatus.OK: sent_bodies.append((body, status))
        handler._mark_device_seen = lambda device_id: None
        handler.server = type("FakeServer", (), {"morrow_coordinator": object() if morrow_enabled else None})()
        handler._enqueue_command = lambda device_id, command: enqueued_commands.append((device_id, command))

        def send_morrow_event(device_id, event_type, details):
            forwarded_events.append((device_id, event_type, details))
            return {"morrow_enabled": True, "morrow_submitted": True, "queued_commands": []}

        handler._send_morrow_event = send_morrow_event
        return handler, sent_bodies, enqueued_commands, forwarded_events

    def test_head_touch_uses_local_shortcut_even_when_morrow_enabled(self):
        handler, sent_bodies, enqueued_commands, forwarded_events = self.make_handler(morrow_enabled=True)

        handler._handle_device_event(
            {"device_id": ["robot-001"], "type": ["head_touch"], "name": ["click"]},
            None,
        )

        self.assertEqual(forwarded_events, [])
        self.assertEqual(len(enqueued_commands), 1)
        self.assertEqual(enqueued_commands[0][1]["type"], "face")
        body, _status = sent_bodies[-1]
        self.assertEqual(body["morrow_skipped"], "local_head_touch_expression")
        self.assertFalse(body["morrow_submitted"])
        self.assertEqual(len(body["queued_commands"]), 1)

    def test_head_touch_keeps_local_shortcut_when_morrow_disabled(self):
        handler, sent_bodies, enqueued_commands, forwarded_events = self.make_handler(morrow_enabled=False)

        handler._handle_device_event(
            {"device_id": ["robot-001"], "type": ["head_touch"], "name": ["click"]},
            None,
        )

        self.assertEqual(forwarded_events, [])
        self.assertEqual(len(enqueued_commands), 1)
        self.assertEqual(enqueued_commands[0][1]["type"], "face")
        body, _status = sent_bodies[-1]
        self.assertEqual(body["morrow_skipped"], "local_head_touch_expression")
        self.assertEqual(len(body["queued_commands"]), 1)


if __name__ == "__main__":
    unittest.main()
