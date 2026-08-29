import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class WebRemoteControlTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.page = (ROOT / "static" / "web" / "index.html").read_text(encoding="utf-8")

    def test_controls_start_disabled_and_only_offer_online_devices(self):
        self.assertIn('id="remoteSpeech"', self.page)
        self.assertIn('id="remoteSpeak" type="button" disabled', self.page)
        self.assertIn('id="remoteFindOwner" type="button" disabled', self.page)
        self.assertIn('id="remoteRehearsal" type="button" disabled', self.page)
        self.assertIn("renderRemoteControls(onlineDevices);", self.page)
        self.assertIn("const multiple = onlineRemoteDevices.length > 1;", self.page)
        self.assertIn("els.remoteDevice.hidden = !multiple;", self.page)

    def test_speak_and_find_owner_commands_target_the_selected_device(self):
        self.assertIn("'/command/speak'", self.page)
        self.assertIn("'/command/find_owner'", self.page)
        self.assertIn("{ speak: 'false', interrupt: 'true' }", self.page)
        self.assertIn("new URLSearchParams({ device_id: deviceId, ...params })", self.page)

    def test_rehearsal_uses_the_face_gated_reply(self):
        self.assertIn(
            "const REHEARSAL_REPLY = '我看你站在这里好久啦，有什么需要帮助的吗？';",
            self.page,
        )
        self.assertIn("{ reply: REHEARSAL_REPLY, interrupt: 'true' }", self.page)

    def test_servo_direction_pad_targets_selected_online_device(self):
        for direction in ("up", "down", "left", "right"):
            self.assertIn(f'data-servo-direction="{direction}"', self.page)
        self.assertIn("'/command/move'", self.page)
        self.assertIn("const deviceDirections = { up: 'up', down: 'down', left: 'right', right: 'left' };", self.page)
        self.assertIn("{ type: deviceDirections[direction], degree: '10', duration_ms: '350' }", self.page)
        self.assertIn("按面向小派时看到的方向控制", self.page)
        self.assertIn("els.servoButtons.forEach(button => { button.disabled = remoteBusy || !available; });", self.page)
        self.assertIn("els.servoButtons.forEach(button => button.addEventListener('click'", self.page)

    def test_network_debug_switch_uses_reported_device_state(self):
        self.assertIn('id="networkDebugState">OFF</output>', self.page)
        self.assertIn("Boolean(device?.network_debug)", self.page)
        self.assertIn("/command/network_debug?${query}", self.page)
        self.assertIn("等待设备心跳确认实际状态", self.page)

    def test_brightness_slider_targets_device_and_defaults_to_seventy_percent(self):
        self.assertIn('id="brightnessRange" type="range" min="10" max="100" step="5" value="70"', self.page)
        self.assertIn("device?.capabilities?.includes('display_brightness')", self.page)
        self.assertIn("/command/brightness?${query}", self.page)
        self.assertIn("设置会保存在设备上", self.page)

    def test_shared_conversation_is_refreshed_automatically(self):
        self.assertIn("async function syncSharedSession()", self.page)
        self.assertIn("renderHistory(historyMessages(payload), force);", self.page)
        self.assertIn("setInterval(syncSharedSession, 1000);", self.page)
        self.assertIn("if (busy || sessionSyncing) return;", self.page)


if __name__ == "__main__":
    unittest.main()
