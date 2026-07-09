import json
import sys
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

    def close(self):
        self.closed = True


class OpenClawAgentSessionTest(unittest.TestCase):
    def test_session_key_is_stable_and_safe(self):
        self.assertEqual(
            build_openclaw_session_key("xiaopai", "44:1b f6/e4"),
            "xiaopai-44:1b_f6_e4",
        )

    def test_chat_sends_session_header_and_user_fallback(self):
        captured = []

        def fake_urlopen(request, timeout):
            captured.append((request, timeout))
            return FakeResponse()

        agent = OpenClawAgent(
            base_url="http://openclaw/v1",
            token="token",
            model="openclaw/default",
            session_prefix="xiaopai",
        )

        with patch("urllib.request.urlopen", fake_urlopen):
            reply = agent.chat("dev 1", "你好")

        self.assertEqual(reply, "ok")
        request, timeout = captured[0]
        self.assertEqual(timeout, 45)
        body = json.loads(request.data.decode("utf-8"))
        self.assertEqual(body["user"], "xiaopai-dev_1")
        self.assertEqual(request.get_header("X-openclaw-session-key"), "xiaopai-dev_1")

    def test_morrow_ws_url_uses_default_session(self):
        self.assertEqual(
            build_morrow_ws_url("http://127.0.0.1:3000", "default"),
            "ws://127.0.0.1:3000/api/sessions/default/ws",
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

        def fake_urlopen(request, timeout):
            captured["session_url"] = request.full_url
            return FakeSessionResponse()

        def fake_create_connection(url, timeout, header=None):
            captured["ws_url"] = url
            captured["header"] = header
            return websocket

        agent = OpenClawAgent(base_url="http://morrow:3000", token="", model="ignored")
        fake_websocket_module = types.SimpleNamespace(create_connection=fake_create_connection)

        with patch("urllib.request.urlopen", fake_urlopen), patch.dict(sys.modules, {"websocket": fake_websocket_module}):
            segments = list(agent.chat_stream("dev1", "今天怎么样"))

        self.assertEqual(segments, ["你好，", "今天不错。"])
        self.assertEqual(captured["session_url"], "http://morrow:3000/api/sessions/default")
        self.assertEqual(captured["ws_url"], "ws://morrow:3000/api/sessions/default/ws")
        sent = json.loads(websocket.sent[0])
        self.assertEqual(sent["type"], "start_turn")
        self.assertEqual(sent["data"]["prompt"], "今天怎么样")


if __name__ == "__main__":
    unittest.main()
