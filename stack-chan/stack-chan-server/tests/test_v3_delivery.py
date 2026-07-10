import os
import sys
import tempfile
import unittest
import datetime as dt
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))

from command_store import CommandStore  # noqa: E402
from database import Database  # noqa: E402
from delivery_coordinator import DeliveryCoordinator  # noqa: E402


class V3DeliveryTest(unittest.TestCase):
    def make_store(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        database = Database(os.path.join(tmp.name, "xiaopai.sqlite3"))
        return CommandStore(database)

    def test_delivery_reaches_delivered_only_after_terminal_ack(self):
        store = self.make_store()
        enqueued = []
        coordinator = DeliveryCoordinator(store, enqueue=lambda device_id, command: enqueued.append((device_id, command)) or True)

        result = coordinator.submit(
            {
                "device_id": "robot-1",
                "event_id": "evt-1",
                "response": {
                    "speech": "五分钟后项目会议开始。",
                    "presentation": {"emotion": "thinking"},
                    "delivery_policy": {"presence_requirement": "preferred", "ttl_ms": 30000},
                },
            }
        )

        self.assertEqual(result["state"], "submitted")
        self.assertEqual(len(result["commands"]), 2)
        self.assertEqual(len(enqueued), 2)

        speak = next(command for _device_id, command in enqueued if command["type"] == "speak")
        store.record_ack({"device_id": "robot-1", "cmd_id": speak["cmd_id"], "state": "received"})
        self.assertEqual(store.get_delivery(result["delivery_id"])["state"], "submitted")

        for _device_id, command in enqueued:
            terminal = "rendered" if command["type"] == "speak" else "done"
            store.record_ack({"device_id": "robot-1", "cmd_id": command["cmd_id"], "state": terminal})

        self.assertEqual(store.get_delivery(result["delivery_id"])["state"], "delivered")

    def test_lease_next_command_returns_v3_envelope(self):
        store = self.make_store()
        coordinator = DeliveryCoordinator(store)
        result = coordinator.submit({"device_id": "robot-1", "speech": "你好。"})

        leased = store.lease_next_command("robot-1", boot_id=7)

        self.assertIsNotNone(leased)
        self.assertEqual(leased["type"], "speak")
        self.assertEqual(leased["attempt"], 1)
        self.assertEqual(leased["payload"]["text"], "你好。")
        self.assertEqual(store.get_delivery(result["delivery_id"])["commands"][0]["state"], "leased")

    def test_sequence_action_uses_device_array_payload(self):
        store = self.make_store()
        enqueued = []
        coordinator = DeliveryCoordinator(store, enqueue=lambda device_id, command: enqueued.append(command) or True)

        coordinator.submit(
            {
                "device_id": "robot-1",
                "response": {
                    "actions": [
                        {
                            "type": "sequence",
                            "steps": [
                                {"type": "face", "expression": "thinking"},
                                {"type": "speak", "text": "收到。"},
                            ],
                        }
                    ]
                },
            }
        )

        self.assertEqual(enqueued[0]["type"], "sequence")
        self.assertEqual(enqueued[0]["payload"], [{"type": "face", "expression": "thinking"}, {"type": "speak", "text": "收到。"}])

    def test_exhausted_lease_marks_delivery_failed(self):
        store = self.make_store()
        coordinator = DeliveryCoordinator(store)
        result = coordinator.submit(
            {
                "device_id": "robot-1",
                "speech": "你好。",
                "delivery_policy": {"max_attempts": 1, "ttl_ms": 30000},
            }
        )
        leased = store.lease_next_command("robot-1", boot_id=1, lease_ms=1)
        self.assertIsNotNone(leased)

        past = (dt.datetime.now(dt.timezone.utc) - dt.timedelta(seconds=1)).isoformat()
        with store.database.connect() as conn:
            conn.execute("UPDATE commands SET lease_expires_at=? WHERE cmd_id=?", (past, leased["cmd_id"]))

        self.assertIsNone(store.lease_next_command("robot-1", boot_id=1, lease_ms=1))
        self.assertEqual(store.get_delivery(result["delivery_id"])["state"], "failed")


if __name__ == "__main__":
    unittest.main()
