#!/usr/bin/env python3
"""Turn the two MultiStream DSP plugins of Soulcalibur II HD into liftable SPU images.

MultiStream's SPU program loads these plugins by DMA into local store at 0x33680 and calls them there
(the code is position independent and is not patched after the copy). The extracted images
(spu_0003_at_00441A80, spu_0004_at_00443D00) are lifted at their own virtual addresses (0x80), where the
running SPU never executes them. This writes a copy of each with its code placed at the real address, and a
registration file that tells the runtime which streamed content is which (by its first 16 bytes).

usage: make_sc2_overlays.py prepare <spu/images dir> <spu dir>    (before lifting: writes the images + overlay_*.txt)
       make_sc2_overlays.py emit <spu dir>                         (after lifting: writes sc2_overlays.c)
"""
import re
import glob
import os
import struct
import sys

LS_BASE = 0x33680                         # LS address of file offset 0x80 of the plugin image
EXTRA_ENTRIES = {0x33FB8, 0x34528}
# The images are named spu_<index>_at_<offset in the EBOOT>; the offset differs between game versions, the index does not.
PLUGINS = [("spu_0003", "spu_0005_ovl_plugin_a"), ("spu_0004", "spu_0006_ovl_plugin_b")]
MULTISTREAM = "spu_0002"


def u32(b, o):
    return struct.unpack(">I", b[o:o + 4])[0]


def text_span(elf):
    phoff = u32(elf, 28)
    phentsize, phnum = struct.unpack(">HH", elf[42:46])
    for i in range(phnum):
        t, off, va, _pa, fs, _ms, fl, _al = struct.unpack(">IIIIIIII", elf[phoff + i * phentsize:phoff + (i + 1) * phentsize])
        if t == 1 and fl & 1:
            return off, va, fs
    raise SystemExit("no executable segment")


def synth_elf(body, entry):
    # ELF32 big-endian SPU executable, one PT_LOAD at LS_BASE
    ehsize, phentsize = 52, 32
    hdr = b"\x7fELF" + bytes([1, 2, 1, 0]) + bytes(8)
    hdr += struct.pack(">HHIIIIIHHHHHH", 2, 23, 1, entry, ehsize, 0, 0, ehsize, phentsize, 1, 0, 0, 0)
    off = 0x80
    ph = struct.pack(">IIIIIIII", 1, off, LS_BASE, LS_BASE, len(body), len(body), 5, 0x80)
    return (hdr + ph).ljust(off, b"\0") + body


def emit(spudir):
    wl = open(os.path.join(spudir, "spu_workloads.c")).read()
    lines = ["#include <stdint.h>", "extern void spu_overlay_register_sig(const uint8_t sig[16], int image_id);",
             "__attribute__((constructor)) static void sc2hd_register_overlays(void)", "{"]
    for line in open(os.path.join(spudir, "overlay_sigs.txt")):
        name, sig = line.split()
        m = re.search(r'spu_workload_register_img\(0x[0-9A-F]+ULL, \w+, (\d+), "%s"\)' % re.escape(name), wl)
        if not m:
            raise SystemExit(f"{name} is not in spu_workloads.c")
        arr = ", ".join("0x" + sig[i:i + 2] for i in range(0, 32, 2))
        lines.append(f"    {{ static const uint8_t sig[16] = {{ {arr} }}; spu_overlay_register_sig(sig, {m.group(1)}); }}   /* {name} */")
    lines.append("}")
    open(os.path.join(spudir, "sc2_overlays.c"), "w").write("\n".join(lines) + "\n")


def main():
    if sys.argv[1] == "emit":
        return emit(sys.argv[2])
    images, spudir = sys.argv[2], sys.argv[3]
    regs, extras = [], {}
    for k, (src, dst) in enumerate(PLUGINS):
        path = glob.glob(os.path.join(images, src + "_at_*.elf"))
        if len(path) != 1:
            raise SystemExit(f"expected one {src}_at_*.elf in {images}, found {len(path)}")
        elf = open(path[0], "rb").read()
        off, va, fs = text_span(elf)
        # the DMA'd body starts at file offset 0x80 and covers the header words, the code and its data
        body = elf[0x80:off + fs]
        out = os.path.join(images, dst + ".elf")
        open(out, "wb").write(synth_elf(body, LS_BASE + 0x10))
        # entry points: the two stubs at the head, direct call targets, and every word that looks like a code pointer
        ent = {LS_BASE + 0x10, LS_BASE + 0x18} | EXTRA_ENTRIES   # extras: targets the running SPU program reached by an indirect call
        end = LS_BASE + len(body)
        for i in range(0, len(body) - 3, 4):
            w = u32(body, i)
            pc = LS_BASE + i
            if (w >> 23) in (0x66, 0x62):                    # brsl / brasl
                i16 = (w >> 7) & 0xFFFF
                i16 -= 0x10000 if i16 & 0x8000 else 0
                tgt = ((i16 << 2) & 0x3FFFF) if (w >> 23) == 0x62 else pc + (i16 << 2)
                if LS_BASE <= tgt < end:
                    ent.add(tgt)
                ent.add(pc + 4)
            elif (w >> 21) == 0x1A9:                         # bisl: return point
                ent.add(pc + 4)
            elif LS_BASE <= w < end and w % 4 == 0:
                ent.add(w)
        extras[dst] = sorted(ent)
        regs.append((dst, body[:16]))
        print(f"{os.path.basename(path[0])} -> {dst}: {len(body)} bytes at LS 0x{LS_BASE:05X}, {len(ent)} entry points")
    # MultiStream's own image: the return point of every call is an entry too. The SPU program returns
    # through a link register, and a return into the middle of a lifted function ended its thread.
    ms = glob.glob(os.path.join(images, MULTISTREAM + "_at_*.elf"))
    if len(ms) != 1:
        raise SystemExit(f"expected one {MULTISTREAM}_at_*.elf in {images}, found {len(ms)}")
    elf = open(ms[0], "rb").read()
    off, va, fs = text_span(elf)
    ret = {0x9BBC}
    for i in range(0, fs, 4):
        w = u32(elf, off + i)
        if (w >> 23) in (0x66, 0x62) or (w >> 21) == 0x1A9:
            ret.add(va + i + 4)
    extras[os.path.basename(ms[0])[:-len(".elf")]] = sorted(ret)
    with open(os.path.join(spudir, "lift_args.txt"), "w") as f:
        f.write(" ".join(f"--extra-funcs {k}={','.join(hex(e) for e in v)}" for k, v in extras.items()) + "\n")
    with open(os.path.join(spudir, "overlay_sigs.txt"), "w") as f:
        for dst, sig in regs:
            f.write(f"{dst} {sig.hex()}\n")


if __name__ == "__main__":
    main()
