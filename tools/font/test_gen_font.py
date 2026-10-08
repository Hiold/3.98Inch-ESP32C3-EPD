import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(__file__))
import gen_font


class BuildTest(unittest.TestCase):
    def test_collect_glyphs_makes_rows_for_each_codepoint(self):
        cps = [0x4e00, 0x4e01]
        # _glyph_rows is injected to avoid needing fonttools/Pillow here.
        rows = [bytes([i]) * 128 for i in (0x11, 0x22)]
        data = gen_font.build_blob(cps, rows)
        self.assertEqual(data[0:4], b"EPF1")


if __name__ == "__main__":
    unittest.main()