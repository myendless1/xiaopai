import json
import queue
import sys
import unittest
from pathlib import Path
from types import SimpleNamespace


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))

from morrow_protocol import parse_message  # noqa: E402
from morrow_web import (  # noqa: E402
    MorrowWebError,
    MorrowWebGateway,
    clean_assistant_text,
    latest_turn_error,
    validate_session_id,
)


class FakeResponse:
    def __init__(self, payload):
        self.payload = json.dumps(payload, ensure_ascii=False).encode("utf-8")

    def __enter__(self):
        return self

    def __exit__(self, *_args):
        return False

    def read(self):
        return self.payload


class FakeClient:
    instances = []

    def __init__(self, **kwargs):
        self.kwargs = kwargs
        self.events = queue.Queue()
        self.last_error = ""
        self.sent = []
        self.stopped = False
        FakeClient.instances.append(self)

    def wait_ready(self, _timeout):
        self.events.put(parse_message({"type": "snapshot", "data": {"session": {}}}))
        return True

    def start_turn(self, request_id, prompt):
        self.sent.append((request_id, prompt))
        self.events.put(
            parse_message(
                {"type": "agent_event", "data": {"event": {"type": "text_delta", "data": "<happy>你好"}}}
            )
        )
        self.events.put(
            parse_message(
                {
                    "type": "agent_event",
                    "data": {"event": {"type": "agent_message", "data": "<happy>你好，网页。"}},
                }
            )
        )
        self.events.put(parse_message({"type": "turn_saved", "data": {"session": self.kwargs["session"]}}))

    def cancel_turn(self):
        return False

    def stop(self):
        self.stopped = True


class EmptySavedClient(FakeClient):
    def start_turn(self, request_id, prompt):
        self.sent.append((request_id, prompt))
        self.events.put(parse_message({"type": "turn_saved", "data": {"session": self.kwargs["session"]}}))


