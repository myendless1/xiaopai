import json
import sys
import threading
import types
import unittest
from pathlib import Path
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))

from openclaw_agent import OpenClawAgent, build_morrow_ws_url, build_openclaw_session_key  # noqa: E402


class FakeResponse:
    status = 200

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc, tb):
        return False

    def read(self):
        return b'{"choices":[{"message":{"content":"ok"}}]}'


class FakeSessionResponse:
    status = 201

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc, tb):
        return False


class FakeMorrowWebSocket:
    def __init__(self, frames):
        self.frames = list(frames)
        self.sent = []
        self.closed = False

    def send(self, payload):
        self.sent.append(payload)

    def recv(self):
        if not self.frames:
            return json.dumps({"type": "turn_saved", "data": {"session": "default"}}, ensure_ascii=False)
        return self.frames.pop(0)

    def settimeout(self, _timeout):
        return None

    def close(self):
        self.closed = True


class OpenClawAgentSessionTest(unittest.TestCase):
    def setUp(self):
        for client in list(OpenClawAgent._clients.values()):
            client.stop()
        OpenClawAgent._clients.clear()

    def test_session_key_is_stable_and_safe(self):
        self.assertEqual(
            build_openclaw_session_key("xiaopai", "44:1b f6/e4"),
            "xiaopai-44:1b_f6_e4",
        )

    def test_morrow_ws_url_uses_default_session(self):
        self.assertEqual(
            build_morrow_ws_url("http://127.0.0.1:3000", "xiaopai"),
            "ws://127.0.0.1:3000/api/sessions/xiaopai/ws",
        )

    def test_morrow_chat_stream_segments_on_punctuation(self):
        frames = [
            json.dumps({"type": "snapshot", "data": {}}, ensure_ascii=False),
            json.dumps(
                {
                    "type": "agent_event",
                    "data": {"event": {"type": "text_delta", "data": "你好，"}},
                },
                ensure_ascii=False,
            ),
            json.dumps(
                {
                    "type": "agent_event",
                    "data": {"event": {"type": "text_delta", "data": "今天"}},
                },
                ensure_ascii=False,
            ),
            json.dumps(
                {
                    "type": "agent_event",
                    "data": {"event": {"type": "text_delta", "data": "不错。"}},
                },
                ensure_ascii=False,
            ),
            json.dumps(
                {
                    "type": "agent_event",
                    "data": {"event": {"type": "agent_message", "data": "你好，今天不错。"}},
                },
                ensure_ascii=False,
            ),
            json.dumps({"type": "turn_saved", "data": {"session": "default"}}, ensure_ascii=False),
        ]
        websocket = FakeMorrowWebSocket(frames)
        captured = {}

        def fake_create_connection(url, timeout, header=None):
            captured["ws_url"] = url
            captured["header"] = header
            return websocket

        agent = OpenClawAgent(base_url="http://morrow:3000", token="", model="ignored", session_prefix="xiaopai")
        fake_websocket_module = types.SimpleNamespace(create_connection=fake_create_connection)

        with patch("urllib.request.urlopen", side_effect=AssertionError("HTTP session creation is forbidden")), patch.dict(sys.modules, {"websocket": fake_websocket_module}):
            segments = list(agent.chat_stream("dev1", "今天怎么样"))

        self.assertEqual(segments, ["你好，", "今天不错。"])
        self.assertEqual(captured["ws_url"], "ws://morrow:3000/api/sessions/xiaopai/ws")
        sent = json.loads(websocket.sent[0])
        self.assertEqual(sent["type"], "start_turn")
        self.assertEqual(sent["data"]["prompt"], "今天怎么样")
        self.assertEqual(sent["data"]["conversation_id"], "xiaopai-dev1")

    def test_morrow_robot_notice_stream_reuses_dialogue_stream_without_start_turn(self):
        frames = [
            json.dumps({"type": "snapshot", "data": {}}, ensure_ascii=False),
            json.dumps(
                {"type": "robot_notice", "data": {"text": "该喝水了。"}},
                ensure_ascii=False,
            ),
        ]
        websocket = FakeMorrowWebSocket(frames)
        captured = {}
        stop_event = threading.Event()

        def fake_create_connection(url, timeout, header=None):
            captured["ws_url"] = url
            captured["header"] = header
            return websocket

        agent = OpenClawAgent(base_url="http://morrow:3000", token="", model="ignored", session_prefix="xiaopai")
        fake_websocket_module = types.SimpleNamespace(create_connection=fake_create_connection)

        with patch("urllib.request.urlopen", side_effect=AssertionError("HTTP session creation is forbidden")), patch.dict(sys.modules, {"websocket": fake_websocket_module}):
            notice_iter = agent.morrow_robot_notice_stream(stop_event=stop_event)
            notices = [next(notice_iter)]
            stop_event.set()

        self.assertEqual(notices, ["该喝水了。"])
        self.assertEqual(captured["ws_url"], "ws://morrow:3000/api/sessions/xiaopai/ws")
        self.assertEqual(websocket.sent, [])


if __name__ == "__main__":
    unittest.main()
