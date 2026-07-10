import json
import queue
import re
import threading
import time
import urllib.parse
from collections import OrderedDict
from dataclasses import dataclass, field
from typing import Any

from xiaopai_openclaw_prompt import XIAOPAI_OPENCLAW_SYSTEM_PROMPT


SYSTEM_PROMPT = XIAOPAI_OPENCLAW_SYSTEM_PROMPT
STREAM_SEGMENT_PUNCTUATION = set("。！？!?；;，,、：:\n")


def safe_openclaw_session_part(value: str) -> str:
    safe = re.sub(r"[^A-Za-z0-9_.:-]+", "_", str(value or "").strip())[:64]
    return safe or "default"


def build_openclaw_session_key(session_prefix: str, device_id: str) -> str:
    prefix = safe_openclaw_session_part(session_prefix or "xiaopai")
    return f"{prefix}-{safe_openclaw_session_part(device_id)}"


def build_morrow_session_id(session_prefix: str, _device_id: str = "") -> str:
    return safe_openclaw_session_part(session_prefix or "xiaopai")


def build_morrow_conversation_id(session_prefix: str, device_id: str) -> str:
    return build_openclaw_session_key(session_prefix or "xiaopai", device_id or "default")


def build_morrow_ws_url(base_url: str, session_id: str) -> str:
    base_url = str(base_url or "").strip()
    if not base_url:
        return ""

    quoted_session = urllib.parse.quote(session_id, safe="-_.:")
    if "{session}" in base_url:
        return base_url.replace("{session}", quoted_session)

    parsed = urllib.parse.urlparse(base_url)
    scheme = parsed.scheme.lower()
    if scheme in ("ws", "wss"):
        if parsed.path.rstrip("/").endswith("/ws"):
            return base_url
        path = parsed.path.rstrip("/") + f"/api/sessions/{quoted_session}/ws"
        return urllib.parse.urlunparse((scheme, parsed.netloc, path, "", "", ""))

    ws_scheme = "wss" if scheme == "https" else "ws"
    path = parsed.path.rstrip("/")
    if "/api/sessions/" in path:
        path = path[: path.find("/api/sessions/")]
    path = path.rstrip("/") + f"/api/sessions/{quoted_session}/ws"
    return urllib.parse.urlunparse((ws_scheme, parsed.netloc, path, "", "", ""))


def is_legacy_chat_completions_base_url(_base_url: str) -> bool:
    return False


class PunctuationTextSegmenter:
    def __init__(self, *, max_chars: int = 120):
        self.max_chars = max(1, int(max_chars or 120))
        self._buffer: list[str] = []

    def feed(self, text: str):
        for ch in str(text or ""):
            self._buffer.append(ch)
            if ch in STREAM_SEGMENT_PUNCTUATION or len(self._buffer) >= self.max_chars:
                part = "".join(self._buffer).strip()
                self._buffer.clear()
                if part:
                    yield part

    def flush(self):
        part = "".join(self._buffer).strip()
        self._buffer.clear()
        if part:
            yield part


def extract_openclaw_text(response_text: str) -> str:
    data = json.loads(response_text)
    choices = data.get("choices") if isinstance(data, dict) else None
    if not choices:
        return ""
    first = choices[0] if isinstance(choices[0], dict) else {}
    message = first.get("message") if isinstance(first.get("message"), dict) else {}
    content = message.get("content")
    if isinstance(content, str):
        return content.strip()
    if isinstance(content, list):
        return "".join(str(item.get("text") or "") for item in content if isinstance(item, dict) and item.get("type") == "text").strip()
    return str(first.get("text") or "").strip()


def extract_morrow_robot_notice_text(message: dict) -> str:
    if not isinstance(message, dict) or str(message.get("type") or "") != "robot_notice":
        return ""
    data = message.get("data") if isinstance(message.get("data"), dict) else {}
    return str(data.get("text") or "").strip()


@dataclass
class _TurnState:
    request_id: str
    conversation_id: str
    events: "queue.Queue[dict[str, Any]]" = field(default_factory=queue.Queue)
    final_message: str = ""
    saw_delta: bool = False


