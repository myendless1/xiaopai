import os
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
FIRMWARE_ROOT = ROOT.parent
sys.path.insert(0, str(ROOT / "src"))

from command_store import CommandStore  # noqa: E402
from database import Database  # noqa: E402
import server  # noqa: E402


class FakeServer:
    def __init__(self, database_path: str):
        self.v3_database = Database(database_path)
        self.command_store = CommandStore(self.v3_database)
        self.command_queue_max_size = 8
        self.device_lock = server.threading.Lock()
        self.device_queues = {}
        self.last_seen = {}
        self.device_order = []
        self.realtime_manager = None
        self.find_owner_gain_x = 1.1
        self.find_owner_gain_y = 0.7
        self.find_owner_stop_pixels = 28.0
        self.sedentary_reminder_queued_total = 0


class PeriodicSedentaryReminderTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.server = FakeServer(os.path.join(self.tmp.name, "xiaopai.sqlite3"))

    def set_online(self, device_id: str = "robot-1") -> None:
        self.server.last_seen[device_id] = server.time.time()
        self.server.device_order.append(device_id)

    def test_default_interval_is_thirty_minutes(self):
        self.assertEqual(server.DEFAULT_SEDENTARY_REMINDER_INTERVAL_SECONDS, 1800)

    def test_online_device_receives_face_gated_find_owner_command(self):
        self.set_online()

        queued = server.enqueue_sedentary_reminder_once(
            self.server,
            reminder_index=0,
            trigger_id="sedentary:test-1",
        )

        self.assertEqual(queued, 1)
        command = self.server.device_queues["robot-1"].get_nowait()
        self.assertEqual(command["type"], "find_owner")
        self.assertFalse(command["interrupt"])
        self.assertTrue(command["discardable"])
        self.assertEqual(command["coalesce_key"], "sedentary_timer")
        self.assertEqual(command["ttl_seconds"], 300)
        self.assertEqual(
            command["payload"],
            {
                "rounds": 1,
                "reply": "你已连续工作好长时间啦，起身拉伸一下吧。",
                "trigger_id": "sedentary:test-1",
                "speak": True,
                "preserve_speech": True,
                "wait_for_speech": True,
                "gain_x": 1.1,
                "gain_y": 0.7,
                "stop_pixels": 28.0,
                "speaker_volume": 10,
            },
        )
        self.assertEqual(self.server.sedentary_reminder_queued_total, 1)

        leased = self.server.command_store.lease_next_command("robot-1")
        self.assertEqual(leased["type"], "find_owner")
        with self.server.v3_database.connect() as conn:
            row = conn.execute("SELECT source_type, source_id FROM commands").fetchone()
        self.assertEqual(row["source_type"], "sedentary_timer")
        self.assertEqual(row["source_id"], "robot-1")

    def test_offline_device_is_skipped_without_queueing(self):
        self.assertEqual(server.enqueue_sedentary_reminder_once(self.server), 0)
        self.assertEqual(self.server.device_queues, {})
        self.assertEqual(self.server.sedentary_reminder_queued_total, 0)

    def test_pending_timer_command_is_coalesced(self):
        self.set_online()
        server.enqueue_sedentary_reminder_once(self.server, 0, trigger_id="sedentary:first")
        server.enqueue_sedentary_reminder_once(self.server, 1, trigger_id="sedentary:second")

        queue = self.server.device_queues["robot-1"]
        self.assertEqual(queue.qsize(), 1)
        self.assertEqual(queue.get_nowait()["payload"]["reply"], "小派观察到你一直在忙，站起来活动两分钟吧。")
        with self.server.v3_database.connect() as conn:
            states = [row["state"] for row in conn.execute("SELECT state FROM commands ORDER BY created_at")]
        self.assertEqual(states, ["cancelled", "queued"])

    def test_existing_firmware_find_owner_speaks_only_after_a_face(self):
        camera_source = (FIRMWARE_ROOT / "main" / "main_camera_motion.inc").read_text()
        no_face_guard = camera_source.index("if (!result.saw_face)")
        reply_call = camera_source.index("execute_speak_command_internal(reply", no_face_guard)
        align_call = camera_source.index("refine_head_toward_face", camera_source.index("run_find_owner_detection"))

        self.assertLess(no_face_guard, reply_call)
        self.assertLess(align_call, reply_call)


if __name__ == "__main__":
    unittest.main()
