from __future__ import annotations

import datetime as dt
import json
from typing import Any

from database import Database
from schemas import CommandEnvelope, DEFAULT_LEASE_MS, TERMINAL_COMMAND_STATES, future_time_ms, normalize_ack_state, utc_now


RUNNABLE_STATES = ("queued", "leased")
STALE_DIALOGUE_SECONDS = 10 * 60
STATE_RANK = {
    "queued": 0,
    "leased": 1,
    "received": 2,
    "running": 3,
    "rendered": 4,
    "done": 4,
    "failed": 4,
    "cancelled": 4,
    "expired": 4,
}


class CommandStore:
    def __init__(self, database: Database) -> None:
        self.database = database

    def create_command(self, command: CommandEnvelope, *, max_attempts: int = 3) -> dict[str, Any]:
        row = command.to_store_dict()
        now = utc_now()
        with self.database.connect() as conn:
            if int(command.boot_id or 0) <= 0:
                device = conn.execute(
                    "SELECT current_boot_id FROM devices WHERE device_id=?",
                    (str(command.device_id or "default"),),
                ).fetchone()
                command.boot_id = max(0, int(device["current_boot_id"] or 0)) if device else 0
            conn.execute(
                """
                INSERT OR IGNORE INTO commands (
                  cmd_id, delivery_id, device_id, boot_id, type, priority, ttl_ms, attempt, max_attempts,
                  state, coalesce_key, safety_class, turn_id, admission_json, payload_json,
                  created_at, updated_at, expires_at, source_type, source_id, segment_index,
                  turn_generation, queue_seq, payload_retention_until
                ) VALUES (
                  ?, ?, ?, ?, ?, ?, ?, 0, ?, 'queued', ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?,
                  (SELECT COALESCE(MAX(queue_seq), 0) + 1 FROM commands), ?
                )
                """,
                (
                    command.cmd_id,
                    command.delivery_id,
                    command.device_id,
                    command.boot_id,
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
                    command.source_type,
                    command.source_id,
                    command.segment_index,
                    command.turn_generation,
                    command.payload_retention_until,
                ),
            )
        row["boot_id"] = command.boot_id
        return row

    def current_boot_id(self, device_id: str) -> int:
        with self.database.connect() as conn:
            row = conn.execute(
                "SELECT current_boot_id FROM devices WHERE device_id=?",
                (str(device_id or "default"),),
            ).fetchone()
        return max(0, int(row["current_boot_id"] or 0)) if row else 0

    def observe_device_boot(self, device_id: str, boot_id: int) -> int:
        """Adopt a device boot and expire commands bound to every older boot."""
        device_id = str(device_id or "default")
        boot_id = max(0, int(boot_id or 0))
        if boot_id <= 0:
            return 0
        now = utc_now()
        with self.database.connect() as conn:
            current = conn.execute(
                "SELECT current_boot_id FROM devices WHERE device_id=?",
                (device_id,),
            ).fetchone()
            previous_boot_id = max(0, int(current["current_boot_id"] or 0)) if current else 0
            conn.execute(
                """
                INSERT INTO devices(device_id, current_boot_id)
                VALUES (?, ?)
                ON CONFLICT(device_id) DO UPDATE SET current_boot_id=excluded.current_boot_id
                """,
                (device_id, boot_id),
            )
            if previous_boot_id <= 0:
                conn.execute(
                    """
                    UPDATE commands SET boot_id=?
                     WHERE device_id=? AND boot_id=0
                       AND state NOT IN ('rendered', 'done', 'failed', 'cancelled', 'expired')
                    """,
                    (boot_id, device_id),
                )

            delivery_rows = conn.execute(
                """
                SELECT DISTINCT delivery_id FROM commands
                 WHERE device_id=? AND boot_id>0 AND boot_id!=?
                   AND state NOT IN ('rendered', 'done', 'failed', 'cancelled', 'expired')
                """,
                (device_id, boot_id),
            ).fetchall()
            cursor = conn.execute(
                """
                UPDATE commands
                   SET state='expired', updated_at=?, lease_expires_at='',
                       last_message='device boot expired'
                 WHERE device_id=? AND boot_id>0 AND boot_id!=?
                   AND state NOT IN ('rendered', 'done', 'failed', 'cancelled', 'expired')
                """,
                (now, device_id, boot_id),
            )
            for row in delivery_rows:
                if row["delivery_id"]:
                    self._refresh_delivery_state(conn, row["delivery_id"], now)
        return max(0, int(cursor.rowcount or 0))

    def expire_inactive_boot_commands(self) -> int:
        """Recover commands left behind by device boots that are no longer current."""
        now = utc_now()
        with self.database.connect() as conn:
            delivery_rows = conn.execute(
                """
                SELECT DISTINCT commands.delivery_id
                  FROM commands JOIN devices USING(device_id)
                 WHERE commands.boot_id>0 AND devices.current_boot_id>0
                   AND commands.boot_id!=devices.current_boot_id
                   AND commands.state NOT IN ('rendered', 'done', 'failed', 'cancelled', 'expired')
                """
            ).fetchall()
            cursor = conn.execute(
                """
                UPDATE commands
                   SET state='expired', updated_at=?, lease_expires_at='',
                       last_message='device boot expired'
                 WHERE boot_id>0
                   AND state NOT IN ('rendered', 'done', 'failed', 'cancelled', 'expired')
                   AND EXISTS (
                     SELECT 1 FROM devices
                      WHERE devices.device_id=commands.device_id
                        AND devices.current_boot_id>0
                        AND devices.current_boot_id!=commands.boot_id
                   )
                """,
                (now,),
            )
            for row in delivery_rows:
                if row["delivery_id"]:
                    self._refresh_delivery_state(conn, row["delivery_id"], now)
        return max(0, int(cursor.rowcount or 0))

    def has_unfinished_dialogue(self, device_id: str) -> bool:
        self.expire_stale_dialogue_commands(device_id=device_id)
        current_boot_id = self.current_boot_id(device_id)
        placeholders = ",".join("?" for _ in TERMINAL_COMMAND_STATES)
        with self.database.connect() as conn:
            row = conn.execute(
                f"""
                SELECT 1
                  FROM commands
                 WHERE device_id=?
                   AND source_type='dialogue'
                   AND (?=0 OR boot_id IN (0, ?))
                   AND state NOT IN ({placeholders})
                 LIMIT 1
                """,
                (
                    str(device_id or "default"),
                    current_boot_id,
                    current_boot_id,
                    *sorted(TERMINAL_COMMAND_STATES),
                ),
            ).fetchone()
        return row is not None

    def expire_stale_dialogue_commands(
        self,
        *,
        device_id: str = "",
        stale_after_seconds: int = STALE_DIALOGUE_SECONDS,
    ) -> int:
        """Release dialogue commands that can no longer receive a terminal device ACK."""
        now = dt.datetime.now(dt.timezone.utc)
        cutoff = (now - dt.timedelta(seconds=max(1, int(stale_after_seconds)))).isoformat()
        now_text = now.isoformat()
        device_clause = " AND device_id=?" if device_id else ""
        parameters = [cutoff]
        if device_id:
            parameters.append(str(device_id))
        with self.database.connect() as conn:
            delivery_rows = conn.execute(
                f"""
                SELECT DISTINCT delivery_id FROM commands
                 WHERE source_type='dialogue'
                   AND state NOT IN ('rendered', 'done', 'failed', 'cancelled', 'expired')
                   AND updated_at <= ?
                   {device_clause}
                """,
                tuple(parameters),
            ).fetchall()
            cursor = conn.execute(
                f"""
                UPDATE commands
                   SET state='expired', updated_at=?, lease_expires_at='',
                       last_message='stale dialogue command recovered'
                 WHERE source_type='dialogue'
                   AND state NOT IN ('rendered', 'done', 'failed', 'cancelled', 'expired')
                   AND updated_at <= ?
                   {device_clause}
                """,
                (now_text, *parameters),
            )
            for row in delivery_rows:
                if row["delivery_id"]:
                    self._refresh_delivery_state(conn, row["delivery_id"], now_text)
            return max(0, int(cursor.rowcount or 0))

    def speech_generations(self) -> dict[str, int]:
        with self.database.connect() as conn:
            rows = conn.execute(
                "SELECT device_id, speech_generation FROM devices WHERE speech_generation > 0"
            ).fetchall()
        return {str(row["device_id"]): int(row["speech_generation"] or 0) for row in rows}

    def speech_generation_for_device(self, device_id: str) -> int:
        with self.database.connect() as conn:
            row = conn.execute(
                "SELECT speech_generation FROM devices WHERE device_id=?",
                (str(device_id or "default"),),
            ).fetchone()
        return max(0, int(row["speech_generation"] or 0)) if row else 0

    def set_speech_generation(self, device_id: str, generation: int) -> None:
        with self.database.connect() as conn:
            conn.execute(
                """
                INSERT INTO devices(device_id, speech_generation)
                VALUES (?, ?)
                ON CONFLICT(device_id) DO UPDATE SET
                  speech_generation=MAX(devices.speech_generation, excluded.speech_generation)
                """,
                (str(device_id or "default"), max(0, int(generation))),
            )

    def lease_command(self, cmd_id: str, *, boot_id: int = 0, lease_ms: int = DEFAULT_LEASE_MS) -> dict[str, Any] | None:
        now = utc_now()
        lease_expires_at = future_time_ms(lease_ms)
        with self.database.connect() as conn:
            self._expire_due_locked(conn, now)
            row = conn.execute("SELECT * FROM commands WHERE cmd_id = ?", (cmd_id,)).fetchone()
            if row is None:
                return None
            command_boot_id = max(0, int(row["boot_id"] or 0))
            boot_id = max(0, int(boot_id or 0))
            if command_boot_id > 0 and boot_id > 0 and command_boot_id != boot_id:
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
                   SET state='leased', boot_id=CASE WHEN boot_id=0 THEN ? ELSE boot_id END,
                       attempt=?, lease_expires_at=?, updated_at=?
                 WHERE cmd_id=?
                """,
                (boot_id, attempt, lease_expires_at, now, cmd_id),
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

    def lease_next_command(
        self,
        device_id: str,
        *,
        boot_id: int = 0,
        lease_ms: int = DEFAULT_LEASE_MS,
        allow_speak: bool = True,
        allow_find_owner: bool = True,
    ) -> dict[str, Any] | None:
        now = utc_now()
        with self.database.connect() as conn:
            self._expire_due_locked(conn, now)
            self._fail_exhausted_leases_locked(conn, now)
            row = conn.execute(
                """
                SELECT * FROM commands
                 WHERE device_id=?
                   AND (?=0 OR boot_id IN (0, ?))
                   AND state IN ('queued', 'leased')
                   AND (? OR type!='speak')
                   AND (? OR type NOT IN ('find_owner', 'locate_owner'))
                   AND (expires_at='' OR expires_at > ?)
                   AND (state='queued' OR lease_expires_at='' OR lease_expires_at < ?)
                   AND attempt < max_attempts
                 ORDER BY safety_class DESC, priority DESC, queue_seq ASC
                 LIMIT 1
                """,
                (
                    device_id,
                    int(boot_id or 0),
                    int(boot_id or 0),
                    int(bool(allow_speak)),
                    int(bool(allow_find_owner)),
                    now,
                    now,
                ),
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
                command = conn.execute(
                    "SELECT delivery_id, state, boot_id FROM commands WHERE cmd_id=?",
                    (cmd_id,),
                ).fetchone()
                if command is not None:
                    ack_boot_id = max(0, int(ack.get("boot_id") or 0))
                    command_boot_id = max(0, int(command["boot_id"] or 0))
                    if command_boot_id > 0 and ack_boot_id > 0 and command_boot_id != ack_boot_id:
                        return {
                            "cmd_id": cmd_id,
                            "state": str(command["state"] or "expired"),
                            "received_at": now,
                            "ignored": True,
                            "message": "ack boot_id does not match command boot_id",
                        }
                    current_state = str(command["state"] or "queued")
                    if state == "deferred" and current_state not in TERMINAL_COMMAND_STATES:
                        state = "queued"
                        conn.execute(
                            """
                            UPDATE commands
                               SET state='queued',
                                   attempt=CASE WHEN attempt > 0 THEN attempt - 1 ELSE 0 END,
                                   updated_at=?, last_message=?, lease_expires_at=''
                             WHERE cmd_id=?
                            """,
                            (now, str(ack.get("message") or "device deferred command"), cmd_id),
                        )
                    elif self._ack_can_advance(current_state, state):
                        lease_expires_at = future_time_ms(DEFAULT_LEASE_MS) if state == "running" else ""
                        conn.execute(
                            """
                            UPDATE commands
                               SET state=?, updated_at=?, last_message=?,
                                   lease_expires_at=CASE WHEN ? != '' THEN ? ELSE lease_expires_at END
                             WHERE cmd_id=?
                            """,
                            (state, now, str(ack.get("message") or ""), lease_expires_at, lease_expires_at, cmd_id),
                        )
                        # Link ACK to morrow_notices state
                        notice_state = state
                        if state in ("received", "queued"):
                            notice_state = "queued"
                        elif state in ("running", "leased"):
                            notice_state = "leased"
                        conn.execute(
                            """
                            UPDATE morrow_notices
                               SET state=?, rendered_at=CASE WHEN ?='rendered' THEN ? ELSE rendered_at END,
                                   last_error=CASE WHEN ?='failed' THEN ? ELSE last_error END
                             WHERE command_id=?
                            """,
                            (notice_state, state, now, state, str(ack.get("message") or ""), cmd_id),
                        )
                    delivery_id = str(command["delivery_id"] or "")
                    if delivery_id:
                        self._refresh_delivery_state(conn, delivery_id, now)
        return {"cmd_id": cmd_id, "state": state, "received_at": now}

    def _ack_can_advance(self, current_state: str, next_state: str) -> bool:
        current = normalize_ack_state(current_state)
        incoming = normalize_ack_state(next_state)
        if current in TERMINAL_COMMAND_STATES:
            return incoming == current
        return STATE_RANK.get(incoming, 0) >= STATE_RANK.get(current, 0)

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

    def cancel_pending_by_source(self, source_type: str, source_id: str, message: str = "source cancelled") -> int:
        now = utc_now()
        with self.database.connect() as conn:
            cursor = conn.execute(
                """
                UPDATE commands
                   SET state='cancelled', updated_at=?, last_message=?
                 WHERE source_type=? AND source_id=? AND state='queued'
                """,
                (now, message, str(source_type), str(source_id)),
            )
            # Link cancellation to morrow_notices state
            conn.execute(
                """
                UPDATE morrow_notices
                   SET state='cancelled', last_error=?
                 WHERE state IN ('received', 'queued', 'leased') AND command_id IN (
                     SELECT cmd_id FROM commands WHERE state='cancelled' AND source_type=? AND source_id=?
                 )
                """,
                (message, str(source_type), str(source_id)),
            )
        return int(cursor.rowcount or 0)

    def cancel_pending_before_generation(self, device_id: str, generation: int, message: str = "old generation") -> int:
        now = utc_now()
        with self.database.connect() as conn:
            delivery_rows = conn.execute(
                """
                SELECT DISTINCT delivery_id FROM commands
                 WHERE device_id=? AND type='speak' AND turn_generation < ?
                   AND state NOT IN ('rendered', 'done', 'failed', 'cancelled', 'expired')
                   AND delivery_id!=''
                """,
                (str(device_id), int(generation)),
            ).fetchall()
            cursor = conn.execute(
                """
                UPDATE commands
                   SET state='cancelled', updated_at=?, lease_expires_at='', last_message=?
                 WHERE device_id=? AND type='speak' AND turn_generation < ?
                   AND state NOT IN ('rendered', 'done', 'failed', 'cancelled', 'expired')
                """,
                (now, message, str(device_id), int(generation)),
            )
            # Link generation cancellation to morrow_notices state
            conn.execute(
                """
                UPDATE morrow_notices
                   SET state='cancelled', last_error=?
                 WHERE state IN ('received', 'queued', 'leased') AND command_id IN (
                     SELECT cmd_id FROM commands WHERE state='cancelled' AND device_id=? AND turn_generation < ?
                 )
                """,
                (message, str(device_id), int(generation)),
            )
            for row in delivery_rows:
                self._refresh_delivery_state(conn, row["delivery_id"], now, cancelled=True)
        return int(cursor.rowcount or 0)

    def find_terminal_ack(self, cmd_id: str) -> dict[str, Any] | None:
        placeholders = ",".join("?" for _ in TERMINAL_COMMAND_STATES)
        with self.database.connect() as conn:
            row = conn.execute(
                f"SELECT * FROM command_acks WHERE cmd_id=? AND state IN ({placeholders}) ORDER BY id DESC LIMIT 1",
                (str(cmd_id), *sorted(TERMINAL_COMMAND_STATES)),
            ).fetchone()
        return dict(row) if row else None

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
        # Link expiration to morrow_notices state
        conn.execute(
            """
            UPDATE morrow_notices
               SET state='expired', last_error='command expired'
             WHERE state NOT IN ('rendered', 'done', 'failed', 'cancelled', 'expired')
               AND command_id IN (
                   SELECT cmd_id FROM commands WHERE state='expired'
               )
            """
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
        # Link failure to morrow_notices state
        conn.execute(
            """
            UPDATE morrow_notices
               SET state='failed', last_error='command max attempts reached'
             WHERE state NOT IN ('rendered', 'done', 'failed', 'cancelled', 'expired')
               AND command_id IN (
                   SELECT cmd_id FROM commands WHERE state='failed'
               )
            """
        )
        for row in rows:
            if row["delivery_id"]:
                self._refresh_delivery_state(conn, row["delivery_id"], now)

    def _row_to_command(self, row, *, include_store_fields: bool = False) -> dict[str, Any]:
        command = {
            "cmd_id": row["cmd_id"],
            "type": row["type"],
            "boot_id": row["boot_id"],
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
                    "source_type": row["source_type"],
                    "source_id": row["source_id"],
                    "segment_index": row["segment_index"],
                    "turn_generation": row["turn_generation"],
                    "payload_retention_until": row["payload_retention_until"],
                }
            )
        return command
