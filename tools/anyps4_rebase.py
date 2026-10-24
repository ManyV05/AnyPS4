"""Give a relinked PS4 executable a fixed image base below 4 GB.

The relinker writes a PE32+ with ImageBase 0x140000000 and DYNAMIC_BASE, so the
title's code and static data land wherever ASLR puts them (0x7ff7... in practice).
On the console the executable sits at 0x400000, which is also where shadPS4 maps
it (ExecutableLoadBase), and every static address fits in 32 bits; this title (a
PS3 engine port) keeps some static pointers in 32-bit fields and reassembles them
later, which is harmless there and a crash here (CUSA02799 loading a save: a read
of 0x478ed9b30, the low half of a real static address with a stray high half).

A rebase has to do what the Windows loader would do when the image is moved: add
the base delta to every pointer the .reloc section lists (the first attempt only
rewrote the ImageBase field, so the loader found the image "at its base", applied
no relocation, and wrote the TLS index through the stale 0x14437b028 pointer in
the TLS directory, an access violation before the entry point). With the pointers
rewritten and DYNAMIC_BASE cleared the loader keeps the image at the new base; if
the base is taken it still relocates through .reloc as usual.

Usage: python tools/anyps4_rebase.py <exe> [--base 0x400000] [--keep-aslr]
"""

from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

IMAGE_DLLCHARACTERISTICS_HIGH_ENTROPY_VA = 0x0020
IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE = 0x0040
IMAGE_REL_BASED_ABSOLUTE = 0
IMAGE_REL_BASED_HIGHLOW = 3
IMAGE_REL_BASED_DIR64 = 10


def rebase(path: Path, base: int = 0x400000, clear_aslr: bool = True) -> tuple[int, int, int]:
    data = bytearray(path.read_bytes())
    if data[:2] != b"MZ":
        raise ValueError(f"{path}: not a PE file")
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe : pe + 4] != b"PE\0\0":
        raise ValueError(f"{path}: bad PE signature")
    sections = struct.unpack_from("<H", data, pe + 6)[0]
    optional_size = struct.unpack_from("<H", data, pe + 20)[0]
    optional = pe + 24
    magic = struct.unpack_from("<H", data, optional)[0]
    if magic != 0x20B:
        raise ValueError(f"{path}: not a PE32+ image (magic {magic:#x})")
    old_base = struct.unpack_from("<Q", data, optional + 24)[0]
    characteristics = struct.unpack_from("<H", data, optional + 70)[0]
    if base % 0x10000 != 0 or base == 0:
        raise ValueError(f"image base {base:#x} must be a non-zero multiple of 64 KB")

    table = []
    offset = optional + optional_size
    for _ in range(sections):
        virtual_size, virtual_address, raw_size, raw_pointer = struct.unpack_from("<IIII", data, offset + 8)
        table.append((virtual_address, max(virtual_size, raw_size), raw_pointer, raw_size))
        offset += 40

    def file_offset(rva: int) -> int | None:
        for virtual_address, size, raw_pointer, raw_size in table:
            if virtual_address <= rva < virtual_address + raw_size:
                return raw_pointer + (rva - virtual_address)
        return None

    delta = (base - old_base) & 0xFFFFFFFFFFFFFFFF
    relocated = 0
    reloc_rva, reloc_size = struct.unpack_from("<II", data, optional + 112 + 5 * 8)
    if delta != 0 and reloc_rva and reloc_size:
        block = file_offset(reloc_rva)
        if block is None:
            raise ValueError(f"{path}: .reloc directory outside the file")
        end = block + reloc_size
        while block + 8 <= end:
            page, size = struct.unpack_from("<II", data, block)
            if size < 8:
                break
            for entry_offset in range(block + 8, block + size, 2):
                entry = struct.unpack_from("<H", data, entry_offset)[0]
                kind, low = entry >> 12, entry & 0xFFF
                if kind == IMAGE_REL_BASED_ABSOLUTE:
                    continue
                target = file_offset(page + low)
                if target is None:
                    raise ValueError(f"{path}: relocation target {page + low:#x} outside the file")
                if kind == IMAGE_REL_BASED_DIR64:
                    value = struct.unpack_from("<Q", data, target)[0]
                    struct.pack_into("<Q", data, target, (value + delta) & 0xFFFFFFFFFFFFFFFF)
                elif kind == IMAGE_REL_BASED_HIGHLOW:
                    value = struct.unpack_from("<I", data, target)[0]
                    struct.pack_into("<I", data, target, (value + delta) & 0xFFFFFFFF)
                else:
                    raise ValueError(f"{path}: unsupported relocation type {kind}")
                relocated += 1
            block += size

    struct.pack_into("<Q", data, optional + 24, base)
    if clear_aslr:
        characteristics &= ~(IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE | IMAGE_DLLCHARACTERISTICS_HIGH_ENTROPY_VA)
        struct.pack_into("<H", data, optional + 70, characteristics)
    struct.pack_into("<I", data, optional + 64, 0)
    path.write_bytes(data)
    return old_base, base, relocated


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("executable", type=Path)
    parser.add_argument("--base", type=lambda v: int(v, 0), default=0x400000)
    parser.add_argument("--keep-aslr", action="store_true", help="only move the preferred base, leave DYNAMIC_BASE set")
    args = parser.parse_args()
    old, new, relocated = rebase(args.executable, args.base, not args.keep_aslr)
    print(f"{args.executable}: image base {old:#x} -> {new:#x}, {relocated} pointers relocated{'' if args.keep_aslr else ', ASLR off'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
