"""The installed game: a folder `game/<TITLE ID>/` as `sc2hd install` leaves it."""
from __future__ import annotations

import json
import logging
import shutil
import struct
import subprocess
import zipfile
from dataclasses import dataclass
from pathlib import Path

LOG = logging.getLogger("sc2hd.game")


class GameError(RuntimeError):
    pass


@dataclass(frozen=True)
class GameFiles:
    hdd0: Path             # the fake /dev_hdd0; the game lives in hdd0/game/<title id>/
    title_id: str
    version: str           # APP_VER of the installed PARAM.SFO: the update the ELF comes from

    @property
    def root(self) -> Path:
        return self.hdd0 / "game" / self.title_id

    @property
    def elf(self) -> Path:
        return self.root / "USRDIR" / "EBOOT.elf"

    @property
    def param_sfo(self) -> Path:
        return self.root / "PARAM.SFO"

    def missing(self) -> list[Path]:
        return [p for p in (self.elf, self.param_sfo, self.root / "USRDIR" / "ROOT.EDAT") if not p.exists()]


def read_sfo(path: Path) -> dict[str, str | int]:
    """The key/value pairs of a PARAM.SFO."""
    raw = path.read_bytes()
    if raw[:4] != b"\0PSF":
        raise GameError(f"{path} is not a PARAM.SFO")
    key_off, data_off, count = struct.unpack_from("<III", raw, 8)
    out: dict[str, str | int] = {}
    for i in range(count):
        k, fmt, length, _max, d = struct.unpack_from("<HHIII", raw, 20 + 16 * i)
        key = raw[key_off + k:raw.index(b"\0", key_off + k)].decode()
        value = raw[data_off + d:data_off + d + length]
        out[key] = (struct.unpack("<I", value[:4])[0] if fmt == 0x404
                    else value.split(b"\0")[0].decode("utf-8", "replace"))
    return out


def from_install(hdd0: Path) -> GameFiles:
    games = sorted((hdd0 / "game").glob("*/PARAM.SFO"))
    if len(games) != 1:
        raise GameError(f"expected one game under {hdd0 / 'game'}, found {len(games)}")
    sfo = read_sfo(games[0])
    return GameFiles(hdd0, str(sfo["TITLE_ID"]), str(sfo.get("APP_VER", "")))


def _extract_zip(archive: Path, dest: Path) -> list[Path]:
    """Extracts the .pkg and .rap members of a zip (flattened: member names are not trusted); returns them."""
    out: list[Path] = []
    with zipfile.ZipFile(archive) as z:
        for info in z.infolist():
            name = Path(info.filename).name
            if info.is_dir() or Path(name).suffix.lower() not in (".pkg", ".rap"):
                continue
            target = dest / name
            LOG.info("extracting %s", name)
            with z.open(info) as src, open(target, "wb") as dst:
                shutil.copyfileobj(src, dst, 1 << 22)
            out.append(target)
    return out


def _fingerprint(p: Path) -> list:
    st = p.stat()
    return [p.name, st.st_size, int(st.st_mtime)]


def install(host: Path, inputs: list[Path], rap: Path | None, dest: Path, scratch: Path) -> GameFiles:
    """Installs the game from .pkg files or a .zip holding them, with the host's installer.

    `inputs` are given base game first, then its update (optional); the members of a zip go largest first. The license is `rap`,
    or the only .rap found next to the inputs or inside a zip. Nothing is done when the same inputs were installed.
    """
    key = json.dumps({"inputs": [_fingerprint(p) for p in inputs],
                      "rap": _fingerprint(rap) if rap else None}, sort_keys=True)
    stamp = dest / "install.stamp"
    if stamp.exists() and stamp.read_text() == key:
        LOG.info("already installed in %s", dest)
        return from_install(dest)
    stamp.unlink(missing_ok=True)
    dest.mkdir(parents=True, exist_ok=True)
    # A different set of packages (say, the game alone after the game with its update) must not inherit files of the
    # earlier one. The saves live elsewhere under dest and are kept.
    shutil.rmtree(dest / "game", ignore_errors=True)

    shutil.rmtree(scratch, ignore_errors=True)
    scratch.mkdir(parents=True, exist_ok=True)
    try:
        pkgs: list[Path] = []
        raps = [rap] if rap else []
        for item in inputs:
            if item.suffix.lower() == ".zip":
                members = sorted(_extract_zip(item, scratch), key=lambda m: -m.stat().st_size)
                pkgs += [m for m in members if m.suffix.lower() == ".pkg"]
                raps += [m for m in members if m.suffix.lower() == ".rap"]
            else:
                pkgs.append(item)
                if not rap:
                    raps += sorted(item.parent.glob("*.rap"))
        if not pkgs:
            raise GameError("no .pkg among the inputs")
        if len(set(raps)) != 1:
            raise GameError("give the license with --rap" if not raps else f"more than one .rap: {[r.name for r in raps]}")
        LOG.info("installing %s", ", ".join(p.name for p in pkgs))
        r = subprocess.run([str(host), "install", "--rap", str(raps[0]), "--to", str(dest), *map(str, pkgs)])
        if r.returncode:
            raise GameError(f"the installer failed ({r.returncode})")
    finally:
        shutil.rmtree(scratch, ignore_errors=True)
    stamp.write_text(key)
    return from_install(dest)
