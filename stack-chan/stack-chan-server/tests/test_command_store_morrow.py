import os
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))

from command_store import CommandStore  # noqa: E402
from database import Database  # noqa: E402
from morrow_coordinator import (  # noqa: E402
    DIALOGUE_COMMAND_TTL_MS,
    MorrowRequest,
    command_store_reply_end_sink,
    command_store_segment_sink,
)
from schemas import AdmissionPolicy, CommandEnvelope  # noqa: E402


class MorrowCommandStoreTest(unittest.TestCase):
    def make_store(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        return CommandStore(Database(os.path.join(tmp.name, "xiaopai.sqlite3")))

    def test_segment_sink_persists_source_order_and_generation(self):
        store = self.make_store()
        sink = command_store_segment_sink(store)
        request = MorrowRequest("req-1", "问题", "robot-1", "voice", 1, 60, 3, "happy")
        sink(request, "第一句。", 0)
        sink(request, "第二句。", 1)

        first = store.lease_next_command("robot-1")
        store.record_ack({"cmd_id": first["cmd_id"], "state": "rendered"})
        second = store.lease_next_command("robot-1")

        self.assertEqual(
            first["payload"],
            {
                "text": "第一句。",
                "expression": "happy",
                "turn_id": "req-1",
                "segment_index": 0,
                "generation": 3,
                "reply_end": False,
                "speaker_volume": 10,
            },
        )
        self.assertEqual(second["payload"]["segment_index"], 1)
        with store.database.connect() as conn:
            rows = conn.execute("SELECT * FROM commands ORDER BY segment_index").fetchall()
        self.assertEqual(rows[0]["source_type"], "dialogue")
        self.assertEqual(rows[0]["source_id"], "req-1")
        self.assertEqual(rows[0]["turn_generation"], 3)
        self.assertEqual(rows[0]["ttl_ms"], DIALOGUE_COMMAND_TTL_MS)
        self.assertEqual(rows[0]["expires_at"], "")

    def test_segment_sink_reads_current_global_speaker_volume(self):
        store = self.make_store()
        current = {"value": 10}
        sink = command_store_segment_sink(store, speaker_volume=lambda: current["value"])
        request = MorrowRequest("req-1", "问题", "robot-1", "voice", 1, 60, 3, "happy")

        sink(request, "第一句。", 0)
        current["value"] = 30
        sink(request, "第二句。", 1)

        first = store.lease_next_command("robot-1")
        store.record_ack({"cmd_id": first["cmd_id"], "state": "rendered"})
        second = store.lease_next_command("robot-1")
        self.assertEqual(first["payload"]["speaker_volume"], 10)
        self.assertEqual(second["payload"]["speaker_volume"], 30)

    def test_reply_end_sink_follows_segments_in_the_same_fifo(self):
        store = self.make_store()
        segment_sink = command_store_segment_sink(store)
        end_sink = command_store_reply_end_sink(store)
        request = MorrowRequest("req-1", "问题", "robot-1", "voice", 1, 60, 3, "surprised")
        segment_sink(request, "回复。", 0)
        end_sink(request, 1, "saved")

        first = store.lease_next_command("robot-1")
        store.record_ack({"cmd_id": first["cmd_id"], "state": "rendered"})
        ending = store.lease_next_command("robot-1")

        self.assertEqual(ending["payload"]["text"], "")
        self.assertEqual(ending["payload"]["expression"], "surprised")
        self.assertTrue(ending["payload"]["reply_end"])
        self.assertFalse(ending["payload"]["reply_cancelled"])
        self.assertEqual(ending["payload"]["segment_index"], 1)

    def test_queue_order_does_not_depend_on_wall_clock(self):
        store = self.make_store()

        def command(cmd_id, text, created_at):
            return CommandEnvelope(
                cmd_id=cmd_id,
                device_id="robot-1",
                type="speak",
                payload={"text": text},
                priority=50,
                admission=AdmissionPolicy(),
                created_at=created_at,
            )

        store.create_command(command("first", "第一句。", "2026-08-06T00:00:02+00:00"))
        store.create_command(command("second", "第二句。", "2026-08-06T00:00:01+00:00"))

        first = store.lease_next_command("robot-1")
        store.record_ack({"cmd_id": first["cmd_id"], "state": "rendered"})
        second = store.lease_next_command("robot-1")

        self.assertEqual(first["cmd_id"], "first")
        self.assertEqual(second["cmd_id"], "second")

    def test_unfinished_dialogue_tracks_device_playback(self):
        store = self.make_store()
        sink = command_store_segment_sink(store)
        request = MorrowRequest("req-pending", "问题", "robot-1", "voice", 1, 60, 3)
        sink(request, "还在播放。", 0)

        self.assertTrue(store.has_unfinished_dialogue("robot-1"))
        command = store.lease_next_command("robot-1")
        store.record_ack({"cmd_id": command["cmd_id"], "state": "rendered"})
        self.assertFalse(store.has_unfinished_dialogue("robot-1"))

    def test_full_device_queue_leaves_speech_queued_on_server(self):
        store = self.make_store()
        sink = command_store_segment_sink(store)
        request = MorrowRequest("req-credit", "问题", "robot-1", "voice", 1, 60, 3, "happy")
        sink(request, "等设备有空位再播放。", 0)

        self.assertIsNone(store.lease_next_command("robot-1", allow_speak=False))
        with store.database.connect() as conn:
            state = conn.execute("SELECT state FROM commands").fetchone()[0]
        self.assertEqual(state, "queued")

        leased = store.lease_next_command("robot-1", allow_speak=True)
        self.assertEqual(leased["payload"]["text"], "等设备有空位再播放。")

    def test_find_owner_stays_queued_while_dialogue_is_pending(self):
        store = self.make_store()
        store.create_command(
            CommandEnvelope(
                cmd_id="find-owner",
                device_id="robot-1",
                type="find_owner",
                payload={"reply": "提醒"},
                priority=85,
            )
        )

        self.assertIsNone(store.lease_next_command("robot-1", allow_find_owner=False))
        with store.database.connect() as conn:
            state = conn.execute("SELECT state FROM commands WHERE cmd_id='find-owner'").fetchone()[0]
        self.assertEqual(state, "queued")
        self.assertEqual(store.lease_next_command("robot-1", allow_find_owner=True)["cmd_id"], "find-owner")

    def test_device_deferred_ack_requeues_without_losing_order(self):
        store = self.make_store()
        sink = command_store_segment_sink(store)
        request = MorrowRequest("req-retry", "问题", "robot-1", "voice", 1, 60, 3, "happy")
        sink(request, "第一句。", 0)
        sink(request, "第二句。", 1)

        first = store.lease_next_command("robot-1")
        for _ in range(5):
            result = store.record_ack(
                {
                    "cmd_id": first["cmd_id"],
                    "state": "deferred",
                    "message": "speak queue full; retry",
                    "attempt": first["attempt"],
                }
            )
            self.assertEqual(result["state"], "queued")
            first = store.lease_next_command("robot-1")
            self.assertEqual(first["attempt"], 1)

        store.record_ack(
            {
                "cmd_id": first["cmd_id"],
                "state": "deferred",
                "message": "speak queue full; retry",
                "attempt": first["attempt"],
            }
        )
        retried = store.lease_next_command("robot-1")
        self.assertEqual(retried["cmd_id"], first["cmd_id"])
        self.assertEqual(retried["payload"]["segment_index"], 0)
        store.record_ack({"cmd_id": retried["cmd_id"], "state": "rendered"})
        second = store.lease_next_command("robot-1")
        self.assertEqual(second["payload"]["segment_index"], 1)

    def test_live_queue_keeps_every_segment_and_end_control_in_fifo_order(self):
        store = self.make_store()
        enqueued = []
        segment_sink = command_store_segment_sink(store, enqueue=lambda _device, command: enqueued.append(command))
        end_sink = command_store_reply_end_sink(store, enqueue=lambda _device, command: enqueued.append(command))
        request = MorrowRequest("req-fifo", "问题", "robot-1", "voice", 1, 60, 3, "happy")

        segment_sink(request, "第一句。", 0)
        segment_sink(request, "第二句。", 1)
        end_sink(request, 2, "saved")

        self.assertEqual([item["payload"]["segment_index"] for item in enqueued], [0, 1, 2])
        self.assertEqual(len({item["coalesce_key"] for item in enqueued}), 3)
        self.assertTrue(all(item["discardable"] is False for item in enqueued))
        self.assertTrue(enqueued[-1]["payload"]["reply_end"])

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

    def test_speech_generation_is_monotonic_and_persistent(self):
        store = self.make_store()
        store.set_speech_generation("robot-1", 19)
        store.set_speech_generation("robot-1", 7)

        self.assertEqual(store.speech_generations(), {"robot-1": 19})

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
