import asyncio
from collections.abc import Callable
from dataclasses import dataclass
import json
import random
import re
from queue import Empty, Queue
import threading
import time
from urllib.parse import parse_qs, urlparse

from aliyun_streaming_asr import AliyunStreamingAsrSession, build_stop_transcription, parse_asr_event
from aliyun_streaming_tts import AliyunStreamingTtsClient, split_sentences
from mcp_client import McpRequestTracker, command_to_mcp_calls
from openclaw_agent import OpenClawAgent
from opus_codec import OpusCodec, OpusUnavailableError
from xiaozhi_protocol import (
    build_hello,
    build_llm,
    build_mcp_tools_call,
    build_stt,
    build_tts_state,
    extract_device_id_from_hello,
    json_dumps,
    make_request_id,
    parse_json_message,
)

REALTIME_WAKE_WORDS = (
    "小派同学",
    "小派同學",
    "小派",
    "小胖",
    "小盼",
    "小潘",
    "小排",
    "小白",
    "小坏",
    "小壞",
    "小蔡",
    "小外",
    "小机器",
    "机器人",
    "小盘",
    "小泡",
    "xiaopai",
)
REALTIME_WAKE_ONLY_FILLERS = ("你好", "您好", "在吗", "在嗎", "醒醒", "hello", "hi", "嗨", "哈喽", "哈囉")
REALTIME_WAKE_REPLIES = ("我在。", "有什么要帮忙的", "你好呀", "我在呢", "小派在呢")
REALTIME_SLEEP_BYE_WORDS = (
    "拜拜",
    "再见",
    "再會",
    "再会",
)
REALTIME_SLEEP_REST_WORDS = (
    "退下吧",
    "退下",
    "退一下",
    "退一下吧",
    "退一退",
    "休息",
    "睡觉",
    "睡覺",
    "睡眠",
    "先这样",
    "先這樣",
)
REALTIME_SLEEP_WORDS = REALTIME_SLEEP_REST_WORDS + REALTIME_SLEEP_BYE_WORDS
REALTIME_SLEEP_REPLY_BYE_EVENTS = (
    ("sleep_reply_bye", "拜拜"),
    ("sleep_reply_goodbye", "再见"),
)
REALTIME_SLEEP_REPLY_REST_EVENTS = (
    ("sleep_reply_ok", "好的"),
    ("sleep_reply_ok_master", "好的主人"),
    ("sleep_reply_bye", "拜拜"),
    ("sleep_reply_obey", "遵命"),
)


def normalize_realtime_command_text(text: str) -> str:
    return re.sub(r"[\s,_\-，。.!！?？/（）()]+", "", str(text or "").strip().lower())


def safe_realtime_device_id(device_id: str) -> str:
    safe = re.sub(r"[^A-Za-z0-9_.:-]+", "_", str(device_id or "").strip())[:64]
    return safe or "default"


def has_realtime_wake_word(text: str) -> bool:
    normalized = normalize_realtime_command_text(text)
    return any(normalize_realtime_command_text(word) in normalized for word in REALTIME_WAKE_WORDS)


def has_realtime_sleep_word(text: str) -> bool:
    normalized = normalize_realtime_command_text(text)
    return any(normalize_realtime_command_text(word) in normalized for word in REALTIME_SLEEP_WORDS)


def realtime_sleep_reply_event_for_text(text: str) -> tuple[str, str]:
    normalized = normalize_realtime_command_text(text)
    if any(normalize_realtime_command_text(word) in normalized for word in REALTIME_SLEEP_BYE_WORDS):
        return random.choice(REALTIME_SLEEP_REPLY_BYE_EVENTS)
    if any(normalize_realtime_command_text(word) in normalized for word in REALTIME_SLEEP_REST_WORDS):
        return random.choice(REALTIME_SLEEP_REPLY_REST_EVENTS)
    return random.choice(REALTIME_SLEEP_REPLY_REST_EVENTS)


