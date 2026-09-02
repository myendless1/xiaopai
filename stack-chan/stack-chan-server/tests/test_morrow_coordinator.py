import queue
import sys
import threading
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))

from morrow_coordinator import (  # noqa: E402
    MorrowTurnCoordinator,
    StreamingExpressionTagParser,
    StreamingTextSegmenter,
    parse_expression_tags,
)
from morrow_protocol import parse_message  # noqa: E402


class FakeClient:
    def __init__(self):
        self.events = queue.Queue()
        self.started = []
        self.cancelled = 0
        self.resets = []
        self.ready = threading.Event()
        self.ready.set()

    def start(self):
        pass

    def wait_ready(self, timeout=None):
        return self.ready.wait(timeout)

    def start_turn(self, request_id, prompt):
        self.started.append((request_id, prompt))

    def cancel_turn(self):
        self.cancelled += 1
        return True

    def reset_session(self, request_id):
        self.resets.append(request_id)


def event(message):
    return parse_message(message)


class SegmenterTest(unittest.TestCase):
    def test_hard_punctuation_and_utf8_byte_limit(self):
        segmenter = StreamingTextSegmenter(max_chars=120, max_bytes=9)
        self.assertEqual(segmenter.feed("你好。世界"), ["你好。"])
        self.assertEqual(segmenter.flush(), ["世界"])

    def test_comma_does_not_split(self):
        segmenter = StreamingTextSegmenter()
        self.assertEqual(segmenter.feed("你好，世界"), [])
        self.assertEqual(segmenter.flush(), ["你好，世界"])


class ExpressionTagParserTest(unittest.TestCase):
    def test_valid_unknown_multiple_and_case_insensitive_tags(self):
        cases = (
            ("<happy>你好", ("你好", "happy")),
            ("你好", ("你好", "calm")),
            ("<sad>你好", ("你好", "calm")),
            ("<surprised>你好", ("你好", "calm")),
            ("<foo><HAPPY>你好", ("你好", "happy")),
            ("<thinking><happy>你好", ("你好", "thinking")),
            ("<nod>好的", ("好的", "nod")),
            ("<shake>不行", ("不行", "shake")),
            ("<>你好", ("你好", "calm")),
        )
        for source, expected in cases:
            with self.subTest(source=source):
                self.assertEqual(parse_expression_tags(source), expected)

    def test_tag_can_span_deltas_and_mid_reply_tag_only_gets_removed(self):
        parser = StreamingExpressionTagParser()
        self.assertEqual(parser.feed("<hap"), "")
        self.assertEqual(parser.feed("py>你好<happy>。"), "你好。")
        self.assertEqual(parser.expression, "happy")

    def test_unclosed_tag_never_reaches_speech(self):
        parser = StreamingExpressionTagParser()
        self.assertEqual(parser.feed("<thinking"), "")
        self.assertEqual(parser.flush(), "")
        self.assertEqual(parser.expression, "calm")


