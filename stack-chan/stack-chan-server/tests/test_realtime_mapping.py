import os
import sys
import types
import unittest
import asyncio


SRC_DIR = os.path.join(os.path.dirname(os.path.dirname(__file__)), "src")
sys.path.insert(0, SRC_DIR)

from aliyun_streaming_asr import parse_asr_event, wait_for_transcription_started
from aliyun_streaming_tts import AliyunStreamingTtsClient, split_sentences
from mcp_client import (
    STACKCHAN_TOOL_FACE_ACTION,
    STACKCHAN_TOOL_FACE_SET,
    STACKCHAN_TOOL_HEAD_FIND_OWNER,
    STACKCHAN_TOOL_HEAD_MOVE,
    STACKCHAN_TOOL_VOLUME_SET,
    command_to_mcp_calls,
)
from realtime_server import (
    REALTIME_SLEEP_REPLY_BYE_EVENTS,
    REALTIME_SLEEP_REPLY_REST_EVENTS,
    RealtimeConfig,
    RealtimeDeviceSession,
    RealtimeManager,
    has_realtime_sleep_word,
    has_realtime_wake_word,
    is_realtime_wake_only_text,
    realtime_sleep_reply_event_for_text,
)
from server import (
    SLEEP_REPLY_BYE_EVENTS,
    SLEEP_REPLY_REST_EVENTS,
    event_audio_cache_meta,
    has_dialog_sleep_word,
    sleep_reply_event_for_text,
    tts_request_options_from_params,
)
from xiaozhi_protocol import build_hello, build_mcp_tools_call, build_stt


