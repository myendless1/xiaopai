from __future__ import annotations

import json
from typing import Any

from database import Database
from schemas import DEFAULT_HEARTBEAT_MS, DEFAULT_LEASE_MS, PROTOCOL_VERSION, new_id, utc_now


class DeviceRegistry:
    def __init__(self, database: Database) -> None:
        self.database = database

    def hello(self, payload: dict[str, Any]) -> dict[str, Any]:
        device_id = str(payload.get("device_id") or "default")
        capabilities = payload.get("capabilities") if isinstance(payload.get("capabilities"), list) else []
        now = utc_now()
        session_id = new_id("sess")
        with self.database.connect() as conn:
            conn.execute(
                """
                INSERT INTO devices(device_id, capabilities_json, last_seen_at, last_heartbeat_at, last_ack_seq, online)
                VALUES (?, ?, ?, ?, ?, 1)
                ON CONFLICT(device_id) DO UPDATE SET
                  capabilities_json=excluded.capabilities_json,
                  last_seen_at=excluded.last_seen_at,
                  last_heartbeat_at=excluded.last_heartbeat_at,
                  last_ack_seq=max(devices.last_ack_seq, excluded.last_ack_seq),
                  online=1
                """,
                (
                    device_id,
                    json.dumps(capabilities, ensure_ascii=False),
                    now,
                    now,
                    int(payload.get("last_ack_seq") or 0),
                ),
            )
            conn.execute(
                """
                INSERT INTO device_sessions(
                  session_id, device_id, boot_id, firmware_version, protocol_version,
                  capabilities_json, connected_at, last_seen_at
                ) VALUES (?, ?, ?, ?, ?, ?, ?, ?)
                """,
                (
                    session_id,
                    device_id,
                    int(payload.get("boot_id") or 0),
                    str(payload.get("firmware_version") or ""),
                    int(payload.get("protocol_version") or 0),
                    json.dumps(capabilities, ensure_ascii=False),
                    now,
                    now,
                ),
            )
        return {
            "type": "hello_ack",
            "session_id": session_id,
            "protocol_version": PROTOCOL_VERSION,
            "server_time": now,
            "heartbeat_interval_ms": DEFAULT_HEARTBEAT_MS,
            "lease_ms": DEFAULT_LEASE_MS,
            "device_config_version": 1,
        }

    def heartbeat(self, device_id: str, *, last_ack_seq: int = 0) -> dict[str, Any]:
        now = utc_now()
        with self.database.connect() as conn:
            conn.execute(
                """
                INSERT INTO devices(device_id, last_seen_at, last_heartbeat_at, last_ack_seq, online)
                VALUES (?, ?, ?, ?, 1)
                ON CONFLICT(device_id) DO UPDATE SET
                  last_seen_at=excluded.last_seen_at,
                  last_heartbeat_at=excluded.last_heartbeat_at,
                  last_ack_seq=max(devices.last_ack_seq, excluded.last_ack_seq),
                  online=1
                """,
                (device_id, now, now, int(last_ack_seq or 0)),
            )
        return {"type": "heartbeat_ack", "device_id": device_id, "server_time": now}

    def list_devices(self) -> list[dict[str, Any]]:
        with self.database.connect() as conn:
            rows = conn.execute("SELECT * FROM devices ORDER BY last_seen_at DESC").fetchall()
        devices = []
        for row in rows:
            item = dict(row)
            item["online"] = bool(item["online"])
            item["capabilities"] = json.loads(item.pop("capabilities_json") or "[]")
            devices.append(item)
        return devices
