from __future__ import annotations

from typing import Any, Callable

from command_store import CommandStore
from policy import command_ttl_ms, delivery_policy
from schemas import AdmissionPolicy, CommandEnvelope, new_id


class DeliveryCoordinator:
    def __init__(self, store: CommandStore, enqueue: Callable[[str, dict[str, Any]], bool] | None = None) -> None:
        self.store = store
        self.enqueue = enqueue

    def submit(self, body: dict[str, Any]) -> dict[str, Any]:
        response = self._response_from_body(body)
        context = body.get("context") if isinstance(body.get("context"), dict) else {}
        policy = delivery_policy(response.get("delivery_policy") if isinstance(response.get("delivery_policy"), dict) else body.get("delivery_policy"))
        device_id = str(body.get("device_id") or context.get("device_id") or policy.get("device_id") or "default")
        delivery_id = str(body.get("delivery_id") or new_id("deliv"))
        user_id = str(body.get("user_id") or context.get("user_id") or "")
        event_id = str(body.get("event_id") or "")
        expires_at = str(policy.get("expires_at") or "")

        commands = self._commands_for_response(delivery_id, device_id, response, policy)
        self.store.create_delivery(
            delivery_id=delivery_id,
            device_id=device_id,
            user_id=user_id,
            event_id=event_id,
            response=response,
            policy=policy,
            expires_at=expires_at,
        )
        queued = []
        for command in commands:
            stored = self.store.create_command(command, max_attempts=int(policy["max_attempts"]))
            if self.enqueue is not None:
                self.enqueue(device_id, command.to_device_command())
            queued.append(stored)

        return {
            "type": "delivery",
            "delivery_id": delivery_id,
            "state": "submitted",
            "device_id": device_id,
            "commands": queued,
            "policy": policy,
        }

    def _response_from_body(self, body: dict[str, Any]) -> dict[str, Any]:
        if isinstance(body.get("response"), dict):
            return dict(body["response"])
        return {
            "speech": str(body.get("speech") or ""),
            "presentation": body.get("presentation") if isinstance(body.get("presentation"), dict) else {},
            "actions": body.get("actions") if isinstance(body.get("actions"), list) else [],
            "follow_up": body.get("follow_up") if isinstance(body.get("follow_up"), dict) else {"expected": False},
            "context_patch": body.get("context_patch") if isinstance(body.get("context_patch"), dict) else {},
            "delivery_policy": body.get("delivery_policy") if isinstance(body.get("delivery_policy"), dict) else {},
        }

    def _commands_for_response(
        self,
        delivery_id: str,
        device_id: str,
        response: dict[str, Any],
        policy: dict[str, Any],
    ) -> list[CommandEnvelope]:
        presentation = response.get("presentation") if isinstance(response.get("presentation"), dict) else {}
        commands: list[CommandEnvelope] = []
        presence = str(policy.get("presence_requirement") or "none")
        admission = AdmissionPolicy.from_raw(None, presence_requirement=presence)
        ttl_ms = command_ttl_ms(policy)

        if presentation.get("motion") in ("look_at_user", "find_owner", "locate_owner"):
            commands.append(
                CommandEnvelope(
                    cmd_id=new_id("cmd"),
                    device_id=device_id,
                    delivery_id=delivery_id,
                    type="find_owner",
                    priority=70,
                    ttl_ms=min(ttl_ms, 8_000),
                    coalesce_key=f"{delivery_id}:find_owner",
                    admission=admission,
                    payload={"rounds": 3, "reply": "", "speak": False},
                )
            )

        emotion = str(presentation.get("emotion") or presentation.get("expression") or "").strip()
        if emotion:
            commands.append(
                CommandEnvelope(
                    cmd_id=new_id("cmd"),
                    device_id=device_id,
                    delivery_id=delivery_id,
                    type="face",
                    priority=65,
                    ttl_ms=min(ttl_ms, 8_000),
                    coalesce_key=f"{delivery_id}:face",
                    admission=admission,
                    payload={"expression": emotion},
                )
            )

        speech = str(response.get("speech") or "").strip()
        if speech:
            commands.append(
                CommandEnvelope(
                    cmd_id=new_id("cmd"),
                    device_id=device_id,
                    delivery_id=delivery_id,
                    type="speak",
                    priority=50,
                    ttl_ms=ttl_ms,
                    coalesce_key=f"{delivery_id}:speech",
                    admission=admission,
                    payload={"text": speech},
                )
            )

        for raw in response.get("actions") if isinstance(response.get("actions"), list) else []:
            if not isinstance(raw, dict):
                continue
            action_type = str(raw.get("type") or "").strip()
            if not action_type:
                continue
            if action_type == "sequence" and isinstance(raw.get("steps"), list):
                payload = raw["steps"]
            elif isinstance(raw.get("payload"), dict):
                payload = raw["payload"]
            else:
                payload = {key: value for key, value in raw.items() if key != "type"}
            commands.append(
                CommandEnvelope(
                    cmd_id=new_id("cmd"),
                    device_id=device_id,
                    delivery_id=delivery_id,
                    type=action_type,
                    priority=int(raw.get("priority") or 40),
                    ttl_ms=ttl_ms,
                    coalesce_key=f"{delivery_id}:{action_type}",
                    admission=admission,
                    payload=payload,
                )
            )

        if not commands:
            commands.append(
                CommandEnvelope(
                    cmd_id=new_id("cmd"),
                    device_id=device_id,
                    delivery_id=delivery_id,
                    type="state",
                    priority=10,
                    ttl_ms=5_000,
                    coalesce_key=f"{delivery_id}:noop",
                    admission=admission,
                    payload={"state": "waiting", "reason": "empty_delivery"},
                )
            )
        return commands
