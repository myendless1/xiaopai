import os
import sys
import types
import unittest
import asyncio
import io
import wave


SRC_DIR = os.path.join(os.path.dirname(os.path.dirname(__file__)), "src")
sys.path.insert(0, SRC_DIR)

from aliyun_streaming_asr import parse_asr_event, wait_for_transcription_started
from aliyun_streaming_tts import AliyunStreamingTtsClient, split_sentences
from realtime_server import (
    REALTIME_SLEEP_REPLY_BYE_EVENTS,
    REALTIME_SLEEP_REPLY_REST_EVENTS,
    RealtimeAsrBridge,
    RealtimeConfig,
    RealtimeDeviceSession,
    RealtimeManager,
    has_realtime_sleep_word,
    has_realtime_wake_word,
    is_realtime_wake_only_text,
    realtime_sleep_reply_event_for_text,
    _wav_header,
)
from server import (
    AVAILABLE_ACTIONS,
    AVAILABLE_EXPRESSIONS,
    SLEEP_REPLY_BYE_EVENTS,
    SLEEP_REPLY_REST_EVENTS,
    command_payload_from_query,
    event_audio_cache_meta,
    has_dialog_sleep_word,
    has_dialog_wake_word,
    is_wake_only_text,
    sleep_reply_event_for_text,
    tts_request_options_from_params,
)
from realtime_protocol import build_hello, build_stt


