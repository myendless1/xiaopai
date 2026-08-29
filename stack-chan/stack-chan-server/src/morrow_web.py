"""Small HTTP-facing gateway used by the Xiaopai Morrow chat page.

The public server only exposes port 8091.  Each web turn therefore uses a
short-lived server-side WebSocket connection to Morrow instead of requiring
the browser to reach Morrow's private port directly.
"""

from __future__ import annotations

import json
import os
import queue
import re
import subprocess
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid
from typing import Any, Callable

from morrow_client import MorrowClient


SESSION_ID_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._:-]{0,127}$")
EXPRESSION_TAG_RE = re.compile(r"<(?:happy|thinking|surprised)>", re.IGNORECASE)
MORROW_WEB_MODES = {
    "nolark": {
        "id": "nolark",
        "label": "通用问答",
        "description": "基础对话，不启用飞书工具",
        "config_name": "config-final-event.toml",
    },
    "lark": {
        "id": "lark",
        "label": "飞书办公助手",
        "description": "启用飞书日程及办公工具",
        "config_name": "config-full.toml",
    },
}


class MorrowWebError(RuntimeError):
    def __init__(self, message: str, status: int = 502) -> None:
        super().__init__(message)
        self.status = int(status)


def validate_session_id(session_id: str) -> str:
    session_id = str(session_id or "").strip()
    if not SESSION_ID_RE.fullmatch(session_id):
        raise MorrowWebError("invalid session id", 400)
    return session_id


def clean_assistant_text(text: str) -> str:
    return EXPRESSION_TAG_RE.sub("", str(text or "")).strip()


def latest_turn_error(payload: dict[str, Any]) -> str:
    session = payload.get("session") if isinstance(payload.get("session"), dict) else payload
    turns = session.get("turns") if isinstance(session, dict) else None
    if not isinstance(turns, list) or not turns:
        return ""
    latest = turns[-1]
    if not isinstance(latest, dict):
        return ""
    turn = latest.get("turn") if isinstance(latest.get("turn"), dict) else latest
    status = str(turn.get("status") or "").lower()
    error = str(turn.get("error") or "").strip()
    return error if error or status == "failed" else ""


