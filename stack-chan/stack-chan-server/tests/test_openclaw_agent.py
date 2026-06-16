import json
import sys
import unittest
from pathlib import Path
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))

from openclaw_agent import OpenClawAgent, build_openclaw_session_key  # noqa: E402


class FakeResponse:
    status = 200

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc, tb):
        return False

    def read(self):
        return b'{"choices":[{"message":{"content":"ok"}}]}'


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


if __name__ == "__main__":
    unittest.main()
