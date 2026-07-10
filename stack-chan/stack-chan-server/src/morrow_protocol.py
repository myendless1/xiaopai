"""Strict helpers for Morrow's public session WebSocket protocol."""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any, Literal


InboundType = Literal[
    "snapshot",
    "agent_event",
    "robot_notice",
    "turn_saved",
    "turn_rejected",
    "error",
    "disconnected",
    "unknown",
]


def build_start_turn(request_id: str, prompt: str) -> dict[str, Any]:
    request_id = str(request_id or "").strip()
    prompt = str(prompt or "").strip()
    if not request_id:
        raise ValueError("request_id is required")
    if not prompt:
        raise ValueError("prompt is required")
    return {
        "type": "start_turn",
        "data": {"request_id": request_id, "prompt": prompt},
    }


def build_cancel_turn(turn_id: str) -> dict[str, Any]:
    turn_id = str(turn_id or "").strip()
    if not turn_id:
        raise ValueError("turn_id is required")
    return {"type": "cancel_turn", "data": {"turn_id": turn_id}}


def build_reset_session(request_id: str) -> dict[str, Any]:
    request_id = str(request_id or "").strip()
    if not request_id:
        raise ValueError("request_id is required")
    return {"type": "reset_session", "data": {"request_id": request_id}}


@dataclass(frozen=True)
class MorrowEvent:
    type: InboundType
    data: Any
    raw: dict[str, Any]


def parse_message(message: Any) -> MorrowEvent:
    if not isinstance(message, dict):
        return MorrowEvent("unknown", None, {})

    message_type = str(message.get("type") or "")
    if message_type == "agent_event":
        outer = message.get("data")
        event = outer.get("event") if isinstance(outer, dict) else None
        if not isinstance(event, dict):
            return MorrowEvent("unknown", None, message)
        event_type = str(event.get("type") or "")
        if event_type not in ("text_delta", "agent_message"):
            return MorrowEvent("unknown", event, message)
        return MorrowEvent("agent_event", event, message)

    if message_type in {
        "snapshot",
        "robot_notice",
        "turn_saved",
        "turn_rejected",
        "error",
    }:
        data = message.get("data")
        return MorrowEvent(message_type, data if isinstance(data, dict) else {}, message)  # type: ignore[arg-type]

    return MorrowEvent("unknown", message.get("data"), message)


def snapshot_running_turn_id(data: Any) -> str:
    if not isinstance(data, dict):
        return ""
    running = data.get("running_turn")
    if not isinstance(running, dict):
        return ""
    return str(running.get("turn_id") or running.get("id") or "").strip()
