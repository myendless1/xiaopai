from __future__ import annotations

import datetime as dt
import uuid
from dataclasses import dataclass, field
from typing import Any


PROTOCOL_VERSION = 3
DEFAULT_LEASE_MS = 15_000
DEFAULT_HEARTBEAT_MS = 5_000
TERMINAL_COMMAND_STATES = {"rendered", "done", "failed", "cancelled", "expired"}


def utc_now() -> str:
    return dt.datetime.now(dt.timezone.utc).isoformat()


def parse_time(value: str | None) -> dt.datetime | None:
    if not value:
        return None
    text = str(value).replace("Z", "+00:00")
    parsed = dt.datetime.fromisoformat(text)
    return parsed if parsed.tzinfo else parsed.replace(tzinfo=dt.timezone.utc)


def future_time_ms(ms: int) -> str:
    return (dt.datetime.now(dt.timezone.utc) + dt.timedelta(milliseconds=max(0, int(ms)))).isoformat()


def new_id(prefix: str) -> str:
    return f"{prefix}_{uuid.uuid4().hex}"


@dataclass
class AdmissionPolicy:
    allow_in_quiet: bool = False
    defer_during_recording: bool = True
    defer_during_speaking: bool = True
    presence_requirement: str = "none"

    @classmethod
    def from_raw(cls, raw: dict[str, Any] | None, *, presence_requirement: str = "none") -> "AdmissionPolicy":
        raw = raw or {}
        return cls(
            allow_in_quiet=bool(raw.get("allow_in_quiet", False)),
            defer_during_recording=bool(raw.get("defer_during_recording", True)),
            defer_during_speaking=bool(raw.get("defer_during_speaking", True)),
            presence_requirement=str(raw.get("presence_requirement") or presence_requirement or "none"),
        )

    def to_dict(self) -> dict[str, Any]:
        return {
            "allow_in_quiet": self.allow_in_quiet,
            "defer_during_recording": self.defer_during_recording,
            "defer_during_speaking": self.defer_during_speaking,
            "presence_requirement": self.presence_requirement,
        }


@dataclass
class CommandEnvelope:
    cmd_id: str
    device_id: str
    type: str
    payload: Any
    priority: int = 50
    ttl_ms: int = 30_000
    attempt: int = 1
    coalesce_key: str = ""
    safety_class: str = "normal"
    turn_id: str = ""
    admission: AdmissionPolicy = field(default_factory=AdmissionPolicy)
    delivery_id: str = ""
    created_at: str = field(default_factory=utc_now)
    expires_at: str = ""
    source_type: str = ""
    source_id: str = ""
    segment_index: int = 0
    turn_generation: int = 0
    payload_retention_until: str = ""

    @classmethod
    def from_legacy(cls, device_id: str, command: dict[str, Any], *, delivery_id: str = "") -> "CommandEnvelope":
        command_type = str(command.get("type") or "speak")
        ttl_ms = int(float(command.get("ttl_ms") or float(command.get("ttl_seconds") or 30) * 1000))
        safety_class = "local_stop" if command_type == "stop" or command.get("interrupt") else "normal"
        presence_requirement = "preferred" if command_type in ("find_owner", "locate_owner") else "none"
        return cls(
            cmd_id=str(command.get("cmd_id") or new_id("cmd")),
            device_id=str(device_id or command.get("device_id") or "default"),
            type=command_type,
            payload=command.get("payload", {}),
            priority=int(command.get("priority") or 50),
            ttl_ms=ttl_ms,
            attempt=int(command.get("attempt") or 1),
            coalesce_key=str(command.get("coalesce_key") or ""),
            safety_class=safety_class,
            turn_id=str(command.get("turn_id") or ""),
            admission=AdmissionPolicy.from_raw(command.get("admission"), presence_requirement=presence_requirement),
            delivery_id=delivery_id,
            created_at=utc_now(),
            expires_at=future_time_ms(ttl_ms),
            source_type=str(command.get("source_type") or ""),
            source_id=str(command.get("source_id") or ""),
            segment_index=int(command.get("segment_index") or 0),
            turn_generation=int(command.get("turn_generation") or 0),
            payload_retention_until=str(command.get("payload_retention_until") or ""),
        )

    def to_device_command(self) -> dict[str, Any]:
        return {
            "cmd_id": self.cmd_id,
            "type": self.type,
            "priority": self.priority,
            "ttl_ms": self.ttl_ms,
            "attempt": self.attempt,
            "coalesce_key": self.coalesce_key,
            "safety_class": self.safety_class,
            "turn_id": self.turn_id,
            "admission": self.admission.to_dict(),
            "payload": self.payload,
        }

    def to_store_dict(self) -> dict[str, Any]:
        body = self.to_device_command()
        body.update(
            {
                "device_id": self.device_id,
                "delivery_id": self.delivery_id,
                "created_at": self.created_at,
                "expires_at": self.expires_at,
                "source_type": self.source_type,
                "source_id": self.source_id,
                "segment_index": self.segment_index,
                "turn_generation": self.turn_generation,
                "payload_retention_until": self.payload_retention_until,
            }
        )
        return body


def normalize_ack_state(value: str) -> str:
    state = str(value or "received").strip().lower()
    aliases = {
        "success": "done",
        "ok": "done",
        "sent_realtime": "leased",
        "played": "rendered",
        "complete": "done",
        "completed": "done",
        "dropped": "expired",
        "timeout": "expired",
        "error": "failed",
    }
    return aliases.get(state, state)
