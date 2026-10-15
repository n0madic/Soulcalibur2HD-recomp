"""Compiles the lifted PPU and SPU code into the game module the host loads.

The module holds everything derived from the game and nothing else. Its undefined symbols (vm_read32, ps3_hle_call, the
SPU channel API, ...) are resolved against the host's exports when the host loads it, so it links without the host.
"""
from __future__ import annotations

import hashlib
import logging
import platform
import subprocess
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass
from pathlib import Path

from .toolchain import Compiler

LOG = logging.getLogger("sc2hd.module")

# Thread-local variables cannot be imported from another image (Mach-O, PE), so the lift reaches the host's
# trampoline state through exported accessors (toolkit patch 0022).
MODULE_DEFS = ("-DPPU_MODULE_ACCESSORS", "-DSPU_MODULE_ACCESSORS")
# The symbols the host looks up in the module (main.cpp, load_game_module).
MODULE_EXPORTS = ("sc2hd_module_abi", "function_table", "function_table_count")
# The SPU sources need the local-store path of the musttail dispatcher; the PPU chunks are huge and generated, so
# warnings are off and floating-point contraction is off (a PPC fmadd is spelt as one where the lifter means one).
PPU_FLAGS = ("-std=gnu++20", "-O3", "-DNDEBUG", "-w", "-ffp-contract=off", "-fno-strict-aliasing", "-fPIC")
SPU_FLAGS = ("-std=gnu17", "-O2", "-DNDEBUG", "-w", "-DSPU_USE_DISPATCH_MT", "-fno-strict-aliasing", "-fPIC")


class ModuleBuildError(RuntimeError):
    pass


def arch_flags(cc: Compiler) -> tuple[str, ...]:
    """Flags for the CPU the module is built for. On x86-64 the runtime's SPU helpers have SSE4.1 paths (spu_shufb is
    about a tenth of the audio mixer task) that are only compiled in under __SSE4_1__, as the host is."""
    x86 = any("x86_64" in t or "x86-64" in t for t in cc.target) or (
        not cc.target and platform.machine().lower() in ("x86_64", "amd64"))
    return ("-msse4.1",) if x86 else ()


@dataclass(frozen=True)
class ModuleInputs:
    sdk: Path          # the host build's sdk/ directory (headers, tools, spu_dispatch_mt.c)
    lift: Path         # ppu_recomp.h + ppu_recomp_NNN.cpp
    spu: Path          # <image>/spu_recomp.c, spu_workloads.c, sc2_overlays.c, ...
    out: Path          # object cache and the finished module


def module_name(system: str) -> str:
    return {"darwin": "game.dylib", "linux": "game.so", "windows": "game.dll"}[system]


def _units(inp: ModuleInputs) -> list[tuple[str, Path, tuple[str, ...]]]:
    """(language, source, flags) for every translation unit of the module."""
    units: list[tuple[str, Path, tuple[str, ...]]] = []
    for src in sorted(inp.lift.glob("ppu_recomp_*.cpp")):
        units.append(("c++", src, PPU_FLAGS))
    spu_c = sorted(inp.spu.glob("*/spu_recomp.c"))
    for name in ("spu_workloads.c", "jobmods_register.c", "taskset_policy_bytes.c", "sc2_overlays.c"):
        if (inp.spu / name).exists():
            spu_c.append(inp.spu / name)
    spu_c.append(inp.sdk / "runtime" / "spu" / "spu_dispatch_mt.c")
    for src in spu_c:
        units.append(("c", src, SPU_FLAGS))
    if not any(u[0] == "c++" for u in units):
        raise ModuleBuildError(f"no lifted PPU sources in {inp.lift}")
    return units


def write_module_def(out: Path) -> Path:
    """The .def file that exports what the host looks up (MODULE_EXPORTS) from the Windows DLL."""
    def_file = out / "module.def"
    def_text = "EXPORTS\n" + "".join(f"    {name} DATA\n" for name in MODULE_EXPORTS)
    if not def_file.exists() or def_file.read_text() != def_text:
        def_file.write_text(def_text)
    return def_file


def _meta_source(abi: str) -> str:
    return f'const char sc2hd_module_abi[] = "{abi}";\n'


def _run(cmd: list[str]) -> str:
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        raise ModuleBuildError(f"{' '.join(cmd[:3])} ... {cmd[-3]}\n{r.stderr[-3000:]}")
    return r.stderr


def build_module(inp: ModuleInputs, cc: Compiler, system: str, *, jobs: int = 8, implib: Path | None = None) -> Path:
    abi = (inp.sdk / "ABI").read_text().strip()
    obj_dir = inp.out / "obj"
    obj_dir.mkdir(parents=True, exist_ok=True)

    meta = inp.out / "module_meta.c"
    if not meta.exists() or meta.read_text() != _meta_source(abi):
        meta.write_text(_meta_source(abi))
    units = _units(inp) + [("c", meta, ())]

    include = [f"-I{inp.lift}", f"-I{inp.sdk / 'include'}", f"-I{inp.sdk / 'runtime' / 'ppu'}",
               f"-I{inp.sdk / 'runtime' / 'spu'}", f"-I{inp.sdk / 'runtime' / 'memory'}",
               f"-I{inp.sdk / 'runtime' / 'platform'}"]

    def compile_one(unit: tuple[str, Path, tuple[str, ...]]) -> Path:
        lang, src, flags = unit
        driver = cc.cxx if lang == "c++" else cc.cc
        # The cache key covers the source, the command and the compiler: a changed lift or flag rebuilds the unit.
        h = hashlib.sha256()
        h.update(src.read_bytes())
        cmd = [*driver, *cc.target, *arch_flags(cc), *flags, *MODULE_DEFS, *include]
        h.update(" ".join(cmd).encode())
        key = h.hexdigest()[:16]
        obj = obj_dir / f"{hashlib.sha1(str(src).encode()).hexdigest()[:8]}-{src.name}.o"
        stamp = Path(str(obj) + ".key")
        if obj.exists() and stamp.exists() and stamp.read_text() == key:
            return obj
        _run([*cmd, "-c", str(src), "-o", str(obj)])
        stamp.write_text(key)
        return obj

    LOG.info("compiling %d translation units", len(units))
    with ThreadPoolExecutor(jobs) as ex:
        objs = list(ex.map(compile_one, units))

    out = inp.out / module_name(system)
    link = [*cc.cxx, *cc.target, "-shared", "-o", str(out)]
    if system == "darwin":
        link += ["-undefined", "dynamic_lookup"]
    elif system == "windows":
        # A DLL cannot leave symbols undefined: it links against the import library the host build writes. What the
        # host looks up (MODULE_EXPORTS) is exported through a .def file, since lld does not auto-export read-only data
        # and there is no point in exporting the 11000 lifted functions.
        if implib is None or not implib.exists():
            raise ModuleBuildError(f"building the module for Windows needs the host's import library ({implib})")
        link += [str(write_module_def(inp.out)), str(implib)]
    else:
        link += ["-Wl,-s"]
    _run([*link, *map(str, objs)])
    return out
