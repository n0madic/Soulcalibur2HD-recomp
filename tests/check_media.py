#!/usr/bin/env python3
"""Checks the built-in H.264 and AAC decoders and the MP4 demuxer against ffmpeg.

    tests/check_media.py             # generated clips covering the encoder features the decoder supports
    tests/check_media.py a.mp4 ...   # also (or instead, with --only) given movies

Builds tests/h264_dump.c and tests/aac_dump.c with the system C compiler, decodes with both decoders and compares: video
frames must be bit-identical to ffmpeg's, audio must match to float precision (SNR above 100 dB). Also checks that an
unsupported (CAVLC) H.264 stream is refused when it is opened, and, where ffmpeg has the AudioToolbox HE-AAC encoder,
that explicitly signalled SBR decodes exactly like its AAC core. Needs ffmpeg (with libx264 and the aac encoder) and
ffprobe, numpy and a C compiler. Exit status 0 when everything matches.
"""
import argparse
import json
import logging
import os
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Sequence

import numpy as np

LOG = logging.getLogger("check_media")
ROOT = Path(__file__).resolve().parent.parent
MEDIA = ROOT / "src" / "media"


@dataclass(frozen=True)
class Clip:
    name: str
    size: str = "320x240"
    x264: str = ""                 # -x264-params
    vf: str = "noise=alls=10:allf=t"
    audio: str = "sine=frequency=440:sample_rate=48000"
    channels: int = 2
    aac: tuple = ()                # extra encoder arguments
    seconds: int = 3


CLIPS = (
    Clip("default"),
    Clip("pyramid_temporal", x264="bframes=3:b-pyramid=normal:ref=4:direct=temporal:8x8dct=1"),
    Clip("spatial_no8x8", x264="bframes=2:direct=spatial:8x8dct=0:ref=3"),
    Clip("weighted", vf="noise=alls=10:allf=t,fade=t=in:d=2,fade=t=out:st=2:d=1", x264="bframes=2:weightb=1:weightp=2:ref=3"),
    Clip("slices_constrained", x264="slices=4:constrained-intra=1:bframes=2"),
    Clip("deblock_offsets", x264="deblock=-3:3:bframes=1"),
    Clip("no_deblock_many_refs", x264="no-deblock=1:ref=8:bframes=4"),
    Clip("cropped", size="322x242"),
    Clip("mono_44k", audio="sine=frequency=330:sample_rate=44100", channels=1),
    Clip("stereo_32k", audio="sine=frequency=500:sample_rate=32000", aac=("-ar", "32000")),
    Clip("stereo_22k_tns", audio="sine=frequency=700:sample_rate=22050", aac=("-ar", "22050", "-aac_tns", "1")),
)


def run(*cmd: str, **kw: object) -> subprocess.CompletedProcess:
    return subprocess.run(cmd, check=True, **kw)


def build(tmp: Path) -> None:
    cc = os.environ.get("CC", "cc")
    video = [MEDIA / f for f in ("h264.c", "h264_mb.c", "h264_mc.c", "h264_dbf.c", "mp4.c")]
    run(cc, "-O2", "-std=c99", "-o", str(tmp / "h264_dump"), str(ROOT / "tests/h264_dump.c"), *map(str, video))
    run(cc, "-O2", "-std=c99", "-o", str(tmp / "aac_dump"), str(ROOT / "tests/aac_dump.c"), str(MEDIA / "aac.c"),
        str(MEDIA / "mp4.c"), "-lm")


def generate(clip: Clip, tmp: Path) -> Path:
    out = tmp / f"{clip.name}.mp4"
    layout = "mono" if clip.channels == 1 else "stereo"
    cmd = ["ffmpeg", "-v", "error", "-y",
           "-f", "lavfi", "-i", f"testsrc2=size={clip.size}:rate=30:duration={clip.seconds}",
           "-f", "lavfi", "-i", f"{clip.audio}:duration={clip.seconds}",
           "-vf", clip.vf, "-c:v", "libx264", "-preset", "fast", "-x264-params", clip.x264 or "keyint=60",
           "-pix_fmt", "yuv420p", "-c:a", "aac", "-ac", str(clip.channels), "-channel_layout", layout, "-b:a", "128k", "-aac_pns", "0", "-aac_is", "0", *clip.aac,
           str(out)]
    run(*cmd)
    return out


