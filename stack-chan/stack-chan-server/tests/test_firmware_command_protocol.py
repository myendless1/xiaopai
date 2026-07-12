import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


class FirmwareCommandProtocolTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tts = (ROOT / "main" / "main_tts_commands.inc").read_text()
        cls.commands = (ROOT / "main" / "main_command_services.inc").read_text()
        cls.state = (ROOT / "main" / "main_app_state.inc").read_text()
        cls.realtime = (ROOT / "main" / "main_realtime_transport.inc").read_text()
        cls.touch = (ROOT / "main" / "main_head_touch.inc").read_text()
        cls.speech = (ROOT / "main" / "main_realtime_speech.inc").read_text()

    def test_tts_uses_post_v3_endpoint_without_legacy_ack_fallback(self):
        self.assertIn('make_server_url("/v3/tts")', self.tts)
        self.assertIn("HTTP_METHOD_POST", self.tts)
        self.assertNotIn('make_server_url("/device/ack")', self.tts)

    def test_attempt_and_ack_lifecycle_are_preserved(self):
        self.assertIn('json_int_value(command, "attempt", 0)', self.commands)
        self.assertIn('send_command_ack(item.cmd_id, "running"', self.tts)
        self.assertIn('ok ? "rendered" : "failed"', self.tts)
        received = self.commands.index('send_command_ack(cmd_id.c_str(), "received", "", attempt)')
        enqueue = self.commands.index("bool queued = enqueue_speak_command")
        self.assertGreater(received, enqueue)

    def test_speech_item_has_delivery_metadata_and_utf8_capacity(self):
        for field in ("attempt", "generation", "segment_index", "deadline_tick"):
            self.assertIn(field, self.state)
        self.assertIn("kSpeakCommandMaxTextBytes = 768", self.state)

    def test_dedupe_ack_replay_and_generation_stop_exist(self):
        self.assertIn("command_dedupe_entries[64]", self.state)
        self.assertIn("replay_pending_command_acks", self.commands)
        self.assertIn("advance_speech_generation", self.commands)

    def test_realtime_has_no_command_or_mcp_input(self):
        self.assertNotIn('type == "mcp"', self.realtime)
        self.assertNotIn('type == "command"', self.realtime)
        self.assertNotIn("tools/call", self.realtime)

    def test_local_long_press_stops_and_reports_generation(self):
        self.assertIn("kLocalStopLongPressMs = 1200", self.touch)
        self.assertIn("request_speak_preempt", self.touch)
        self.assertIn("advance_speech_generation", self.touch)
        self.assertIn('"local_stop"', self.touch)
        self.assertIn('make_server_url("/device/event")', self.touch)

    def test_terminal_non_idempotent_dedupe_is_persisted_in_nvs(self):
        self.assertIn('nvs_set_blob(handle, "cmd_dedupe"', self.tts)
        self.assertIn('nvs_get_blob(handle, "cmd_dedupe"', self.tts)
        self.assertIn("CommandDedupeEntry entries[32]", self.tts)
        self.assertIn("restore_terminal_command_dedupe_log", self.commands)

    def test_firmware_ui_names_morrow(self):
        self.assertNotIn("OpenClaw", self.speech)
        self.assertIn("Morrow is thinking", self.speech)

    def test_ota_checks_do_not_mark_user_interaction(self):
        helper_start = self.commands.index("static bool command_marks_user_interaction")
        helper_end = self.commands.index("static bool execute_command_object_internal", helper_start)
        helper = self.commands[helper_start:helper_end]
        for command_type in ('"check_ota"', '"ota_check"', '"firmware_ota"'):
            self.assertIn(f"type != {command_type}", helper)
        self.assertIn("if (command_marks_user_interaction(type))", self.commands)
        self.assertIn("if (command_marks_user_interaction(cmd_type))", self.commands)


if __name__ == "__main__":
    unittest.main()
