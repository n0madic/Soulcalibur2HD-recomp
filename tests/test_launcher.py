"""Unit tests for the launcher's module build helpers: python3 tests/test_launcher.py"""
from __future__ import annotations

import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "launcher"))

from sc2hd_launcher import module  # noqa: E402
from sc2hd_launcher.toolchain import Compiler  # noqa: E402


def compiler(*target: str) -> Compiler:
    return Compiler("test", ("cc",), ("c++",), tuple(target))


class ArchFlagsTest(unittest.TestCase):
    def test_x86_64_targets_get_sse41(self):
        for triple in ("x86_64-windows-gnu", "x86_64-linux-gnu", "x86_64-macos"):
            with self.subTest(triple=triple):
                self.assertEqual(module.arch_flags(compiler("-target", triple)), ("-msse4.1",))

    def test_other_targets_get_nothing(self):
        self.assertEqual(module.arch_flags(compiler("-target", "aarch64-linux-gnu")), ())

    def test_native_compiler_follows_the_machine(self):
        for machine, flags in (("x86_64", ("-msse4.1",)), ("AMD64", ("-msse4.1",)), ("arm64", ()), ("aarch64", ())):
            with self.subTest(machine=machine), mock.patch.object(module.platform, "machine", return_value=machine):
                self.assertEqual(module.arch_flags(compiler()), flags)


class ModuleDefTest(unittest.TestCase):
    def test_exports_what_the_host_looks_up_as_data(self):
        with tempfile.TemporaryDirectory() as td:
            text = module.write_module_def(Path(td)).read_text()
        self.assertEqual(text.splitlines()[0], "EXPORTS")
        for name in module.MODULE_EXPORTS:
            self.assertIn(f"    {name} DATA", text.splitlines())

    def test_unchanged_def_file_is_not_rewritten(self):
        with tempfile.TemporaryDirectory() as td:
            path = module.write_module_def(Path(td))
            before = path.stat().st_mtime_ns
            module.write_module_def(Path(td))
            self.assertEqual(path.stat().st_mtime_ns, before)


class WindowsLinkTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        root = Path(self.tmp.name)
        self.sdk = root / "sdk"
        (self.sdk / "runtime" / "spu").mkdir(parents=True)
        (self.sdk / "ABI").write_text("abi\n")
        (self.sdk / "runtime" / "spu" / "spu_dispatch_mt.c").write_text("")
        self.lift = root / "lift"
        self.lift.mkdir()
        (self.lift / "ppu_recomp_000.cpp").write_text("")
        self.spu = root / "spu"
        self.spu.mkdir()
        self.inp = module.ModuleInputs(self.sdk, self.lift, self.spu, root / "out")

    def test_missing_import_library_is_an_error(self):
        cc = compiler("-target", "x86_64-windows-gnu")
        with mock.patch.object(module, "_run", return_value=""):
            with self.assertRaises(module.ModuleBuildError):
                module.build_module(self.inp, cc, "windows", jobs=1, implib=None)
            with self.assertRaises(module.ModuleBuildError):
                module.build_module(self.inp, cc, "windows", jobs=1, implib=self.sdk / "libsc2hd_host.dll.a")

    def test_link_command_carries_the_def_file_and_the_import_library(self):
        implib = self.sdk / "libsc2hd_host.dll.a"
        implib.write_text("")
        cc = compiler("-target", "x86_64-windows-gnu")
        with mock.patch.object(module, "_run", return_value="") as run:
            module.build_module(self.inp, cc, "windows", jobs=1, implib=implib)
        link = run.call_args_list[-1].args[0]
        self.assertIn("-shared", link)
        self.assertIn(str(self.inp.out / "module.def"), link)
        self.assertIn(str(implib), link)


if __name__ == "__main__":
    unittest.main()
