import io
import sys
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))

import server  # noqa: E402


class FakeTtsResponse:
    def __init__(self, chunks, on_read=None):
        self.chunks = list(chunks)
        self.headers = {"Content-Type": "application/octet-stream"}
        self.closed = False
        self.on_read = on_read

    def read(self, _size=-1):
        if self.on_read is not None:
            self.on_read()
        if not self.chunks:
            return b""
        return self.chunks.pop(0)

    def close(self):
        self.closed = True


class TtsStreamingTest(unittest.TestCase):
    def make_handler(self):
        class FakeServer:
            voice = "zhimiao_emo"
            sample_rate = 16000
            volume = 80
            speech_rate = 0
            pitch_rate = 0
            max_sentence_chars = 120
            chunk_size = 4
            tts_prefetch_workers = 1
            tts_request_timeout = 1
            tts_retries = 0
            tts_tail_silence_ms = 0
            debug_log = False

        handler = object.__new__(server.Handler)
        handler.server = FakeServer()
        handler.headers = {}
        handler.wfile = io.BytesIO()
        handler.responses = []
        handler.response_headers = []
        handler.headers_ended = False
        handler.close_connection = False
        handler._log_info = lambda _msg: None
        handler._log_debug = lambda _msg: None
        handler._log_error = lambda _msg: None
        handler._send_json = lambda body, status=server.HTTPStatus.OK: handler.responses.append((body, status))
        handler.send_response = lambda status: handler.responses.append(("response", status))
        handler.send_header = lambda key, value: handler.response_headers.append((key, value))
        handler.end_headers = lambda: setattr(handler, "headers_ended", True)
        handler.send_error = lambda status, message="": handler.responses.append(("error", status, message))
        return handler

    def test_stream_speak_writes_pcm_without_content_length(self):
        handler = self.make_handler()

        def assert_headers_started():
            self.assertTrue(handler.headers_ended)

        first_response = FakeTtsResponse([b"aa", b"bb"], on_read=assert_headers_started)
        handler._open_aliyun_tts_stream_with_retries = lambda text, options: first_response
        handler._aliyun_tts_pcm_with_retries = lambda text, options: b"cc"

        handler._handle_stream_speak({"text": ["第一句。第二句。"]})

        self.assertIn(("response", server.HTTPStatus.OK), handler.responses)
        self.assertEqual(handler.wfile.getvalue(), b"aabbcc")
        self.assertNotIn("Content-Length", {key for key, _value in handler.response_headers})
        self.assertIn(("Connection", "close"), handler.response_headers)
        self.assertTrue(first_response.closed)


if __name__ == "__main__":
    unittest.main()
