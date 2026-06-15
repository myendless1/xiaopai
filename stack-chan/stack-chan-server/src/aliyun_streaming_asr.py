import json
import time
import uuid


ASR_WS_URLS = {
    "shanghai": "wss://nls-gateway-cn-shanghai.aliyuncs.com/ws/v1",
    "beijing": "wss://nls-gateway-cn-beijing.aliyuncs.com/ws/v1",
    "shenzhen": "wss://nls-gateway-cn-shenzhen.aliyuncs.com/ws/v1",
}


def build_asr_ws_url(region: str, token: str, override_url: str = "") -> str:
    base = (override_url or ASR_WS_URLS.get(region, ASR_WS_URLS["shanghai"])).rstrip("?")
    separator = "&" if "?" in base else "?"
    if "token=" in base:
        return base
    return f"{base}{separator}token={token}"


def build_start_transcription(appkey: str, *, sample_rate: int = 16000, task_id: str = "") -> dict:
    return {
        "header": {
            "appkey": appkey,
            "namespace": "SpeechTranscriber",
            "name": "StartTranscription",
            "task_id": task_id or uuid.uuid4().hex,
            "message_id": uuid.uuid4().hex,
        },
        "payload": {
            "format": "pcm",
            "sample_rate": int(sample_rate),
            "enable_intermediate_result": True,
            "enable_punctuation_prediction": True,
            "enable_inverse_text_normalization": True,
            "max_sentence_silence": 400,
        },
    }


def build_stop_transcription(appkey: str, task_id: str) -> dict:
    return {
        "header": {
            "appkey": appkey,
            "namespace": "SpeechTranscriber",
            "name": "StopTranscription",
            "task_id": task_id,
            "message_id": uuid.uuid4().hex,
        }
    }


def parse_asr_event(raw: str | bytes) -> dict:
    if isinstance(raw, bytes):
        raw = raw.decode("utf-8", errors="replace")
    data = json.loads(raw)
    header = data.get("header") if isinstance(data, dict) else {}
    payload = data.get("payload") if isinstance(data, dict) else {}
    header = header if isinstance(header, dict) else {}
    payload = payload if isinstance(payload, dict) else {}
    top = data if isinstance(data, dict) else {}
    name = str(header.get("name") or top.get("name") or "")
    status = header.get("status") if "status" in header else top.get("status")
    text = (
        payload.get("result")
        or payload.get("text")
        or payload.get("sentence")
        or top.get("result")
        or top.get("text")
        or ""
    )
    is_final = name in ("SentenceEnd", "TranscriptionCompleted")
    is_partial = name == "TranscriptionResultChanged"
    return {
        "name": name,
        "status": status,
        "text": text,
        "is_final": is_final,
        "is_partial": is_partial,
        "task_id": header.get("task_id") or top.get("task_id") or "",
        "raw": data,
    }


def wait_for_transcription_started(ws, *, timeout_s: float = 5.0) -> dict:
    deadline = time.monotonic() + float(timeout_s)
    original_timeout = None
    try:
        original_timeout = ws.gettimeout()
    except Exception:
        pass
    try:
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise RuntimeError("Timed out waiting for Aliyun ASR TranscriptionStarted")
            try:
                ws.settimeout(max(0.05, remaining))
            except Exception:
                pass
            try:
                raw = ws.recv()
            except Exception as exc:
                if exc.__class__.__name__ in ("WebSocketTimeoutException", "TimeoutError"):
                    raise RuntimeError("Timed out waiting for Aliyun ASR TranscriptionStarted") from exc
                raise
            if isinstance(raw, bytes):
                continue
            event = parse_asr_event(raw)
            if event["name"] == "TaskFailed":
                raise RuntimeError(f"Aliyun ASR failed: {event['raw']}")
            if event["name"] == "TranscriptionStarted":
                return event
    finally:
        if original_timeout is not None:
            try:
                ws.settimeout(original_timeout)
            except Exception:
                pass


class AliyunStreamingAsrSession:
    def __init__(self, *, appkey: str, token_getter, region: str = "shanghai", ws_url: str = "", sample_rate: int = 16000):
        self.appkey = appkey
        self.token_getter = token_getter
        self.region = region
        self.ws_url = ws_url
        self.sample_rate = sample_rate

    def connect(self):
        try:
            import websocket  # type: ignore
        except Exception as exc:
            raise RuntimeError("websocket-client is required for Aliyun streaming ASR") from exc
        token = self.token_getter()
        ws = websocket.create_connection(build_asr_ws_url(self.region, token, self.ws_url), timeout=10)
        start = build_start_transcription(self.appkey, sample_rate=self.sample_rate)
        try:
            ws.send(json.dumps(start, ensure_ascii=False))
            wait_for_transcription_started(ws)
            return ws, start["header"]["task_id"]
        except Exception:
            try:
                ws.close()
            except Exception:
                pass
            raise
