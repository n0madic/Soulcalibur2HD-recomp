"""Lifts a decrypted PS3 ELF with the SDK's tools: PPU code to C++, SPU images to C.

Everything it writes is derived from the user's copy of the game and stays in the data directory.
"""
from __future__ import annotations

import logging
import shutil
import subprocess
from dataclasses import dataclass
from pathlib import Path

LOG = logging.getLogger("sc2hd.lift")

HELPERS = Path(__file__).resolve().parent.parent / "helpers"


class LiftError(RuntimeError):
    pass


@dataclass(frozen=True)
class Profile:
    """What the lifter cannot find out from the ELF itself."""

    title_id: str
    version: str
    code_end: int          # end of the executable sections; .rodata follows and must not be disassembled
    chunk_lines: int = 150000


# The game versions this launcher knows: the base release (01.00) and its optional update (01.01). code_end is where the
# section after the last code section (.rodata) starts. The SPU programs are byte-identical in both versions.
# A second title is a second entry plus its SPU overlay helper.
PROFILES = {
    ("NPUB31366", "01.00"): Profile("NPUB31366", "01.00", code_end=0x3A9BAC),
    ("NPUB31366", "01.01"): Profile("NPUB31366", "01.01", code_end=0x3B0904),
}


@dataclass(frozen=True)
class LiftResult:
    lift: Path         # ppu_recomp.h + chunks
    spu: Path          # SPU lift + registration sources
    loader_out: Path


def _tool(python: str, script: Path, *args: str | Path) -> None:
    cmd = [python, "-I", str(script), *map(str, args)]
    LOG.debug("%s", " ".join(cmd))
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        raise LiftError(f"{script.name} failed ({r.returncode}):\n{(r.stdout + r.stderr)[-3000:]}")


def lift_game(sdk: Path, elf: Path, work: Path, profile: Profile, python: str) -> LiftResult:
    tools = sdk / "tools"
    loader_out, lift, spu_images, spu = work / "out", work / "recompiled", work / "spu_images", work / "spu"
    for d in (loader_out, lift, spu):
        d.mkdir(parents=True, exist_ok=True)

    LOG.info("loading the image")
    _tool(python, tools / "ppu_loader.py", elf, "-o", f"{loader_out}/")
    LOG.info("lifting PPU code")
    _tool(python, tools / "ppu_lifter.py", elf,
          "--functions", loader_out / "EBOOT.functions.json", "--hle-stubs", loader_out / "EBOOT.imports.json",
          "--code-end", hex(profile.code_end), "--chunk-lines", profile.chunk_lines, "-o", lift)

    LOG.info("lifting SPU images")
    for d in (spu_images, spu / "images"):
        if d.exists():
            shutil.rmtree(d)
    _tool(python, tools / "extract_spu_images.py", elf, "--output", spu_images)
    shutil.copytree(spu_images, spu / "images")
    # MultiStream, the game's audio engine, loads two DSP plugins into local store at run time: give them images at
    # that address, and entry points for the returns its SPU program makes into the middle of its own code.
    _tool(python, HELPERS / "make_sc2_overlays.py", "prepare", spu / "images", spu)
    extra = (spu / "lift_args.txt").read_text().split()
    _tool(python, tools / "build_spu_workloads.py", "--images", spu / "images", "--lifted", spu,
          "--out", spu / "spu_workloads.c", "--register-fn", "sc2hd_spu_register_all", "--constructor",
          "--title", "sc2hd", *extra)
    _tool(python, HELPERS / "make_sc2_overlays.py", "emit", spu)
    return LiftResult(lift, spu, loader_out)
