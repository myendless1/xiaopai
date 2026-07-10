import json
import queue
import sys
import threading
import time
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))

from morrow_client import MorrowClient, build_morrow_ws_url  # noqa: E402


class FakeWebSocket:
    def __init__(self):
        self.frames = queue.Queue()
        self.sent = []
        self.closed = False
        self.timeouts = []

    def settimeout(self, timeout):
        self.timeouts.append(timeout)

    def recv(self):
        frame = self.frames.get(timeout=1)
        if isinstance(frame, BaseException):
            raise frame
        return frame

    def send(self, payload):
        self.sent.append(json.loads(payload))

    def close(self):
        self.closed = True
        self.frames.put(None)


class MorrowClientTest(unittest.TestCase):
    def make_client(self, ws):
        return MorrowClient(
            base_url="http://morrow:3000",
            websocket_factory=lambda *_args, **_kwargs: ws,
            reconnect_min=0.01,
            reconnect_max=0.02,
        )

    def test_default_session_url(self):
        self.assertEqual(build_morrow_ws_url("http://morrow:3000"), "ws://morrow:3000/api/sessions/default/ws")

    def test_connect_timeout_is_disabled_after_handshake(self):
        ws = FakeWebSocket()
        client = self.make_client(ws)
        client.start()
        deadline = time.time() + 0.2
        while not ws.timeouts and time.time() < deadline:
            time.sleep(0.005)
        self.assertEqual(ws.timeouts, [None])
        self.assertTrue(client.connected)
        client.stop()

    def test_not_ready_until_snapshot_and_strict_start_turn(self):
        ws = FakeWebSocket()
        client = self.make_client(ws)
        client.start()
        self.assertFalse(client.wait_ready(0.02))
        ws.frames.put(json.dumps({"type": "snapshot", "data": {}}))
        self.assertTrue(client.wait_ready(0.2))
        client.start_turn("req-1", "你好")
        self.assertEqual(ws.sent, [{"type": "start_turn", "data": {"request_id": "req-1", "prompt": "你好"}}])
        client.stop()

    def test_notice_is_not_reduced_to_text(self):
        ws = FakeWebSocket()
        client = self.make_client(ws)
        client.start()
        ws.frames.put(json.dumps({"type": "snapshot", "data": {}}))
        self.assertTrue(client.wait_ready(0.2))
        notice = {"id": "meeting:1", "timestamp_ms": 123, "kind": "meeting_reminder", "text": "开会"}
        ws.frames.put(json.dumps({"type": "robot_notice", "data": notice}))
        self.assertEqual(client.notices.get(timeout=0.2), notice)
        client.stop()

    def test_cancel_uses_snapshot_turn_id_only(self):
        ws = FakeWebSocket()
        client = self.make_client(ws)
        client.start()
        ws.frames.put(json.dumps({"type": "snapshot", "data": {}}))
        self.assertTrue(client.wait_ready(0.2))
        self.assertFalse(client.cancel_turn())
        ws.frames.put(json.dumps({"type": "snapshot", "data": {"running_turn": {"turn_id": "turn-7"}}}))
        deadline = time.time() + 0.2
        while client.morrow_turn_id != "turn-7" and time.time() < deadline:
            time.sleep(0.005)
        self.assertTrue(client.cancel_turn())
        self.assertEqual(ws.sent[-1], {"type": "cancel_turn", "data": {"turn_id": "turn-7"}})
        client.stop()


if __name__ == "__main__":
    unittest.main()