def check(movie: Path, tmp: Path) -> bool:
    info = json.loads(run("ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries", "stream=width,height",
                          "-of", "json", str(movie), capture_output=True, text=True).stdout)["streams"][0]
    w, h = info["width"], info["height"]
    ok = True
    # video: every frame bit-exact (ffmpeg may drop the very last frame, so compare the common prefix)
    run(str(tmp / "h264_dump"), str(movie), str(tmp / "v.yuv"), stderr=subprocess.DEVNULL)
    run("ffmpeg", "-v", "error", "-y", "-i", str(movie), "-f", "rawvideo", "-pix_fmt", "yuv420p", str(tmp / "ref.yuv"))
    fs = w * h * 3 // 2
    with open(tmp / "v.yuv", "rb") as a, open(tmp / "ref.yuv", "rb") as b:
        n = min(os.path.getsize(tmp / "v.yuv"), os.path.getsize(tmp / "ref.yuv")) // fs
        bad = sum(1 for _ in range(n) if a.read(fs) != b.read(fs))
    LOG.info("%s: video %d frames, %d differ", movie.name, n, bad)
    ok &= bad == 0 and n > 0
    # audio: float precision
    run(str(tmp / "aac_dump"), str(movie), str(tmp / "a.f32"), stderr=subprocess.DEVNULL)
    run("ffmpeg", "-v", "error", "-y", "-i", str(movie), "-vn", "-f", "f32le", "-acodec", "pcm_f32le", str(tmp / "ref.f32"))
    a = np.fromfile(tmp / "a.f32", dtype=np.float32).astype(np.float64)
    b = np.fromfile(tmp / "ref.f32", dtype=np.float32).astype(np.float64)
    m = min(len(a), len(b))
    err = float(((a[:m] - b[:m]) ** 2).mean())
    snr = 10 * np.log10(float((b[:m] ** 2).mean()) / max(err, 1e-30))
    LOG.info("%s: audio %d samples, SNR %.1f dB", movie.name, m, snr)
    ok &= snr > 100 and m > 0
    return bool(ok)


def check_cavlc_refused(tmp: Path) -> bool:
    """A stream the decoder cannot decode (CAVLC) must fail at h264_open, not play as a blank movie."""
    movie = tmp / "cavlc.mp4"
    run("ffmpeg", "-v", "error", "-y", "-f", "lavfi", "-i", "testsrc2=size=160x120:rate=30:duration=1",
        "-c:v", "libx264", "-profile:v", "baseline", "-pix_fmt", "yuv420p", str(movie))
    dump = subprocess.run([str(tmp / "h264_dump"), str(movie), os.devnull], capture_output=True, text=True)
    refused = dump.returncode != 0 and "h264_open failed" in dump.stderr
    LOG.info("cavlc.mp4: %s", "refused at open" if refused else "NOT refused")
    return refused


def check_explicit_sbr(tmp: Path) -> bool:
    """An AudioSpecificConfig with explicit SBR signalling (object type 5) decodes to the same AAC core as the
    backward-compatible form. Needs ffmpeg's aac_at (HE-AAC) encoder; skipped without it."""
    encoders = run("ffmpeg", "-v", "error", "-encoders", capture_output=True, text=True).stdout
    if " aac_at " not in encoders:
        LOG.info("explicit SBR: skipped (no aac_at encoder)")
        return True
    implicit, explicit = tmp / "he_implicit.mp4", tmp / "he_explicit.mp4"
    run("ffmpeg", "-v", "error", "-y", "-f", "lavfi", "-i", "sine=frequency=1000:sample_rate=48000:duration=2",
        "-ac", "1", "-c:a", "aac_at", "-profile:a", "4", "-b:a", "32k", str(implicit))
    data = bytearray(implicit.read_bytes())
    # DecoderSpecificInfo (tag 5) with ffmpeg's 4-byte length: the 5-byte backward-compatible ASC of HE-AAC
    at = data.find(bytes([0x05, 0x80, 0x80, 0x80, 0x05]), data.find(b"esds"))
    if at < 0:
        LOG.info("explicit SBR: unexpected esds layout")
        return False
    asc = int.from_bytes(data[at + 5:at + 10], "big")
    rate_idx, channels = (asc >> 31) & 15, (asc >> 27) & 15
    bits = f"00101{rate_idx:04b}{channels:04b}0011000100000"   # AOT 5, core rate and channels, ext. rate 48 kHz, AOT 2
    data[at + 5:at + 10] = int(bits.ljust(40, "0"), 2).to_bytes(5, "big")
    explicit.write_bytes(bytes(data))
    decoded = all(subprocess.run([str(tmp / "aac_dump"), str(m), str(m.with_suffix(".f32"))],
                                 stderr=subprocess.DEVNULL).returncode == 0 for m in (implicit, explicit))
    same = decoded and implicit.with_suffix(".f32").read_bytes() == explicit.with_suffix(".f32").read_bytes()
    LOG.info("explicit SBR: %s", "decodes like the core" if same else "DIFFERS from the core")
    return same


def main(argv: Sequence[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("movies", nargs="*", type=Path, help="MP4 files to check in addition to the generated clips")
    ap.add_argument("--only", action="store_true", help="check only the given files")
    args = ap.parse_args(argv)
    logging.basicConfig(level=logging.INFO, format="%(message)s")
    with tempfile.TemporaryDirectory() as t:
        tmp = Path(t)
        build(tmp)
        movies = list(args.movies)
        if not args.only:
            movies = [generate(c, tmp) for c in CLIPS] + movies
        results = [check(m, tmp) for m in movies]
        if not args.only:
            results += [check_cavlc_refused(tmp), check_explicit_sbr(tmp)]
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
