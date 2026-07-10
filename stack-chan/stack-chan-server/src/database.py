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
                      text TEXT NOT NULL,
                      state TEXT NOT NULL,
                      attempts INTEGER NOT NULL DEFAULT 0,
                      created_at TEXT NOT NULL,
                      updated_at TEXT NOT NULL,
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
                conn.commit()
            finally:
                conn.close()

    @staticmethod
    def _migrate_commands(conn: sqlite3.Connection) -> None:
        columns = {str(row[1]) for row in conn.execute("PRAGMA table_info(commands)")}
        additions = {
            "source_type": "TEXT NOT NULL DEFAULT ''",
            "source_id": "TEXT NOT NULL DEFAULT ''",
            "segment_index": "INTEGER NOT NULL DEFAULT 0",
            "turn_generation": "INTEGER NOT NULL DEFAULT 0",
            "payload_retention_until": "TEXT NOT NULL DEFAULT ''",
        }
        for name, definition in additions.items():
            if name not in columns:
                conn.execute(f"ALTER TABLE commands ADD COLUMN {name} {definition}")
        conn.execute(
            """
            CREATE UNIQUE INDEX IF NOT EXISTS idx_commands_source_segment
              ON commands(source_type, source_id, segment_index, type)
             WHERE source_type != '' AND source_id != ''
            """
        )