class CoordinatorTest(unittest.TestCase):
    def setUp(self):
        self.client = FakeClient()
        self.spoken = []
        self.ended = []
        self.cancelled_generations = []
        self.coordinator = MorrowTurnCoordinator(
            self.client,
            lambda request, text, index: self.spoken.append(
                (request.request_id, text, index, request.generation, request.expression)
            ),
            reply_end_sink=lambda request, index, state: self.ended.append(
                (request.request_id, index, state, request.expression)
            ),
            cancel_sink=lambda device_id, generation: self.cancelled_generations.append((device_id, generation)),
            initial_generations={"restored-device": 12},
            turn_timeout=1,
        )
        self.coordinator.start()

    def tearDown(self):
        self.coordinator.stop()

    def submit(self, request_id, prompt="问题"):
        return self.coordinator.submit(prompt, "dev1", request_id=request_id)

    def test_fifo_allows_only_one_turn_before_saved(self):
        first = self.submit("req-1")
        second = self.submit("req-2")
        self.assertTrue(self.coordinator.has_pending_turn("dev1"))
        self.assertTrue(self._wait(lambda: self.client.started == [("req-1", "问题")]))
        self.client.events.put(event({"type": "turn_saved", "data": {"session": "default"}}))
        self.assertTrue(first.finished.wait(0.3))
        self.assertTrue(self._wait(lambda: len(self.client.started) == 2))
        self.assertEqual(self.client.started[1], ("req-2", "问题"))
        self.client.events.put(event({"type": "turn_saved", "data": {}}))
        self.assertTrue(second.finished.wait(0.3))
        self.assertTrue(self._wait(lambda: not self.coordinator.has_pending_turn("dev1")))

    def test_restores_initial_device_generation(self):
        self.assertEqual(self.coordinator.generation_for_device("restored-device"), 12)

    def test_device_heartbeat_generation_resync_is_monotonic(self):
        self.assertEqual(self.coordinator.sync_device_generation("dev1", 30), 30)
        self.assertEqual(self.coordinator.sync_device_generation("dev1", 29), 30)
        self.assertEqual(self.coordinator.generation_for_device("dev1"), 30)
        self.assertEqual(self.cancelled_generations, [("dev1", 30)])
        outcome = self.submit("req-after-resync")
        self.assertTrue(self._wait(lambda: len(self.client.started) == 1))
        self.client.events.put(event({"type": "turn_saved", "data": {}}))
        self.assertTrue(outcome.finished.wait(0.3))
        self.assertTrue(self._wait(lambda: self.ended == [("req-after-resync", 0, "saved", "calm")]))

    def test_delta_streams_and_final_is_not_duplicated(self):
        outcome = self.submit("req-1")
        self.assertTrue(self._wait(lambda: len(self.client.started) == 1))
        self.client.events.put(event({"type": "agent_event", "data": {"event": {"type": "text_delta", "data": "你好。"}}}))
        self.client.events.put(event({"type": "agent_event", "data": {"event": {"type": "agent_message", "data": "你好。"}}}))
        self.client.events.put(event({"type": "turn_saved", "data": {}}))
        self.assertTrue(outcome.finished.wait(0.3))
        self.assertEqual([item[1] for item in self.spoken], ["你好。"])
        self.assertTrue(self._wait(lambda: self.ended == [("req-1", 1, "saved", "calm")]))

    def test_expression_tag_spans_deltas_and_is_shared_by_all_segments(self):
        outcome = self.submit("req-1")
        self.assertTrue(self._wait(lambda: len(self.client.started) == 1))
        for delta in ("<thi", "nking>第一句。", "第二句。<happy>"):
            self.client.events.put(
                event({"type": "agent_event", "data": {"event": {"type": "text_delta", "data": delta}}})
            )
        self.client.events.put(event({"type": "turn_saved", "data": {}}))
        self.assertTrue(outcome.finished.wait(0.3))
        self.assertEqual([item[1] for item in self.spoken], ["第一句。", "第二句。"])
        self.assertEqual([item[4] for item in self.spoken], ["thinking", "thinking"])
        self.assertTrue(self._wait(lambda: self.ended == [("req-1", 2, "saved", "thinking")]))

    def test_final_is_used_when_no_delta(self):
        outcome = self.submit("req-1")
        self.assertTrue(self._wait(lambda: len(self.client.started) == 1))
        self.client.events.put(event({"type": "agent_event", "data": {"event": {"type": "agent_message", "data": "最终回复"}}}))
        self.client.events.put(event({"type": "turn_saved", "data": {}}))
        self.assertTrue(outcome.finished.wait(0.3))
        self.assertEqual([item[1] for item in self.spoken], ["最终回复"])

    def test_web_turn_uses_shared_queue_without_sending_device_speech(self):
        outcome = self.coordinator.submit("网页问题", "__web__", source="web", request_id="web-1")
        self.assertTrue(self._wait(lambda: self.client.started == [("web-1", "网页问题")]))
        self.client.events.put(
            event({"type": "agent_event", "data": {"event": {"type": "text_delta", "data": "共享回复。"}}})
        )
        self.client.events.put(event({"type": "turn_saved", "data": {}}))
        self.assertTrue(outcome.finished.wait(0.3))
        self.assertEqual(outcome.response_text, "共享回复。")
        self.assertEqual(self.spoken, [])
        self.assertEqual(self.ended, [])

    def test_saved_empty_reply_still_emits_one_end_control(self):
        outcome = self.submit("req-empty")
        self.assertTrue(self._wait(lambda: len(self.client.started) == 1))
        self.client.events.put(event({"type": "turn_saved", "data": {}}))
        self.assertTrue(outcome.finished.wait(0.3))
        self.assertTrue(self._wait(lambda: self.ended == [("req-empty", 0, "saved", "calm")]))
        self.assertEqual(self.spoken, [])

    def test_error_before_first_delta_emits_cancel_end_control(self):
        outcome = self.submit("req-error")
        self.assertTrue(self._wait(lambda: len(self.client.started) == 1))
        self.client.events.put(event({"type": "error", "data": {"message": "lost"}}))
        self.assertTrue(outcome.finished.wait(0.3))
        self.assertTrue(self._wait(lambda: self.ended == [("req-error", 0, "error", "calm")]))
        self.assertEqual(self.spoken, [])

    def test_stale_events_are_discarded_before_starting_next_turn(self):
        self.client.events.put(event({"type": "error", "data": {"message": "previous turn failed"}}))
        self.client.events.put(event({"type": "turn_saved", "data": {"session": "default"}}))
        self.client.events.put(
            event({"type": "agent_event", "data": {"event": {"type": "text_delta", "data": "旧回复。"}}})
        )

        outcome = self.submit("req-after-stale-events")
        self.assertTrue(self._wait(lambda: self.client.started == [("req-after-stale-events", "问题")]))
        self.client.events.put(
            event({"type": "agent_event", "data": {"event": {"type": "text_delta", "data": "新回复。"}}})
        )
        self.client.events.put(event({"type": "turn_saved", "data": {}}))

        self.assertTrue(outcome.finished.wait(0.3))
        self.assertEqual(outcome.state, "saved")
        self.assertEqual([item[1] for item in self.spoken], ["新回复。"])

    def test_stop_generation_discards_late_delta(self):
        outcome = self.submit("req-1")
        self.assertTrue(self._wait(lambda: len(self.client.started) == 1))
        self.assertEqual(self.coordinator.generation_for_device("dev1"), 0)
        self.assertEqual(self.coordinator.cancel_device("dev1"), 1)
        self.assertEqual(self.coordinator.generation_for_device("dev1"), 1)
        self.client.events.put(event({"type": "agent_event", "data": {"event": {"type": "text_delta", "data": "不应播放。"}}}))
        self.client.events.put(event({"type": "turn_saved", "data": {}}))
        self.assertTrue(outcome.finished.wait(0.3))
        self.assertEqual(self.spoken, [])
        self.assertEqual(self.client.cancelled, 1)
        self.assertTrue(self._wait(lambda: not self.coordinator.has_pending_turn("dev1")))
        self.assertEqual(self.ended, [])

    def test_reset_session_cancels_dialogue_and_waits_for_fresh_snapshot(self):
        result_holder = []

        thread = threading.Thread(
            target=lambda: result_holder.append(self.coordinator.reset_session("dev1", timeout=0.5))
        )
        thread.start()
        self.assertTrue(self._wait(lambda: len(self.client.resets) == 1))
        self.client.events.put(event({"type": "snapshot", "data": {"session": {"active_thread": {"messages": []}}}}))
        thread.join(timeout=0.5)

        self.assertFalse(thread.is_alive())
        self.assertEqual(len(result_holder), 1)
        self.assertTrue(result_holder[0].success)
        self.assertEqual(result_holder[0].generation, 1)
        self.assertEqual(self.coordinator.generation_for_device("dev1"), 1)

    def test_reset_session_cancels_active_turn_before_reset(self):
        outcome = self.submit("req-active")
        self.assertTrue(self._wait(lambda: len(self.client.started) == 1))
        result_holder = []
        thread = threading.Thread(
            target=lambda: result_holder.append(self.coordinator.reset_session("dev1", timeout=0.5))
        )
        thread.start()

        self.assertTrue(outcome.finished.wait(0.3))
        self.assertEqual(outcome.state, "cancelled")
        self.assertEqual(self.client.cancelled, 1)
        self.assertTrue(self._wait(lambda: len(self.client.resets) == 1))
        self.client.events.put(event({"type": "snapshot", "data": {"session": {"active_thread": {"messages": []}}}}))
        thread.join(timeout=0.5)
        self.assertTrue(result_holder[0].success)

    def test_shared_reset_advances_every_device_generation(self):
        result_holder = []
        thread = threading.Thread(
            target=lambda: result_holder.append(
                self.coordinator.reset_shared_session({"dev1", "dev2"}, timeout=0.5)
            )
        )
        thread.start()
        self.assertTrue(self._wait(lambda: len(self.client.resets) == 1))
        self.client.events.put(event({"type": "snapshot", "data": {"session": {}}}))
        thread.join(timeout=0.5)
        self.assertTrue(result_holder[0].success)
        self.assertEqual(self.coordinator.generation_for_device("dev1"), 1)
        self.assertEqual(self.coordinator.generation_for_device("dev2"), 1)
        self.assertIn(("dev1", 1), self.cancelled_generations)
        self.assertIn(("dev2", 1), self.cancelled_generations)

    def test_disconnect_ends_turn_without_flushing_incomplete_tail(self):
        outcome = self.submit("req-1")
        self.assertTrue(self._wait(lambda: len(self.client.started) == 1))
        self.client.events.put(event({"type": "agent_event", "data": {"event": {"type": "text_delta", "data": "完整句。未完成"}}}))
        self.client.events.put(type("Disconnected", (), {"type": "disconnected", "data": {"message": "lost"}})())
        self.assertTrue(outcome.finished.wait(0.3))
        self.assertEqual(outcome.state, "disconnected")
        self.assertEqual([item[1] for item in self.spoken], ["完整句。"])
        self.assertTrue(self._wait(lambda: self.ended == [("req-1", 1, "disconnected", "calm")]))

    @staticmethod
    def _wait(predicate, timeout=0.3):
        done = threading.Event()
        for _ in range(60):
            if predicate():
                return True
            done.wait(timeout / 60)
        return predicate()


if __name__ == "__main__":
    unittest.main()
