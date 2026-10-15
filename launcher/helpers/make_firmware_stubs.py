#!/usr/bin/env python3
"""Write stand-ins for the firmware SPU modules the game loads from /dev_flash.

MultiStream (the game's audio engine) reads /dev_flash/sys/external/flashMP3.pic and flashATRAC.pic into
memory at start-up and its SPU program maps them into local store. The port decodes MP3 on the host
(src/mp3_hle.c) and never runs the ATRAC decoder, so the loader only needs files of the same
size and shape: a valid SPU ELF with the same entry point, segments and sections as Sony's, whose code is `bi $r0`
at the entry points and zeros elsewhere. Nothing here is derived from the firmware images.

usage: make_firmware_stubs.py <dev_flash root to create>
"""
import os
import struct
import sys
from pathlib import Path

BI_R0 = 0x35000000
LOAD_FILE_BASE = 0x100   # file offset of the first segment, vaddr 0x80
LOAD_VADDR_BASE = 0x80

SHT_PROGBITS, SHT_NOBITS = 1, 8
SHF_WRITE, SHF_ALLOC, SHF_EXEC = 1, 2, 4

# The game's loader (FUN_0034ced4) reads the SECTION headers, not the program headers: the alloc sections give
# the image's span and its .bss, and e_entry the call target. (type, flags, addr, offset, size)
# name: (file size, entry, section table offset, [program headers], [sections], entry stubs (vaddrs of `bi $r0`))
MODULES = {
    "flashMP3.pic": (69695, 0x74A0, 0x10214,
                     [(0x100, 0x80, 0xFF10, 0xFF10, 5), (0x10080, 0x10000, 0x10, 0x3E0, 6)],
                     [(SHT_PROGBITS, SHF_ALLOC | SHF_EXEC, 0x80, 0x100, 0x10),
                      (SHT_PROGBITS, SHF_ALLOC | SHF_EXEC, 0x90, 0x110, 0x93A0),
                      (SHT_PROGBITS, SHF_ALLOC, 0x9480, 0x9500, 0x6B10),
                      (SHT_PROGBITS, SHF_ALLOC | SHF_WRITE, 0x10000, 0x10080, 0x10),
                      (SHT_NOBITS, SHF_ALLOC | SHF_WRITE, 0x10010, 0x10090, 0x3D0)],
                     [0x7480, 0x74A0]),
    "flashATRAC.pic": (35131, 0x90, 0x8060,
                       [(0x100, 0x80, 0x7D40, 0x7D40, 5), (0x7E80, 0x7E00, 0xA0, 0xF0, 6)],
                       [(SHT_PROGBITS, SHF_ALLOC | SHF_EXEC, 0x80, 0x100, 0x10),
                        (SHT_PROGBITS, SHF_ALLOC | SHF_EXEC, 0x90, 0x110, 0x5440),
                        (SHT_PROGBITS, SHF_ALLOC, 0x5500, 0x5580, 0x28C0),
                        (SHT_PROGBITS, SHF_ALLOC | SHF_WRITE, 0x7E00, 0x7E80, 0xA0),
                        (SHT_NOBITS, SHF_ALLOC | SHF_WRITE, 0x7EA0, 0x7F20, 0x50)],
                       [0x90]),
}


def build(size, entry, shoff, loads, sections, stubs):
    # ELF32, big endian, machine 0x17 (SPU); a NOTE program header follows the two LOADs as in Sony's files
    phdrs = [(1, o, v, v, fs, ms, fl, 0x80) for (o, v, fs, ms, fl) in loads]
    note_off = loads[1][0] + 0x10
    phdrs.append((4, note_off, 0, 0, 0x34, 0x34, 4, 4))
    ident = b"\x7fELF" + bytes([1, 2, 1, 0]) + bytes(8)
    ehdr = ident + struct.pack(">HHIIIIIHHHHHH", 2, 0x17, 1, entry, 0x34, shoff, 0, 0x34, 0x20, len(phdrs), 0x28, len(sections) + 1, 0)
    img = bytearray(size)
    img[:len(ehdr)] = ehdr
    for i, ph in enumerate(phdrs):
        img[0x34 + 32 * i:0x34 + 32 * (i + 1)] = struct.pack(">8I", *ph)
    # section 0 is the null entry
    for i, (typ, flags, addr, off, sz) in enumerate(sections, start=1):
        img[shoff + 40 * i:shoff + 40 * (i + 1)] = struct.pack(">10I", 0, typ, flags, addr, off, sz, 0, 0, 16, 0)
    for v in stubs:
        o = LOAD_FILE_BASE + (v - LOAD_VADDR_BASE)
        img[o:o + 4] = struct.pack(">I", BI_R0)
    return bytes(img)


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    out = Path(sys.argv[1]) / "sys" / "external"
    out.mkdir(parents=True, exist_ok=True)
    for name, (size, entry, shoff, loads, sections, stubs) in MODULES.items():
        data = build(size, entry, shoff, loads, sections, stubs)
        target = out / name
        # A running game may be reading the file (the launcher calls this on every launch): leave identical
        # files alone and replace changed ones atomically, never truncate in place.
        if target.exists() and target.read_bytes() == data:
            print(f"kept  {target} ({size} bytes)")
            continue
        tmp = target.with_name(f".{name}.{os.getpid()}.tmp")
        tmp.write_bytes(data)
        os.replace(tmp, target)
        print(f"wrote {target} ({size} bytes)")


if __name__ == "__main__":
    main()
