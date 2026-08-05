#!/usr/bin/env python3
"""Send one Xiaopai debug command over the USB Serial/JTAG console."""

from __future__ import annotations

import argparse
import json
import sys
import time

import serial


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", help="JSON command object, or the plain command 'status'/'help'")
    parser.add_argument("--port", default="/dev/ttyACM0")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--timeout", type=float, default=15.0)
    return parser.parse_args()


def normalize_command(value: str) -> str:
    if value in {"help", "status"}:
        return value
    parsed = json.loads(value)
    if not isinstance(parsed, dict):
        raise ValueError("command must be a JSON object")
    return json.dumps(parsed, ensure_ascii=False, separators=(",", ":"))


def main() -> int:
    args = parse_args()
    try:
        command = normalize_command(args.command)
    except (ValueError, json.JSONDecodeError) as exc:
        print(f"Invalid command: {exc}", file=sys.stderr)
        return 2

    deadline = time.monotonic() + args.timeout
    with serial.Serial(args.port, args.baud, timeout=0.2, write_timeout=2, exclusive=True) as console:
        console.reset_input_buffer()
        console.write((command + "\n").encode("utf-8"))
        console.flush()

        while time.monotonic() < deadline:
            raw = console.readline()
            if not raw:
                continue
            line = raw.decode("utf-8", errors="replace").rstrip()
            print(line)
            if "SERIAL_CMD result=" in line or "SERIAL_CMD status " in line or "SERIAL_CMD help:" in line:
                failed = "result=failed" in line or "result=error" in line
                return 1 if failed else 0

    print("Timed out waiting for SERIAL_CMD response", file=sys.stderr)
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
