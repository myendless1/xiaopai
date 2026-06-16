import sys
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))

import server  # noqa: E402


class OpenClawEventContentTest(unittest.TestCase):
    def test_openclaw_prompt_is_voice_first(self):
        self.assertIn("输出默认面向语音播报", server.XIAOPAI_OPENCLAW_SYSTEM_PROMPT)
        self.assertIn("不要使用 Markdown 表格", server.XIAOPAI_OPENCLAW_SYSTEM_PROMPT)
        self.assertIn("消化成适合朗读的总结", server.XIAOPAI_OPENCLAW_SYSTEM_PROMPT)

    def test_speech_recognition_uses_recognized_text_as_content(self):
        content = server.build_openclaw_event_content(
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
        self.assertNotIn("openclaw.stackchan.event.v1", content)

    def test_non_touch_device_event_uses_plain_text_summary(self):
        content = server.build_openclaw_event_content(
            "robot-001",
            "button_press",
            {
                "name": "side_button",
                "timestamp": "2026-06-11T17:31:00+08:00",
            },
        )

        self.assertEqual(content, "小派设备事件：设备 robot-001，事件类型 button_press，事件名称 side_button。")
        self.assertNotIn("{", content)

    def test_openclaw_event_content_is_plain_text(self):
        content = server.build_openclaw_event_content(
            "robot-001",
            "speech_recognition",
            {"text": "你好", "task_id": "task-456"},
        )

        self.assertEqual(content, "你好")
        self.assertNotIn("\n", content)


class CommandPayloadTest(unittest.TestCase):
    def test_sequence_query_speak_step_pauses_listener(self):
        payload = server.command_payload_from_query(
            "sequence",
            {"expression": ["calm"], "text": ["在的。"]},
        )

        self.assertEqual(payload[0], {"type": "face", "expression": "calm"})
        self.assertEqual(payload[1], {"type": "speak", "text": "在的。", "pause_listener": True})

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


class DeviceEventForwardingTest(unittest.TestCase):
    def make_handler(self, openclaw_enabled=True):
        handler = object.__new__(server.Handler)
        sent_bodies = []
        enqueued_commands = []
        forwarded_events = []

        handler._send_json = lambda body, status=server.HTTPStatus.OK: sent_bodies.append((body, status))
        handler._mark_device_seen = lambda device_id: None
        handler._openclaw_enabled = lambda: openclaw_enabled
        handler._enqueue_command = lambda device_id, command: enqueued_commands.append((device_id, command))

        def send_openclaw_event(device_id, event_type, details):
            forwarded_events.append((device_id, event_type, details))
            return {"openclaw_enabled": True, "openclaw_sent": True, "queued_commands": []}

        handler._send_openclaw_event = send_openclaw_event
        return handler, sent_bodies, enqueued_commands, forwarded_events

    def test_head_touch_uses_local_shortcut_even_when_openclaw_enabled(self):
        handler, sent_bodies, enqueued_commands, forwarded_events = self.make_handler(openclaw_enabled=True)

        handler._handle_device_event(
            {"device_id": ["robot-001"], "type": ["head_touch"], "name": ["click"]},
            None,
        )

        self.assertEqual(forwarded_events, [])
        self.assertEqual(len(enqueued_commands), 1)
        self.assertEqual(enqueued_commands[0][1]["type"], "face")
        body, _status = sent_bodies[-1]
        self.assertEqual(body["openclaw_skipped"], "local_head_touch_expression")
        self.assertFalse(body["openclaw_sent"])
        self.assertEqual(len(body["queued_commands"]), 1)

    def test_head_touch_keeps_local_shortcut_when_openclaw_disabled(self):
        handler, sent_bodies, enqueued_commands, forwarded_events = self.make_handler(openclaw_enabled=False)

        handler._handle_device_event(
            {"device_id": ["robot-001"], "type": ["head_touch"], "name": ["click"]},
            None,
        )

        self.assertEqual(forwarded_events, [])
        self.assertEqual(len(enqueued_commands), 1)
        self.assertEqual(enqueued_commands[0][1]["type"], "face")
        body, _status = sent_bodies[-1]
        self.assertEqual(body["openclaw_skipped"], "local_head_touch_expression")
        self.assertEqual(len(body["queued_commands"]), 1)


if __name__ == "__main__":
    unittest.main()
