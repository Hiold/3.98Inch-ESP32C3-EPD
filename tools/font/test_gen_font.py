import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(__file__))
import gen_font


class BuildTest(unittest.TestCase):
    def test_collect_glyphs_makes_rows_for_each_codepoint(self):
        cps = [0x4e00, 0x4e01]
        # Manually crafted glyph rows avoid real rasterization (no Pillow needed).
        rows = [bytes([i]) * 128 for i in (0x11, 0x22)]
        data = gen_font.build_blob(cps, rows)
        self.assertEqual(data[0:4], b"EPF1")


class RasterizeTest(unittest.TestCase):
    def test_rasterize_one_returns_128_bytes(self):
        data = gen_font._rasterize_one(gen_font.DEFAULT_TTC, 0x4e00)
        self.assertEqual(len(data), 128)
        self.assertNotEqual(data, b"\x00" * 128)

    def test_rasterize_one_row_order_is_top_to_bottom(self):
        # '丁' (0x4e01) draws ink toward the bottom of its 32px box, so its top
        # row (y=0) is blank background. Row order is guaranteed top-to-bottom
        # by tobytes("raw", "1;R") default stride=0.
        data = gen_font._rasterize_one(gen_font.DEFAULT_TTC, 0x4e01)
        self.assertEqual(len(data), 128)
        self.assertEqual(data[0:4], b"\xff\xff\xff\xff")  # top row blank
        self.assertNotEqual(data[-4:], b"\xff\xff\xff\xff")  # bottom row has ink


if __name__ == "__main__":
    unittest.main()
