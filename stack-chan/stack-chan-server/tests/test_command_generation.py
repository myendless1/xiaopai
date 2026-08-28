import sys
import unittest
from pathlib import Path
from types import SimpleNamespace


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))

from server import apply_current_speech_generation  # noqa: E402


class CommandGenerationTest(unittest.TestCase):
    def make_server(self, generation):
        coordinator = SimpleNamespace(generation_for_device=lambda _device_id: generation)
        return SimpleNamespace(morrow_coordinator=coordinator)

    def test_direct_speak_uses_current_device_generation(self):
        payload = {"text": "你好"}

        apply_current_speech_generation(self.make_server(7), "robot-1", "speak", payload)

        self.assertEqual(payload["generation"], 7)

    def test_existing_dialogue_generation_is_preserved(self):
        payload = {"text": "已有回复", "generation": 5}

        apply_current_speech_generation(self.make_server(8), "robot-1", "speak", payload)

        self.assertEqual(payload["generation"], 5)

    def test_sequence_speech_steps_receive_generation(self):
        payload = [
            {"type": "face", "expression": "happy"},
            {"type": "speak", "text": "第一句"},
            {"type": "speak", "payload": {"text": "第二句"}},
        ]

        apply_current_speech_generation(self.make_server(9), "robot-1", "sequence", payload)

        self.assertNotIn("generation", payload[0])
        self.assertEqual(payload[1]["generation"], 9)
        self.assertEqual(payload[2]["payload"]["generation"], 9)


if __name__ == "__main__":
    unittest.main()
