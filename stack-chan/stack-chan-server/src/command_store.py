from __future__ import annotations

import json
from typing import Any

from database import Database
from schemas import CommandEnvelope, DEFAULT_LEASE_MS, TERMINAL_COMMAND_STATES, future_time_ms, normalize_ack_state, utc_now


RUNNABLE_STATES = ("queued", "leased")


class CommandStore:
    def __init__(self, database: Database) -> None:
        self.database = database

    def create_command(self, command: CommandEnvelope, *, max_attempts: int = 3) -> dict[str, Any]:
        row = command.to_store_dict()
        now = utc_now()
        with self.database.connect() as conn:
            conn.execute(
                """
                INSERT OR IGNORE INTO commands (
                  cmd_id, delivery_id, device_id, type, priority, ttl_ms, attempt, max_attempts,
                  state, coalesce_key, safety_class, turn_id, admission_json, payload_json,
                  created_at, updated_at, expires_at
                ) VALUES (?, ?, ?, ?, ?, ?, 0, ?, 'queued', ?, ?, ?, ?, ?, ?, ?, ?)
                """,
                (
                    command.cmd_id,
                    command.delivery_id,
                    command.device_id,
                    command.type,
                    command.priority,
                    command.ttl_ms,
                    max_attempts,
                    command.coalesce_key,
                    command.safety_class,
                    command.turn_id,
                    json.dumps(command.admission.to_dict(), ensure_ascii=False),
                    json.dumps(command.payload, ensure_ascii=False),
                    command.created_at or now,
                    now,
                    command.expires_at,
                ),
            )
        return row

    def lease_command(self, cmd_id: str, *, boot_id: int = 0, lease_ms: int = DEFAULT_LEASE_MS) -> dict[str, Any] | None:
        now = utc_now()
        lease_expires_at = future_time_ms(lease_ms)
        with self.database.connect() as conn:
            self._expire_due_locked(conn, now)
            row = conn.execute("SELECT * FROM commands WHERE cmd_id = ?", (cmd_id,)).fetchone()
            if row is None:
                return None
            if row["state"] in TERMINAL_COMMAND_STATES:
                return self._row_to_command(row)
            if int(row["attempt"] or 0) >= int(row["max_attempts"] or 1):
                conn.execute(
                    "UPDATE commands SET state='failed', updated_at=?, last_message=? WHERE cmd_id=?",
                    (now, "max attempts reached", cmd_id),
                )
                if row["delivery_id"]:
                    self._refresh_delivery_state(conn, row["delivery_id"], now)
                return None
            attempt = int(row["attempt"] or 0) + 1
            conn.execute(
                """
                UPDATE commands
                   SET state='leased', attempt=?, lease_expires_at=?, updated_at=?
                 WHERE cmd_id=?
                """,
                (attempt, lease_expires_at, now, cmd_id),
            )
            conn.execute(
                """
                INSERT INTO command_attempts(cmd_id, device_id, boot_id, attempt, leased_at, lease_expires_at)
                VALUES (?, ?, ?, ?, ?, ?)
                """,
                (cmd_id, row["device_id"], int(boot_id or 0), attempt, now, lease_expires_at),
            )
            leased = conn.execute("SELECT * FROM commands WHERE cmd_id = ?", (cmd_id,)).fetchone()
        return self._row_to_command(leased)

    def lease_next_command(self, device_id: str, *, boot_id: int = 0, lease_ms: int = DEFAULT_LEASE_MS) -> dict[str, Any] | None:
        now = utc_now()
        with self.database.connect() as conn:
            self._expire_due_locked(conn, now)
            self._fail_exhausted_leases_locked(conn, now)
            row = conn.execute(
                """
                SELECT * FROM commands
                 WHERE device_id=?
                   AND state IN ('queued', 'leased')
                   AND (expires_at='' OR expires_at > ?)
                   AND (state='queued' OR lease_expires_at='' OR lease_expires_at < ?)
                   AND attempt < max_attempts
                 ORDER BY safety_class DESC, priority DESC, created_at ASC
                 LIMIT 1
                """,
                (device_id, now, now),
            ).fetchone()
        if row is None:
            return None
        return self.lease_command(row["cmd_id"], boot_id=boot_id, lease_ms=lease_ms)

    def record_ack(self, ack: dict[str, Any]) -> dict[str, Any]:
        state = normalize_ack_state(str(ack.get("state") or ack.get("status") or "received"))
        cmd_id = str(ack.get("cmd_id") or "")
        now = utc_now()
        with self.database.connect() as conn:
            conn.execute(
                """
                INSERT INTO command_acks(
                  ack_seq, device_id, boot_id, cmd_id, attempt, state, effect,
                  started_at_tick, finished_at_tick, message, received_at
                ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
                """,
                (
                    int(ack.get("ack_seq") or 0),
                    str(ack.get("device_id") or ""),
                    int(ack.get("boot_id") or 0),
                    cmd_id,
                    int(ack.get("attempt") or 0),
                    state,
                    str(ack.get("effect") or ""),
                    int(ack.get("started_at_tick") or 0),
                    int(ack.get("finished_at_tick") or 0),
                    str(ack.get("message") or ""),
                    now,
                ),
            )
            if cmd_id:
                command = conn.execute("SELECT delivery_id FROM commands WHERE cmd_id=?", (cmd_id,)).fetchone()
                if command is not None:
                    conn.execute(
                        "UPDATE commands SET state=?, updated_at=?, last_message=? WHERE cmd_id=?",
                        (state, now, str(ack.get("message") or ""), cmd_id),
                    )
                    delivery_id = str(command["delivery_id"] or "")
                    if delivery_id:
                        self._refresh_delivery_state(conn, delivery_id, now)
        return {"cmd_id": cmd_id, "state": state, "received_at": now}

    def cancel_delivery(self, delivery_id: str, message: str = "cancelled") -> dict[str, Any]:
        now = utc_now()
        with self.database.connect() as conn:
            conn.execute(
                """
                UPDATE commands
                   SET state='cancelled', updated_at=?, last_message=?
                 WHERE delivery_id=? AND state NOT IN ('rendered', 'done', 'failed', 'cancelled', 'expired')
                """,
                (now, message, delivery_id),
            )
            self._refresh_delivery_state(conn, delivery_id, now, cancelled=True)
            row = conn.execute("SELECT * FROM deliveries WHERE delivery_id=?", (delivery_id,)).fetchone()
        return dict(row) if row else {}

    def get_delivery(self, delivery_id: str) -> dict[str, Any] | None:
        with self.database.connect() as conn:
            now = utc_now()
            self._expire_due_locked(conn, now)
            self._fail_exhausted_leases_locked(conn, now)
            self._refresh_delivery_state(conn, delivery_id, now)
            delivery = conn.execute("SELECT * FROM deliveries WHERE delivery_id=?", (delivery_id,)).fetchone()
            if delivery is None:
                return None
            commands = conn.execute(
                "SELECT * FROM commands WHERE delivery_id=? ORDER BY created_at ASC",
                (delivery_id,),
            ).fetchall()
            acks = conn.execute(
                """
                SELECT a.* FROM command_acks a
                JOIN commands c ON c.cmd_id = a.cmd_id
                WHERE c.delivery_id=?
                ORDER BY a.received_at ASC
                """,
                (delivery_id,),
            ).fetchall()
        body = dict(delivery)
        body["response"] = json.loads(body.pop("response_json") or "{}")
        body["policy"] = json.loads(body.pop("policy_json") or "{}")
        body["commands"] = [self._row_to_command(row, include_store_fields=True) for row in commands]
        body["acks"] = [dict(row) for row in acks]
        return body

    def create_delivery(
        self,
        *,
        delivery_id: str,
        device_id: str,
        user_id: str,
        event_id: str,
        response: dict[str, Any],
        policy: dict[str, Any],
        expires_at: str = "",
    ) -> None:
        now = utc_now()
        with self.database.connect() as conn:
            conn.execute(
                """
                INSERT INTO deliveries(
                  delivery_id, user_id, device_id, event_id, state, response_json,
                  policy_json, created_at, updated_at, expires_at
                ) VALUES (?, ?, ?, ?, 'submitted', ?, ?, ?, ?, ?)
                """,
                (
                    delivery_id,
                    user_id,
                    device_id,
                    event_id,
                    json.dumps(response, ensure_ascii=False),
                    json.dumps(policy, ensure_ascii=False),
                    now,
                    now,
                    expires_at,
                ),
            )

    def _refresh_delivery_state(self, conn, delivery_id: str, now: str, *, cancelled: bool = False) -> None:
        rows = conn.execute("SELECT state FROM commands WHERE delivery_id=?", (delivery_id,)).fetchall()
        if not rows:
            state = "cancelled" if cancelled else "submitted"
        else:
            states = {str(row["state"]) for row in rows}
            if states <= {"rendered", "done"}:
                state = "delivered"
            elif states & {"failed"}:
                state = "failed"
            elif states & {"expired"}:
                state = "expired"
            elif states <= {"cancelled"}:
                state = "cancelled"
            else:
                state = "submitted"
        conn.execute("UPDATE deliveries SET state=?, updated_at=? WHERE delivery_id=?", (state, now, delivery_id))

    def _expire_due_locked(self, conn, now: str) -> None:
        rows = conn.execute(
            """
            SELECT DISTINCT delivery_id FROM commands
             WHERE state NOT IN ('rendered', 'done', 'failed', 'cancelled', 'expired')
               AND expires_at!=''
               AND expires_at <= ?
            """,
            (now,),
        ).fetchall()
        conn.execute(
            """
            UPDATE commands
               SET state='expired', updated_at=?, last_message='expired'
             WHERE state NOT IN ('rendered', 'done', 'failed', 'cancelled', 'expired')
               AND expires_at!=''
               AND expires_at <= ?
            """,
            (now, now),
        )
        for row in rows:
            if row["delivery_id"]:
                self._refresh_delivery_state(conn, row["delivery_id"], now)

    def _fail_exhausted_leases_locked(self, conn, now: str) -> None:
        rows = conn.execute(
            """
            SELECT DISTINCT delivery_id FROM commands
             WHERE state='leased'
               AND lease_expires_at!=''
               AND lease_expires_at < ?
               AND attempt >= max_attempts
            """,
            (now,),
        ).fetchall()
        conn.execute(
            """
            UPDATE commands
               SET state='failed', updated_at=?, last_message='max attempts reached'
             WHERE state='leased'
               AND lease_expires_at!=''
               AND lease_expires_at < ?
               AND attempt >= max_attempts
            """,
            (now, now),
        )
        for row in rows:
            if row["delivery_id"]:
                self._refresh_delivery_state(conn, row["delivery_id"], now)

    def _row_to_command(self, row, *, include_store_fields: bool = False) -> dict[str, Any]:
        command = {
            "cmd_id": row["cmd_id"],
            "type": row["type"],
            "priority": row["priority"],
            "ttl_ms": row["ttl_ms"],
            "attempt": row["attempt"],
            "coalesce_key": row["coalesce_key"],
            "safety_class": row["safety_class"],
            "turn_id": row["turn_id"],
            "admission": json.loads(row["admission_json"] or "{}"),
            "payload": json.loads(row["payload_json"] or "{}"),
        }
        if include_store_fields:
            command.update(
                {
                    "device_id": row["device_id"],
                    "delivery_id": row["delivery_id"],
                    "state": row["state"],
                    "created_at": row["created_at"],
                    "updated_at": row["updated_at"],
                    "expires_at": row["expires_at"],
                    "lease_expires_at": row["lease_expires_at"],
                    "last_message": row["last_message"],
                }
            )
        return command
