import json
import re
import time
import urllib.error
import urllib.parse
import urllib.request
from http import HTTPStatus

from xiaopai_openclaw_prompt import XIAOPAI_OPENCLAW_SYSTEM_PROMPT

SYSTEM_PROMPT = XIAOPAI_OPENCLAW_SYSTEM_PROMPT
STREAM_SEGMENT_PUNCTUATION = set("。！？!?；;，,、：:\n")


def safe_openclaw_session_part(value: str) -> str:
    safe = re.sub(r"[^A-Za-z0-9_.:-]+", "_", str(value or "").strip())[:64]
    return safe or "default"


def build_openclaw_session_key(session_prefix: str, device_id: str) -> str:
    prefix = safe_openclaw_session_part(session_prefix or "xiaopai")
    return f"{prefix}-{safe_openclaw_session_part(device_id)}"


def build_morrow_session_id(session_prefix: str, device_id: str) -> str:
    prefix = safe_openclaw_session_part(session_prefix or "default")
    if prefix == "default":
        return "default"
    return build_openclaw_session_key(prefix, device_id)


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


def morrow_http_root_url(base_url: str) -> str:
    parsed = urllib.parse.urlparse(str(base_url or "").strip())
    if not parsed.scheme or not parsed.netloc:
        return ""
    scheme = {"ws": "http", "wss": "https"}.get(parsed.scheme.lower(), parsed.scheme.lower())
    if scheme not in ("http", "https"):
        return ""
    path = parsed.path.rstrip("/")
    if "/api/sessions/" in path:
        path = path[: path.find("/api/sessions/")]
    return urllib.parse.urlunparse((scheme, parsed.netloc, path.rstrip("/"), "", "", ""))


def is_legacy_chat_completions_base_url(base_url: str) -> bool:
    path = urllib.parse.urlparse(str(base_url or "")).path.rstrip("/")
    return path.endswith("/v1") or path.endswith("/chat/completions")


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
        parts = []
        for item in content:
            if isinstance(item, dict) and item.get("type") == "text":
                parts.append(str(item.get("text") or ""))
        return "".join(parts).strip()
    return str(first.get("text") or "").strip()


