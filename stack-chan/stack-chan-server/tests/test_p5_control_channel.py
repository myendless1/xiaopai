import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class P5SingleControlChannelTest(unittest.TestCase):
    def test_obsolete_dispatch_modules_are_removed(self):
        self.assertFalse((ROOT / "src" / "mcp_client.py").exists())
        self.assertFalse((ROOT / "src" / "delivery_coordinator.py").exists())

    def test_server_has_no_delivery_agent_route_or_realtime_dispatch(self):
        source = (ROOT / "src" / "server.py").read_text()
        self.assertNotIn("DeliveryCoordinator", source)
        self.assertNotIn('"/v3/deliveries"', source)
        self.assertNotIn("manager.enqueue_command", source)
        self.assertNotIn("manager.set_device_state", source)

    def test_realtime_has_no_agent_or_device_command_fallback(self):
        source = (ROOT / "src" / "realtime_server.py").read_text()
        self.assertNotIn("OpenClawAgent", source)
        self.assertNotIn("_speak_openclaw_stream", source)
        self.assertNotIn("_send_mcp_command", source)
        self.assertNotIn("def enqueue_command", source)

    def test_realtime_protocol_has_no_mcp_frames(self):
        source = (ROOT / "src" / "realtime_protocol.py").read_text()
        self.assertNotIn("build_mcp_request", source)
        self.assertNotIn('"tools/call"', source)


if __name__ == "__main__":
    unittest.main()
