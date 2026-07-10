from __future__ import annotations

from typing import Any


DEFAULT_DELIVERY_POLICY = {
    "presence_requirement": "none",
    "offline_behavior": "queue",
    "max_attempts": 3,
    "initial_retry_ms": 3000,
    "max_retry_ms": 15000,
}


def delivery_policy(raw: dict[str, Any] | None) -> dict[str, Any]:
    policy = dict(DEFAULT_DELIVERY_POLICY)
    if isinstance(raw, dict):
        policy.update({str(key): value for key, value in raw.items()})
    policy["presence_requirement"] = str(policy.get("presence_requirement") or "none")
    policy["offline_behavior"] = str(policy.get("offline_behavior") or "queue")
    policy["max_attempts"] = max(1, int(policy.get("max_attempts") or 3))
    return policy


def command_ttl_ms(policy: dict[str, Any], default_ms: int = 30_000) -> int:
    if policy.get("ttl_ms") not in (None, ""):
        return max(1, int(policy["ttl_ms"]))
    return default_ms