class MorrowStreamClient:
    def __init__(
        self,
        *,
        base_url: str,
        token: str,
        session_id: str,
        timeout: int,
        max_segment_chars: int,
    ) -> None:
        self.base_url = base_url
        self.token = token
        self.session_id = session_id
        self.timeout = max(1, int(timeout or 45))
        self.max_segment_chars = max_segment_chars
        self.ws_url = build_morrow_ws_url(base_url, session_id)
        self._ws = None
        self._websocket_module = None
        self._lock = threading.RLock()
        self._stop = threading.Event()
        self._connected = threading.Event()
        self._outgoing: "queue.Queue[dict[str, Any] | None]" = queue.Queue(maxsize=128)
        self._notices: "queue.Queue[dict[str, Any]]" = queue.Queue(maxsize=256)
        self._turns: "OrderedDict[str, _TurnState]" = OrderedDict()
        self._active_by_conversation: dict[str, str] = {}
        self._reader: threading.Thread | None = None
        self._writer: threading.Thread | None = None
        self._last_error: BaseException | None = None

    @property
    def connected(self) -> bool:
        return self._connected.is_set() and self._reader is not None and self._reader.is_alive()

    def start(self) -> None:
        if not self.ws_url:
            raise RuntimeError("Morrow stream URL is empty")
        with self._lock:
            if self.connected:
                return
            self._stop.clear()
            self._last_error = None
            try:
                import websocket  # type: ignore
            except Exception as exc:
                raise RuntimeError("websocket-client is required for Morrow streaming") from exc

            headers = []
            if self.token:
                headers.append(f"Authorization: Bearer {self.token}")
            self._websocket_module = websocket
            self._ws = websocket.create_connection(self.ws_url, timeout=self.timeout, header=headers)
            try:
                self._ws.settimeout(min(self.timeout, 10))
            except Exception:
                pass
            self._reader = threading.Thread(target=self._read_loop, name=f"morrow-read-{self.session_id}", daemon=True)
            self._writer = threading.Thread(target=self._write_loop, name=f"morrow-write-{self.session_id}", daemon=True)
            self._reader.start()
            self._writer.start()
            self._connected.set()

    def stop(self) -> None:
        self._stop.set()
        self._connected.clear()
        self._outgoing.put(None)
        ws = self._ws
        if ws is not None:
            try:
                ws.close()
            except Exception:
                pass

    def chat_stream(self, *, conversation_id: str, prompt: str):
        request_id = f"xiaopai-{int(time.time() * 1000)}-{threading.get_ident()}"
        turn = _TurnState(request_id=request_id, conversation_id=conversation_id)
        with self._lock:
            self._turns[request_id] = turn
            self._active_by_conversation[conversation_id] = request_id

        segmenter = PunctuationTextSegmenter(max_chars=self.max_segment_chars)
        try:
            self.start()
            self._send_frame(
                {
                    "type": "start_turn",
                    "data": {
                        "request_id": request_id,
                        "conversation_id": conversation_id,
                        "prompt": str(prompt or ""),
                    },
                }
            )
            while True:
                event = turn.events.get(timeout=self.timeout)
                event_type = str(event.get("type") or "")
                if event_type == "text_delta":
                    turn.saw_delta = True
                    for segment in segmenter.feed(str(event.get("data") or "")):
                        yield segment
                    continue
                if event_type == "agent_message":
                    turn.final_message = str(event.get("data") or "")
                    continue
                if event_type == "turn_saved":
                    break
                if event_type == "turn_rejected":
                    raise RuntimeError(f"Morrow turn rejected: {event.get('reason') or 'unknown reason'}")
                if event_type == "error":
                    raise RuntimeError(f"Morrow error: {event.get('message') or 'unknown error'}")

            if turn.saw_delta:
                yield from segmenter.flush()
            elif turn.final_message:
                for segment in segmenter.feed(turn.final_message):
                    yield segment
                yield from segmenter.flush()
        finally:
            with self._lock:
                self._turns.pop(request_id, None)
                if self._active_by_conversation.get(conversation_id) == request_id:
                    self._active_by_conversation.pop(conversation_id, None)

    def notice_stream(self, *, stop_event=None):
        self.start()
        while stop_event is None or not stop_event.is_set():
            try:
                message = self._notices.get(timeout=0.5)
            except queue.Empty:
                if self._last_error is not None and not self.connected:
                    raise RuntimeError(f"Morrow stream disconnected: {self._last_error}") from self._last_error
                continue
            text = extract_morrow_robot_notice_text(message)
            if text:
                yield text

    def cancel_turn(self, *, conversation_id: str, reason: str = "cancelled") -> bool:
        self.start()
        with self._lock:
            request_id = self._active_by_conversation.get(conversation_id, "")
        self._send_frame(
            {
                "type": "cancel_turn",
                "data": {
                    "conversation_id": conversation_id,
                    "request_id": request_id,
                    "reason": reason,
                },
            }
        )
        return True

    def send_device_result(self, payload: dict[str, Any]) -> bool:
        self.start()
        self._send_frame({"type": "device_result", "data": payload})
        return True

    def _send_frame(self, frame: dict[str, Any]) -> None:
        self._outgoing.put(frame, timeout=self.timeout)
        self._outgoing.join()

    def _write_loop(self) -> None:
        while not self._stop.is_set():
            frame = self._outgoing.get()
            try:
                if frame is None:
                    return
                ws = self._ws
                if ws is None:
                    raise RuntimeError("Morrow websocket is not connected")
                ws.send(json.dumps(frame, ensure_ascii=False))
            except BaseException as exc:
                self._last_error = exc
                self._connected.clear()
                self._fail_all_turns(exc)
            finally:
                self._outgoing.task_done()

    def _read_loop(self) -> None:
        timeout_error = getattr(self._websocket_module, "WebSocketTimeoutException", None)
        try:
            while not self._stop.is_set():
                try:
                    raw = self._ws.recv()
                except Exception as exc:
                    if timeout_error is not None and isinstance(exc, timeout_error):
                        continue
                    raise
                if raw in (None, ""):
                    raise RuntimeError("Morrow websocket closed")
                try:
                    message = json.loads(raw)
                except (TypeError, json.JSONDecodeError):
                    continue
                self._dispatch_message(message)
        except BaseException as exc:
            self._last_error = exc
            self._connected.clear()
            self._fail_all_turns(exc)
        finally:
            self._connected.clear()

    def _dispatch_message(self, message: dict[str, Any]) -> None:
        message_type = str(message.get("type") or "")
        if message_type == "robot_notice":
            self._notices.put(message)
            return
        if message_type == "agent_event":
            event = message.get("data", {}).get("event") if isinstance(message.get("data"), dict) else {}
            if isinstance(event, dict):
                self._route_turn_event(event)
            return
        if message_type in ("turn_saved", "turn_rejected", "error"):
            self._route_turn_event(message)

    def _route_turn_event(self, event: dict[str, Any]) -> None:
        data = event.get("data") if isinstance(event.get("data"), dict) else event.get("data")
        if isinstance(data, dict):
            request_id = str(data.get("request_id") or "")
            conversation_id = str(data.get("conversation_id") or "")
        else:
            request_id = str(event.get("request_id") or "")
            conversation_id = str(event.get("conversation_id") or "")

        with self._lock:
            turn = self._turns.get(request_id) if request_id else None
            if turn is None and conversation_id:
                active = self._active_by_conversation.get(conversation_id)
                turn = self._turns.get(active or "")
            if turn is None and len(self._turns) == 1:
                turn = next(iter(self._turns.values()))
        if turn is None:
            return

        event_type = str(event.get("type") or "")
        if event_type == "text_delta":
            turn.events.put({"type": "text_delta", "data": data})
        elif event_type == "agent_message":
            turn.events.put({"type": "agent_message", "data": data})
        elif event_type == "turn_saved":
            turn.events.put({"type": "turn_saved"})
        elif event_type == "turn_rejected":
            reason = data.get("reason") if isinstance(data, dict) else ""
            turn.events.put({"type": "turn_rejected", "reason": reason})
        elif event_type == "error":
            message = data.get("message") if isinstance(data, dict) else ""
            turn.events.put({"type": "error", "message": message})

    def _fail_all_turns(self, exc: BaseException) -> None:
        with self._lock:
            turns = list(self._turns.values())
        for turn in turns:
            turn.events.put({"type": "error", "message": str(exc)})