class OpenClawAgent:
    def __init__(
        self,
        *,
        base_url: str,
        token: str,
        model: str,
        backend_model: str = "",
        timeout: int = 45,
        session_prefix: str = "default",
        max_completion_tokens: int = 512,
        max_segment_chars: int = 120,
    ):
        self.base_url = base_url
        self.token = token
        self.model = model
        self.backend_model = backend_model
        self.timeout = timeout
        self.session_prefix = session_prefix
        self.max_completion_tokens = max_completion_tokens
        self.max_segment_chars = max_segment_chars

    @property
    def enabled(self) -> bool:
        return bool(self.base_url)

    def chat(self, device_id: str, user_text: str) -> str:
        if not self.enabled:
            return ""
        if self._uses_legacy_chat_completions():
            return self._legacy_chat(device_id, user_text)
        return "".join(self.chat_stream(device_id, user_text)).strip()

    def chat_stream(self, device_id: str, user_text: str):
        if not self.enabled:
            return
        if self._uses_legacy_chat_completions():
            segmenter = PunctuationTextSegmenter(max_chars=self.max_segment_chars)
            for segment in segmenter.feed(self._legacy_chat(device_id, user_text)):
                yield segment
            yield from segmenter.flush()
            return
        yield from self._morrow_chat_stream(device_id, user_text)

    def _uses_legacy_chat_completions(self) -> bool:
        return is_legacy_chat_completions_base_url(self.base_url)

    def _legacy_chat(self, device_id: str, user_text: str) -> str:
        session_key = build_openclaw_session_key(self.session_prefix, device_id)
        payload = {
            "model": self.model,
            "messages": [
                {"role": "system", "content": SYSTEM_PROMPT},
                {"role": "user", "content": user_text},
            ],
            "user": session_key,
            "max_completion_tokens": self.max_completion_tokens,
        }
        headers = {
            "Content-Type": "application/json",
            "x-openclaw-session-key": session_key,
        }
        if self.token:
            headers["Authorization"] = f"Bearer {self.token}"
        if self.backend_model:
            headers["x-openclaw-model"] = self.backend_model
        url = self.base_url.rstrip("/")
        if not url.endswith("/chat/completions"):
            url += "/chat/completions"
        request = urllib.request.Request(
            url,
            data=json.dumps(payload, ensure_ascii=False).encode("utf-8"),
            method="POST",
            headers=headers,
        )
        try:
            with urllib.request.urlopen(request, timeout=self.timeout) as resp:
                status = getattr(resp, "status", HTTPStatus.OK)
                response_text = resp.read().decode("utf-8", errors="replace")
        except urllib.error.HTTPError as exc:
            detail = exc.read().decode("utf-8", errors="replace")
            raise RuntimeError(f"OpenClaw HTTP {exc.code}: {detail}") from exc
        if int(status) >= 400:
            raise RuntimeError(f"OpenClaw HTTP {status}: {response_text}")
        return extract_openclaw_text(response_text)

    def _morrow_chat_stream(self, device_id: str, user_text: str):
        session_id = build_morrow_session_id(self.session_prefix, device_id)
        self._ensure_morrow_session(session_id)
        ws_url = build_morrow_ws_url(self.base_url, session_id)
        if not ws_url:
            return

        try:
            import websocket  # type: ignore
        except Exception as exc:
            raise RuntimeError("websocket-client is required for Morrow streaming") from exc

        headers = []
        if self.token:
            headers.append(f"Authorization: Bearer {self.token}")

        request_id = f"xiaopai-{int(time.time() * 1000)}"
        ws = websocket.create_connection(ws_url, timeout=self.timeout, header=headers)
        segmenter = PunctuationTextSegmenter(max_chars=self.max_segment_chars)
        saw_delta = False
        final_message = ""
        try:
            ws.send(json.dumps(
                {
                    "type": "start_turn",
                    "data": {
                        "request_id": request_id,
                        "prompt": str(user_text or ""),
                    },
                },
                ensure_ascii=False,
            ))
            while True:
                raw = ws.recv()
                if raw in (None, ""):
                    break
                try:
                    message = json.loads(raw)
                except (TypeError, json.JSONDecodeError):
                    continue
                message_type = str(message.get("type") or "")

                if message_type == "agent_event":
                    event = message.get("data", {}).get("event") if isinstance(message.get("data"), dict) else {}
                    if not isinstance(event, dict):
                        continue
                    event_type = str(event.get("type") or "")
                    data = event.get("data")
                    if event_type == "text_delta":
                        saw_delta = True
                        for segment in segmenter.feed(str(data or "")):
                            yield segment
                    elif event_type == "agent_message":
                        final_message = str(data or "")
                    continue

                if message_type == "robot_notice":
                    data = message.get("data") if isinstance(message.get("data"), dict) else {}
                    notice_text = str(data.get("text") or "").strip()
                    if notice_text:
                        notice_segmenter = PunctuationTextSegmenter(max_chars=self.max_segment_chars)
                        for segment in notice_segmenter.feed(notice_text):
                            yield segment
                        yield from notice_segmenter.flush()
                    continue

                if message_type == "turn_rejected":
                    data = message.get("data") if isinstance(message.get("data"), dict) else {}
                    if str(data.get("request_id") or "") in ("", request_id):
                        raise RuntimeError(f"Morrow turn rejected: {data.get('reason') or 'unknown reason'}")
                    continue

                if message_type == "error":
                    data = message.get("data") if isinstance(message.get("data"), dict) else {}
                    raise RuntimeError(f"Morrow error: {data.get('message') or raw}")

                if message_type == "turn_saved":
                    break

            if saw_delta:
                yield from segmenter.flush()
            elif final_message:
                for segment in segmenter.feed(final_message):
                    yield segment
                yield from segmenter.flush()
        finally:
            try:
                ws.close()
            except Exception:
                pass

    def _ensure_morrow_session(self, session_id: str) -> None:
        root_url = morrow_http_root_url(self.base_url)
        if not root_url:
            return
        url = f"{root_url}/api/sessions/{urllib.parse.quote(session_id, safe='-_.:')}"
        headers = {}
        if self.token:
            headers["Authorization"] = f"Bearer {self.token}"
        request = urllib.request.Request(url, data=b"", method="POST", headers=headers)
        try:
            with urllib.request.urlopen(request, timeout=min(self.timeout, 10)):
                return
        except urllib.error.HTTPError as exc:
            if exc.code in (HTTPStatus.CONFLICT, HTTPStatus.NOT_FOUND, HTTPStatus.METHOD_NOT_ALLOWED):
                return
            detail = exc.read().decode("utf-8", errors="replace")
            raise RuntimeError(f"Morrow session create HTTP {exc.code}: {detail}") from exc
