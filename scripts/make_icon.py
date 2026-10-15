#!/usr/bin/env python3
"""Turns a PNG into what the host build embeds as its icon (standard library only).

    make_icon.py <icon.png> <out dir>

Writes <out dir>/sc2hd.ico (Windows resource: 256, 48, 32 and 16 px PNG entries) and <out dir>/sc2hd_icon.h (a 128 px
RGBA bitmap the SDL window icon is made from on macOS and Linux). A picture that is not square is fitted into the
square and centred on transparency. Reads 8-bit grey, RGB, palette and RGBA PNGs without interlacing.
"""
from __future__ import annotations

import struct
import sys
import zlib
from pathlib import Path

PNG_MAGIC = b"\x89PNG\r\n\x1a\n"
ICO_SIZES = (256, 48, 32, 16)
RUNTIME_SIZE = 128


def read_png(data: bytes) -> tuple[int, int, bytes]:
    """(width, height, RGBA bytes) of a PNG."""
    if not data.startswith(PNG_MAGIC):
        raise ValueError("not a PNG file")
    pos, chunks, idat = 8, {}, []
    while pos < len(data):
        length, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + length]
        if kind == b"IDAT":
            idat.append(body)
        else:
            chunks.setdefault(kind, body)
        pos += 12 + length
    width, height, depth, ctype, _, _, interlace = struct.unpack(">IIBBBBB", chunks[b"IHDR"])
    if depth != 8 or interlace or ctype not in (0, 2, 3, 4, 6):
        raise ValueError(f"unsupported PNG (bit depth {depth}, colour type {ctype}, interlace {interlace})")
    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[ctype]
    stride = width * channels
    raw = zlib.decompress(b"".join(idat))
    rows, prev = [], bytearray(stride)
    for y in range(height):
        ftype, line = raw[y * (stride + 1)], bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - channels] if i >= channels else 0
            b, c = prev[i], (prev[i - channels] if i >= channels else 0)
            if ftype == 1:
                line[i] = (line[i] + a) & 255
            elif ftype == 2:
                line[i] = (line[i] + b) & 255
            elif ftype == 3:
                line[i] = (line[i] + ((a + b) >> 1)) & 255
            elif ftype == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
            elif ftype != 0:
                raise ValueError(f"bad PNG filter {ftype}")
        rows.append(line)
        prev = line
    palette = chunks.get(b"PLTE", b"")
    alpha = chunks.get(b"tRNS", b"")
    out = bytearray()
    for line in rows:
        for x in range(width):
            px = line[x * channels:(x + 1) * channels]
            if ctype == 0:
                out += bytes((px[0], px[0], px[0], 255))
            elif ctype == 2:
                out += bytes((*px, 255))
            elif ctype == 3:
                i = px[0]
                out += bytes((*palette[i * 3:i * 3 + 3], alpha[i] if i < len(alpha) else 255))
            elif ctype == 4:
                out += bytes((px[0], px[0], px[0], px[1]))
            else:
                out += bytes(px)
    return width, height, bytes(out)


def fit_square(width: int, height: int, rgba: bytes, size: int) -> bytes:
    """The picture scaled (bilinear, premultiplied) to fit size x size, centred on transparency."""
    scale = min(size / width, size / height)
    w, h = max(1, round(width * scale)), max(1, round(height * scale))
    x0, y0 = (size - w) // 2, (size - h) // 2
    out = bytearray(size * size * 4)
    for y in range(h):
        fy = min(max((y + 0.5) / scale - 0.5, 0), height - 1)
        iy, ty = int(fy), fy - int(fy)
        iy1 = min(iy + 1, height - 1)
        for x in range(w):
            fx = min(max((x + 0.5) / scale - 0.5, 0), width - 1)
            ix, tx = int(fx), fx - int(fx)
            ix1 = min(ix + 1, width - 1)
            acc = [0.0, 0.0, 0.0, 0.0]
            for (py, wy) in ((iy, 1 - ty), (iy1, ty)):
                for (px, wx) in ((ix, 1 - tx), (ix1, tx)):
                    o = (py * width + px) * 4
                    wt, a = wx * wy, rgba[o + 3]
                    acc[0] += rgba[o] * a * wt
                    acc[1] += rgba[o + 1] * a * wt
                    acc[2] += rgba[o + 2] * a * wt
                    acc[3] += a * wt
            o = ((y0 + y) * size + x0 + x) * 4
            if acc[3] > 0:
                out[o:o + 4] = bytes((round(acc[0] / acc[3]), round(acc[1] / acc[3]), round(acc[2] / acc[3]),
                                      round(acc[3])))
    return bytes(out)


def write_png(size: int, rgba: bytes) -> bytes:
    def chunk(kind: bytes, body: bytes) -> bytes:
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))

    rows = b"".join(b"\x00" + rgba[y * size * 4:(y + 1) * size * 4] for y in range(size))
    return (PNG_MAGIC + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(rows, 9)) + chunk(b"IEND", b""))


def write_ico(width: int, height: int, rgba: bytes) -> bytes:
    images = [write_png(s, fit_square(width, height, rgba, s)) for s in ICO_SIZES]
    out = struct.pack("<HHH", 0, 1, len(images))
    offset = 6 + 16 * len(images)
    for size, png in zip(ICO_SIZES, images):
        out += struct.pack("<BBBBHHII", size % 256, size % 256, 0, 0, 1, 32, len(png), offset)
        offset += len(png)
    return out + b"".join(images)


def write_header(width: int, height: int, rgba: bytes) -> str:
    pixels = fit_square(width, height, rgba, RUNTIME_SIZE)
    lines = []
    for i in range(0, len(pixels), 24):
        lines.append("    " + ",".join(str(b) for b in pixels[i:i + 24]) + ",")
    return ("/* Generated by scripts/make_icon.py: the window icon, RGBA, row by row. */\n"
            f"#define SC2HD_ICON_SIZE {RUNTIME_SIZE}\n"
            "static const unsigned char sc2hd_icon_rgba[SC2HD_ICON_SIZE * SC2HD_ICON_SIZE * 4] = {\n"
            + "\n".join(lines) + "\n};\n")


def main(argv: list[str]) -> int:
    if len(argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    src, out = Path(argv[1]), Path(argv[2])
    width, height, rgba = read_png(src.read_bytes())
    out.mkdir(parents=True, exist_ok=True)
    (out / "sc2hd.ico").write_bytes(write_ico(width, height, rgba))
    (out / "sc2hd_icon.h").write_text(write_header(width, height, rgba))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
