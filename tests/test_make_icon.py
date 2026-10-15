"""Unit tests for scripts/make_icon.py: python3 tests/test_make_icon.py"""
from __future__ import annotations

import struct
import sys
import tempfile
import unittest
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))

import make_icon  # noqa: E402

ICON = Path(__file__).resolve().parents[1] / "assets" / "icon" / "sc2hd.png"


def _png(width: int, height: int) -> bytes:
    """An opaque RGB PNG (colour type 2) with a red left half and a blue right half."""
    def chunk(kind: bytes, body: bytes) -> bytes:
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))

    row = b"".join((b"\xff\x00\x00" if x < width // 2 else b"\x00\x00\xff") for x in range(width))
    raw = b"".join(b"\x00" + row for _ in range(height))
    return (make_icon.PNG_MAGIC + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


class ReadPngTest(unittest.TestCase):
    def test_reads_rgb_as_opaque_rgba(self):
        w, h, rgba = make_icon.read_png(_png(8, 4))
        self.assertEqual((w, h, len(rgba)), (8, 4, 8 * 4 * 4))
        self.assertEqual(rgba[:4], b"\xff\x00\x00\xff")
        self.assertEqual(rgba[-4:], b"\x00\x00\xff\xff")

    def test_rejects_other_files(self):
        with self.assertRaises(ValueError):
            make_icon.read_png(b"GIF89a")

    def test_reads_the_shipped_icon(self):
        w, h, rgba = make_icon.read_png(ICON.read_bytes())
        self.assertEqual(len(rgba), w * h * 4)
        self.assertEqual(rgba[3], 0)   # a corner of the rounded square is transparent


class FitSquareTest(unittest.TestCase):
    def test_wide_picture_is_centred_on_transparency(self):
        w, h, rgba = make_icon.read_png(_png(32, 16))
        out = make_icon.fit_square(w, h, rgba, 16)
        self.assertEqual(len(out), 16 * 16 * 4)
        self.assertEqual(out[3], 0)                       # top row: padding
        centre = (8 * 16 + 2) * 4
        self.assertEqual(out[centre + 3], 255)            # the picture
        self.assertEqual(out[centre:centre + 3], b"\xff\x00\x00")


class OutputTest(unittest.TestCase):
    def test_writes_an_ico_and_a_header(self):
        with tempfile.TemporaryDirectory() as td:
            self.assertEqual(make_icon.main(["make_icon.py", str(ICON), td]), 0)
            ico = (Path(td) / "sc2hd.ico").read_bytes()
            header = (Path(td) / "sc2hd_icon.h").read_text()
        reserved, kind, count = struct.unpack("<HHH", ico[:6])
        self.assertEqual((reserved, kind, count), (0, 1, len(make_icon.ICO_SIZES)))
        for i, size in enumerate(make_icon.ICO_SIZES):
            width, _, _, _, _, bpp, length, offset = struct.unpack("<BBBBHHII", ico[6 + 16 * i:22 + 16 * i])
            self.assertEqual(width, size % 256)
            self.assertEqual(bpp, 32)
            self.assertEqual(ico[offset:offset + 8], make_icon.PNG_MAGIC)
            self.assertLessEqual(offset + length, len(ico))
        self.assertIn(f"#define SC2HD_ICON_SIZE {make_icon.RUNTIME_SIZE}", header)
        values = header.split("= {")[1].split("};")[0].replace("\n", "").strip().rstrip(",").split(",")
        self.assertEqual(len(values), make_icon.RUNTIME_SIZE ** 2 * 4)


if __name__ == "__main__":
    unittest.main()
