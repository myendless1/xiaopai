#!/usr/bin/env python3
import argparse
import json
import os
import time
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


DEFAULT_REPLY = "我在，刚刚听到你了。"


def _json_bytes(value: dict) -> bytes:
    return json.dumps(value, ensure_ascii=False, separators=(",", ":")).encode("utf-8")


class OpenClawStubHandler(BaseHTTPRequestHandler):
    server: "OpenClawStubServer"

    def log_message(self, fmt: str, *args) -> None:
        print(f"[{time.strftime('%Y-%m-%d %H:%M:%S')}] {self.address_string()} - {fmt % args}", flush=True)

    def _send_json(self, status: int, body: dict) -> None:
        data = _json_bytes(body)
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def _authorized(self) -> bool:
        token = self.server.token
        if not token:
            return True
        auth = self.headers.get("Authorization", "")
        return auth == f"Bearer {token}"

    def do_GET(self) -> None:
        if self.path in ("/health", "/v1/health"):
            self._send_json(HTTPStatus.OK, {"ok": True, "service": "openclaw-stub"})
            return
        self._send_json(HTTPStatus.NOT_FOUND, {"error": {"message": "Not Found", "type": "not_found"}})

    def do_POST(self) -> None:
        if self.path.rstrip("/") != "/v1/chat/completions":
            self._send_json(HTTPStatus.NOT_FOUND, {"error": {"message": "Not Found", "type": "not_found"}})
            return
        if not self._authorized():
            self._send_json(HTTPStatus.UNAUTHORIZED, {"error": {"message": "Unauthorized", "type": "unauthorized"}})
            return

        length = int(self.headers.get("Content-Length") or "0")
        raw = self.rfile.read(length) if length else b"{}"
        try:
            request = json.loads(raw.decode("utf-8"))
        except json.JSONDecodeError:
            request = {}

        model = str(request.get("model") or "openclaw/default")
        body = {
            "id": f"chatcmpl_stub_{int(time.time() * 1000)}",
            "object": "chat.completion",
            "created": int(time.time()),
            "model": model,
            "choices": [
                {
                    "index": 0,
                    "message": {"role": "assistant", "content": self.server.reply},
                    "finish_reason": "stop",
                }
            ],
            "usage": {"prompt_tokens": 0, "completion_tokens": 0, "total_tokens": 0},
        }
        self._send_json(HTTPStatus.OK, body)


class OpenClawStubServer(ThreadingHTTPServer):
    def __init__(self, server_address, handler, *, reply: str, token: str):
        super().__init__(server_address, handler)
        self.reply = reply
        self.token = token


def main() -> None:
    parser = argparse.ArgumentParser(description="Minimal OpenClaw-compatible stub for Xiaopai local testing.")
    parser.add_argument("--host", default=os.environ.get("OPENCLAW_STUB_HOST", "127.0.0.1"))
    parser.add_argument("--port", type=int, default=int(os.environ.get("OPENCLAW_STUB_PORT", "18789")))
    parser.add_argument("--reply", default=os.environ.get("OPENCLAW_STUB_REPLY", DEFAULT_REPLY))
    parser.add_argument("--token", default=os.environ.get("OPENCLAW_GATEWAY_TOKEN", ""))
    args = parser.parse_args()

    server = OpenClawStubServer(
        (args.host, args.port),
        OpenClawStubHandler,
        reply=args.reply,
        token=args.token,
    )
    print(f"OpenClaw stub listening on http://{args.host}:{args.port}/v1/chat/completions", flush=True)
    server.serve_forever()


if __name__ == "__main__":
    main()
