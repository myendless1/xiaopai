import sys
import tempfile
import threading
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))

import server  # noqa: E402


class DeviceLogFileTest(unittest.TestCase):
    def make_handler(self, device_log_dir):
        class FakeServer:
            command_queue_max_size = 24

            def __init__(self):
                self.device_lock = threading.Lock()
                self.device_order = []
                self.last_seen = {}
                self.device_logs = {}
                self.device_log_dir = device_log_dir

        handler = object.__new__(server.Handler)
        handler.server = FakeServer()
        handler.errors = []
        handler._log_error = lambda msg: handler.errors.append(msg)
        return handler

    def test_first_device_seen_writes_connected_line(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            handler = self.make_handler(tmpdir)

            handler._mark_device_seen("44:1b:f6:df:5d:b8")
            handler._mark_device_seen("44:1b:f6:df:5d:b8")

            path = Path(server.device_log_file_path(tmpdir, "44:1b:f6:df:5d:b8"))
            lines = path.read_text(encoding="utf-8").splitlines()
            self.assertEqual(len(lines), 1)
            self.assertIn("类型=连接", lines[0])
            self.assertIn("设备=44:1b:f6:df:5d:b8", lines[0])
            self.assertIn('消息="服务端首次看到设备"', lines[0])
            self.assertEqual(handler.errors, [])

    def test_device_log_file_is_one_readable_line_per_event(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            handler = self.make_handler(tmpdir)

            handler._append_device_log(
                "44:1b:f6:df:5d:b8",
                {
                    "type": "log",
                    "source": "esp-log",
                    "device_ms": 123,
                    "line": "I (123) Xiaopai: hello\nworld",
                    "server_ts": 1783238866.5,
                },
            )

            path = Path(server.device_log_file_path(tmpdir, "44:1b:f6:df:5d:b8"))
            lines = path.read_text(encoding="utf-8").splitlines()
            self.assertEqual(len(lines), 1)
            self.assertIn("类型=日志", lines[0])
            self.assertIn("来源=设备日志", lines[0])
            self.assertIn("设备毫秒=123", lines[0])
            self.assertIn("hello\\nworld", lines[0])
            self.assertEqual(handler.errors, [])

    def test_first_seen_resets_existing_device_log_file(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            handler = self.make_handler(tmpdir)
            server.append_device_log_file(
                handler.server,
                "44:1b:f6:df:5d:b8",
                {"type": "log", "device_id": "44:1b:f6:df:5d:b8", "message": "old server run"},
            )

            handler._mark_device_seen("44:1b:f6:df:5d:b8")

            path = Path(server.device_log_file_path(tmpdir, "44:1b:f6:df:5d:b8"))
            lines = path.read_text(encoding="utf-8").splitlines()
            self.assertEqual(len(lines), 1)
            self.assertIn("类型=连接", lines[0])
            self.assertNotIn("old server run", lines[0])

    def test_reconnect_resets_device_log_file(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            handler = self.make_handler(tmpdir)
            server.append_device_log_file(
                handler.server,
                "44:1b:f6:df:5d:b8",
                {
                    "type": "log",
                    "source": "esp-log",
                    "device_id": "44:1b:f6:df:5d:b8",
                    "message": "old line",
                },
            )

            server.reset_device_logs_for_reconnect(handler.server, "44:1b:f6:df:5d:b8")

            path = Path(server.device_log_file_path(tmpdir, "44:1b:f6:df:5d:b8"))
            lines = path.read_text(encoding="utf-8").splitlines()
            self.assertEqual(len(lines), 1)
            self.assertIn("类型=连接", lines[0])
            self.assertIn("实时 WebSocket 已连接", lines[0])
            self.assertNotIn("old line", lines[0])
            self.assertEqual(len(handler.server.device_logs["44:1b:f6:df:5d:b8"]), 1)

    def test_device_log_format_is_chinese_and_message_stays_raw(self):
        line = server.format_device_log_line(
            {
                "type": "state_change",
                "source": "xiaopai-state",
                "device_id": "44:1b:f6:df:5d:b8",
                "from": "listening",
                "to": "speaking",
                "message": "APP1 status: Listening | WebSocket connected | Ready for voice |",
                "server_ts": 1783238866.5,
            }
        )

        self.assertIn("类型=状态变化", line)
        self.assertIn("来源=小派状态", line)
        self.assertIn("状态=监听中->播放中", line)
        self.assertIn("APP1 status: Listening", line)
        self.assertIn("WebSocket connected", line)
        self.assertNotIn("WebSocket 已连接", line)


if __name__ == "__main__":
    unittest.main()
