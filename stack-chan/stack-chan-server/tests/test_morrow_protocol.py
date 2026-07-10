import sys
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))

from morrow_protocol import (  # noqa: E402
    build_cancel_turn,
    build_reset_session,
    build_start_turn,
    parse_message,
    snapshot_running_turn_id,
)


class MorrowProtocolTest(unittest.TestCase):
    def test_start_turn_has_only_public_fields(self):
        self.assertEqual(
            build_start_turn("req-1", "你好"),
            {"type": "start_turn", "data": {"request_id": "req-1", "prompt": "你好"}},
        )

    def test_cancel_turn_requires_real_turn_id(self):
        self.assertEqual(build_cancel_turn("turn-1"), {"type": "cancel_turn", "data": {"turn_id": "turn-1"}})
        with self.assertRaises(ValueError):
            build_cancel_turn("")

    def test_reset_session_shape(self):
        self.assertEqual(build_reset_session("req-reset"), {"type": "reset_session", "data": {"request_id": "req-reset"}})

    def test_parse_supported_messages_and_unknown_agent_event(self):
        delta = parse_message({"type": "agent_event", "data": {"event": {"type": "text_delta", "data": "你"}}})
        self.assertEqual(delta.type, "agent_event")
        self.assertEqual(delta.data, {"type": "text_delta", "data": "你"})
        unknown = parse_message({"type": "agent_event", "data": {"event": {"type": "tool_call", "data": {}}}})
        self.assertEqual(unknown.type, "unknown")

    def test_notice_keeps_complete_authoritative_data(self):
        data = {"id": "meeting:1", "timestamp_ms": 123, "kind": "meeting_reminder", "text": "开会"}
        event = parse_message({"type": "robot_notice", "data": data})
        self.assertEqual(event.data, data)

    def test_snapshot_turn_id(self):
        self.assertEqual(snapshot_running_turn_id({"running_turn": {"turn_id": "turn-9"}}), "turn-9")


if __name__ == "__main__":
    unittest.main()
