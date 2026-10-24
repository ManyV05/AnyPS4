"""Give a relinked PS4 executable a fixed image base below 4 GB.

The relinker writes a PE32+ with ImageBase 0x140000000 and DYNAMIC_BASE, so the
title's code and static data land wherever ASLR puts them (0x7ff7... in practice).
On the console the executable sits at 0x400000 and every static address fits in
32 bits; this title (a PS3 engine port) keeps some static pointers in 32-bit
fields and reassembles them later, which is harmless there and a crash here
(CUSA02799 loading a save: a read of 0x478ed9b30, the low half of a real static
address with a stray high half). Rebasing the image to 0x400000 and clearing
DYNAMIC_BASE would restore the console's address regime, but the relinked
image does not start there (Windows reports 0xc0000005 before the entry
point: the relinker's generated startup also carries addresses computed from
its constant ImageBase that the .reloc section does not cover), so the
installer leaves the base alone by default. Clearing DYNAMIC_BASE alone
(--base 0x140000000) is known to work and gives a fixed, repeatable base.

Usage: python tools/anyps4_rebase.py <exe> [--base 0x400000] [--keep-aslr]
"""

from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

IMAGE_DLLCHARACTERISTICS_HIGH_ENTROPY_VA = 0x0020
IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE = 0x0040


def rebase(path: Path, base: int = 0x400000, clear_aslr: bool = True) -> tuple[int, int]:
    data = bytearray(path.read_bytes())
    if data[:2] != b"MZ":
        raise ValueError(f"{path}: not a PE file")
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe : pe + 4] != b"PE\0\0":
        raise ValueError(f"{path}: bad PE signature")
    optional = pe + 24
    magic = struct.unpack_from("<H", data, optional)[0]
    if magic != 0x20B:
        raise ValueError(f"{path}: not a PE32+ image (magic {magic:#x})")
    old_base = struct.unpack_from("<Q", data, optional + 24)[0]
    characteristics = struct.unpack_from("<H", data, optional + 70)[0]
    if base % 0x10000 != 0 or base == 0:
        raise ValueError(f"image base {base:#x} must be a non-zero multiple of 64 KB")
    struct.pack_into("<Q", data, optional + 24, base)
    if clear_aslr:
        characteristics &= ~(IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE | IMAGE_DLLCHARACTERISTICS_HIGH_ENTROPY_VA)
        struct.pack_into("<H", data, optional + 70, characteristics)
    struct.pack_into("<I", data, optional + 64, 0)
    path.write_bytes(data)
    return old_base, base


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("executable", type=Path)
    parser.add_argument("--base", type=lambda v: int(v, 0), default=0x400000)
    parser.add_argument("--keep-aslr", action="store_true", help="only change the preferred base, leave DYNAMIC_BASE set")
    args = parser.parse_args()
    old, new = rebase(args.executable, args.base, not args.keep_aslr)
    print(f"{args.executable}: image base {old:#x} -> {new:#x}{'' if args.keep_aslr else ', ASLR off'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