class OpenClawAgent:
    _clients: dict[tuple[str, str, str], MorrowStreamClient] = {}
    _clients_lock = threading.Lock()

    def __init__(
        self,
        *,
        base_url: str,
        token: str,
        model: str,
        backend_model: str = "",
        timeout: int = 45,
        session_prefix: str = "xiaopai",
        max_completion_tokens: int = 512,
        max_segment_chars: int = 120,
    ):
        self.base_url = base_url
        self.token = token
        self.model = model
        self.backend_model = backend_model
        self.timeout = timeout
        self.session_prefix = session_prefix or "xiaopai"
        self.max_completion_tokens = max_completion_tokens
        self.max_segment_chars = max_segment_chars

    @property
    def enabled(self) -> bool:
        return bool(self.base_url)

    def chat(self, device_id: str, user_text: str) -> str:
        if not self.enabled:
            return ""
        return "".join(self.chat_stream(device_id, user_text)).strip()

    def chat_stream(self, device_id: str, user_text: str):
        if not self.enabled:
            return
        client = self._stream_client()
        yield from client.chat_stream(
            conversation_id=build_morrow_conversation_id(self.session_prefix, device_id),
            prompt=str(user_text or ""),
        )

    def morrow_robot_notice_stream(self, *, stop_event=None, session_id: str = ""):
        if not self.enabled:
            return
        client = self._stream_client(session_id=session_id or build_morrow_session_id(self.session_prefix))
        yield from client.notice_stream(stop_event=stop_event)

    def cancel_device_turn(self, device_id: str, reason: str = "cancelled") -> bool:
        if not self.enabled:
            return False
        return self._stream_client().cancel_turn(
            conversation_id=build_morrow_conversation_id(self.session_prefix, device_id),
            reason=reason,
        )

    def send_device_result(self, payload: dict[str, Any]) -> bool:
        if not self.enabled:
            return False
        return self._stream_client().send_device_result(payload)

    def close(self) -> None:
        key = self._client_key(build_morrow_session_id(self.session_prefix))
        with self._clients_lock:
            client = self._clients.pop(key, None)
        if client is not None:
            client.stop()

    def _stream_client(self, *, session_id: str | None = None) -> MorrowStreamClient:
        session = session_id or build_morrow_session_id(self.session_prefix)
        key = self._client_key(session)
        with self._clients_lock:
            client = self._clients.get(key)
            if client is None:
                client = MorrowStreamClient(
                    base_url=self.base_url,
                    token=self.token,
                    session_id=session,
                    timeout=self.timeout,
                    max_segment_chars=self.max_segment_chars,
                )
                self._clients[key] = client
        return client

    def _client_key(self, session_id: str) -> tuple[str, str, str]:
        return (str(self.base_url or ""), str(self.token or ""), session_id)