class RealtimeMappingTest(unittest.TestCase):
    def test_realtime_audio_rates_are_split_by_direction(self):
        manager = RealtimeManager(RealtimeConfig(), logger=lambda _msg: None)
        self.assertEqual(manager.config.upstream_sample_rate, 16000)
        self.assertEqual(manager.upstream_opus.sample_rate, 16000)
        self.assertEqual(manager.upstream_opus.samples_per_frame, 960)
        self.assertEqual(manager.config.downstream_sample_rate, 24000)
        self.assertEqual(manager.downstream_opus.sample_rate, 24000)
        self.assertEqual(manager.downstream_opus.samples_per_frame, 1440)

    def test_realtime_debug_wav_header_is_16khz(self):
        pcm = b"\x00\x00" * 160
        with wave.open(io.BytesIO(_wav_header(len(pcm), 16000) + pcm), "rb") as wav:
            self.assertEqual(wav.getframerate(), 16000)
            self.assertEqual(wav.getnchannels(), 1)
            self.assertEqual(wav.getsampwidth(), 2)
            self.assertEqual(wav.getnframes(), 160)

    def test_supported_face_values_are_expression_only(self):
        self.assertEqual(
            AVAILABLE_EXPRESSIONS,
            (
                "calm",
                "shy",
                "happy",
                "thinking",
                "surprised",
                "sleep_dark",
                "screen_off",
            ),
        )

    def test_face_query_contains_only_expression(self):
        payload = command_payload_from_query("face", {"expression": ["thinking"]})
        self.assertEqual(payload, {"expression": "thinking"})

    def test_realtime_protocol_shapes(self):
        hello = build_hello("sess_1")
        self.assertEqual(hello["type"], "hello")
        self.assertEqual(hello["audio_params"]["frame_duration"], 60)
        stt = build_stt("你好", is_final=True)
        self.assertTrue(stt["is_final"])

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

    def test_realtime_asr_bridge_waits_for_final_text_after_stop(self):
        import realtime_server

        class FakeAsrWebSocket:
            def __init__(self):
                self.sent = []
                self.binary = []
                self.timeout = None
                self.closed = False
                self.stop_seen = False
                self.events = [
                    '{"header":{"name":"SentenceEnd","status":20000000},'
                    '"payload":{"result":"你好小派"}}',
                ]

            def gettimeout(self):
                return self.timeout

            def settimeout(self, timeout):
                self.timeout = timeout

            def send(self, payload):
                self.sent.append(payload)
                self.stop_seen = self.stop_seen or "StopTranscription" in payload

            def send_binary(self, payload):
                self.binary.append(payload)

            def recv(self):
                if self.stop_seen and self.events:
                    return self.events.pop(0)
                raise TimeoutError("timeout")

            def close(self):
                self.closed = True

        class FakeAsrSession:
            def __init__(self, **_kwargs):
                pass

            def connect(self):
                return fake_ws, "task_1"

        fake_ws = FakeAsrWebSocket()
        original_asr_session = realtime_server.AliyunStreamingAsrSession
        realtime_server.AliyunStreamingAsrSession = FakeAsrSession
        try:
            manager = RealtimeManager(RealtimeConfig(appkey="app", token_getter=lambda: "token"), logger=lambda _msg: None)
            manager._upstream_opus = types.SimpleNamespace(decode=lambda _frame: b"pcm")
            submitted = []
            manager.submit_asr_text = lambda device_id, session_id, text, is_final: submitted.append(
                (device_id, session_id, text, is_final)
            )
            session = RealtimeDeviceSession(device_id="dev1", websocket=types.SimpleNamespace(), session_id="sess1")
            bridge = RealtimeAsrBridge(manager, session)
            bridge.start()
            bridge.push_opus(b"opus")
            bridge.stop(graceful=True)
            bridge._thread.join(timeout=1.0)
            self.assertFalse(bridge._thread.is_alive())
            self.assertEqual(fake_ws.binary, [b"pcm"])
            self.assertTrue(any("StopTranscription" in payload for payload in fake_ws.sent))
            self.assertEqual(submitted, [("dev1", "sess1", "你好小派", True)])
            self.assertTrue(fake_ws.closed)
        finally:
            realtime_server.AliyunStreamingAsrSession = original_asr_session

    def test_sentence_split(self):
        self.assertEqual(split_sentences("你好。我们开始吧！"), ["你好。", "我们开始吧！"])

    def test_realtime_wake_only_text(self):
        self.assertTrue(has_realtime_wake_word("你好，小派。"))
        self.assertTrue(is_realtime_wake_only_text("你好，小派。"))
        self.assertTrue(has_realtime_wake_word("小蔡同学。"))
        self.assertTrue(is_realtime_wake_only_text("小蔡同学。"))
        self.assertTrue(has_realtime_wake_word("小的同学。"))
        self.assertTrue(is_realtime_wake_only_text("小的同学。"))
        self.assertFalse(is_realtime_wake_only_text("小派，今天深圳天气怎么样"))

    def test_http_wake_only_text_accepts_asr_aliases(self):
        self.assertTrue(has_dialog_wake_word("小蔡同学。"))
        self.assertTrue(is_wake_only_text("小蔡同学。"))
        self.assertTrue(has_dialog_wake_word("小的同学。"))
        self.assertTrue(is_wake_only_text("小的同学。"))

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

    def test_realtime_session_starts_listening(self):
        manager = RealtimeManager(RealtimeConfig(), logger=lambda _msg: None)
        registered = []

        class FakeWebSocket:
            request_headers = {}

            def __init__(self):
                self.sent = []

            async def send(self, payload):
                self.sent.append(payload)

            def __aiter__(self):
                return self

            async def __anext__(self):
                raise StopAsyncIteration

            async def close(self, code=None, reason=None):
                pass

        original_register_session = manager._register_session

        def capture_register_session(session):
            registered.append(session)
            original_register_session(session)

        manager._register_session = capture_register_session
        websocket = FakeWebSocket()
        asyncio.run(manager._dispatch(websocket, "/ws"))

        self.assertTrue(registered[0].dialog_awake)
        self.assertTrue(any('"type":"device_state"' in payload and '"state":"listening"' in payload for payload in websocket.sent))
        self.assertFalse(any('"type":"device_state"' in payload and '"state":"idle"' in payload for payload in websocket.sent))

    def test_realtime_reconnect_preserves_dialog_sleep(self):
        manager = RealtimeManager(RealtimeConfig(), logger=lambda _msg: None)
        manager._dialog_awake_by_device["dev1"] = False
        registered = []

        class FakeWebSocket:
            request_headers = {}

            def __init__(self):
                self.sent = []

            async def send(self, payload):
                self.sent.append(payload)

            def __aiter__(self):
                return self

            async def __anext__(self):
                raise StopAsyncIteration

            async def close(self, code=None, reason=None):
                pass

        original_register_session = manager._register_session

        def capture_register_session(session):
            registered.append(session)
            original_register_session(session)

        manager._register_session = capture_register_session
        websocket = FakeWebSocket()
        asyncio.run(manager._dispatch(websocket, "/ws?device_id=dev1"))

        self.assertFalse(registered[0].dialog_awake)
        self.assertTrue(any('"state":"sleep"' in payload for payload in websocket.sent))
        self.assertFalse(any('"state":"listening"' in payload for payload in websocket.sent))

    def test_realtime_binary_without_listen_start_is_ignored(self):
        manager = RealtimeManager(RealtimeConfig(), logger=lambda _msg: None)
        started = []

        class FakeWebSocket:
            request_headers = {}

            def __init__(self):
                self.sent = []
                self.frames = [b"orphan-opus"]

            async def send(self, payload):
                self.sent.append(payload)

            def __aiter__(self):
                return self

            async def __anext__(self):
                if not self.frames:
                    raise StopAsyncIteration
                return self.frames.pop(0)

            async def close(self, code=None, reason=None):
                pass

        manager._start_asr = lambda session: started.append(session.device_id)

        websocket = FakeWebSocket()
        asyncio.run(manager._dispatch(websocket, "/ws"))

        self.assertEqual(started, [])

    def test_realtime_listen_start_lazily_starts_asr_on_first_audio(self):
        manager = RealtimeManager(RealtimeConfig(), logger=lambda _msg: None)
        started = []
        pushed = []

        class FakeBridge:
            active = True

            def push_opus(self, payload):
                pushed.append(payload)

            def stop(self, *, graceful=True):
                pass

        class FakeWebSocket:
            request_headers = {}

            def __init__(self):
                self.sent = []
                self.frames = ['{"type":"listen","state":"start"}', b"opus"]

            async def send(self, payload):
                self.sent.append(payload)

            def __aiter__(self):
                return self

            async def __anext__(self):
                if not self.frames:
                    raise StopAsyncIteration
                return self.frames.pop(0)

            async def close(self, code=None, reason=None):
                pass

        def fake_start_asr(session):
            started.append(session.device_id)
            session.asr_bridge = FakeBridge()

        manager._start_asr = fake_start_asr

        websocket = FakeWebSocket()
        asyncio.run(manager._dispatch(websocket, "/ws"))

        self.assertEqual(started, ["default"])
        self.assertEqual(pushed, [b"opus"])

    def test_realtime_reconnect_retires_existing_session(self):
        manager = RealtimeManager(RealtimeConfig(), logger=lambda _msg: None)

        class FakeBridge:
            def __init__(self):
                self.stopped = []

            def stop(self, *, graceful=True):
                self.stopped.append(graceful)

        class FakeWebSocket:
            def __init__(self):
                self.closed = []

            async def close(self, code=None, reason=None):
                self.closed.append((code, reason))

        async def run_case():
            old_ws = FakeWebSocket()
            bridge = FakeBridge()
            old_session = RealtimeDeviceSession(device_id="dev1", websocket=old_ws, session_id="old")
            old_session.asr_bridge = bridge
            manager._register_session(old_session)

            new_session = RealtimeDeviceSession(device_id="dev1", websocket=FakeWebSocket(), session_id="new")
            manager._register_session(new_session)
            await asyncio.sleep(0)
            return bridge.stopped, old_ws.closed, manager._sessions.get("dev1")

        stopped, closed, current = asyncio.run(run_case())

        self.assertEqual(stopped, [False])
        self.assertTrue(closed)
        self.assertIsNotNone(current)
        self.assertEqual(current.session_id, "new")

    def test_realtime_asr_bridge_finished_clears_current_session(self):
        manager = RealtimeManager(RealtimeConfig(), logger=lambda _msg: None)

        async def run_case():
            bridge = types.SimpleNamespace()
            session = RealtimeDeviceSession(device_id="dev1", websocket=types.SimpleNamespace(), session_id="sess1")
            session.asr_bridge = bridge
            session.listen_active = True
            manager._register_session(session)

            await manager._handle_asr_bridge_finished("dev1", "sess1", bridge)
            return session.asr_bridge, session.listen_active

        bridge, listen_active = asyncio.run(run_case())
        self.assertIsNone(bridge)
        self.assertFalse(listen_active)

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

    def test_tts_default_voice_sentinel_uses_server_voice(self):
        class FakeServer:
            voice = "zhimiao_emo"
            sample_rate = 16000
            volume = 80
            speech_rate = 0
            pitch_rate = 0

        options = tts_request_options_from_params(FakeServer, {"voice": "default"})

        self.assertEqual(options.voice, "zhimiao_emo")

    def test_realtime_speak_only_aborts_when_interrupting(self):
        class FakeWebSocket:
            def __init__(self):
                self.sent = []

            async def send(self, payload):
                self.sent.append(payload)

        async def run_case():
            queued = []
            aborted = []

            def command_callback(device_id, command):
                queued.append((device_id, command))
                return True

            manager = RealtimeManager(
                RealtimeConfig(command_callback=command_callback),
                logger=lambda _msg: None,
            )

            async def fake_abort(session):
                aborted.append(session.session_id)

            manager._abort_session_tts = fake_abort
            session = RealtimeDeviceSession(device_id="dev1", websocket=FakeWebSocket(), session_id="sess1")
            await manager._speak(session, "第一句", interrupt=False)
            await manager._speak(session, "第二句", interrupt=True)
            return queued, aborted

        queued, aborted = asyncio.run(run_case())
        self.assertEqual([item[1]["payload"]["text"] for item in queued], ["第一句", "第二句"])
        self.assertEqual(aborted, ["sess1"])

    def test_realtime_device_connected_callback_runs_on_register_and_id_update(self):
        connected = []
        manager = RealtimeManager(
            RealtimeConfig(device_connected_callback=lambda device_id: connected.append(device_id)),
            logger=lambda _msg: None,
        )

        session = RealtimeDeviceSession(device_id="default", websocket=types.SimpleNamespace(), session_id="sess1")
        manager._register_session(session)
        manager._update_session_device_id(session, "dev-1")

        self.assertEqual(connected, ["default", "dev-1"])

    def test_realtime_wake_from_sleep_does_not_send_find_owner(self):
        class FakeWebSocket:
            async def send(self, payload):
                pass

        async def run_case():
            manager = RealtimeManager(RealtimeConfig(), logger=lambda _msg: None)
            spoken = []
            commands = []

            async def fake_speak(_session, text):
                spoken.append(text)

            manager._speak = fake_speak
            session = RealtimeDeviceSession(device_id="dev1", websocket=FakeWebSocket(), session_id="sess1")
            await manager._handle_final_text(session, "你好，小派。")
            first_wake_commands = list(commands)
            commands.clear()
            await manager._handle_final_text(session, "小派")
            return session.dialog_awake, spoken, first_wake_commands, list(commands)

        awake, spoken, commands, repeated_wake_commands = asyncio.run(run_case())
        self.assertTrue(awake)
        self.assertEqual(len(spoken), 2)
        self.assertEqual(commands, [])
        self.assertEqual(repeated_wake_commands, [])

    def test_realtime_sleep_sends_cached_reply_before_sleep(self):
        class FakeWebSocket:
            def __init__(self):
                self.sent = []

            async def send(self, payload):
                self.sent.append(payload)

        async def run_case():
            commands = []

            def command_callback(device_id, command):
                commands.append((device_id, command))
                return True

            manager = RealtimeManager(
                RealtimeConfig(
                    command_callback=command_callback,
                ),
                logger=lambda _msg: None,
            )
            websocket = FakeWebSocket()
            session = RealtimeDeviceSession(device_id="dev1", websocket=websocket, session_id="sess1")
            session.dialog_awake = True
            await manager._handle_final_text(session, "小派，退下吧")
            return session.dialog_awake, commands, websocket.sent

        awake, commands, sent = asyncio.run(run_case())
        self.assertFalse(awake)
        self.assertEqual(len(commands), 1)
        self.assertEqual(commands[0][0], "dev1")
        self.assertEqual(commands[0][1]["type"], "speak")
        speak_payload = commands[0][1]["payload"]
        self.assertIn((speak_payload["cache_name"], speak_payload["text"]), REALTIME_SLEEP_REPLY_REST_EVENTS)
        self.assertTrue(any('"type":"llm"' in payload for payload in sent))
        self.assertTrue(any('"state":"sleep"' in payload for payload in sent))

    def test_realtime_sleeping_ignores_repeated_sleep_word(self):
        class FakeWebSocket:
            def __init__(self):
                self.sent = []

            async def send(self, payload):
                self.sent.append(payload)

        async def run_case():
            manager = RealtimeManager(RealtimeConfig(), logger=lambda _msg: None)
            spoken = []

            async def fake_speak(_session, text, **_kwargs):
                spoken.append(text)

            manager._speak = fake_speak
            websocket = FakeWebSocket()
            session = RealtimeDeviceSession(device_id="dev1", websocket=websocket, session_id="sess1")
            session.dialog_awake = True
            await manager._handle_final_text(session, "拜拜")
            await manager._handle_final_text(session, "今天天气怎么样")
            await manager._handle_final_text(session, "拜拜")
            return session.dialog_awake, spoken, websocket.sent, session.latency_marks

        awake, spoken, sent, marks = asyncio.run(run_case())
        self.assertFalse(awake)
        self.assertEqual(len(spoken), 1)
        self.assertIn("dialog_sleeping_ignore", marks)
        self.assertEqual(sum('"state":"sleep"' in payload for payload in sent), 3)

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
