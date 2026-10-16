"""Running from a source checkout: finds the host build in dist/ and rebuilds it when its sources changed."""
from __future__ import annotations

import logging
import os
import shutil
import subprocess
from pathlib import Path

LOG = logging.getLogger("sc2hd.dev")

# What the host is built from (scripts/build_host.sh): a change to any of these makes the built host stale.
HOST_SOURCES = ("src", "patches", "scripts/build_host.sh", "scripts/mingw-w64.cmake")
GIT_BASH_NAMES = ("Git/bin/bash.exe", "Git/usr/bin/bash.exe")


def is_checkout(root: Path) -> bool:
    return (root / "scripts" / "build_host.sh").exists() and (root / "src" / "CMakeLists.txt").exists()


def dist_host_dir(root: Path) -> Path:
    """The directory build_host.sh writes for the OS the launcher runs on."""
    return root / "dist" / ("host-windows" if os.name == "nt" else "host")


def host_exe(host_dir: Path) -> Path:
    return host_dir / ("sc2hd_host.exe" if os.name == "nt" else "sc2hd_host")


def newest_source(root: Path) -> float:
    newest = 0.0
    for rel in HOST_SOURCES:
        path = root / rel
        files = [path] if path.is_file() else (p for p in path.rglob("*") if p.is_file())
        for f in files:
            newest = max(newest, f.stat().st_mtime)
    return newest


def is_stale(root: Path, host_dir: Path) -> bool:
    exe = host_exe(host_dir)
    return not exe.exists() or not (host_dir / "sdk" / "ABI").exists() or newest_source(root) > exe.stat().st_mtime


def find_bash() -> str:
    """bash for build_host.sh. On Windows the one of Git for Windows: a bash found on PATH may be WSL's."""
    if os.name == "nt":
        for base in filter(None, (os.environ.get("ProgramFiles"), os.environ.get("ProgramW6432"))):
            for name in GIT_BASH_NAMES:
                if (candidate := Path(base) / name).exists():
                    return str(candidate)
    if bash := shutil.which("bash"):
        return bash
    raise SystemExit("building the host needs bash (Windows: Git for Windows); or pass --no-host-build to use dist/ as it is")


def ensure_host(root: Path) -> Path:
    """Builds the host with scripts/build_host.sh if it is missing or older than its sources; returns its directory."""
    host_dir = dist_host_dir(root)
    if not is_stale(root, host_dir):
        return host_dir
    LOG.info("%s is missing or older than src/, patches/: running scripts/build_host.sh", host_dir)
    try:
        subprocess.run([find_bash(), str(root / "scripts" / "build_host.sh")], check=True, cwd=root)
    except subprocess.CalledProcessError as e:
        raise SystemExit(f"scripts/build_host.sh failed ({e.returncode})")
    return host_dir
