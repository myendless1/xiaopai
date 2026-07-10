import sys
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))

from morrow_client import MorrowClient  # noqa: E402


class P7CleanupObservabilityTest(unittest.TestCase):
    def test_openclaw_sources_and_config_are_gone(self):
        self.assertFalse((ROOT / "src" / "openclaw_agent.py").exists())
        self.assertFalse((ROOT / "src" / "xiaopai_openclaw_prompt.py").exists())
        for path in (ROOT / "src").glob("*.py"):
            self.assertNotIn("openclaw", path.read_text().lower(), path.name)

    def test_required_metrics_are_declared(self):
        client = MorrowClient(base_url="http://morrow:3000", websocket_factory=lambda *_a, **_k: None)
        self.assertIn("morrow_ws_reconnect_total", client.metrics)
        self.assertIn("morrow_notice_received_total", client.metrics)
        server_source = (ROOT / "src" / "server.py").read_text()
        self.assertIn('path == "/metrics"', server_source)
        self.assertIn("morrow_turn_rejected_total", (ROOT / "src" / "morrow_coordinator.py").read_text())


if __name__ == "__main__":
    unittest.main()
