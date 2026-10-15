"""Lays out the fake /dev_hdd0 the game expects and starts the host."""
from __future__ import annotations

import logging
import os
import shutil
import subprocess
import sys
from pathlib import Path

from .game import GameFiles

LOG = logging.getLogger("sc2hd.run")

HELPERS = Path(__file__).resolve().parent.parent / "helpers"


def layout(run: Path, files: GameFiles, python: str) -> dict[str, str]:
    """Creates the run directory for `files` and returns the environment the host needs."""
    run.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(files.param_sfo, run / "PARAM.SFO")

    # The SPU modules MultiStream loads from /dev_flash (flashMP3.pic, flashATRAC.pic) are stand-ins of the same
    # shape: the port decodes MP3 on the host. Nothing here is derived from the firmware.
    stubs = run / "dev_flash_stubs"
    subprocess.run([python, "-I", str(HELPERS / "make_firmware_stubs.py"), str(stubs)], check=True,
                   stdout=subprocess.DEVNULL)
    return {
        "PS3_PARAM_SFO": str(run / "PARAM.SFO"),
        "PS3_HDD0_ROOT": str(files.hdd0),
        "PS3_VFS_ROOT": str(run),
        "PS3_TITLE": "Soulcalibur II HD",
        "PS3_DEV_FLASH": str(stubs),
    }


def start(host: Path, module: Path | None, files: GameFiles, run: Path, env_extra: dict[str, str], python: str) -> int:
    env = dict(os.environ)
    env.update(layout(run, files, python))
    env.update(env_extra)
    cmd = [str(host), *(["--module", str(module)] if module else []), str(files.elf)]
    LOG.info("starting %s", " ".join(cmd))
    if os.name == "nt":
        return subprocess.call(cmd, env=env)
    sys.stdout.flush()
    os.execve(cmd[0], cmd, env)
