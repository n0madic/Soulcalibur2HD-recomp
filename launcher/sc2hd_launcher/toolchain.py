"""Finds a C/C++ compiler for the game module, or fetches a portable one (zig) into the data directory.

The module is plain C and C++ (no platform headers, no frameworks), so `zig cc` builds it on any host and for any
target. A system compiler is used only when it is clang and can build the lift as it is (C++20, musttail).
"""
from __future__ import annotations

import hashlib
import logging
import os
import platform
import shutil
import subprocess
import sys
import tarfile
import tempfile
import urllib.request
import zipfile
from dataclasses import dataclass
from pathlib import Path

LOG = logging.getLogger("sc2hd.toolchain")

ZIG_VERSION = "0.17.0"
# (archive name, sha256) per host, from https://ziglang.org/download/index.json
ZIG_ARCHIVES = {
    ("darwin", "arm64"): ("zig-aarch64-macos-{v}.tar.xz", "b607e9b9234790a008116ae5bdb71c6243b84b9fb42a53a9e70fde41c06c536a"),
    ("darwin", "x86_64"): ("zig-x86_64-macos-{v}.tar.xz", "4f9a1c5269aa17ebda5e6d3c2b89d6cbf36f7d2b22a0306e9ab98f25f95529c6"),
    ("linux", "x86_64"): ("zig-x86_64-linux-{v}.tar.xz", "1cbe9df9f27e6b78d14ccbca43b6703a404ef79ef1c463de901d7f088d4e2026"),
    ("linux", "aarch64"): ("zig-aarch64-linux-{v}.tar.xz", "9e8d11661d4ae3bd57702a3832781e23ad151dde5798e16a5ccd503f65234ff8"),
    ("windows", "x86_64"): ("zig-x86_64-windows-{v}.zip", "b5663f69581dcf391293fbf16c06cb80d81d806545ce618b4d0bab7f0eb8c428"),
    ("windows", "aarch64"): ("zig-aarch64-windows-{v}.zip", "0a59d91fa1cb40cf068e9b0954434ce973500c7a2ea749f1e01af62cdab52d26"),
}
ZIG_URL = "https://ziglang.org/download/{v}/{name}"


class ToolchainError(RuntimeError):
    pass


@dataclass(frozen=True)
class Compiler:
    """`cc` and `cxx` are argv prefixes; `target` is the extra argv that selects the target ([] = native)."""

    kind: str
    cc: tuple[str, ...]
    cxx: tuple[str, ...]
    target: tuple[str, ...] = ()


def host_key() -> tuple[str, str]:
    system = {"Darwin": "darwin", "Linux": "linux", "Windows": "windows"}.get(platform.system())
    machine = platform.machine().lower()
    machine = {"amd64": "x86_64", "arm64": "arm64" if system == "darwin" else "aarch64"}.get(machine, machine)
    if system is None:
        raise ToolchainError(f"unsupported host OS {platform.system()}")
    return system, machine


def zig_target(system: str, machine: str) -> str:
    """The zig target triple for the module of a given OS and CPU."""
    arch = {"arm64": "aarch64", "aarch64": "aarch64", "x86_64": "x86_64"}[machine]
    osname = {"darwin": "macos", "linux": "linux-gnu", "windows": "windows-gnu"}[system]
    return f"{arch}-{osname}"


_PROBE = r"""
#include <atomic>
int f(int x);
int g(int x) { __attribute__((musttail)) return f(x); }
static_assert(__cplusplus >= 202002L, "C++20");
"""


def _command_line_tools_present() -> bool:
    """On macOS /usr/bin/clang is a stub that pops up an installer when the developer tools are missing: ask first."""
    if platform.system() != "Darwin":
        return True
    try:
        return subprocess.run(["xcode-select", "-p"], capture_output=True, timeout=30).returncode == 0
    except (OSError, subprocess.SubprocessError):
        return False


