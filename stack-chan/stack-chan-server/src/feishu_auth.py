"""Web adapter for the existing lark-cli device-flow authentication."""
from __future__ import annotations
import json
import subprocess
import threading
from typing import Any
SESSION_COOKIE = "xiaopai_feishu_session"
class FeishuOAuth:
    def __init__(self, *, cli_path: str = "lark-cli", **_kwargs) -> None:
        self.cli_path = cli_path
        self._lock = threading.Lock()
        self._login_process: subprocess.Popen[str] | None = None
        self._verification_url = ""
        self._device_code = ""
    @property
    def configured(self) -> bool:
        return bool(self.cli_path)
    def _run_json(self, args: list[str], timeout: float = 15) -> dict[str, Any]:
        try:
            result = subprocess.run([self.cli_path, *args], capture_output=True, text=True, timeout=timeout, check=False)
            return json.loads(result.stdout or "{}")
        except (OSError, subprocess.TimeoutExpired, json.JSONDecodeError):
            return {}
    def _poll_login(self, device_code: str) -> None:
        try:
            subprocess.run([self.cli_path, "auth", "login", "--device-code", device_code, "--json"], capture_output=True, text=True, timeout=660, check=False)
        except (OSError, subprocess.TimeoutExpired):
            pass
        finally:
            with self._lock:
                self._login_process = None
                self._device_code = ""
    def authorization_url(self) -> str:
        with self._lock:
            if self._login_process is not None and self._verification_url:
                return self._verification_url
        payload = self._run_json(["auth", "login", "--recommend", "--no-wait", "--json"])
        url = str(payload.get("verification_url") or "")
        device_code = str(payload.get("device_code") or "")
        if not url or not device_code:
            return ""
        process = threading.Thread(target=self._poll_login, args=(device_code,), daemon=True)
        with self._lock:
            self._verification_url = url
            self._device_code = device_code
            self._login_process = process
        process.start()
        return url
    def status(self, _session_id: str = "") -> dict[str, Any]:
        payload = self._run_json(["auth", "status", "--json"])
        user = payload.get("identities", {}).get("user", {}) if isinstance(payload.get("identities"), dict) else {}
        ready = str(user.get("status") or "").lower() == "ready" and bool(user.get("userName"))
        with self._lock:
            login_url = "/web/api/feishu/login" if not ready else ""
        return {"configured": True, "authenticated": ready, "user": ({"name": user.get("userName"), "open_id": user.get("openId")} if ready else None), "login_url": login_url}
    def logout(self, _session_id: str = "") -> None:
        self._run_json(["auth", "logout", "--json"])