class MorrowWebGateway:
    def __init__(
        self,
        *,
        base_url: str,
        default_session: str = "default",
        auth_token: str = "",
        connect_timeout: float = 10,
        turn_timeout: float = 120,
        client_factory: Callable[..., Any] = MorrowClient,
        urlopen: Callable[..., Any] = urllib.request.urlopen,
        start_script: str = "",
        run_command: Callable[..., Any] = subprocess.run,
        device_session_switcher: Callable[[str], Any] | None = None,
        shared_turn_submitter: Callable[[str], Any] | None = None,
        shared_session_resetter: Callable[[], Any] | None = None,
    ) -> None:
        self.base_url = str(base_url or "").strip()
        self.default_session = validate_session_id(default_session or "default")
        self.auth_token = str(auth_token or "")
        self.connect_timeout = max(0.1, float(connect_timeout))
        self.turn_timeout = max(0.1, float(turn_timeout))
        self.client_factory = client_factory
        self.urlopen = urlopen
        self.start_script = os.path.abspath(
            start_script
            or os.path.join(os.path.dirname(__file__), "..", "..", "..", "start_morrow.sh")
        )
        self.run_command = run_command
        self.device_session_switcher = device_session_switcher
        self.shared_turn_submitter = shared_turn_submitter
        self.shared_session_resetter = shared_session_resetter
        self._session_locks: dict[str, threading.Lock] = {}
        self._locks_guard = threading.Lock()
        self._mode_lock = threading.Lock()
        self._switching = threading.Event()
        self._session_revision = 0

    def status(self) -> dict[str, Any]:
        try:
            upstream = self._request_json("GET", "/api/status")
            return {
                "connected": True,
                "default_session": self.default_session,
                "morrow": upstream,
                "active_mode": self._mode_from_status(upstream),
                "modes": self.available_modes(),
                "switching": self._switching.is_set(),
                "session_revision": self._session_revision,
            }
        except MorrowWebError as exc:
            return {
                "connected": False,
                "default_session": self.default_session,
                "error": str(exc),
                "active_mode": "",
                "modes": self.available_modes(),
                "switching": self._switching.is_set(),
                "session_revision": self._session_revision,
            }

    @staticmethod
    def available_modes() -> list[dict[str, str]]:
        return [
            {key: str(value[key]) for key in ("id", "label", "description")}
            for value in MORROW_WEB_MODES.values()
        ]

    def switch_mode(self, mode: str) -> dict[str, Any]:
        mode = str(mode or "").strip().lower()
        selected = MORROW_WEB_MODES.get(mode)
        if selected is None:
            raise MorrowWebError("unsupported Morrow mode", 400)
        if not self._mode_lock.acquire(blocking=False):
            raise MorrowWebError("Morrow mode switch is already in progress", 409)
        self._switching.set()
        try:
            if not os.path.isfile(self.start_script) or not os.access(self.start_script, os.X_OK):
                raise MorrowWebError("Morrow start script is unavailable", 500)
            try:
                result = self.run_command(
                    [self.start_script, mode],
                    cwd=os.path.dirname(self.start_script),
                    capture_output=True,
                    text=True,
                    timeout=30,
                    check=False,
                )
            except subprocess.TimeoutExpired as exc:
                raise MorrowWebError("Morrow mode switch timed out", 504) from exc
            except OSError as exc:
                raise MorrowWebError(f"could not start Morrow: {exc}", 500) from exc
            if int(result.returncode) != 0:
                detail = str(result.stderr or result.stdout or "Morrow failed to start").strip()
                raise MorrowWebError(detail[-2000:], 502)

            upstream = self._request_json("GET", "/api/status")
            if self._mode_from_status(upstream) != mode:
                raise MorrowWebError("Morrow started with an unexpected configuration", 502)
            new_session = self._create_session("shared")
            device_session_id = new_session["session_id"]
            if self.device_session_switcher is not None:
                try:
                    self.device_session_switcher(device_session_id)
                except Exception as exc:
                    raise MorrowWebError(f"could not switch Xiaopai Morrow session: {exc}", 502) from exc
            self.default_session = device_session_id
            self.mark_shared_session_changed()
            return {
                "mode": {key: selected[key] for key in ("id", "label", "description")},
                "session_id": new_session["session_id"],
                "session": new_session["session"],
                "xiaopai_session_id": device_session_id,
                "morrow": upstream,
            }
        finally:
            self._switching.clear()
            self._mode_lock.release()

    def get_session(self, session_id: str) -> dict[str, Any]:
        session_id = self._shared_session_id(session_id)
        return self._request_json("GET", f"/api/sessions/{self._quote(session_id)}")

    def create_session(self) -> dict[str, Any]:
        if self.shared_session_resetter is not None:
            try:
                result = self.shared_session_resetter()
            except Exception as exc:
                raise MorrowWebError(f"could not reset shared Morrow session: {exc}", 502) from exc
            if not bool(getattr(result, "success", result)):
                message = str(getattr(result, "message", "shared session reset failed"))
                raise MorrowWebError(message, 409)
            self.mark_shared_session_changed()
            return {
                "session_id": self.default_session,
                "session": self.get_session(self.default_session),
            }
        return self._create_session("web")

    def mark_shared_session_changed(self) -> int:
        """Advance the browser-visible revision after a global clear or switch."""
        with self._locks_guard:
            self._session_revision += 1
            return self._session_revision

    def _create_session(self, prefix: str) -> dict[str, Any]:
        session_id = f"{prefix}-{time.strftime('%Y%m%d-%H%M%S')}-{uuid.uuid4().hex[:8]}"
        upstream = self._request_json("POST", f"/api/sessions/{self._quote(session_id)}")
        return {"session_id": session_id, "session": upstream}

    def send_message(self, session_id: str, prompt: str) -> dict[str, Any]:
        if self._switching.is_set():
            raise MorrowWebError("Morrow mode switch is in progress", 409)
        session_id = self._shared_session_id(session_id)
        prompt = str(prompt or "").strip()
        if not prompt:
            raise MorrowWebError("message is required", 400)
        if len(prompt) > 20_000:
            raise MorrowWebError("message is too long", 400)

        if self.shared_turn_submitter is not None:
            return self._run_shared_turn(session_id, prompt)

        lock = self._lock_for_session(session_id)
        if not lock.acquire(blocking=False):
            raise MorrowWebError("this session is already processing a message", 409)
        try:
            return self._run_turn(session_id, prompt)
        finally:
            lock.release()

    def _run_shared_turn(self, session_id: str, prompt: str) -> dict[str, Any]:
        try:
            outcome = self.shared_turn_submitter(prompt)
        except Exception as exc:
            raise MorrowWebError(str(exc), 409) from exc
        if not outcome.finished.wait(self.turn_timeout):
            raise MorrowWebError("Morrow response timed out", 504)
        if outcome.state != "saved":
            raise MorrowWebError(outcome.message or f"Morrow turn ended as {outcome.state}", 502)
        return {
            "request_id": outcome.request_id,
            "session_id": session_id,
            "message": clean_assistant_text(outcome.response_text),
        }

    def _shared_session_id(self, requested_session_id: str) -> str:
        requested_session_id = validate_session_id(requested_session_id)
        if self.shared_turn_submitter is not None or self.shared_session_resetter is not None:
            return self.default_session
        return requested_session_id

    def resolved_session_id(self, requested_session_id: str) -> str:
        return self._shared_session_id(requested_session_id)

    def _run_turn(self, session_id: str, prompt: str) -> dict[str, Any]:
        request_id = f"web-{uuid.uuid4()}"
        client = self.client_factory(
            base_url=self.base_url,
            session=session_id,
            auth_token=self.auth_token,
            connect_timeout=self.connect_timeout,
            reconnect_min=0.25,
            reconnect_max=2,
        )
        try:
            if not client.wait_ready(self.connect_timeout):
                detail = getattr(client, "last_error", "") or "snapshot was not received"
                raise MorrowWebError(f"Morrow is not ready: {detail}", 503)

            # The ready snapshot describes history, not this new turn.
            while True:
                try:
                    client.events.get_nowait()
                except queue.Empty:
                    break

            client.start_turn(request_id, prompt)
            deadline = time.monotonic() + self.turn_timeout
            deltas: list[str] = []
            final_message = ""
            while True:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    client.cancel_turn()
                    raise MorrowWebError("Morrow response timed out", 504)
                try:
                    event = client.events.get(timeout=min(0.25, remaining))
                except queue.Empty:
                    continue

                if event.type == "agent_event":
                    event_type = str(event.data.get("type") or "")
                    text = str(event.data.get("data") or "")
                    if event_type == "text_delta":
                        deltas.append(text)
                    elif event_type == "agent_message":
                        final_message = text
                    continue
                if event.type == "turn_saved":
                    answer = clean_assistant_text(final_message or "".join(deltas))
                    if not answer:
                        turn_error = latest_turn_error(self.get_session(session_id))
                        if turn_error:
                            raise MorrowWebError(turn_error, 502)
                    return {
                        "request_id": request_id,
                        "session_id": session_id,
                        "message": answer,
                    }
                if event.type == "turn_rejected":
                    data = event.data if isinstance(event.data, dict) else {}
                    event_request_id = str(data.get("request_id") or "")
                    if event_request_id and event_request_id != request_id:
                        continue
                    raise MorrowWebError(str(data.get("reason") or "turn was rejected"), 409)
                if event.type in {"error", "disconnected"}:
                    data = event.data if isinstance(event.data, dict) else {}
                    raise MorrowWebError(str(data.get("message") or event.type), 502)
        except MorrowWebError:
            raise
        except Exception as exc:
            raise MorrowWebError(str(exc), 502) from exc
        finally:
            client.stop()

    def _lock_for_session(self, session_id: str) -> threading.Lock:
        with self._locks_guard:
            return self._session_locks.setdefault(session_id, threading.Lock())

    def _request_json(self, method: str, path: str) -> dict[str, Any]:
        if not self.base_url:
            raise MorrowWebError("Morrow is not configured", 503)
        request = urllib.request.Request(
            self._http_url(path),
            data=b"" if method != "GET" else None,
            method=method,
            headers={"Authorization": f"Bearer {self.auth_token}"} if self.auth_token else {},
        )
        try:
            with self.urlopen(request, timeout=self.connect_timeout) as response:
                raw = response.read()
        except urllib.error.HTTPError as exc:
            detail = exc.read().decode("utf-8", errors="replace").strip()
            raise MorrowWebError(detail or f"Morrow HTTP {exc.code}", exc.code) from exc
        except Exception as exc:
            raise MorrowWebError(f"cannot reach Morrow: {exc}", 503) from exc
        if not raw:
            return {}
        try:
            payload = json.loads(raw.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError) as exc:
            raise MorrowWebError("Morrow returned invalid JSON", 502) from exc
        return payload if isinstance(payload, dict) else {"data": payload}

    def _http_url(self, path: str) -> str:
        parsed = urllib.parse.urlparse(self.base_url)
        scheme = "https" if parsed.scheme == "wss" else "http" if parsed.scheme == "ws" else parsed.scheme
        base_path = parsed.path.rstrip("/")
        if "/api/sessions/" in base_path:
            base_path = base_path[: base_path.index("/api/sessions/")]
        return urllib.parse.urlunparse((scheme or "http", parsed.netloc, f"{base_path}{path}", "", "", ""))

    @staticmethod
    def _mode_from_status(upstream: dict[str, Any]) -> str:
        config_name = os.path.basename(str(upstream.get("config_path") or ""))
        for mode, details in MORROW_WEB_MODES.items():
            if details["config_name"] == config_name:
                return mode
        return ""

    @staticmethod
    def _quote(value: str) -> str:
        return urllib.parse.quote(value, safe="")
