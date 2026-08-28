"""Native client for one Morrow robot session WebSocket.

The client owns one reconnecting connection. It never replays an outbound turn after
a disconnect because prompts may have side effects in Morrow.
"""

from __future__ import annotations

import json
import logging
import queue
import threading
import time
import urllib.parse
import urllib.request
from typing import Any, Callable

from morrow_protocol import (
    MorrowEvent,
    build_cancel_turn,
    build_reset_session,
    build_start_turn,
    parse_message,
    snapshot_running_turn_id,
)


LOGGER = logging.getLogger(__name__)


def build_morrow_ws_url(base_url: str, session: str = "default") -> str:
    base_url = str(base_url or "").strip()
    if not base_url:
        return ""
    session = urllib.parse.quote(str(session or "default"), safe="-_.:")
    parsed = urllib.parse.urlparse(base_url)
    scheme = parsed.scheme.lower()
    ws_scheme = scheme if scheme in ("ws", "wss") else ("wss" if scheme == "https" else "ws")
    path = parsed.path.rstrip("/")
    if path.endswith("/ws"):
        return urllib.parse.urlunparse((ws_scheme, parsed.netloc, path, "", "", ""))
    if "/api/sessions/" in path:
        path = path[: path.index("/api/sessions/")]
    path = f"{path}/api/sessions/{session}/ws"
    return urllib.parse.urlunparse((ws_scheme, parsed.netloc, path, "", "", ""))


