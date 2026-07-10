import os
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))

from command_store import CommandStore  # noqa: E402
from database import Database  # noqa: E402
from morrow_coordinator import MorrowRequest, command_store_segment_sink  # noqa: E402


class MorrowCommandStoreTest(unittest.TestCase):
    def make_store(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        return CommandStore(Database(os.path.join(tmp.name, "xiaopai.sqlite3")))

    def test_segment_sink_persists_source_order_and_generation(self):
        store = self.make_store()
        sink = command_store_segment_sink(store)
        request = MorrowRequest("req-1", "问题", "robot-1", "voice", 1, 60, 3)
        sink(request, "第一句。", 0)
        sink(request, "第二句。", 1)

        first = store.lease_next_command("robot-1")
        store.record_ack({"cmd_id": first["cmd_id"], "state": "rendered"})
        second = store.lease_next_command("robot-1")

        self.assertEqual(first["payload"], {"text": "第一句。", "segment_index": 0, "generation": 3})
        self.assertEqual(second["payload"]["segment_index"], 1)
        with store.database.connect() as conn:
            rows = conn.execute("SELECT * FROM commands ORDER BY segment_index").fetchall()
        self.assertEqual(rows[0]["source_type"], "dialogue")
        self.assertEqual(rows[0]["source_id"], "req-1")
        self.assertEqual(rows[0]["turn_generation"], 3)

    def test_source_segment_unique_constraint_prevents_duplicate_speech(self):
        store = self.make_store()
        sink = command_store_segment_sink(store)
        request = MorrowRequest("req-1", "问题", "robot-1", "voice", 1, 60, 0)
        sink(request, "只播放一次。", 0)
        sink(request, "只播放一次。", 0)
        with store.database.connect() as conn:
            count = conn.execute("SELECT COUNT(*) FROM commands").fetchone()[0]
        self.assertEqual(count, 1)

    def test_cancel_old_generation_only_cancels_queued(self):
        store = self.make_store()
        sink = command_store_segment_sink(store)
        old = MorrowRequest("old", "问题", "robot-1", "voice", 1, 60, 0)
        current = MorrowRequest("new", "问题", "robot-1", "voice", 1, 60, 1)
        sink(old, "旧回复。", 0)
        sink(current, "新回复。", 0)

        self.assertEqual(store.cancel_pending_before_generation("robot-1", 1), 1)
        leased = store.lease_next_command("robot-1")
        self.assertEqual(leased["turn_id"], "new")

    def test_find_terminal_ack(self):
        store = self.make_store()
        sink = command_store_segment_sink(store)
        request = MorrowRequest("req-1", "问题", "robot-1", "voice", 1, 60, 0)
        sink(request, "完成。", 0)
        command = store.lease_next_command("robot-1")
        store.record_ack({"cmd_id": command["cmd_id"], "state": "rendered", "attempt": 1})
        self.assertEqual(store.find_terminal_ack(command["cmd_id"])["state"], "rendered")


if __name__ == "__main__":
    unittest.main()
