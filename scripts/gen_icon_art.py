#!/usr/bin/env python3
"""Draws assets/icon/sc2hd.png, the host's neutral icon: an upright sword between an "S" and a roman "II". The artwork is original (no
game or publisher artwork, no logo lettering) and this script is how it was made; the PNG is what the build uses, so
Pillow is only needed to redraw it.

    python3 scripts/gen_icon_art.py [output.png]
"""
from __future__ import annotations

import sys
from pathlib import Path

from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont

OUT_SIZE = 512
SS = 4                         # supersampling factor
N = OUT_SIZE * SS
FONT_CANDIDATES = (
    "/System/Library/Fonts/Supplemental/Arial Black.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
    "C:/Windows/Fonts/ariblk.ttf",
)


def lerp(a, b, t):
    return tuple(round(x + (y - x) * t) for x, y in zip(a, b))


def background() -> Image.Image:
    top, bottom = (38, 30, 84), (8, 10, 28)
    col = Image.new("RGB", (1, N))
    col.putdata([lerp(top, bottom, y / (N - 1)) for y in range(N)])
    bg = col.resize((N, N))
    glow = Image.new("L", (N, N), 0)       # a soft light behind the blade
    ImageDraw.Draw(glow).ellipse((N * .15, N * .15, N * .85, N * .85), fill=90)
    glow = glow.filter(ImageFilter.GaussianBlur(N * .12))
    bg.paste(Image.new("RGB", (N, N), (70, 90, 190)), mask=glow)
    return bg


def blade_layer() -> Image.Image:
    """An upright sword, point up, in the middle of the icon."""
    s = N
    layer = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    cx = s / 2
    w = s * 0.05                                     # half width of the blade at the guard
    tip, guard, grip_end, pommel = s * 0.07, s * 0.60, s * 0.80, s * 0.88
    d.polygon([(cx - w, guard), (cx - w, s * 0.12), (cx, tip), (cx, guard)], fill=(222, 232, 246, 255))   # lit half
    d.polygon([(cx + w, guard), (cx + w, s * 0.12), (cx, tip), (cx, guard)], fill=(128, 146, 180, 255))   # shaded half
    d.line([(cx, tip + s * .04), (cx, guard - s * .02)], fill=(255, 255, 255, 255), width=round(s * .006))  # edge highlight
    gw, gh = s * 0.15, s * 0.04                      # cross guard
    d.rounded_rectangle((cx - gw, guard, cx + gw, guard + gh), radius=gh / 2, fill=(212, 170, 72, 255))
    d.rectangle((cx - gw, guard + gh * .55, cx + gw, guard + gh), fill=(160, 120, 40, 255))
    d.rounded_rectangle((cx - s * .018, guard + gh, cx + s * .018, grip_end), radius=s * .008, fill=(96, 48, 64, 255))
    for y in range(round(guard + gh * 2), round(grip_end), round(s * .03)):                              # grip wrap
        d.line([(cx - s * .018, y), (cx + s * .018, y + s * .012)], fill=(60, 28, 40, 255), width=round(s * .005))
    d.ellipse((cx - s * .03, grip_end - s * .004, cx + s * .03, pommel), fill=(212, 170, 72, 255))
    outline = layer.getchannel("A").filter(ImageFilter.MaxFilter(2 * round(N * 0.008) + 1))
    framed = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    framed.paste(Image.new("RGBA", (s, s), (12, 10, 30, 255)), mask=outline)
    framed.alpha_composite(layer)
    return framed


def lettering() -> Image.Image:
    font_path = next((p for p in FONT_CANDIDATES if Path(p).exists()), None)
    if font_path is None:
        raise SystemExit("no bold font found; add one to FONT_CANDIDATES")
    font = ImageFont.truetype(font_path, round(N * 0.40))
    mask = Image.new("L", (N, N), 0)
    d = ImageDraw.Draw(mask)
    for ch, cx in (("S", N * 0.235), ("II", N * 0.775)):     # the sword stands between the two
        box = d.textbbox((0, 0), ch, font=font)
        d.text((cx - (box[2] - box[0]) / 2 - box[0], N * 0.60 - (box[3] - box[1]) / 2 - box[1]), ch, font=font, fill=255)
    outline = mask.filter(ImageFilter.MaxFilter(2 * round(N * 0.012) + 1))
    layer = Image.new("RGBA", (N, N), (0, 0, 0, 0))
    layer.paste(Image.new("RGBA", (N, N), (12, 10, 30, 255)), mask=outline)
    grad = Image.new("RGB", (1, N))
    grad.putdata([lerp((255, 255, 255), (255, 198, 96), max(0.0, (y / N - .3) / .5)) for y in range(N)])
    layer.paste(grad.resize((N, N)), mask=mask)
    return layer


def main() -> None:
    out = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parents[1] / "assets" / "icon" / "sc2hd.png"
    img = background().convert("RGBA")
    img.alpha_composite(lettering())
    img.alpha_composite(blade_layer())      # in front, so the whole sword shows
    ImageDraw.Draw(img).rounded_rectangle((N * .012, N * .012, N * .988, N * .988), radius=N * .2, outline=(150, 170, 255, 255),
                                          width=round(N * .01))
    # Round the corners (the icon is a rounded square with transparent corners).
    mask = Image.new("L", (N, N), 0)
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, N - 1, N - 1), radius=N * 0.2, fill=255)
    img.putalpha(ImageChops.multiply(img.getchannel("A"), mask))
    img.resize((OUT_SIZE, OUT_SIZE), Image.LANCZOS).save(out, optimize=True)
    print("wrote", out)


if __name__ == "__main__":
    main()
