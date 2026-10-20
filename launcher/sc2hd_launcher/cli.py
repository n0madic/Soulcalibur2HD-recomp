"""sc2hd: lift the user's copy of Soulcalibur II HD, build the game module, run it on the prebuilt host.

    sc2hd.py [options]            build whatever is stale, then run
    sc2hd.py build [options]      build only
    sc2hd.py run [options]        run only (needs a previous build)

Everything derived from the game is written under the data directory (--data, SC2HD_HOME), by default in ./data of the directory it is started from.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import logging
import os
import subprocess
import sys
from pathlib import Path

from . import devhost, runner
from .game import GameError, GameFiles, from_install, install, unlock_all_characters_in_save, unlock_env
from .lift import PROFILES, LiftResult, lift_game
from .module import ModuleInputs, build_module, module_name
from .toolchain import ToolchainError, describe, find_compiler, host_key

LOG = logging.getLogger("sc2hd")
DEFAULT_DATA_DIR = "data"   # relative to the directory the launcher is started from
ROOT = Path(__file__).resolve().parents[2]   # the release directory, or the source checkout (devhost.is_checkout)


def default_data() -> Path:
    return Path(os.environ.get("SC2HD_HOME") or DEFAULT_DATA_DIR).resolve()


def find_host(host_dir: Path) -> tuple[Path, Path]:
    exe = host_dir / ("sc2hd_host.exe" if os.name == "nt" else "sc2hd_host")
    if not exe.exists() or not (host_dir / "sdk" / "ABI").exists():
        raise SystemExit(f"no host in {host_dir} (expected sc2hd_host and sdk/); pass --host-dir")
    return exe, host_dir / "sdk"


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while chunk := f.read(1 << 22):
            h.update(chunk)
    return h.hexdigest()


def work_dir(files: GameFiles, data: Path) -> Path:
    return data / "games" / f"{files.title_id}-{files.version}"


def lift_stage(files: GameFiles, sdk: Path, work: Path) -> LiftResult:
    profile = PROFILES.get((files.title_id, files.version))
    if profile is None:
        known = ", ".join(f"{t} {v}" for t, v in sorted(PROFILES))
        raise SystemExit(f"{files.title_id} {files.version} is not a supported game version (supported: {known})")
    work.mkdir(parents=True, exist_ok=True)

    # The lift is a function of the ELF and the SDK's lifter; skip it when both are unchanged.
    stamp = work / "lift.stamp"
    want = json.dumps({"elf": sha256_file(files.elf), "abi": (sdk / "ABI").read_text().strip(),
                       "profile": profile.code_end}, sort_keys=True)
    if stamp.exists() and stamp.read_text() == want and (work / "recompiled" / "ppu_recomp.h").exists():
        LOG.info("lift is up to date")
        return LiftResult(work / "recompiled", work / "spu", work / "out")
    stamp.unlink(missing_ok=True)
    result = lift_game(sdk, files.elf, work, profile, sys.executable)
    stamp.write_text(want)
    return result


def build(files: GameFiles, sdk: Path, data: Path, *, allow_download: bool, compiler: str, jobs: int) -> Path:
    work = work_dir(files, data)
    result = lift_stage(files, sdk, work)
    cc = find_compiler(data, allow_download=allow_download, prefer=compiler)
    LOG.info("compiler: %s", describe(cc))
    system, _ = host_key()
    implib = sdk / "libsc2hd_host.dll.a" if system == "windows" else None
    module = build_module(ModuleInputs(sdk, result.lift, result.spu, work / "module"), cc, system, jobs=jobs,
                          implib=implib)
    LOG.info("module: %s", module)
    return module


def build_monolith(files: GameFiles, sdk: Path, data: Path) -> Path:
    """Personal use: one executable with the lifted code linked in. Needs a source checkout (scripts/)."""
    script = ROOT / "scripts" / "build_monolith.sh"
    if not script.exists():
        raise SystemExit("the monolith is built from a source checkout; this launcher is not in one")
    work = work_dir(files, data)
    lift_stage(files, sdk, work)
    subprocess.run([str(script), str(work)], check=True)
    return work / "sc2hd_monolith"


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="sc2hd", description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("command", nargs="?", choices=("build", "run", "monolith"),
                    help="default: build, then run. monolith: personal use, link the game into one executable")
    ap.add_argument("--data", type=Path, default=default_data(), help="where derived data lives (default: %(default)s)")
    ap.add_argument("--host-dir", type=Path,
                    help="directory with sc2hd_host and sdk/ (default: host/ of a release; in a source checkout dist/host, "
                         "or dist/host-windows on Windows, rebuilt by scripts/build_host.sh when src/ or patches/ are newer)")
    ap.add_argument("--no-host-build", action="store_true", help="in a source checkout, use the host in dist/ as it is")
    src = ap.add_argument_group("the game (first run; remembered in the data directory afterwards)")
    src.add_argument("--pkg", type=Path, action="append", default=[], metavar="PKG|ZIP",
                     help="a package or a zip holding it (repeatable: the game first, then its optional 01.01 update)")
    src.add_argument("--rap", type=Path, help="the license (.rap); optional if it is next to the package or in the zip")
    ap.add_argument("--compiler", choices=("auto", "system", "zig"), default="auto")
    ap.add_argument("--yes", action="store_true", help="download the portable compiler without asking")
    ap.add_argument("--monolith", action="store_true", help="run the executable `sc2hd monolith` built, not the host")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    ap.add_argument("-v", "--verbose", action="store_true")
    g = ap.add_argument_group("run options")
    g.add_argument("--fxaa", dest="fxaa", action="store_true", default=None, help="FXAA on (default)")
    g.add_argument("--no-fxaa", dest="fxaa", action="store_false", help="FXAA off")
    g.add_argument("--af", type=int, metavar="N", help="anisotropic filtering 1 (off) .. 16 (default)")
    g.add_argument("--window", metavar="WxH", help="initial window size in points (default 1280x720)")
    g.add_argument("--fullscreen", action="store_true", help="start in fullscreen (F11 / Alt+Enter, macOS: Ctrl+Cmd+F)")
    g.add_argument("--unlock", action="store_true",
                   help="unlock all playable characters for this run, without finishing the game; the save data on "
                        "disk is not changed")
    g.add_argument("--unlock-save", action="store_true",
                   help="unlock all playable characters for good, by changing the save data (needs a save: start the "
                        "game once first)")
    args = ap.parse_args(argv)
    logging.basicConfig(level=logging.DEBUG if args.verbose else logging.INFO, format="%(levelname).1s %(name)s: %(message)s")

    if args.af is not None and not 1 <= args.af <= 16:
        ap.error("--af expects a number 1..16")
    if args.host_dir is None:
        if devhost.is_checkout(ROOT):
            args.host_dir = devhost.dist_host_dir(ROOT) if args.no_host_build else devhost.ensure_host(ROOT)
        else:
            args.host_dir = ROOT / "host"
    host, sdk = find_host(args.host_dir)
    hdd0 = args.data / "hdd0"
    try:
        if args.pkg:
            files = install(host, args.pkg, args.rap, hdd0, args.data / "import")
        else:
            files = from_install(hdd0)
    except GameError as e:
        raise SystemExit(f"{e}\nfirst run: sc2hd.py --pkg <game.pkg> [--pkg <update.pkg>] --rap <license.rap>")
    if missing := files.missing():
        raise SystemExit("missing game files:\n  " + "\n  ".join(map(str, missing)))

    work = work_dir(files, args.data)
    exe, module = host, None
    try:
        if args.command == "monolith":
            LOG.info("monolith: %s", build_monolith(files, sdk, args.data))
            return 0
        if args.monolith:
            exe = work / "sc2hd_monolith"
            if args.command is None and not exe.exists():
                build_monolith(files, sdk, args.data)
            if not exe.exists():
                raise SystemExit(f"{exe} does not exist; run `sc2hd monolith` first")
        else:
            if args.command in (None, "build"):
                build(files, sdk, args.data, allow_download=args.yes or sys.stdin.isatty(), compiler=args.compiler,
                      jobs=args.jobs)
            if args.command == "build":
                return 0
            system, _ = host_key()
            module = work / "module" / module_name(system)
            if not module.exists():
                raise SystemExit(f"{module} does not exist; run `sc2hd build` first")
    except ToolchainError as e:
        raise SystemExit(str(e))

    if args.unlock_save:
        unlock_all_characters_in_save(files)
    env: dict[str, str] = {}
    if args.unlock:
        env.update(unlock_env())
    # The toolkit's own present options (PS3_AA, PS3_ANISO) are off unless set; the launcher turns them on by default
    env["PS3_AA"] = "off" if args.fxaa is False else "fxaa"
    env["PS3_ANISO"] = str(args.af if args.af is not None else 16)
    if args.af is not None:
        env["PS3RECOMP_METAL_AF"] = str(args.af)
    if args.window:
        env["PS3RECOMP_METAL_WINDOW"] = args.window
    if args.fullscreen:
        env["PS3RECOMP_FULLSCREEN"] = "1"
    return runner.start(exe, module, files, work / "run", env, sys.executable)
