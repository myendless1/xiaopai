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
        cls.ota = (ROOT / "main" / "main_firmware_ota.inc").read_text()
        cls.speech = (ROOT / "main" / "main_realtime_speech.inc").read_text()
        cls.expression_state = (ROOT / "main" / "expression_state.cpp").read_text()
        cls.expression_header = (ROOT / "main" / "expression_state.h").read_text()
        cls.expression_controller = (ROOT / "main" / "expression_controller.cpp").read_text()
        cls.main = (ROOT / "main" / "main.cpp").read_text()
        cls.camera_motion = (ROOT / "main" / "main_camera_motion.inc").read_text()
        cls.dji_power = (ROOT / "main" / "audio" / "dji_mic_usb_power.cpp").read_text()

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
        for field in (
            "attempt",
            "generation",
            "segment_index",
            "deadline_tick",
            "turn_id",
            "expression",
            "reply_end",
            "reply_cancelled",
            "speaker_volume",
        ):
            self.assertIn(field, self.state)
        self.assertIn("kSpeakCommandMaxTextBytes = 768", self.state)
        self.assertIn("kSpeakCommandQueueCapacity = 4", self.state)

    def test_speech_queue_credit_is_reported_and_full_queue_defers(self):
        self.assertIn('url += "&speech_queue_depth="', self.commands)
        self.assertIn('url += "&speech_queue_capacity="', self.commands)
        self.assertIn('queue_full ? "deferred" : "failed"', self.commands)
        self.assertIn('"speak queue full; retry"', self.commands)

    def test_server_speaker_volume_is_applied_at_playback_and_to_find_owner(self):
        self.assertIn('json_int_value(payload, "speaker_volume", -1)', self.commands)
        self.assertIn("item.speaker_volume = speaker_volume", self.tts)
        self.assertIn("item.speaker_volume >= 0", self.tts)
        self.assertIn("clamp_speaker_volume_percent(item.speaker_volume)", self.tts)
        find_owner = self.commands.index('if (type == "find_owner" || type == "locate_owner")')
        apply_volume = self.commands.index("apply_command_speaker_volume(payload)", find_owner)
        run_find_owner = self.commands.index("return run_find_owner_command", find_owner)
        self.assertLess(apply_volume, run_find_owner)

    def test_volume_command_applies_immediately_and_queues_confirmation(self):
        start = self.tts.index("static bool execute_volume_command")
        end = self.tts.index("\n}", start)
        volume_command = self.tts[start:end]

        apply_volume = volume_command.index("apply_speaker_volume();")
        queue_reply = volume_command.index('enqueue_speak_command("", reply, "", true)')
        self.assertLess(apply_volume, queue_reply)
        self.assertIn('snprintf(reply, sizeof(reply), "%d", speaker_volume_percent)', volume_command)
        self.assertNotIn("已经将声音调到", volume_command)
        self.assertNotIn("execute_speak_command", volume_command)

    def test_boot_pose_and_find_owner_scan_order(self):
        self.assertIn("kTrackingHomePitchDeg = 45.0f", self.state)
        self.assertIn("kFindOwnerLookUpDeltaDeg = 30.0f", self.state)
        self.assertIn("kTrackingScanLowPitchDeg = 10.0f", self.state)
        self.assertIn("kTrackingScanHighPitchDeg = 32.0f", self.state)
        self.assertIn("move_head_to_tracking_angles(0.0f, kTrackingHomePitchDeg, 800)", self.main)

        scan_start = self.camera_motion.index("const ScanPose scan_poses[]")
        scan_end = self.camera_motion.index("};", scan_start)
        scan = self.camera_motion[scan_start:scan_end]
        up = scan.index('{"Up"')
        center = scan.index('{"Center"')
        left = scan.index('{"Left"')
        right = scan.index('{"Right"')
        self.assertLess(up, center)
        self.assertLess(center, left)
        self.assertLess(left, right)

        find_owner = self.camera_motion.index("static FindOwnerResult run_find_owner_detection")
        ordered_scan = self.camera_motion.index("scan_for_face_near_home(&target)", find_owner)
        initial_capture = self.camera_motion.find('capture_face_at_current_pose("Find Owner", "Initial photo"', find_owner)
        self.assertGreater(ordered_scan, find_owner)
        self.assertEqual(initial_capture, -1)

    def test_firmware_fallback_speaker_volume_is_ten_percent(self):
        kconfig = (ROOT / "main" / "Kconfig.projbuild").read_text()
        defaults = (ROOT / "sdkconfig.defaults").read_text()
        audio_service = (ROOT / "main" / "audio" / "xiaopai_audio_service.cpp").read_text()
        self.assertIn('int "Default output volume percent"\n        default 10', kconfig)
        self.assertIn("CONFIG_STACKCHAN_AUDIO_OUTPUT_VOLUME_DEFAULT=10", defaults)
        self.assertIn("#define CONFIG_STACKCHAN_AUDIO_OUTPUT_VOLUME_DEFAULT 10", audio_service)

    def test_dedupe_ack_replay_and_generation_stop_exist(self):
        self.assertIn("command_dedupe_entries[64]", self.state)
        self.assertIn("replay_pending_command_acks", self.commands)
        self.assertIn("advance_speech_generation", self.commands)

    def test_realtime_has_no_command_or_mcp_input(self):
        self.assertNotIn('type == "mcp"', self.realtime)
        self.assertNotIn('type == "command"', self.realtime)
        self.assertNotIn("tools/call", self.realtime)

    def test_realtime_final_stt_stops_audio_upstream_without_long_write_block(self):
        self.assertIn('json_bool_value(root, "is_final", false)', self.realtime)
        self.assertIn("bool* stt_is_final", self.realtime)
        self.assertIn("!stt_is_final", self.speech)
        self.assertIn("Stop recording because final STT was received", self.speech)
        self.assertIn("Skip listen stop because Server already finalized", self.speech)
        self.assertIn('"audio", kRealtimeAudioWriteTimeoutMs, 1', self.speech)
        self.assertIn("kRealtimeAudioWriteTimeoutMs = 500", self.state)

    def test_realtime_ws_read_waits_for_complete_frame_without_reconnecting_on_control_frames(self):
        self.assertIn("kRealtimeWsFrameReadTimeoutMs = 250", self.state)
        self.assertIn(
            "std::max(timeout_ms, kRealtimeWsFrameReadTimeoutMs)",
            self.realtime,
        )
        self.assertIn("if (read_len == 0)", self.realtime)
        self.assertIn("WS read completed without application payload", self.realtime)
        receive_start = self.realtime.index("static bool receive_ws_once")
        receive_body = self.realtime[receive_start:]
        self.assertNotIn("if (read_len <= 0)", receive_body)

    def test_uart_debug_interface_reuses_device_command_protocol(self):
        self.assertIn("SERIAL_CMD ready transport=uart0 format=jsonl", self.commands)
        self.assertIn("execute_serial_debug_line", self.commands)
        self.assertIn("execute_command_object(command)", self.commands)
        self.assertIn("discard_until_newline", self.commands)
        self.assertIn("reason=line_too_long", self.commands)
        self.assertIn("enqueue_serial_debug_speak", self.commands)
        self.assertIn("speak_sequence_not_supported", self.commands)
        self.assertIn("#if CONFIG_STACKCHAN_SERIAL_DEBUG_COMMAND", self.main)
        self.assertIn("# CONFIG_STACKCHAN_SERIAL_DEBUG_COMMAND is not set", (ROOT / "sdkconfig.defaults").read_text())

    def test_network_debug_is_runtime_dormant_by_default(self):
        kconfig = (ROOT / "main" / "Kconfig.projbuild").read_text()
        defaults = (ROOT / "sdkconfig.defaults").read_text()
        self.assertIn('bool "Enable permanent Wi-Fi command and log debugging"', kconfig)
        self.assertIn("CONFIG_STACKCHAN_NETWORK_DEBUG=y", defaults)
        self.assertIn("CONFIG_STACKCHAN_NETWORK_DEBUG_QUEUE_DEPTH=64", defaults)
        self.assertIn("xQueueCreateStatic(", self.commands)
        self.assertIn("network_debug_enabled{false}", self.commands)
        self.assertIn("load_network_debug_preference();", self.main)
        self.assertIn("ulTaskNotifyTake(pdTRUE, portMAX_DELAY)", self.commands)
        self.assertIn("network_debug_uploader_task", self.commands)
        self.assertIn('type == "network_debug"', self.commands)
        self.assertIn("persist_network_debug_preference", self.commands)
        self.assertIn('esp_log_level_set("*", enabled ? ESP_LOG_INFO : ESP_LOG_WARN)', self.commands)
        self.assertNotIn("network_debug_flush_once();", self.commands)
        self.assertNotIn("CONFIG_STACKCHAN_NETWORK_DEBUG_RING_BYTES", defaults)
        self.assertIn('make_server_url("/device/logs")', self.commands)
        self.assertIn('type == "debug_status"', self.commands)
        self.assertIn('type == "debug_log_level"', self.commands)
        self.assertIn('cJSON_CreateString("network_debug")', self.commands)
        self.assertIn("start_network_debug_service();", self.main)
        self.assertIn("usb_serial_jtag_is_connected()", self.main)
        self.assertIn('"PC USB host detected"', self.main)
        self.assertIn(
            "#if CONFIG_STACKCHAN_DJI_MIC_USB_INPUT && CONFIG_STACKCHAN_DJI_MIC_AUTO_START",
            self.main,
        )
        self.assertNotIn(
            "#if CONFIG_STACKCHAN_DJI_MIC_USB_INPUT && !CONFIG_STACKCHAN_DJI_MIC_AUTO_START",
            self.main,
        )
        self.assertIn('type == "dji_mic_start"', self.commands)
        self.assertIn('type == "dji_mic_stop"', self.commands)

    def test_display_brightness_defaults_to_seventy_and_is_persistent(self):
        self.assertIn("kDefaultDisplayBrightnessPercent = 70", self.main)
        self.assertIn('kDisplayBrightnessNvsKey[] = "brightness"', self.main)
        self.assertIn("load_display_brightness_preference();", self.main)
        self.assertIn("set_display_brightness(percent, true)", self.commands)
        self.assertIn('cJSON_CreateString("display_brightness")', self.commands)
        self.assertIn('"display_brightness"', self.commands)

    def test_dji_power_sequence_does_not_change_display_brightness(self):
        self.assertNotIn("setBrightness", self.dji_power)
        self.assertNotIn("dim_display_for_vbus", self.dji_power)

    def test_three_second_screen_press_resets_speech_queue_and_session(self):
        self.assertIn("kSessionResetLongPressMs = 3000", self.touch)
        self.assertIn('"reset_session"', self.touch)
        self.assertIn('make_server_url("/device/event")', self.touch)
        long_press_handler = self.touch[
            self.touch.index("if (event == HeadTouchEvent::LongPress)"):
            self.touch.index("LocalVoiceState state = xiaopai_state_get().state;")
        ]
        self.assertIn('request_speak_preempt("session reset")', long_press_handler)
        self.assertIn("advance_speech_generation();", long_press_handler)
        self.assertNotIn("xiaopai_state_set", long_press_handler)
        self.assertNotIn("expression_state_set", long_press_handler)
        self.assertIn('"generation"', long_press_handler)

    def test_firmware_ota_stops_dji_and_delays_boot_autostart(self):
        self.assertIn('dji_mic_receiver_input_stop("firmware ota")', self.ota)
        self.assertIn("while (s_firmware_ota_in_progress.load())", self.main)

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

    def test_reply_expression_starts_on_first_pcm_and_ends_from_fifo_control_item(self):
        enqueue = self.tts.index("audio_service_play_pcm_24k")
        expression_start = self.tts.index("expression_state_reply_audio_started", enqueue)
        self.assertGreater(expression_start, enqueue)
        self.assertIn("expression_state_reply_prepare(item.turn_id", self.tts)
        self.assertIn("if (item.text[0] == '\\0')", self.tts)
        self.assertIn("expression_state_reply_end(item.turn_id", self.tts)
        self.assertIn("expression_state_reply_segment_finished(item.turn_id", self.tts)

    def test_reply_state_has_release_hold_watchdog_and_generation_matching(self):
        for phase in ("PendingAudio", "Playing", "BetweenSegments", "Releasing"):
            self.assertIn(phase, self.expression_header)
        self.assertIn("kReplyReleaseMs = 200", self.expression_state)
        self.assertIn("kReplyWatchdogMs = 20000", self.expression_state)
        self.assertIn("reply_session.generation == generation", self.expression_state)
        self.assertIn('show_expression(kDefaultExpression)', self.expression_state)
        self.assertIn('strcmp(requested, "happy") == 0 || strcmp(requested, "thinking") == 0', self.expression_state)
        self.assertNotIn('strcmp(requested, "surprised")', self.expression_state)

    def test_reply_voice_stays_speaking_between_segments_and_uses_fifo_end(self):
        self.assertIn("enum class ReplyVoicePhase", self.state)
        self.assertIn("PendingAudio", self.state)
        self.assertIn("BetweenSegments", self.state)
        self.assertIn("reply_voice_audio_started(active_speak_turn_id", self.tts)
        self.assertIn("reply_voice_segment_finished(item.turn_id", self.tts)
        self.assertIn('item.reply_cancelled ? "reply cancelled control" : "reply end control"', self.tts)
        self.assertIn("xiaopai_state_set(LocalVoiceState::Listening, reason)", self.tts)
        queued_call = self.tts.index("ok = execute_speak_command_internal(item.text")
        self.assertIn("item.cmd_id, false", self.tts[queued_call:queued_call + 600])

    def test_reply_segment_failure_keeps_multi_segment_turn_alive(self):
        self.assertIn("active_speak_segment_audio_started = false", self.tts)
        self.assertIn("if (!ok && segment_audio_started && reply_audio_started && !app2_stop_requested)", self.tts)
        self.assertIn("else if (!app2_stop_requested && !item.reply_end)", self.tts)
        self.assertIn("keeping later reply segments", self.tts)

    def test_reply_waiting_and_between_segment_timeouts_are_thirty_seconds(self):
        self.assertIn("kReplyContinuationTimeoutMs = 30 * 1000", self.state)
        self.assertIn("kWaitingReplyTimeoutMs = kReplyContinuationTimeoutMs", self.state)
        self.assertIn("run_reply_voice_watchdog", self.tts)
        self.assertIn("reply_continuation_timeout", self.tts)
        self.assertGreaterEqual(self.commands.count('"speech_generation"'), 2)
        self.assertIn("if (reply_voice_is_active())", self.speech)
        self.assertIn('xiaopai_state_set(LocalVoiceState::Listening, "Morrow waiting timeout")', self.speech)
        self.assertNotIn('xiaopai_state_set(current.state, "Morrow waiting timeout")', self.speech)

    def test_followup_speak_does_not_wake_device_during_reply_preparation(self):
        self.assertIn('strcmp(type, "speak") == 0', self.realtime)
        self.assertIn("current == LocalVoiceState::Waiting || reply_voice_is_active()", self.realtime)

    def test_expression_renderer_draws_static_mouths_on_old_anchors(self):
        draw_start = self.expression_controller.index("static void draw_face_locked")
        draw_end = self.expression_controller.index("static bool ensure_face_canvas_locked", draw_start)
        draw_face = self.expression_controller[draw_start:draw_end]
        self.assertIn("draw_mouth_locked", draw_face)
        self.assertIn("draw_cheek_pair_locked", draw_face)
        self.assertIn("draw_styled_eyes_locked", draw_face)
        self.assertNotIn("draw_eye_only_face_locked", draw_face)
        self.assertIn("kMouthCenter = {160, 152}", self.expression_controller)
        self.assertIn("kLeftEyeCenter = {88, 101}", self.expression_controller)
        for expression in ("Happy", "Thinking", "Surprised", "Calm", "Shy"):
            self.assertIn(f"FaceKind::{expression}", self.expression_controller)


if __name__ == "__main__":
    unittest.main()
