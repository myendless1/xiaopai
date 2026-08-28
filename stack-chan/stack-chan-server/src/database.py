from __future__ import annotations

import os
import sqlite3
import threading
from contextlib import contextmanager
from typing import Iterator


class Database:
    def __init__(self, path: str) -> None:
        self.path = os.path.abspath(path)
        self._lock = threading.RLock()
        os.makedirs(os.path.dirname(self.path), exist_ok=True)
        self.initialize()

    @contextmanager
    def connect(self) -> Iterator[sqlite3.Connection]:
        with self._lock:
            conn = sqlite3.connect(self.path)
            conn.row_factory = sqlite3.Row
            conn.execute("PRAGMA journal_mode=WAL")
            conn.execute("PRAGMA foreign_keys=ON")
            try:
                yield conn
                conn.commit()
            finally:
                conn.close()

    def initialize(self) -> None:
        with self._lock:
            conn = sqlite3.connect(self.path)
            try:
                conn.executescript(
                    """
                    PRAGMA journal_mode=WAL;
                    PRAGMA foreign_keys=ON;

                    CREATE TABLE IF NOT EXISTS devices (
                      device_id TEXT PRIMARY KEY,
                      user_id TEXT NOT NULL DEFAULT '',
                      capabilities_json TEXT NOT NULL DEFAULT '[]',
                      last_seen_at TEXT NOT NULL DEFAULT '',
                      last_heartbeat_at TEXT NOT NULL DEFAULT '',
                      last_ack_seq INTEGER NOT NULL DEFAULT 0,
                      speech_generation INTEGER NOT NULL DEFAULT 0,
                      current_boot_id INTEGER NOT NULL DEFAULT 0,
                      online INTEGER NOT NULL DEFAULT 0
                    );

                    CREATE TABLE IF NOT EXISTS device_sessions (
                      session_id TEXT PRIMARY KEY,
                      device_id TEXT NOT NULL,
                      boot_id INTEGER NOT NULL DEFAULT 0,
                      firmware_version TEXT NOT NULL DEFAULT '',
                      protocol_version INTEGER NOT NULL DEFAULT 0,
                      capabilities_json TEXT NOT NULL DEFAULT '[]',
                      connected_at TEXT NOT NULL,
                      last_seen_at TEXT NOT NULL,
                      closed_at TEXT NOT NULL DEFAULT ''
                    );

                    CREATE TABLE IF NOT EXISTS deliveries (
                      delivery_id TEXT PRIMARY KEY,
                      user_id TEXT NOT NULL DEFAULT '',
                      device_id TEXT NOT NULL DEFAULT '',
                      event_id TEXT NOT NULL DEFAULT '',
                      state TEXT NOT NULL,
                      response_json TEXT NOT NULL,
                      policy_json TEXT NOT NULL DEFAULT '{}',
                      created_at TEXT NOT NULL,
                      updated_at TEXT NOT NULL,
                      expires_at TEXT NOT NULL DEFAULT ''
                    );

                    CREATE TABLE IF NOT EXISTS commands (
                      cmd_id TEXT PRIMARY KEY,
                      delivery_id TEXT NOT NULL DEFAULT '',
                      device_id TEXT NOT NULL,
                      boot_id INTEGER NOT NULL DEFAULT 0,
                      type TEXT NOT NULL,
                      priority INTEGER NOT NULL DEFAULT 50,
                      ttl_ms INTEGER NOT NULL DEFAULT 30000,
                      attempt INTEGER NOT NULL DEFAULT 0,
                      max_attempts INTEGER NOT NULL DEFAULT 3,
                      state TEXT NOT NULL,
                      coalesce_key TEXT NOT NULL DEFAULT '',
                      safety_class TEXT NOT NULL DEFAULT 'normal',
                      turn_id TEXT NOT NULL DEFAULT '',
                      admission_json TEXT NOT NULL DEFAULT '{}',
                      payload_json TEXT NOT NULL DEFAULT '{}',
                      created_at TEXT NOT NULL,
                      updated_at TEXT NOT NULL,
                      expires_at TEXT NOT NULL DEFAULT '',
                      lease_expires_at TEXT NOT NULL DEFAULT '',
                      last_message TEXT NOT NULL DEFAULT '',
                      source_type TEXT NOT NULL DEFAULT '',
                      source_id TEXT NOT NULL DEFAULT '',
                      segment_index INTEGER NOT NULL DEFAULT 0,
                      turn_generation INTEGER NOT NULL DEFAULT 0,
                      queue_seq INTEGER NOT NULL DEFAULT 0,
                      payload_retention_until TEXT NOT NULL DEFAULT ''
                    );

                    CREATE TABLE IF NOT EXISTS command_attempts (
                      id INTEGER PRIMARY KEY AUTOINCREMENT,
                      cmd_id TEXT NOT NULL,
                      device_id TEXT NOT NULL,
                      boot_id INTEGER NOT NULL DEFAULT 0,
                      attempt INTEGER NOT NULL,
                      leased_at TEXT NOT NULL,
                      lease_expires_at TEXT NOT NULL
                    );

                    CREATE TABLE IF NOT EXISTS command_acks (
                      id INTEGER PRIMARY KEY AUTOINCREMENT,
                      ack_seq INTEGER NOT NULL DEFAULT 0,
                      device_id TEXT NOT NULL,
                      boot_id INTEGER NOT NULL DEFAULT 0,
                      cmd_id TEXT NOT NULL,
                      attempt INTEGER NOT NULL DEFAULT 0,
                      state TEXT NOT NULL,
                      effect TEXT NOT NULL DEFAULT '',
                      started_at_tick INTEGER NOT NULL DEFAULT 0,
                      finished_at_tick INTEGER NOT NULL DEFAULT 0,
                      message TEXT NOT NULL DEFAULT '',
                      received_at TEXT NOT NULL
                    );

                    CREATE TABLE IF NOT EXISTS captures (
                      capture_id TEXT PRIMARY KEY,
                      device_id TEXT NOT NULL,
                      metadata_json TEXT NOT NULL DEFAULT '{}',
                      result_json TEXT NOT NULL DEFAULT '{}',
                      created_at TEXT NOT NULL
                    );

                    CREATE TABLE IF NOT EXISTS ota_releases (
                      version TEXT PRIMARY KEY,
                      image_path TEXT NOT NULL,
                      sha256 TEXT NOT NULL,
                      signature TEXT NOT NULL DEFAULT '',
                      metadata_json TEXT NOT NULL DEFAULT '{}',
                      created_at TEXT NOT NULL
                    );

                    CREATE TABLE IF NOT EXISTS morrow_notices (
                      notice_id TEXT PRIMARY KEY,
                      kind TEXT NOT NULL,
                      timestamp_ms INTEGER NOT NULL,
                      text TEXT NOT NULL,
                      state TEXT NOT NULL,
                      expires_at TEXT,
                      command_id TEXT,
                      received_at TEXT NOT NULL,
                      rendered_at TEXT,
                      last_error TEXT,
                      -- keep old columns for compatibility
                      attempts INTEGER NOT NULL DEFAULT 0,
                      created_at TEXT NOT NULL DEFAULT '',
                      updated_at TEXT NOT NULL DEFAULT '',
                      next_attempt_at TEXT NOT NULL DEFAULT '',
                      last_message TEXT NOT NULL DEFAULT ''
                    );

                    CREATE INDEX IF NOT EXISTS idx_commands_device_state
                      ON commands(device_id, state, priority, created_at);
                    CREATE INDEX IF NOT EXISTS idx_commands_delivery
                      ON commands(delivery_id);
                    CREATE INDEX IF NOT EXISTS idx_acks_cmd
                      ON command_acks(cmd_id);
                    """
                )
                self._migrate_commands(conn)
                self._migrate_devices(conn)
                self._migrate_notices(conn)
                conn.commit()
            finally:
                conn.close()

    @staticmethod
    def _migrate_devices(conn: sqlite3.Connection) -> None:
        columns = {str(row[1]) for row in conn.execute("PRAGMA table_info(devices)")}
        if "speech_generation" not in columns:
            conn.execute(
                "ALTER TABLE devices ADD COLUMN speech_generation INTEGER NOT NULL DEFAULT 0"
            )
        if "current_boot_id" not in columns:
            conn.execute(
                "ALTER TABLE devices ADD COLUMN current_boot_id INTEGER NOT NULL DEFAULT 0"
            )
        # Older databases already contain the generation on dialogue commands.
        # Backfill it so a Server restart cannot make new speech stale.
        conn.execute(
            """
            INSERT OR IGNORE INTO devices(device_id, speech_generation)
            SELECT device_id, MAX(turn_generation) FROM commands GROUP BY device_id
            """
        )
        conn.execute(
            """
            UPDATE devices
               SET speech_generation=MAX(
                     speech_generation,
                     COALESCE((
                       SELECT MAX(commands.turn_generation)
                         FROM commands
                        WHERE commands.device_id=devices.device_id
                     ), 0)
                   )
            """
        )
        conn.execute(
            """
            UPDATE devices
               SET current_boot_id=COALESCE((
                     SELECT sessions.boot_id
                       FROM device_sessions AS sessions
                      WHERE sessions.device_id=devices.device_id
                      ORDER BY sessions.connected_at DESC
                      LIMIT 1
                   ), current_boot_id)
             WHERE current_boot_id=0
            """
        )

    @staticmethod
    def _migrate_commands(conn: sqlite3.Connection) -> None:
        columns = {str(row[1]) for row in conn.execute("PRAGMA table_info(commands)")}
        additions = {
            "boot_id": "INTEGER NOT NULL DEFAULT 0",
            "source_type": "TEXT NOT NULL DEFAULT ''",
            "source_id": "TEXT NOT NULL DEFAULT ''",
            "segment_index": "INTEGER NOT NULL DEFAULT 0",
            "turn_generation": "INTEGER NOT NULL DEFAULT 0",
            "queue_seq": "INTEGER NOT NULL DEFAULT 0",
            "payload_retention_until": "TEXT NOT NULL DEFAULT ''",
        }
        for name, definition in additions.items():
            if name not in columns:
                conn.execute(f"ALTER TABLE commands ADD COLUMN {name} {definition}")
        # Wall time can move backwards under WSL/NTP. Preserve the actual SQLite
        # insertion order so speech controls can never overtake earlier segments.
        conn.execute("UPDATE commands SET queue_seq=rowid WHERE queue_seq=0")
        conn.execute(
            """
            UPDATE commands
               SET boot_id=COALESCE((
                     SELECT attempts.boot_id
                       FROM command_attempts AS attempts
                      WHERE attempts.cmd_id=commands.cmd_id
                      ORDER BY attempts.id DESC
                      LIMIT 1
                   ), 0)
             WHERE boot_id=0
            """
        )
        conn.execute(
            """
            CREATE INDEX IF NOT EXISTS idx_commands_device_boot_queue
              ON commands(device_id, boot_id, state, priority, queue_seq)
            """
        )
        conn.execute(
            """
            CREATE UNIQUE INDEX IF NOT EXISTS idx_commands_source_segment
              ON commands(source_type, source_id, segment_index, type)
             WHERE source_type != '' AND source_id != ''
            """
        )

    @staticmethod
    def _migrate_notices(conn: sqlite3.Connection) -> None:
        columns = {str(row[1]) for row in conn.execute("PRAGMA table_info(morrow_notices)")}
        additions = {
            "kind": "TEXT NOT NULL DEFAULT 'unknown'",
            "timestamp_ms": "INTEGER NOT NULL DEFAULT 0",
            "expires_at": "TEXT",
            "command_id": "TEXT",
            "received_at": "TEXT NOT NULL DEFAULT ''",
            "rendered_at": "TEXT",
            "last_error": "TEXT",
        }
        for name, definition in additions.items():
            if name not in columns:
                conn.execute(f"ALTER TABLE morrow_notices ADD COLUMN {name} {definition}")