class MorrowClient:
    def __init__(
        self,
        *,
        base_url: str,
        session: str = "default",
        auth_token: str = "",
        connect_timeout: float = 10,
        reconnect_min: float = 1,
        reconnect_max: float = 30,
        event_queue_size: int = 256,
        websocket_factory: Callable[..., Any] | None = None,
    ) -> None:
        self.base_url = str(base_url or "").strip()
        self.session = str(session or "default").strip() or "default"
        self.auth_token = str(auth_token or "")
        self.connect_timeout = max(0.1, float(connect_timeout))
        self.reconnect_min = max(0.01, float(reconnect_min))
        self.reconnect_max = max(self.reconnect_min, float(reconnect_max))
        self.ws_url = build_morrow_ws_url(self.base_url, self.session)
        self.events: queue.Queue[MorrowEvent] = queue.Queue(maxsize=max(1, event_queue_size))
        self.notices: queue.Queue[dict[str, Any]] = queue.Queue(maxsize=max(1, event_queue_size))
        self._websocket_factory = websocket_factory
        self._stop = threading.Event()
        self._connected = threading.Event()
        self._ready = threading.Event()
        self._thread: threading.Thread | None = None
        self._ws: Any = None
        self._send_lock = threading.Lock()
        self._state_lock = threading.Lock()
        self._session_lock = threading.Lock()
        self._session_generation = 0
        self._reconnect_now = threading.Event()
        self._morrow_turn_id = ""
        self.last_message_at = 0.0
        self.last_notice_at = 0.0
        self.last_error = ""
        self.metrics = {"morrow_ws_reconnect_total": 0, "morrow_notice_received_total": 0}

    @property
    def connected(self) -> bool:
        return self._connected.is_set()

    @property
    def ready(self) -> bool:
        return self._connected.is_set() and self._ready.is_set()

    @property
    def morrow_turn_id(self) -> str:
        with self._state_lock:
            return self._morrow_turn_id

    def start(self) -> None:
        if not self.ws_url:
            raise RuntimeError("Morrow WebSocket URL is empty")
        if self._thread is not None and self._thread.is_alive():
            return
        self._stop.clear()
        self._reconnect_now.clear()
        self._thread = threading.Thread(target=self._connection_loop, name="morrow-client", daemon=True)
        self._thread.start()

    def stop(self) -> None:
        self._stop.set()
        self._reconnect_now.set()
        self._ready.clear()
        self._connected.clear()
        ws = self._ws
        if ws is not None:
            try:
                ws.close()
            except Exception:
                pass
        thread = self._thread
        if thread is not None and thread is not threading.current_thread():
            thread.join(timeout=self.connect_timeout + 1)

    def switch_session(self, session: str, timeout: float | None = None) -> None:
        """Reconnect this client to a different, already-created Morrow session."""
        session = str(session or "").strip()
        if not session:
            raise ValueError("Morrow session is required")
        ws_url = build_morrow_ws_url(self.base_url, session)
        if not ws_url:
            raise RuntimeError("Morrow WebSocket URL is empty")

        with self._session_lock:
            if session == self.session and self.ready:
                return
            self.session = session
            self.ws_url = ws_url
            self._session_generation += 1
            self._ready.clear()
            self._connected.clear()
            ws = self._ws

        # Closing the old socket retires its reader.  The separate wake event
        # skips any reconnect backoff left over from the Morrow process restart.
        if ws is not None:
            try:
                ws.close()
            except Exception:
                pass
        self._reconnect_now.set()
        self.start()
        wait_timeout = self.connect_timeout if timeout is None else max(0.1, float(timeout))
        if not self._ready.wait(wait_timeout):
            detail = self.last_error or "snapshot was not received"
            raise RuntimeError(f"Morrow session switch failed: {detail}")

    def wait_ready(self, timeout: float | None = None) -> bool:
        self.start()
        return self._ready.wait(timeout)

    def check_status(self) -> bool:
        """Perform the deployment startup probe without making availability fatal."""
        parsed = urllib.parse.urlparse(self.base_url)
        scheme = "https" if parsed.scheme == "wss" else "http" if parsed.scheme == "ws" else parsed.scheme
        url = urllib.parse.urlunparse((scheme or "http", parsed.netloc, "/api/status", "", "", ""))
        request = urllib.request.Request(url, headers={"Authorization": f"Bearer {self.auth_token}"} if self.auth_token else {})
        try:
            with urllib.request.urlopen(request, timeout=self.connect_timeout) as response:
                return 200 <= int(response.status) < 300
        except Exception as exc:
            self.last_error = f"status check failed: {exc}"
            return False

    def start_turn(self, request_id: str, prompt: str) -> None:
        self._send(build_start_turn(request_id, prompt))

    def cancel_turn(self) -> bool:
        turn_id = self.morrow_turn_id
        if not turn_id:
            return False
        self._send(build_cancel_turn(turn_id))
        return True

    def reset_session(self, request_id: str) -> None:
        self._send(build_reset_session(request_id))

    def _send(self, frame: dict[str, Any]) -> None:
        if not self.ready:
            raise RuntimeError("Morrow is not ready (snapshot not received)")
        payload = json.dumps(frame, ensure_ascii=False)
        with self._send_lock:
            if not self.ready or self._ws is None:
                raise RuntimeError("Morrow disconnected before send")
            self._ws.send(payload)

    def _connect(self, ws_url: str | None = None) -> Any:
        ws_url = ws_url or self.ws_url
        headers = [f"Authorization: Bearer {self.auth_token}"] if self.auth_token else []
        if self._websocket_factory is not None:
            ws = self._websocket_factory(ws_url, timeout=self.connect_timeout, header=headers)
            self._disable_read_timeout(ws)
            return ws
        try:
            import websocket  # type: ignore
        except Exception as exc:
            raise RuntimeError("websocket-client is required for Morrow") from exc
        ws = websocket.create_connection(ws_url, timeout=self.connect_timeout, header=headers)
        self._disable_read_timeout(ws)
        return ws

    @staticmethod
    def _disable_read_timeout(ws: Any) -> None:
        """Keep the connect timeout from expiring an otherwise healthy idle WebSocket."""
        settimeout = getattr(ws, "settimeout", None)
        if callable(settimeout):
            settimeout(None)

    def _connection_loop(self) -> None:
        delay = self.reconnect_min
        while not self._stop.is_set():
            try:
                with self._session_lock:
                    ws_url = self.ws_url
                    session_generation = self._session_generation
                ws = self._connect(ws_url)
                with self._session_lock:
                    if session_generation != self._session_generation:
                        ws.close()
                        continue
                    self._ws = ws
                self._connected.set()
                self._ready.clear()
                self.last_error = ""
                delay = self.reconnect_min
                self._read_connection(session_generation)
                if not self._stop.is_set():
                    raise RuntimeError("Morrow WebSocket closed")
            except Exception as exc:
                self.metrics["morrow_ws_reconnect_total"] += 1
                self.last_error = str(exc)
                LOGGER.warning("Morrow connection failed: %s", exc)
                try:
                    self.events.put_nowait(MorrowEvent("disconnected", {"message": str(exc)}, {}))
                except queue.Full:
                    pass
            finally:
                self._connected.clear()
                self._ready.clear()
                with self._state_lock:
                    self._morrow_turn_id = ""
                ws, self._ws = self._ws, None
                if ws is not None:
                    try:
                        ws.close()
                    except Exception:
                        pass
            if self._stop.is_set():
                break
            self._reconnect_now.wait(delay)
            self._reconnect_now.clear()
            delay = min(self.reconnect_max, delay * 2)

    def _read_connection(self, session_generation: int) -> None:
        while not self._stop.is_set():
            raw = self._ws.recv()
            if raw in (None, ""):
                return
            with self._session_lock:
                if session_generation != self._session_generation:
                    return
            try:
                message = json.loads(raw)
            except (TypeError, json.JSONDecodeError):
                LOGGER.debug("Ignoring malformed Morrow message")
                continue
            event = parse_message(message)
            self.last_message_at = time.time()
            if event.type == "snapshot":
                with self._state_lock:
                    self._morrow_turn_id = snapshot_running_turn_id(event.data)
                self._ready.set()
            elif event.type == "robot_notice":
                self.metrics["morrow_notice_received_total"] += 1
                notice = event.data if isinstance(event.data, dict) else {}
                self.last_notice_at = self.last_message_at
                try:
                    self.notices.put_nowait(notice)
                except queue.Full:
                    LOGGER.error("Morrow notice queue is full; notice_id=%s", notice.get("id", ""))
            try:
                self.events.put_nowait(event)
            except queue.Full:
                LOGGER.error("Morrow event queue is full; type=%s", event.type)
