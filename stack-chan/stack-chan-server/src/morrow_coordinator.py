"""Serializes all Xiaopai requests onto Morrow's single-session turn model."""

from __future__ import annotations

import queue
import threading
import time
import uuid
from dataclasses import dataclass, field
from typing import Callable

from morrow_protocol import MorrowEvent


HARD_PUNCTUATION = frozenset("。！？!?；;\n")
SUPPORTED_EXPRESSIONS = frozenset({"happy", "thinking", "surprised"})


class StreamingTextSegmenter:
    def __init__(self, *, max_chars: int = 120, max_bytes: int = 480) -> None:
        self.max_chars = max(1, int(max_chars))
        self.max_bytes = max(4, int(max_bytes))
        self._buffer = ""

    def feed(self, text: str) -> list[str]:
        segments: list[str] = []
        for char in str(text or ""):
            candidate = self._buffer + char
            if self._buffer and (
                len(candidate) > self.max_chars
                or len(candidate.encode("utf-8")) > self.max_bytes
            ):
                self._emit(segments)
            self._buffer += char
            if char in HARD_PUNCTUATION:
                self._emit(segments)
        return segments

    def flush(self) -> list[str]:
        segments: list[str] = []
        self._emit(segments)
        return segments

    def _emit(self, output: list[str]) -> None:
        segment = self._buffer.strip()
        self._buffer = ""
        if segment:
            output.append(segment)


class StreamingExpressionTagParser:
    """Remove angle-bracket metadata while selecting one leading expression."""

    def __init__(self) -> None:
        self.expression = "calm"
        self._leading = True
        self._selected = False
        self._tag_buffer = ""

    def feed(self, text: str) -> str:
        output: list[str] = []
        for char in str(text or ""):
            if self._tag_buffer:
                if char == ">":
                    self._finish_tag()
                elif char == "<":
                    self._tag_buffer = "<"
                else:
                    self._tag_buffer += char
                continue

            if char == "<":
                self._tag_buffer = "<"
                continue

            output.append(char)
            if not char.isspace():
                self._leading = False
        return "".join(output)

    def flush(self) -> str:
        # An unterminated tag is metadata, never speech.
        self._tag_buffer = ""
        return ""

    def _finish_tag(self) -> None:
        tag = self._tag_buffer[1:].strip().lower()
        self._tag_buffer = ""
        if self._leading and not self._selected and tag in SUPPORTED_EXPRESSIONS:
            self.expression = tag
            self._selected = True


def parse_expression_tags(text: str) -> tuple[str, str]:
    """Parse a complete notice using the same rules as streamed replies."""
    parser = StreamingExpressionTagParser()
    cleaned = parser.feed(text)
    cleaned += parser.flush()
    return cleaned, parser.expression


@dataclass
class MorrowRequest:
    request_id: str
    prompt: str
    device_id: str
    source: str
    created_at: float
    expires_at: float
    generation: int
    expression: str = "calm"


@dataclass
class TurnOutcome:
    request_id: str
    state: str
    message: str = ""
    segment_count: int = 0
    finished: threading.Event = field(default_factory=threading.Event, repr=False)


SegmentSink = Callable[[MorrowRequest, str, int], None]
ReplyEndSink = Callable[[MorrowRequest, int, str], None]
CancelSink = Callable[[str, int], None]


def command_store_segment_sink(
    command_store,
    *,
    ttl_ms: int = 30_000,
    enqueue=None,
    speaker_volume=10,
) -> SegmentSink:
    """Build a sink that persists every dialogue segment before device delivery."""
    from schemas import AdmissionPolicy, CommandEnvelope, future_time_ms, new_id

    def persist(request: MorrowRequest, text: str, segment_index: int) -> None:
        resolved_speaker_volume = speaker_volume() if callable(speaker_volume) else speaker_volume
        command = CommandEnvelope(
                cmd_id=new_id("cmd"),
                device_id=request.device_id,
                type="speak",
                payload={
                    "text": text,
                    "expression": request.expression,
                    "turn_id": request.request_id,
                    "segment_index": segment_index,
                    "generation": request.generation,
                    "reply_end": False,
                    "speaker_volume": int(resolved_speaker_volume),
                },
                priority=50,
                ttl_ms=ttl_ms,
                turn_id=request.request_id,
                admission=AdmissionPolicy(),
                expires_at=future_time_ms(ttl_ms),
                source_type="dialogue",
                source_id=request.request_id,
                segment_index=segment_index,
                turn_generation=request.generation,
            )
        command_store.create_command(command)
        if enqueue is not None:
            enqueue(request.device_id, command.to_device_command())

    return persist