class RealtimeMappingTest(unittest.TestCase):
    def test_face_alias_maps_to_action(self):
        calls = command_to_mcp_calls({"type": "face", "payload": {"expression": "爱心"}})
        self.assertEqual(calls[0].name, STACKCHAN_TOOL_FACE_ACTION)
        self.assertEqual(calls[0].arguments, {"action": "heart_action"})

    def test_face_expression_maps_to_expression_tool(self):
        calls = command_to_mcp_calls({"type": "face", "payload": {"expression": "thinking"}})
        self.assertEqual(calls[0].name, STACKCHAN_TOOL_FACE_SET)
        self.assertEqual(calls[0].arguments, {"expression": "thinking"})

    def test_motion_maps_to_head_move(self):
        calls = command_to_mcp_calls({"type": "motion", "payload": {"type": "left", "degree": 12, "duration_ms": 300}})
        self.assertEqual(calls[0].name, STACKCHAN_TOOL_HEAD_MOVE)
        self.assertEqual(calls[0].arguments["type"], "left")
        self.assertEqual(calls[0].arguments["degree"], 12.0)

    def test_volume_set_maps_to_volume_tool(self):
        calls = command_to_mcp_calls({"type": "volume", "payload": {"mode": "set", "value": 80}})
        self.assertEqual(calls[0].name, STACKCHAN_TOOL_VOLUME_SET)
        self.assertEqual(calls[0].arguments, {"value": 80})

    def test_find_owner_preserves_direct_wake_arguments(self):
        calls = command_to_mcp_calls(
            {
                "type": "find_owner",
                "payload": {
                    "rounds": 1,
                    "reply": "",
                    "preserve_speech": True,
                    "wait_for_speech": False,
                    "gain_x": 1.2,
                    "gain_y": 0.9,
                    "stop_pixels": 24,
                },
            }
        )
        self.assertEqual(calls[0].name, STACKCHAN_TOOL_HEAD_FIND_OWNER)
        self.assertEqual(
            calls[0].arguments,
            {
                "rounds": 1,
                "reply": "",
                "preserve_speech": True,
                "wait_for_speech": False,
                "gain_x": 1.2,
                "gain_y": 0.9,
                "stop_pixels": 24.0,
            },
        )

    def test_xiaozhi_protocol_shapes(self):
        hello = build_hello("sess_1")
        self.assertEqual(hello["type"], "hello")
        self.assertEqual(hello["audio_params"]["frame_duration"], 60)
        stt = build_stt("你好", is_final=True)
        self.assertTrue(stt["is_final"])
        mcp = build_mcp_tools_call("self.stackchan.stop", {})
        self.assertEqual(mcp["payload"]["method"], "tools/call")

    def test_aliyun_asr_event_parser(self):
        event = parse_asr_event(
            '{"header":{"name":"SentenceEnd","status":20000000,"task_id":"t1"},"payload":{"result":"你好小派"}}'
        )
        self.assertTrue(event["is_final"])
        self.assertEqual(event["text"], "你好小派")

    def test_wait_for_aliyun_asr_started(self):
        class FakeWebSocket:
            def __init__(self, frames):
                self.frames = list(frames)
                self.timeout = None

            def gettimeout(self):
                return self.timeout

            def settimeout(self, timeout):
                self.timeout = timeout

            def recv(self):
                if not self.frames:
                    raise TimeoutError("timeout")
                return self.frames.pop(0)

        started = wait_for_transcription_started(
            FakeWebSocket(
                [
                    '{"header":{"name":"TaskStarted","status":20000000}}',
                    '{"header":{"name":"TranscriptionStarted","status":20000000,"task_id":"t1"}}',
                ]
            )
        )
        self.assertEqual(started["name"], "TranscriptionStarted")

        with self.assertRaisesRegex(RuntimeError, "Aliyun ASR failed"):
            wait_for_transcription_started(
                FakeWebSocket(['{"header":{"name":"TaskFailed","status":40000002}}'])
            )

        with self.assertRaisesRegex(RuntimeError, "Timed out"):
            wait_for_transcription_started(FakeWebSocket([]), timeout_s=0.01)

    def test_sentence_split(self):
        self.assertEqual(split_sentences("你好。我们开始吧！"), ["你好。", "我们开始吧！"])

    def test_realtime_wake_only_text(self):
        self.assertTrue(has_realtime_wake_word("你好，小派。"))
        self.assertTrue(is_realtime_wake_only_text("你好，小派。"))
        self.assertFalse(is_realtime_wake_only_text("小派，今天深圳天气怎么样"))

    def test_realtime_hello_updates_registered_device_id(self):
        manager = RealtimeManager(RealtimeConfig(), logger=lambda _msg: None)

        class FakeWebSocket:
            pass

        session = RealtimeDeviceSession(device_id="default", websocket=FakeWebSocket(), session_id="sess1")
        session.asr_bridge = types.SimpleNamespace(device_id="default")
        manager._register_session(session)

        manager._update_session_device_id(session, "44:1b f6/e4:83:8c")

        self.assertEqual(session.device_id, "44:1b_f6_e4:83:8c")
        self.assertEqual(session.asr_bridge.device_id, "44:1b_f6_e4:83:8c")
        self.assertEqual(set(manager._sessions), {"44:1b_f6_e4:83:8c"})

    def test_realtime_sleep_text(self):
        self.assertTrue(has_realtime_sleep_word("小派，先休息吧"))
        self.assertTrue(has_realtime_sleep_word("不用了，拜拜"))
        self.assertTrue(has_realtime_sleep_word("小派，退下吧"))
        self.assertFalse(has_realtime_sleep_word("不用了"))
        self.assertFalse(has_realtime_sleep_word("小派，继续聊天"))

    def test_server_sleep_text(self):
        self.assertTrue(has_dialog_sleep_word("小派，先休息吧"))
        self.assertTrue(has_dialog_sleep_word("拜拜"))
        self.assertTrue(has_dialog_sleep_word("小派，退下吧"))
        self.assertFalse(has_dialog_sleep_word("不用了"))
        self.assertFalse(has_dialog_sleep_word("小派，继续聊天"))

    def test_sleep_reply_groups(self):
        for _ in range(20):
            self.assertIn(sleep_reply_event_for_text("拜拜"), SLEEP_REPLY_BYE_EVENTS)
            self.assertIn(sleep_reply_event_for_text("再见"), SLEEP_REPLY_BYE_EVENTS)
            self.assertIn(sleep_reply_event_for_text("退下吧"), SLEEP_REPLY_REST_EVENTS)
            self.assertIn(sleep_reply_event_for_text("休息一下"), SLEEP_REPLY_REST_EVENTS)
            self.assertIn(realtime_sleep_reply_event_for_text("拜拜"), REALTIME_SLEEP_REPLY_BYE_EVENTS)
            self.assertIn(realtime_sleep_reply_event_for_text("退下吧"), REALTIME_SLEEP_REPLY_REST_EVENTS)

    def test_event_audio_cache_meta_changes_with_voice(self):
        class FakeServer:
            appkey = "app1"
            tts_url = "https://example.invalid/tts"
            voice = "xiaoyun"
            sample_rate = 16000
            volume = 80
            speech_rate = 0
            pitch_rate = 0

        first = event_audio_cache_meta(FakeServer, "拜拜")
        FakeServer.voice = "xiaomei"
        second = event_audio_cache_meta(FakeServer, "拜拜")
        self.assertNotEqual(first, second)
        self.assertEqual(second["voice"], "xiaomei")

    def test_tts_debug_options_override_server_defaults(self):
        class FakeServer:
            voice = "xiaoyun"
            sample_rate = 16000
            volume = 80
            speech_rate = 0
            pitch_rate = 0

        options = tts_request_options_from_params(
            FakeServer,
            {
                "voice": "xiaomei",
                "sample_rate": "24000",
                "volume": "60",
                "speech_rate": "-80",
                "pitch_rate": "20",
                "format": "wav",
            },
        )

        self.assertEqual(options.voice, "xiaomei")
        self.assertEqual(options.sample_rate, 24000)
        self.assertEqual(options.volume, 60)
        self.assertEqual(options.speech_rate, -80)
        self.assertEqual(options.pitch_rate, 20)
        self.assertEqual(options.audio_format, "wav")

    def test_tts_debug_options_validate_ranges(self):
        class FakeServer:
            voice = "xiaoyun"
            sample_rate = 16000
            volume = 80
            speech_rate = 0
            pitch_rate = 0

        with self.assertRaisesRegex(ValueError, "speech_rate"):
            tts_request_options_from_params(FakeServer, {"speech_rate": "999"})

    def test_openclaw_realtime_reply_is_not_spoken_twice(self):
        class FakeOpenClaw:
            enabled = True

            def chat(self, device_id, text):
                return "你好，有什么我能帮到你的？"

        class FakeWebSocket:
            def __init__(self):
                self.sent = []

            async def send(self, payload):
                self.sent.append(payload)

        async def run_case():
            manager = RealtimeManager(RealtimeConfig(openclaw_base_url="http://openclaw", openclaw_token="token"), logger=lambda _msg: None)
            manager._openclaw = FakeOpenClaw()
            spoken = []

            async def fake_speak(_session, text):
                spoken.append(text)

            manager._speak = fake_speak
            websocket = FakeWebSocket()
            session = RealtimeDeviceSession(device_id="dev1", websocket=websocket, session_id="sess1")
            session.dialog_awake = True
            await manager._handle_final_text(session, "今天有什么安排")
            return spoken, websocket.sent

        spoken, sent = asyncio.run(run_case())
        self.assertEqual(spoken, [])
        self.assertTrue(any('"type":"device_state"' in payload and '"state":"waiting"' in payload for payload in sent))
        self.assertTrue(any('"type":"llm"' in payload for payload in sent))

    def test_realtime_wake_from_sleep_sends_find_owner_directly(self):
        class FakeOpenClaw:
            enabled = True

            def chat(self, device_id, text):
                raise AssertionError("wake-only should not call OpenClaw")

        class FakeWebSocket:
            async def send(self, payload):
                pass

        async def run_case():
            manager = RealtimeManager(
                RealtimeConfig(
                    openclaw_base_url="http://openclaw",
                    openclaw_token="token",
                    find_owner_gain_x=1.3,
                    find_owner_gain_y=0.7,
                    find_owner_stop_pixels=28,
                ),
                logger=lambda _msg: None,
            )
            manager._openclaw = FakeOpenClaw()
            spoken = []
            commands = []

            async def fake_speak(_session, text):
                spoken.append(text)

            async def fake_send_mcp(_session, command):
                commands.append(command)
                return True

            manager._speak = fake_speak
            manager._send_mcp_command = fake_send_mcp
            session = RealtimeDeviceSession(device_id="dev1", websocket=FakeWebSocket(), session_id="sess1")
            await manager._handle_final_text(session, "你好，小派。")
            first_wake_commands = list(commands)
            commands.clear()
            await manager._handle_final_text(session, "小派")
            return session.dialog_awake, spoken, first_wake_commands, list(commands)

        awake, spoken, commands, repeated_wake_commands = asyncio.run(run_case())
        self.assertTrue(awake)
        self.assertEqual(len(spoken), 2)
        self.assertEqual(len(commands), 1)
        self.assertEqual(repeated_wake_commands, [])
        self.assertEqual(commands[0]["type"], "find_owner")
        self.assertEqual(
            commands[0]["payload"],
            {
                "rounds": 1,
                "reply": "",
                "preserve_speech": True,
                "wait_for_speech": False,
                "gain_x": 1.3,
                "gain_y": 0.7,
                "stop_pixels": 28,
            },
        )

    def test_realtime_sleep_sends_cached_reply_before_sleep(self):
        class FakeOpenClaw:
            enabled = True

            def chat(self, device_id, text):
                raise AssertionError("sleep command should not call OpenClaw")

        class FakeWebSocket:
            def __init__(self):
                self.sent = []

            async def send(self, payload):
                self.sent.append(payload)

        async def run_case():
            manager = RealtimeManager(
                RealtimeConfig(openclaw_base_url="http://openclaw", openclaw_token="token"),
                logger=lambda _msg: None,
            )
            manager._openclaw = FakeOpenClaw()
            commands = []

            async def fake_send_mcp(_session, command):
                commands.append(command)
                return True

            manager._send_mcp_command = fake_send_mcp
            websocket = FakeWebSocket()
            session = RealtimeDeviceSession(device_id="dev1", websocket=websocket, session_id="sess1")
            session.dialog_awake = True
            await manager._handle_final_text(session, "小派，退下吧")
            return session.dialog_awake, commands, websocket.sent

        awake, commands, sent = asyncio.run(run_case())
        self.assertFalse(awake)
        self.assertEqual(len(commands), 1)
        self.assertEqual(commands[0]["type"], "sequence")
        speak_step = commands[0]["payload"][0]
        self.assertEqual(speak_step["type"], "speak")
        self.assertIn((speak_step["cache_name"], speak_step["text"]), REALTIME_SLEEP_REPLY_REST_EVENTS)
        self.assertTrue(any('"type":"llm"' in payload for payload in sent))
        self.assertTrue(any('"state":"sleep"' in payload for payload in sent))

    def test_tts_iter_pcm_chunks_streams_binary_frames(self):
        class FakeWebSocket:
            def __init__(self):
                self.sent = []
                self.frames = [
                    '{"header":{"name":"SynthesisStarted","status":20000000}}',
                    b"pcm1",
                    b"pcm2",
                    '{"header":{"name":"SynthesisCompleted","status":20000000}}',
                ]

            def send(self, payload):
                self.sent.append(payload)

            def recv(self):
                return self.frames.pop(0)

            def close(self):
                pass

        fake_ws = FakeWebSocket()
        fake_module = types.SimpleNamespace(create_connection=lambda *args, **kwargs: fake_ws)
        original = sys.modules.get("websocket")
        sys.modules["websocket"] = fake_module
        try:
            client = AliyunStreamingTtsClient(appkey="app", token_getter=lambda: "token")
            self.assertEqual(list(client.iter_pcm_chunks("你好")), [b"pcm1", b"pcm2"])
            self.assertTrue(fake_ws.sent)
        finally:
            if original is None:
                sys.modules.pop("websocket", None)
            else:
                sys.modules["websocket"] = original


if __name__ == "__main__":
    unittest.main()