class MorrowWebGatewayTest(unittest.TestCase):
    def setUp(self):
        FakeClient.instances.clear()
        self.requests = []

        def urlopen(request, timeout):
            self.requests.append((request.get_method(), request.full_url, timeout))
            if request.full_url.endswith("/api/status"):
                return FakeResponse({"config_path": "/tmp/morrow.toml", "version": "test"})
            return FakeResponse({"active_thread": {"messages": []}})

        self.gateway = MorrowWebGateway(
            base_url="http://127.0.0.1:3000",
            default_session="default",
            client_factory=FakeClient,
            urlopen=urlopen,
        )

    def test_status_and_session_history_use_private_morrow_http(self):
        status = self.gateway.status()
        self.assertTrue(status["connected"])
        self.assertEqual([mode["label"] for mode in status["modes"]], ["通用问答", "飞书办公助手"])
        history = self.gateway.get_session("web-one")
        self.assertEqual(history, {"active_thread": {"messages": []}})
        self.assertEqual(self.requests[1][1], "http://127.0.0.1:3000/api/sessions/web-one")

    def test_new_chat_creates_a_distinct_morrow_session(self):
        first = self.gateway.create_session()["session_id"]
        second = self.gateway.create_session()["session_id"]
        self.assertNotEqual(first, second)
        self.assertTrue(first.startswith("web-"))
        self.assertIn(f"/api/sessions/{first}", self.requests[0][1])

    def test_send_message_waits_for_final_answer_and_stops_connection(self):
        result = self.gateway.send_message("web-one", "你好")
        self.assertEqual(result["session_id"], "web-one")
        self.assertEqual(result["message"], "你好，网页。")
        self.assertEqual(FakeClient.instances[0].sent[0][1], "你好")
        self.assertTrue(FakeClient.instances[0].stopped)

    def test_shared_gateway_uses_one_session_for_web_turns_and_new_chat_reset(self):
        submitted = []
        resets = []

        def submit(prompt):
            submitted.append(prompt)
            return SimpleNamespace(
                request_id="shared-web-1",
                state="saved",
                message="",
                response_text="<happy>统一回复",
                finished=SimpleNamespace(wait=lambda _timeout: True),
            )

        gateway = MorrowWebGateway(
            base_url="http://127.0.0.1:3000",
            default_session="shared-current",
            client_factory=FakeClient,
            urlopen=self.gateway.urlopen,
            shared_turn_submitter=submit,
            shared_session_resetter=lambda: resets.append(True) or SimpleNamespace(success=True, message=""),
        )

        result = gateway.send_message("obsolete-web-session", "网页问题")
        self.assertEqual(result["session_id"], "shared-current")
        self.assertEqual(result["message"], "统一回复")
        self.assertEqual(submitted, ["网页问题"])
        fresh = gateway.create_session()
        self.assertEqual(fresh["session_id"], "shared-current")
        self.assertEqual(resets, [True])
        self.assertEqual(gateway.status()["session_revision"], 1)

    def test_validation_and_expression_cleanup(self):
        self.assertEqual(validate_session_id("web-1:chat"), "web-1:chat")
        self.assertEqual(clean_assistant_text("<thinking>正在处理"), "正在处理")
        self.assertEqual(clean_assistant_text("<surprised>网页上也不显示"), "网页上也不显示")
        self.assertEqual(clean_assistant_text("<nod>好的</nod>"), "好的")
        self.assertEqual(clean_assistant_text("<shy>兼容旧标签"), "兼容旧标签")
        self.assertEqual(
            clean_assistant_text(
                "<nod></think_never_used_51bce0c785ca2f68081bfa7d91973934>已经完成"
            ),
            "已经完成",
        )
        with self.assertRaises(MorrowWebError):
            validate_session_id("../escape")
        with self.assertRaises(MorrowWebError):
            self.gateway.send_message("web-one", "  ")

    def test_failed_saved_turn_error_is_read_from_real_session_shape(self):
        payload = {
            "schema_version": 3,
            "session": {
                "turns": [
                    {"turn": {"status": "failed", "error": "model provider returned HTTP 429"}, "messages": []}
                ]
            },
        }
        self.assertEqual(latest_turn_error(payload), "model provider returned HTTP 429")

    def test_empty_saved_turn_surfaces_upstream_failure(self):
        def failed_history(_request, timeout):
            return FakeResponse(
                {"session": {"turns": [{"turn": {"status": "failed", "error": "provider unavailable"}}]}}
            )

        gateway = MorrowWebGateway(
            base_url="http://127.0.0.1:3000",
            client_factory=EmptySavedClient,
            urlopen=failed_history,
        )
        with self.assertRaisesRegex(MorrowWebError, "provider unavailable") as raised:
            gateway.send_message("web-one", "你好")
        self.assertEqual(raised.exception.status, 502)

    def test_switch_mode_runs_allowlisted_script_and_returns_fresh_session(self):
        commands = []
        switched_sessions = []

        def run_command(command, **kwargs):
            commands.append((command, kwargs))
            return SimpleNamespace(returncode=0, stdout="started", stderr="")

        def urlopen(request, timeout):
            if request.full_url.endswith("/api/status"):
                return FakeResponse({"config_path": "/workspace/morrow/config-full.toml"})
            return FakeResponse({"session": {"active_thread": {"messages": []}}})

        gateway = MorrowWebGateway(
            base_url="http://127.0.0.1:3000",
            client_factory=FakeClient,
            urlopen=urlopen,
            start_script="/bin/true",
            run_command=run_command,
            device_session_switcher=switched_sessions.append,
        )
        result = gateway.switch_mode("lark")

        self.assertEqual(commands[0][0], ["/bin/true", "lark"])
        self.assertEqual(result["mode"]["label"], "飞书办公助手")
        self.assertTrue(result["session_id"].startswith("shared-"))
        self.assertEqual(result["xiaopai_session_id"], result["session_id"])
        self.assertEqual(switched_sessions, [result["xiaopai_session_id"]])
        self.assertEqual(gateway.default_session, result["xiaopai_session_id"])

    def test_switch_mode_rejects_values_outside_allowlist(self):
        with self.assertRaises(MorrowWebError) as raised:
            self.gateway.switch_mode("demo; rm -rf /")
        self.assertEqual(raised.exception.status, 400)


if __name__ == "__main__":
    unittest.main()
