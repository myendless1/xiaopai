import sys
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))

import server  # noqa: E402


class ImageFormatTest(unittest.TestCase):
    def test_yuv422_neutral_luma_converts_to_rgb_rows(self):
        yuv422 = bytes([16, 128, 235, 128])

        rows = server.yuv422_to_rgb_rows(yuv422, 2, 1)

        self.assertEqual(rows, [bytes([0, 0, 0, 255, 255, 255])])

    def test_yuv422_png_has_png_signature(self):
        yuv422 = bytes([16, 128, 235, 128])

        png = server.yuv422_to_png(yuv422, 2, 1)

        self.assertTrue(png.startswith(b"\x89PNG\r\n\x1a\n"))


if __name__ == "__main__":
    unittest.main()