def is_realtime_wake_only_text(text: str) -> bool:
    compact = re.sub(r"[\s,，。.!！?？、~～：:；;\"'“”‘’]+", "", str(text or "").lower())
    for wake_word in REALTIME_WAKE_WORDS:
        wake = re.sub(r"[\s,，。.!！?？、~～：:；;\"'“”‘’]+", "", wake_word.lower())
        if wake and wake in compact:
            compact = compact.replace(wake, "", 1)
            break
    for filler in REALTIME_WAKE_ONLY_FILLERS:
        compact = compact.replace(filler.lower(), "")
    return compact == ""


@dataclass
class RealtimeConfig:
    host: str = "0.0.0.0"
    port: int = 8092
    path: str = "/xiaozhi/ws"
    token: str = ""
    region: str = "shanghai"
    appkey: str = ""
    token_getter: Callable[[], str] | None = None
    aliyun_asr_ws_url: str = ""
    aliyun_tts_ws_url: str = ""
    voice: str = "zhimiao_emo"
    sample_rate: int = 16000
    volume: int = 80
    speech_rate: int = 0
    pitch_rate: int = 0
    max_sentence_chars: int = 120
    openclaw_base_url: str = ""
    openclaw_token: str = ""
    openclaw_model: str = "openclaw/default"
    openclaw_backend_model: str = ""
    openclaw_timeout: int = 45
    openclaw_session_prefix: str = "xiaopai"
    openclaw_max_completion_tokens: int = 512
    find_owner_gain_x: float = 1.0
    find_owner_gain_y: float = 0.8
    find_owner_stop_pixels: float = 32.0
    debug: bool = False


class RealtimeDeviceSession:
    def __init__(self, *, device_id: str, websocket, session_id: str) -> None:
        self.device_id = device_id
        self.websocket = websocket
        self.session_id = session_id
        self.connected_at = time.time()
        self.last_seen = self.connected_at
        self.last_stt = ""
        self.dialog_awake = False
        self.tts_task: asyncio.Task | None = None
        self.asr_bridge: RealtimeAsrBridge | None = None
        self.latency_started_at = time.perf_counter()
        self.latency_marks: dict[str, float] = {}

    def snapshot(self) -> dict:
        return {
            "device_id": self.device_id,
            "session_id": self.session_id,
            "connected_at": self.connected_at,
            "last_seen_seconds_ago": round(time.time() - self.last_seen, 1),
            "online": True,
            "last_stt": self.last_stt,
            "dialog_awake": self.dialog_awake,
            "asr_active": bool(self.asr_bridge and self.asr_bridge.active),
            "latency_ms": self.latency_snapshot(),
        }

    def reset_latency(self) -> None:
        self.latency_started_at = time.perf_counter()
        self.latency_marks = {"voice_start": self.latency_started_at}

    def mark_latency(self, name: str) -> float:
        now = time.perf_counter()
        self.latency_marks[name] = now
        return (now - self.latency_started_at) * 1000

    def latency_snapshot(self) -> dict[str, int]:
        return {
            name: int((marked_at - self.latency_started_at) * 1000)
            for name, marked_at in self.latency_marks.items()
        }