def _system_clang_ok(cxx: str) -> bool:
    """True if `cxx` is clang and compiles the constructs the lift relies on."""
    with tempfile.TemporaryDirectory() as td:
        src = Path(td, "probe.cpp")
        src.write_text(_PROBE)
        try:
            ver = subprocess.run([cxx, "--version"], capture_output=True, text=True, timeout=30)
            if "clang" not in ver.stdout.lower():
                return False
            r = subprocess.run([cxx, "-std=gnu++20", "-c", str(src), "-o", str(Path(td, "p.o"))],
                               capture_output=True, text=True, timeout=60)
            return r.returncode == 0
        except (OSError, subprocess.SubprocessError):
            return False


def zig_dir(data: Path) -> Path:
    return data / "toolchain" / f"zig-{ZIG_VERSION}"


def zig_exe(data: Path) -> Path:
    return zig_dir(data) / ("zig.exe" if os.name == "nt" else "zig")


def install_zig(data: Path) -> Path:
    """Downloads the pinned zig release into the data directory, verifies its SHA-256 and unpacks it."""
    key = host_key()
    if key not in ZIG_ARCHIVES:
        raise ToolchainError(f"no zig build is pinned for {key}")
    name_fmt, sha256 = ZIG_ARCHIVES[key]
    name = name_fmt.format(v=ZIG_VERSION)
    url = ZIG_URL.format(v=ZIG_VERSION, name=name)
    dest = zig_dir(data)
    dest.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=dest.parent) as td:
        archive = Path(td, name)
        LOG.info("downloading %s", url)
        digest = hashlib.sha256()
        with urllib.request.urlopen(url, timeout=60) as resp, open(archive, "wb") as out:
            while chunk := resp.read(1 << 20):
                digest.update(chunk)
                out.write(chunk)
        if digest.hexdigest() != sha256:
            raise ToolchainError(f"{name}: SHA-256 {digest.hexdigest()} does not match the pinned {sha256}")
        unpack = Path(td, "unpacked")
        if name.endswith(".zip"):
            with zipfile.ZipFile(archive) as z:
                z.extractall(unpack)
        else:
            with tarfile.open(archive) as t:
                for m in t.getmembers():   # the archive is pinned by hash, but do not trust member names anyway
                    if m.name.startswith(("/", "..")) or ".." in Path(m.name).parts:
                        raise ToolchainError(f"unsafe path in {name}: {m.name}")
                t.extractall(unpack)
        (root,) = list(unpack.iterdir())
        if dest.exists():
            shutil.rmtree(dest)
        shutil.move(str(root), dest)
    return zig_exe(data)


def find_compiler(data: Path, *, allow_download: bool, prefer: str = "auto", target: str | None = None) -> Compiler:
    """prefer: "auto" (a suitable system clang, else zig), "system" or "zig". target: zig triple, zig only."""
    if prefer in ("auto", "system") and target is None:
        if _command_line_tools_present():
            env_cxx = os.environ.get("SC2HD_CXX")
            for cxx in ([env_cxx] if env_cxx else []) + ["clang++", "c++"]:
                path = shutil.which(cxx)
                if path and _system_clang_ok(path):
                    cc = shutil.which(os.environ.get("SC2HD_CC", "clang") if env_cxx else "clang") or path.replace("++", "")
                    return Compiler("system", (cc,), (path,))
        if prefer == "system":
            raise ToolchainError("no suitable system clang (needs clang with C++20 and musttail)")
    zig = zig_exe(data)
    if not zig.exists():
        found = shutil.which("zig")
        if found:
            zig = Path(found)
        elif allow_download:
            zig = install_zig(data)
        else:
            raise ToolchainError("no suitable compiler: install clang or let the launcher download zig "
                                 f"{ZIG_VERSION} (~50 MB) into {zig_dir(data)}")
    tgt = ("-target", target) if target else ()
    return Compiler("zig", (str(zig), "cc"), (str(zig), "c++"), tgt)


def describe(c: Compiler) -> str:
    return f"{c.kind} ({' '.join(c.cxx)})"


if __name__ == "__main__":
    logging.basicConfig(level=logging.INFO)
    print(describe(find_compiler(Path(sys.argv[1]), allow_download=False)))
