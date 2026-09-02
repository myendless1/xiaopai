import os
import sys
import tempfile
import unittest
import datetime as _dt
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))

from command_store import CommandStore
from database import Database
from schemas import CommandEnvelope
import server as srv_module


class FakeServer:
    def __init__(self, db_path):
        self.v3_database = Database(db_path)
        self.command_store = CommandStore(self.v3_database)
        self.max_sentence_chars = 120
        self.command_queue_max_size = 32
        self.device_lock = srv_module.threading.Lock()
        self.device_queues = {}
        self.last_seen = {}
        self.device_order = []


class MorrowP4NoticesTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.db_path = os.path.join(self.tmp.name, "xiaopai.sqlite3")
        self.server = FakeServer(self.db_path)

    def tearDown(self):
        self.tmp.cleanup()

    def test_save_morrow_notice_and_deduplication(self):
        # 1. Test saving a new notice
        notice = {
            "id": "meeting:event-123",
            "timestamp_ms": 1783250343202,
            "kind": "meeting_reminder",
            "text": "开会啦。"
        }
        
        # Must return True (inserted)
        success = srv_module.save_morrow_notice(self.server, notice)
        self.assertTrue(success)
        
        # Verify saved record in DB
        with self.server.v3_database.connect() as conn:
            row = conn.execute("SELECT * FROM morrow_notices WHERE notice_id=?", ("meeting:event-123",)).fetchone()
            self.assertIsNotNone(row)
            self.assertEqual(row["kind"], "meeting_reminder")
            self.assertEqual(row["text"], "开会啦。")
            self.assertEqual(row["state"], "received")
            self.assertEqual(row["timestamp_ms"], 1783250343202)
            self.assertIsNotNone(row["expires_at"])
            self.assertIsNotNone(row["received_at"])
            
        # 2. Test saving a duplicate notice (Must return False and not double insert)
        success_dup = srv_module.save_morrow_notice(self.server, notice)
        self.assertFalse(success_dup)

    def test_kind_ttl_and_notice_priority(self):
        # Meeting reminders keep their TTL but use normal dialogue priority.
        meeting_notice = {
            "id": "meeting:1",
            "kind": "meeting_reminder",
            "text": "开会。"
        }
        srv_module.save_morrow_notice(self.server, meeting_notice)
        with self.server.v3_database.connect() as conn:
            row_meeting = conn.execute("SELECT * FROM morrow_notices WHERE notice_id='meeting:1'").fetchone()
            expires_meeting = _dt.datetime.fromisoformat(row_meeting["expires_at"])
            received_meeting = _dt.datetime.fromisoformat(row_meeting["received_at"])
            self.assertAlmostEqual((expires_meeting - received_meeting).total_seconds(), 600, delta=5)

        self.server.last_seen["device-1"] = srv_module.time.time()
        self.server.device_order.append("device-1")
        self.assertTrue(srv_module.submit_morrow_notice_text(self.server, meeting_notice["id"]))
        meeting_command = self.server.command_store.lease_next_command("device-1")
        self.assertEqual(meeting_command["priority"], srv_module.DIALOGUE_COMMAND_PRIORITY)

        # fieldwork_reminder: TTL 1800
        fieldwork_notice = {
            "id": "fieldwork:1",
            "kind": "fieldwork_reminder",
            "text": "外勤。"
        }
        srv_module.save_morrow_notice(self.server, fieldwork_notice)
        with self.server.v3_database.connect() as conn:
            row_fieldwork = conn.execute("SELECT * FROM morrow_notices WHERE notice_id='fieldwork:1'").fetchone()
            expires_fieldwork = _dt.datetime.fromisoformat(row_fieldwork["expires_at"])
            received_fieldwork = _dt.datetime.fromisoformat(row_fieldwork["received_at"])
            self.assertAlmostEqual((expires_fieldwork - received_fieldwork).total_seconds(), 1800, delta=5)

    def test_notice_waits_until_dialogue_playback_finishes(self):
        dialogue = CommandEnvelope(
            cmd_id="dialogue-1",
            device_id="device-1",
            type="speak",
            payload={"text": "会议创建汇报。", "reply_end": True},
            priority=50,
            turn_id="turn-1",
            source_type="dialogue",
            source_id="turn-1",
        )
        self.server.command_store.create_command(dialogue)
        notice = {
            "id": "meeting:deferred",
            "kind": "meeting_reminder",
            "text": "五分钟后开会。",
        }
        srv_module.save_morrow_notice(self.server, notice)
        self.server.last_seen["device-1"] = srv_module.time.time()
        self.server.device_order.append("device-1")

        self.assertFalse(srv_module.submit_morrow_notice_text(self.server, notice["id"]))
        with self.server.v3_database.connect() as conn:
            state = conn.execute(
                "SELECT state FROM morrow_notices WHERE notice_id=?",
                (notice["id"],),
            ).fetchone()["state"]
        self.assertEqual(state, "received")

        self.server.command_store.record_ack({"cmd_id": dialogue.cmd_id, "state": "rendered"})
        self.assertTrue(srv_module.submit_morrow_notice_text(self.server, notice["id"]))
        queued_notice = self.server.command_store.lease_next_command("device-1")
        self.assertEqual(queued_notice["payload"]["text"], "五分钟后开会。")
        self.assertEqual(queued_notice["priority"], 50)

    def test_ack_linkage_lifecycle(self):
        notice = {
            "id": "meeting:ack-test",
            "kind": "meeting_reminder",
            "text": "测试链接状态。"
        }
        srv_module.save_morrow_notice(self.server, notice)
        
        # Mock device online
        self.server.last_seen["device-1"] = srv_module.time.time()
        self.server.device_order.append("device-1")
        
        # Submit the notice
        queued = srv_module.submit_morrow_notice_text(self.server, "meeting:ack-test")
        self.assertTrue(queued)
        
        # Verify it went to state 'queued' and command_id was populated
        with self.server.v3_database.connect() as conn:
            row = conn.execute("SELECT * FROM morrow_notices WHERE notice_id='meeting:ack-test'").fetchone()
            self.assertEqual(row["state"], "queued")
            cmd_id = row["command_id"]
            self.assertTrue(cmd_id.startswith("cmd_"))
            
        # Mock lease of command
        self.server.command_store.lease_next_command("device-1")
        
        # State should remain queued/leased
        with self.server.v3_database.connect() as conn:
            row = conn.execute("SELECT * FROM morrow_notices WHERE notice_id='meeting:ack-test'").fetchone()
            self.assertEqual(row["state"], "queued")

        # A transport-level received ACK must not move the notice back to the
        # outbox's "received" state, which would enqueue duplicate speech.
        self.server.command_store.record_ack({
            "cmd_id": cmd_id,
            "device_id": "device-1",
            "state": "received"
        })
        with self.server.v3_database.connect() as conn:
            row = conn.execute("SELECT * FROM morrow_notices WHERE notice_id='meeting:ack-test'").fetchone()
            self.assertEqual(row["state"], "queued")
            
        # Simulate device sending running ACK
        self.server.command_store.record_ack({
            "cmd_id": cmd_id,
            "device_id": "device-1",
            "state": "running"
        })
        with self.server.v3_database.connect() as conn:
            row = conn.execute("SELECT * FROM morrow_notices WHERE notice_id='meeting:ack-test'").fetchone()
            self.assertEqual(row["state"], "leased")

        # Simulate device sending rendered ACK
        self.server.command_store.record_ack({
            "cmd_id": cmd_id,
            "device_id": "device-1",
            "state": "rendered"
        })
        with self.server.v3_database.connect() as conn:
            row = conn.execute("SELECT * FROM morrow_notices WHERE notice_id='meeting:ack-test'").fetchone()
            self.assertEqual(row["state"], "rendered")
            self.assertIsNotNone(row["rendered_at"])

    def test_notice_expression_tag_is_removed_and_last_segment_ends_reply(self):
        notice = {
            "id": "meeting:expression",
            "kind": "meeting_reminder",
            "text": "<HaPpY>第一句。<unknown>第二句。",
        }
        srv_module.save_morrow_notice(self.server, notice)
        self.server.last_seen["device-1"] = srv_module.time.time()
        self.server.device_order.append("device-1")

        self.assertTrue(srv_module.submit_morrow_notice_text(self.server, notice["id"]))
        first = self.server.command_store.lease_next_command("device-1")
        self.server.command_store.record_ack({"cmd_id": first["cmd_id"], "state": "rendered"})
        second = self.server.command_store.lease_next_command("device-1")

        self.assertEqual(first["payload"]["text"], "第一句。")
        self.assertEqual(second["payload"]["text"], "第二句。")
        self.assertEqual(first["payload"]["expression"], "happy")
        self.assertEqual(second["payload"]["expression"], "happy")
        self.assertFalse(first["payload"]["reply_end"])
        self.assertTrue(second["payload"]["reply_end"])

    def test_notice_uses_current_device_generation(self):
        class FakeCoordinator:
            @staticmethod
            def generation_for_device(device_id):
                return 19 if device_id == "device-1" else 0

        self.server.morrow_coordinator = FakeCoordinator()
        notice = {
            "id": "meeting:generation",
            "kind": "meeting_reminder",
            "text": "十五分钟后开会。",
        }
        srv_module.save_morrow_notice(self.server, notice)
        self.server.last_seen["device-1"] = srv_module.time.time()
        self.server.device_order.append("device-1")

        self.assertTrue(srv_module.submit_morrow_notice_text(self.server, notice["id"]))
        command = self.server.command_store.lease_next_command("device-1")

        self.assertEqual(command["payload"]["generation"], 19)
        with self.server.v3_database.connect() as conn:
            row = conn.execute(
                "SELECT turn_generation FROM commands WHERE cmd_id=?",
                (command["cmd_id"],),
            ).fetchone()
        self.assertEqual(row["turn_generation"], 19)

    def test_reboot_recovery_and_expiration(self):
        # Notice in received state (representing saved but server restarted before queuing)
        notice = {
            "id": "meeting:reboot",
            "kind": "meeting_reminder",
            "text": "重启恢复测试。"
        }
        srv_module.save_morrow_notice(self.server, notice)
        
        # Verify initially 'received'
        with self.server.v3_database.connect() as conn:
            row = conn.execute("SELECT * FROM morrow_notices WHERE notice_id='meeting:reboot'").fetchone()
            self.assertEqual(row["state"], "received")
            
        # Mock device online
        self.server.last_seen["device-1"] = srv_module.time.time()
        self.server.device_order.append("device-1")
        
        # Run one step of outbox check / recovery loop
        # We simulate the inner block of run_morrow_notice_outbox_loop but in a controlled way:
        now_dt = srv_module._dt.datetime.now(srv_module._dt.timezone.utc)
        now_str = now_dt.isoformat()
        
        with self.server.v3_database.connect() as conn:
            rows = conn.execute(
                """
                SELECT notice_id FROM morrow_notices
                 WHERE state='received' AND expires_at >= ?
                """,
                (now_str,),
            ).fetchall()
            
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]["notice_id"], "meeting:reboot")
        
        # Recover by submitting
        srv_module.submit_morrow_notice_text(self.server, rows[0]["notice_id"])
        
        # Verify it transitioned to queued
        with self.server.v3_database.connect() as conn:
            row = conn.execute("SELECT * FROM morrow_notices WHERE notice_id='meeting:reboot'").fetchone()
            self.assertEqual(row["state"], "queued")


if __name__ == "__main__":
    unittest.main()