class RealtimeAsrBridge:
    def __init__(self, manager: "RealtimeManager", session: RealtimeDeviceSession) -> None:
        self.manager = manager
        self.device_id = session.device_id
        self.session_id = session.session_id
        self._queue: Queue[bytes | None] = Queue(maxsize=80)
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._run, name=f"asr-{session.device_id}", daemon=True)
        self._ws = None
        self._task_id = ""

    @property
    def active(self) -> bool:
        return self._thread.is_alive() and not self._stop.is_set()

    def start(self) -> None:
        self._thread.start()

    def stop(self) -> None:
        self._stop.set()
        try:
            self._queue.put_nowait(None)
        except Exception:
            pass
        ws = self._ws
        if ws is not None:
            try:
                if self._task_id:
                    ws.send(json.dumps(build_stop_transcription(self.manager.config.appkey, self._task_id), ensure_ascii=False))
            except Exception:
                pass
            try:
                ws.close()
            except Exception:
                pass

    def push_opus(self, opus_frame: bytes) -> None:
        try:
            pcm = self.manager.opus.decode(opus_frame)
        except OpusUnavailableError as exc:
            self.manager.logger(f"Realtime ASR unavailable: {exc}")
            self.stop()
            return
        try:
            self._queue.put_nowait(pcm)
        except Exception:
            self.manager.logger(f"Realtime ASR audio queue full: {self.device_id}")

    def _run(self) -> None:
        if not self.manager.config.appkey or self.manager.config.token_getter is None:
            self.manager.logger("Realtime ASR skipped: missing Aliyun appkey/token getter")
            return
        try:
            asr = AliyunStreamingAsrSession(
                appkey=self.manager.config.appkey,
                token_getter=self.manager.config.token_getter,
                region=self.manager.config.region,
                ws_url=self.manager.config.aliyun_asr_ws_url,
                sample_rate=self.manager.config.sample_rate,
            )
            ws, task_id = asr.connect()
            self._ws = ws
            self._task_id = task_id
            try:
                ws.settimeout(0.02)
            except Exception:
                pass
            while not self._stop.is_set():
                try:
                    pcm = self._queue.get(timeout=0.02)
                    if pcm is None:
                        break
                    ws.send_binary(pcm)
                except Empty:
                    pass
                except Exception as exc:
                    self.manager.logger(f"Realtime ASR send failed: {exc}")
                    break
                self._drain_events(ws)
            self._drain_events(ws)
        except Exception as exc:
            self.manager.logger(f"Realtime ASR bridge stopped: {exc}")
        finally:
            try:
                if self._ws is not None:
                    self._ws.close()
            except Exception:
                pass

    def _drain_events(self, ws) -> None:
        while not self._stop.is_set():
            try:
                raw = ws.recv()
            except Exception as exc:
                if exc.__class__.__name__ in ("WebSocketTimeoutException", "TimeoutError"):
                    return
                raise
            if isinstance(raw, bytes):
                continue
            event = parse_asr_event(raw)
            text = str(event.get("text") or "")
            if not text:
                if event.get("name") == "TaskFailed":
                    raise RuntimeError(f"Aliyun ASR failed: {event.get('raw')}")
                continue
            self.manager.submit_asr_text(
                self.device_id,
                self.session_id,
                text,
                is_final=bool(event.get("is_final")),
            )


