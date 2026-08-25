#!/usr/bin/env python3
"""Host-side checks for the fixed DJI 48 kHz to 16 kHz FIR pipeline."""

from __future__ import annotations

import cmath
import math
import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "main" / "audio" / "dji_mic_receiver_input.cpp"


def load_coefficients() -> list[int]:
    text = SOURCE.read_text(encoding="utf-8")
    match = re.search(
        r"kDecimatorFirQ15\s*=\s*\{(?P<body>.*?)\};",
        text,
        flags=re.DOTALL,
    )
    if not match:
        raise AssertionError("kDecimatorFirQ15 not found")
    return [int(value) for value in re.findall(r"-?\d+", match.group("body"))]


def response_db(coefficients: list[int], frequency_hz: float) -> float:
    response = sum(
        coefficient * cmath.exp(-2j * math.pi * frequency_hz * index / 48_000)
        for index, coefficient in enumerate(coefficients)
    ) / 32768.0
    return 20.0 * math.log10(max(abs(response), 1e-12))


def main() -> None:
    coefficients = load_coefficients()
    assert len(coefficients) == 63, len(coefficients)
    assert sum(coefficients) == 32768, sum(coefficients)
    assert abs(response_db(coefficients, 4_000)) <= 0.2
    assert response_db(coefficients, 9_000) <= -35.0
    assert response_db(coefficients, 12_000) <= -60.0

    input_frames = 48_000
    output_frames = sum(1 for index in range(input_frames) if (index + 1) % 3 == 0)
    assert output_frames == 16_000

    print(
        "DJI FIR validation OK:",
        f"taps={len(coefficients)}",
        f"4k={response_db(coefficients, 4_000):.2f}dB",
        f"9k={response_db(coefficients, 9_000):.2f}dB",
        f"12k={response_db(coefficients, 12_000):.2f}dB",
        f"frames={input_frames}->{output_frames}",
    )


if __name__ == "__main__":
    main()