def command_store_reply_end_sink(command_store, *, ttl_ms: int = 30_000, enqueue=None) -> ReplyEndSink:
    """Build a FIFO sentinel marking the end (or cancellation) of a reply."""
    from schemas import AdmissionPolicy, CommandEnvelope, future_time_ms, new_id

    def persist(request: MorrowRequest, segment_index: int, outcome: str) -> None:
        command = CommandEnvelope(
            cmd_id=new_id("cmd"),
            device_id=request.device_id,
            type="speak",
            payload={
                "text": "",
                "expression": request.expression,
                "turn_id": request.request_id,
                "segment_index": segment_index,
                "generation": request.generation,
                "reply_end": True,
                "reply_cancelled": outcome != "saved",
            },
            priority=50,
            ttl_ms=ttl_ms,
            turn_id=request.request_id,
            admission=AdmissionPolicy(),
            expires_at=future_time_ms(ttl_ms),
            source_type="dialogue",
            source_id=request.request_id,
            segment_index=segment_index,
            turn_generation=request.generation,
        )
        command_store.create_command(command)
        if enqueue is not None:
            enqueue(request.device_id, command.to_device_command())

    return persist


class MorrowTurnCoordinator:
    def __init__(
        self,
        client,
        segment_sink: SegmentSink,
        *,
        reply_end_sink: ReplyEndSink | None = None,
        cancel_sink: CancelSink | None = None,
        queue_size: int = 8,
        request_ttl: float = 60,
        turn_timeout: float = 120,
        max_segment_chars: int = 120,
        max_segment_bytes: int = 480,
        initial_generations: dict[str, int] | None = None,
        clock: Callable[[], float] = time.monotonic,
    ) -> None:
        self.client = client
        self.segment_sink = segment_sink
        self.reply_end_sink = reply_end_sink
        self.cancel_sink = cancel_sink
        self.request_ttl = max(0.01, float(request_ttl))
        self.turn_timeout = max(0.01, float(turn_timeout))
        self.max_segment_chars = max_segment_chars
        self.max_segment_bytes = max_segment_bytes
        self.clock = clock
        self._queue: queue.Queue[MorrowRequest | None] = queue.Queue(maxsize=max(1, queue_size))
        self._stop = threading.Event()
        self._thread: threading.Thread | None = None
        self._lock = threading.RLock()
        self._active: MorrowRequest | None = None
        self._generations = {
            str(device_id): max(0, int(generation))
            for device_id, generation in (initial_generations or {}).items()
        }
        self._outcomes: dict[str, TurnOutcome] = {}
        self.metrics = {
            "morrow_turn_submitted_total": 0,
            "morrow_turn_rejected_total": 0,
            "morrow_turn_timeout_total": 0,
            "morrow_reply_end_error_total": 0,
            "morrow_first_delta_latency_ms": 0.0,
            "morrow_first_speech_command_latency_ms": 0.0,
        }

    @property
    def active_request_id(self) -> str:
        with self._lock:
            return self._active.request_id if self._active else ""

    @property
    def queued_turns(self) -> int:
        return self._queue.qsize()

    def generation_for_device(self, device_id: str) -> int:
        """Return the generation that new speech for this device must use."""
        device_id = str(device_id or "default")
        with self._lock:
            return self._generations.get(device_id, 0)

    def start(self) -> None:
        self.client.start()
        if self._thread is not None and self._thread.is_alive():
            return
        self._stop.clear()
        self._thread = threading.Thread(target=self._run, name="morrow-coordinator", daemon=True)
        self._thread.start()

    def stop(self) -> None:
        self._stop.set()
        try:
            self._queue.put_nowait(None)
        except queue.Full:
            pass
        if self._thread is not None and self._thread is not threading.current_thread():
            self._thread.join(timeout=2)

    def submit(
        self,
        prompt: str,
        device_id: str,
        *,
        source: str = "voice",
        ttl: float | None = None,
        request_id: str = "",
    ) -> TurnOutcome:
        prompt = str(prompt or "").strip()
        if not prompt:
            raise ValueError("prompt is required")
        if source not in {"voice", "touch", "system", "admin"}:
            raise ValueError(f"unsupported Morrow request source: {source}")
        now = self.clock()
        device_id = str(device_id or "default")
        with self._lock:
            generation = self._generations.get(device_id, 0)
        request = MorrowRequest(
            request_id=request_id or str(uuid.uuid4()),
            prompt=prompt,
            device_id=device_id,
            source=source,
            created_at=now,
            expires_at=now + (self.request_ttl if ttl is None else max(0.01, float(ttl))),
            generation=generation,
        )
        outcome = TurnOutcome(request.request_id, "queued")
        with self._lock:
            self._outcomes[request.request_id] = outcome
        try:
            self._queue.put_nowait(request)
        except queue.Full:
            with self._lock:
                self._outcomes.pop(request.request_id, None)
            raise
        return outcome

    def cancel_device(self, device_id: str) -> int:
        device_id = str(device_id or "default")
        with self._lock:
            generation = self._generations.get(device_id, 0) + 1
            self._generations[device_id] = generation
            active = self._active
        if self.cancel_sink is not None:
            self.cancel_sink(device_id, generation)
        if active is not None and active.device_id == device_id:
            self.client.cancel_turn()
        return generation

    def outcome(self, request_id: str) -> TurnOutcome | None:
        with self._lock:
            return self._outcomes.get(request_id)

    def _run(self) -> None:
        while not self._stop.is_set():
            try:
                request = self._queue.get(timeout=0.2)
            except queue.Empty:
                continue
            try:
                if request is None:
                    return
                self._process(request)
            finally:
                self._queue.task_done()

    def _process(self, request: MorrowRequest) -> None:
        outcome = self._outcomes[request.request_id]
        if self._is_stale(request):
            self._finish(outcome, "expired")
            return
        remaining = max(0.0, request.expires_at - self.clock())
        if not self.client.wait_ready(remaining):
            self._finish(outcome, "expired", "Morrow was not ready before request TTL")
            return
        if self._is_stale(request):
            self._finish(outcome, "cancelled")
            return

        with self._lock:
            self._active = request
        outcome.state = "submitted"
        segmenter = StreamingTextSegmenter(
            max_chars=self.max_segment_chars,
            max_bytes=self.max_segment_bytes,
        )
        tag_parser = StreamingExpressionTagParser()
        saw_delta = False
        final_message = ""
        deadline = self.clock() + self.turn_timeout
        try:
            self._discard_stale_connection_events()
            self.client.start_turn(request.request_id, request.prompt)
            self.metrics["morrow_turn_submitted_total"] += 1
            submitted_at = self.clock()
            while not self._stop.is_set():
                timeout = deadline - self.clock()
                if timeout <= 0:
                    self.metrics["morrow_turn_timeout_total"] += 1
                    self._finish(outcome, "timeout")
                    return
                try:
                    event = self.client.events.get(timeout=min(0.2, timeout))
                except queue.Empty:
                    continue
                if event.type == "agent_event":
                    event_type = str(event.data.get("type") or "")
                    text = str(event.data.get("data") or "")
                    if event_type == "text_delta":
                        if not saw_delta:
                            self.metrics["morrow_first_delta_latency_ms"] = (self.clock() - submitted_at) * 1000
                        saw_delta = True
                        outcome.state = "streaming"
                        cleaned = tag_parser.feed(text)
                        request.expression = tag_parser.expression
                        self._deliver(request, outcome, segmenter.feed(cleaned))
                    elif event_type == "agent_message":
                        final_message = text
                    continue
                if event.type == "turn_saved":
                    if not saw_delta and final_message:
                        cleaned = tag_parser.feed(final_message)
                        request.expression = tag_parser.expression
                        self._deliver(request, outcome, segmenter.feed(cleaned))
                    segmenter.feed(tag_parser.flush())
                    self._deliver(request, outcome, segmenter.flush())
                    self._finish(outcome, "saved")
                    return
                if event.type == "turn_rejected":
                    self.metrics["morrow_turn_rejected_total"] += 1
                    data = event.data if isinstance(event.data, dict) else {}
                    event_request_id = str(data.get("request_id") or "")
                    if event_request_id and event_request_id != request.request_id:
                        continue
                    self._finish(outcome, "rejected", str(data.get("reason") or ""))
                    return
                if event.type == "error":
                    data = event.data if isinstance(event.data, dict) else {}
                    self._finish(outcome, "error", str(data.get("message") or ""))
                    return
                if event.type == "disconnected":
                    data = event.data if isinstance(event.data, dict) else {}
                    self._finish(outcome, "disconnected", str(data.get("message") or ""))
                    return
        except Exception as exc:
            self._finish(outcome, "disconnected", str(exc))
        finally:
            if outcome.finished.is_set() and outcome.segment_count and self.reply_end_sink is not None:
                try:
                    self.reply_end_sink(request, outcome.segment_count, outcome.state)
                except Exception as exc:
                    self.metrics["morrow_reply_end_error_total"] += 1
                    if not outcome.message:
                        outcome.message = f"reply-end enqueue failed: {exc}"
            with self._lock:
                if self._active is request:
                    self._active = None

    def _deliver(self, request: MorrowRequest, outcome: TurnOutcome, segments: list[str]) -> None:
        if self._is_stale(request):
            return
        for segment in segments:
            if outcome.segment_count == 0:
                self.metrics["morrow_first_speech_command_latency_ms"] = (self.clock() - request.created_at) * 1000
            self.segment_sink(request, segment, outcome.segment_count)
            outcome.segment_count += 1

    def _is_stale(self, request: MorrowRequest) -> bool:
        with self._lock:
            generation = self._generations.get(request.device_id, 0)
        return request.generation != generation or self.clock() >= request.expires_at

    def _discard_stale_connection_events(self) -> None:
        retained: list[MorrowEvent] = []
        while True:
            try:
                event = self.client.events.get_nowait()
            except queue.Empty:
                break
            if event.type not in {"snapshot", "robot_notice", "disconnected"}:
                retained.append(event)
        for event in retained:
            self.client.events.put_nowait(event)

    @staticmethod
    def _finish(outcome: TurnOutcome, state: str, message: str = "") -> None:
        outcome.state = state
        outcome.message = message
        outcome.finished.set()