class RealtimeManager:
    def __init__(self, config: RealtimeConfig, *, logger=print) -> None:
        self.config = config
        self.logger = logger
        self._thread: threading.Thread | None = None
        self._loop: asyncio.AbstractEventLoop | None = None
        self._server = None
        self._started = threading.Event()
        self._startup_error: BaseException | None = None
        self._sessions: dict[str, RealtimeDeviceSession] = {}
        self._sessions_lock = threading.Lock()
        self._mcp_tracker = McpRequestTracker()
        self._opus = OpusCodec(sample_rate=config.sample_rate)
        self._openclaw = OpenClawAgent(
            base_url=config.openclaw_base_url,
            token=config.openclaw_token,
            model=config.openclaw_model,
            backend_model=config.openclaw_backend_model,
            timeout=config.openclaw_timeout,
            session_prefix=config.openclaw_session_prefix,
            max_completion_tokens=config.openclaw_max_completion_tokens,
        )
        self._pcm_bytes_per_frame = self._opus.samples_per_frame * self._opus.channels * 2

    @property
    def opus(self) -> OpusCodec:
        return self._opus

    @property
    def enabled(self) -> bool:
        return self._thread is not None and self._thread.is_alive() and self._startup_error is None

    def start(self) -> None:
        if self._thread is not None:
            return
        self._thread = threading.Thread(target=self._run_loop, name="xiaozhi-realtime", daemon=True)
        self._thread.start()
        self._started.wait(timeout=5)
        if self._startup_error is not None:
            raise RuntimeError(f"xiaozhi realtime server failed to start: {self._startup_error}") from self._startup_error

    def _run_loop(self) -> None:
        loop = asyncio.new_event_loop()
        self._loop = loop
        asyncio.set_event_loop(loop)
        try:
            loop.run_until_complete(self._start_server())
            self._started.set()
            loop.run_forever()
        except BaseException as exc:
            self._startup_error = exc
            self._started.set()
            self.logger(f"Xiaozhi realtime loop stopped: {exc}")
        finally:
            if not loop.is_closed():
                loop.run_until_complete(self._close_sessions())
            loop.close()

    async def _start_server(self) -> None:
        try:
            import websockets  # type: ignore
        except Exception as exc:
            self._started.set()
            raise RuntimeError("websockets is required for xiaozhi realtime server") from exc
        self._server = await websockets.serve(
            self._dispatch,
            self.config.host,
            self.config.port,
            compression=None,
            ping_interval=None,
        )
        self.logger(f"Xiaozhi realtime WebSocket ready: ws://{self.config.host}:{self.config.port}{self.config.path}")

    async def _close_sessions(self) -> None:
        with self._sessions_lock:
            sessions = list(self._sessions.values())
            self._sessions.clear()
        for session in sessions:
            try:
                await session.websocket.close()
            except Exception:
                pass

    def stop(self) -> None:
        if self._loop is None:
            return
        self._loop.call_soon_threadsafe(self._loop.stop)

    def devices_snapshot(self) -> list[dict]:
        with self._sessions_lock:
            return [session.snapshot() for session in self._sessions.values()]

    def first_device_id(self) -> str:
        with self._sessions_lock:
            return next(iter(self._sessions), "default")

    def has_device(self, device_id: str) -> bool:
        with self._sessions_lock:
            if device_id in self._sessions:
                return True
            return bool(self._sessions and device_id in ("", "default"))

    def enqueue_command(self, device_id: str, command: dict) -> bool:
        if self._loop is None:
            return False
        future = asyncio.run_coroutine_threadsafe(self._send_command(device_id, command), self._loop)
        try:
            return bool(future.result(timeout=1.5))
        except Exception as exc:
            self.logger(f"Realtime command dispatch failed: {exc}")
            return False

    def set_device_state(self, device_id: str, state: str) -> bool:
        if self._loop is None:
            return False
        future = asyncio.run_coroutine_threadsafe(self._set_device_state(device_id, state), self._loop)
        try:
            return bool(future.result(timeout=1.5))
        except Exception as exc:
            self.logger(f"Realtime device state dispatch failed: {exc}")
            return False

    async def _set_device_state(self, device_id: str, state: str) -> bool:
        session = self._select_session(device_id)
        if session is None:
            return False
        await self._send_device_state(session, state)
        return True

    async def _send_command(self, device_id: str, command: dict) -> bool:
        session = self._select_session(device_id)
        if session is None:
            return False
        if command.get("interrupt"):
            await self._abort_session_tts(session)
        if command.get("type") in ("check_ota", "ota_check", "firmware_ota"):
            await session.websocket.send(
                json_dumps({"type": "command", "command": command, "session_id": session.session_id})
            )
            return True
        if command.get("type") in ("state", "device_state"):
            payload = command.get("payload") if isinstance(command.get("payload"), dict) else {}
            state = str(payload.get("state") or payload.get("name") or "waiting")
            await self._send_device_state(session, state)
            return True
        if command.get("type") == "speak":
            payload = command.get("payload") if isinstance(command.get("payload"), dict) else {}
            await self._speak(
                session,
                str(payload.get("text") or ""),
                cache_name=str(payload.get("cache_name") or ""),
                pause_listener=bool(payload.get("pause_listener", payload.get("pause_voice_listener", True))),
                tts_options={
                    key: payload[key]
                    for key in ("voice", "sample_rate", "volume", "speech_rate", "pitch_rate")
                    if key in payload and payload[key] not in (None, "")
                },
            )
            return True
        return await self._send_mcp_command(session, command)

    async def _send_mcp_command(self, session: RealtimeDeviceSession, command: dict) -> bool:
        sent = False
        for call in command_to_mcp_calls(command):
            request_id = make_request_id("mcp")
            self._mcp_tracker.add(request_id, device_id=session.device_id, tool_name=call.name, command_id=command.get("cmd_id", ""))
            await session.websocket.send(json_dumps(build_mcp_tools_call(call.name, call.arguments, request_id=request_id)))
            sent = True
        return sent

    def _select_session(self, device_id: str) -> RealtimeDeviceSession | None:
        with self._sessions_lock:
            if device_id in self._sessions:
                return self._sessions[device_id]
            if device_id in ("", "default") and self._sessions:
                return next(iter(self._sessions.values()))
            return None

    async def _dispatch(self, websocket, path=None) -> None:
        if path is None:
            request = getattr(websocket, "request", None)
            path = getattr(websocket, "path", "") or getattr(request, "path", "")
        parsed = urlparse(path or "")
        self.logger(f"Xiaozhi realtime WebSocket accepted: path={parsed.path!r} query={parsed.query!r}")
        if parsed.path != self.config.path:
            await websocket.close(code=1008, reason="invalid path")
            return
        if not self._authorized(websocket, parsed.query):
            await websocket.close(code=1008, reason="unauthorized")
            return
        session = RealtimeDeviceSession(
            device_id=self._device_id_from_request(websocket, parsed.query),
            websocket=websocket,
            session_id=make_request_id("sess"),
        )
        self._register_session(session)
        try:
            hello = json_dumps(build_hello(session.session_id))
            hello_resent_after_device_hello = False
            await websocket.send(hello)
            self.logger(f"Xiaozhi realtime server hello sent: device_id={session.device_id} bytes={len(hello)}")
            await self._send_device_state(session, "idle")
            async for frame in websocket:
                if isinstance(frame, bytes):
                    session.last_seen = time.time()
                    if session.asr_bridge is None:
                        self.logger(f"Realtime ASR auto-start on binary audio: device_id={session.device_id}")
                        self._start_asr(session)
                    if session.asr_bridge is not None:
                        session.asr_bridge.push_opus(frame)
                    continue
                message = parse_json_message(frame)
                if message.get("type") == "hello":
                    device_id = safe_realtime_device_id(extract_device_id_from_hello(message, session.device_id))
                    self.logger(f"Xiaozhi realtime hello received: device_id={device_id} type={message.get('type')!r}")
                    self._update_session_device_id(session, device_id)
                    if not hello_resent_after_device_hello:
                        await websocket.send(hello)
                        hello_resent_after_device_hello = True
                        self.logger(
                            f"Xiaozhi realtime server hello resent after device hello: device_id={session.device_id} bytes={len(hello)}"
                        )
                    continue
                session.last_seen = time.time()
                await self._handle_json(session, message)
        except Exception as exc:
            self.logger(f"Xiaozhi realtime session ended: {exc}")
        finally:
            self._stop_asr(session)
            await self._abort_session_tts(session)
            self._unregister_session(session.device_id, session.session_id)

    def _authorized(self, websocket, query: str) -> bool:
        if not self.config.token:
            return True
        token = ""
        headers = self._request_headers(websocket)
        auth = headers.get("Authorization") or headers.get("authorization") or ""
        if auth.lower().startswith("bearer "):
            token = auth.split(" ", 1)[1].strip()
        if not token:
            token = (parse_qs(query).get("token") or [""])[0]
        return token == self.config.token

    def _request_headers(self, websocket) -> dict:
        headers = getattr(websocket, "request_headers", {}) or {}
        if not headers:
            request = getattr(websocket, "request", None)
            headers = getattr(request, "headers", {}) or {}
        return headers

    def _device_id_from_request(self, websocket, query: str) -> str:
        headers = self._request_headers(websocket)
        for key in ("Device-Id", "device-id", "Client-Id", "client-id"):
            value = str(headers.get(key) or "").strip()
            if value:
                return safe_realtime_device_id(value)
        value = (parse_qs(query).get("device_id") or [""])[0].strip()
        return safe_realtime_device_id(value)

    def _register_session(self, session: RealtimeDeviceSession) -> None:
        with self._sessions_lock:
            self._sessions[session.device_id] = session
        self.logger(f"Xiaozhi device connected: {session.device_id}")

    def _update_session_device_id(self, session: RealtimeDeviceSession, device_id: str) -> None:
        device_id = safe_realtime_device_id(device_id)
        old_device_id = session.device_id
        if device_id == old_device_id:
            return
        with self._sessions_lock:
            current = self._sessions.get(old_device_id)
            if current is session:
                self._sessions.pop(old_device_id, None)
            session.device_id = device_id
            self._sessions[device_id] = session
        if session.asr_bridge is not None:
            session.asr_bridge.device_id = device_id
        self.logger(f"Xiaozhi device id updated: {old_device_id} -> {device_id}")

    def _unregister_session(self, device_id: str, session_id: str) -> None:
        with self._sessions_lock:
            current = self._sessions.get(device_id)
            if current and current.session_id == session_id:
                self._sessions.pop(device_id, None)
        self.logger(f"Xiaozhi device disconnected: {device_id}")

    async def _handle_json(self, session: RealtimeDeviceSession, message: dict) -> None:
        message_type = message.get("type")
        if message_type == "abort":
            await self._abort_session_tts(session)
            return
        if message_type == "listen":
            state = str(message.get("state") or "")
            if state in ("start", "detect"):
                await self._abort_session_tts(session)
                self._start_asr(session)
            elif state == "stop":
                self._stop_asr(session)
            return
        if message_type == "mcp":
            payload = message.get("payload") if isinstance(message.get("payload"), dict) else {}
            if "id" in payload:
                self._mcp_tracker.pop(str(payload["id"]))
            return
        if message_type == "stt":
            text = str(message.get("text") or "")
            if text:
                final = bool(message.get("is_final", True))
                session.last_stt = text
                if final:
                    await self._handle_final_text(session, text)

    def submit_asr_text(self, device_id: str, session_id: str, text: str, *, is_final: bool) -> None:
        if self._loop is None:
            return
        asyncio.run_coroutine_threadsafe(
            self._handle_asr_text(device_id, session_id, text, is_final=is_final),
            self._loop,
        )

    async def _handle_asr_text(self, device_id: str, session_id: str, text: str, *, is_final: bool) -> None:
        session = self._select_session(device_id)
        if session is None or session.session_id != session_id:
            return
        session.last_stt = text
        if is_final:
            self._mark(session, "asr_final")
        elif "asr_first_partial" not in session.latency_marks:
            self._mark(session, "asr_first_partial")
        await session.websocket.send(json_dumps(build_stt(text, is_final=is_final, session_id=session.session_id)))
        if is_final:
            self._stop_asr(session)
            await self._handle_final_text(session, text)

    def _start_asr(self, session: RealtimeDeviceSession) -> None:
        if session.asr_bridge and session.asr_bridge.active:
            return
        session.reset_latency()
        self._mark(session, "asr_start")
        session.asr_bridge = RealtimeAsrBridge(self, session)
        session.asr_bridge.start()

    def _stop_asr(self, session: RealtimeDeviceSession) -> None:
        bridge = session.asr_bridge
        session.asr_bridge = None
        if bridge is not None:
            bridge.stop()

    async def _send_device_state(self, session: RealtimeDeviceSession, state: str) -> None:
        await session.websocket.send(
            json_dumps({"type": "device_state", "state": state, "session_id": session.session_id})
        )

    async def _handle_final_text(self, session: RealtimeDeviceSession, text: str) -> None:
        if has_realtime_sleep_word(text):
            reply_name, reply_text = realtime_sleep_reply_event_for_text(text)
            self._mark(session, "sleep_reply_start")
            await self._speak(session, reply_text, cache_name=reply_name)
            session.dialog_awake = False
            self._mark(session, "dialog_sleep")
            await self._send_device_state(session, "sleep")
            return
        if has_realtime_wake_word(text):
            was_awake = session.dialog_awake
            session.dialog_awake = True
            await self._send_device_state(session, "listening")
            if is_realtime_wake_only_text(text):
                self._mark(session, "wake_reply_start")
                await self._speak(session, random.choice(REALTIME_WAKE_REPLIES))
                if not was_awake:
                    await self._send_wake_find_owner(session)
                return
            if not was_awake:
                await self._send_wake_find_owner(session)
        elif not session.dialog_awake:
            self._mark(session, "dialog_sleeping_ignore")
            await self._send_device_state(session, "sleep")
            return
        if not self._openclaw.enabled:
            await self._speak(session, "我没听清，可以再说一遍吗")
            return
        loop = asyncio.get_running_loop()
        try:
            self._mark(session, "openclaw_start")
            await self._send_device_state(session, "waiting")
            reply = await loop.run_in_executor(None, self._openclaw.chat, session.device_id, text)
            self._mark(session, "openclaw_done")
        except Exception as exc:
            self.logger(f"OpenClaw realtime chat failed: {exc}")
            reply = ""
            await self._speak(session, "我没听清，可以再说一遍吗")
            return
        if reply:
            await session.websocket.send(json_dumps(build_llm(reply, session_id=session.session_id)))
            self.logger("Realtime OpenClaw reply left to command playback")

    async def _send_wake_find_owner(self, session: RealtimeDeviceSession) -> None:
        command = {
            "cmd_id": make_request_id("cmd"),
            "type": "find_owner",
            "payload": {
                "rounds": 1,
                "reply": "",
                "preserve_speech": True,
                "wait_for_speech": False,
                "gain_x": self.config.find_owner_gain_x,
                "gain_y": self.config.find_owner_gain_y,
                "stop_pixels": self.config.find_owner_stop_pixels,
            },
        }
        if await self._send_mcp_command(session, command):
            self._mark(session, "wake_find_owner_sent")
            self.logger(f"Realtime wake find-owner sent: device_id={session.device_id}")
        else:
            self.logger(f"Realtime wake find-owner not sent: device_id={session.device_id}")

    async def _speak(
        self,
        session: RealtimeDeviceSession,
        text: str,
        *,
        cache_name: str = "",
        pause_listener: bool = True,
        tts_options: dict | None = None,
    ) -> None:
        text = str(text or "").strip()
        if not text:
            return
        await self._abort_session_tts(session)
        self._mark(session, "device_tts_start")
        await session.websocket.send(json_dumps(build_llm(text, session_id=session.session_id)))
        speak_step = {"type": "speak", "text": text, "pause_listener": bool(pause_listener)}
        if isinstance(tts_options, dict):
            for key in ("voice", "sample_rate", "volume", "speech_rate", "pitch_rate"):
                if key in tts_options and tts_options[key] not in (None, ""):
                    speak_step[key] = tts_options[key]
        cache_name = str(cache_name or "").strip()
        if cache_name:
            speak_step["cache_name"] = cache_name
        command = {
            "cmd_id": make_request_id("cmd"),
            "type": "sequence",
            "payload": [
                speak_step,
                {"type": "face", "expression": "calm"},
            ],
        }
        if await self._send_mcp_command(session, command):
            self._mark(session, "device_speak_command_sent")
        else:
            self.logger(f"Realtime speak command not sent: device_id={session.device_id}")

    async def _abort_session_tts(self, session: RealtimeDeviceSession) -> None:
        task = session.tts_task
        if task is not None and not task.done():
            task.cancel()
            try:
                await task
            except asyncio.CancelledError:
                pass
            except Exception:
                pass
        session.tts_task = None

    async def _run_tts(self, session: RealtimeDeviceSession, text: str) -> None:
        await session.websocket.send(json_dumps(build_llm(text, session_id=session.session_id)))
        await session.websocket.send(json_dumps(build_tts_state("start", session_id=session.session_id)))
        try:
            for sentence in split_sentences(text, self.config.max_sentence_chars):
                await session.websocket.send(json_dumps(build_tts_state("sentence_start", text=sentence, session_id=session.session_id)))
                await self._send_sentence_audio(session, sentence)
        finally:
            await session.websocket.send(json_dumps(build_tts_state("stop", session_id=session.session_id)))

    async def _send_sentence_audio(self, session: RealtimeDeviceSession, sentence: str) -> None:
        if not self.config.appkey or self.config.token_getter is None:
            return
        tts = AliyunStreamingTtsClient(
            appkey=self.config.appkey,
            token_getter=self.config.token_getter,
            region=self.config.region,
            ws_url=self.config.aliyun_tts_ws_url,
            voice=self.config.voice,
            sample_rate=self.config.sample_rate,
            volume=self.config.volume,
            speech_rate=self.config.speech_rate,
            pitch_rate=self.config.pitch_rate,
        )
        loop = asyncio.get_running_loop()
        queue: Queue[bytes | BaseException | None] = Queue(maxsize=16)

        def produce_pcm() -> None:
            try:
                for pcm_chunk in tts.iter_pcm_chunks(sentence):
                    queue.put(pcm_chunk)
            except BaseException as exc:
                queue.put(exc)
            finally:
                queue.put(None)

        threading.Thread(target=produce_pcm, name=f"tts-{session.device_id}", daemon=True).start()

        pcm_buffer = bytearray()
        try:
            while True:
                item = await loop.run_in_executor(None, queue.get)
                if item is None:
                    break
                if isinstance(item, BaseException):
                    raise item
                if item and "tts_first_pcm" not in session.latency_marks:
                    self._mark(session, "tts_first_pcm")
                pcm_buffer.extend(item)
                while len(pcm_buffer) >= self._pcm_bytes_per_frame:
                    pcm_frame = bytes(pcm_buffer[: self._pcm_bytes_per_frame])
                    del pcm_buffer[: self._pcm_bytes_per_frame]
                    await session.websocket.send(self._opus.encode(pcm_frame))
                    if "tts_first_opus_sent" not in session.latency_marks:
                        self._mark(session, "tts_first_opus_sent")
                    await asyncio.sleep(0)
            if pcm_buffer:
                pcm_frame = bytes(pcm_buffer).ljust(self._pcm_bytes_per_frame, b"\x00")
                await session.websocket.send(self._opus.encode(pcm_frame))
                if "tts_first_opus_sent" not in session.latency_marks:
                    self._mark(session, "tts_first_opus_sent")
        except OpusUnavailableError as exc:
            self.logger(f"Realtime TTS audio unavailable: {exc}")
        except Exception as exc:
            self.logger(f"Realtime streaming TTS failed: {exc}")

    def _mark(self, session: RealtimeDeviceSession, name: str) -> None:
        elapsed_ms = session.mark_latency(name)
        if self.config.debug:
            self.logger(f"latency device={session.device_id} stage={name} elapsed_ms={elapsed_ms:.0f}")
