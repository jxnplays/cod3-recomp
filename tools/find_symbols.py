#!/usr/bin/env python3
"""Locate Xbox 360 PPC register save/restore helper functions in an extracted PE image.

XenonRecomp needs the start address of each of these helpers in its TOML config
because their prologues/epilogues overlap with ordinary instruction boundaries.
The byte patterns below come from the XenonRecomp README; this script finds them
by signature so the config does not have to be hand-edited per title.

Usage:
    python find_symbols.py <image> [--all]
"""

import argparse
import struct
import sys

# name -> (pattern, human description)
PATTERNS = {
    "restgprlr_14": (bytes.fromhex("e9c1ff68"),
                     "__restgprlr_14  ld r14, -0x98(r1)"),
    "savegprlr_14": (bytes.fromhex("f9c1ff68"),
                     "__savegprlr_14  std r14, -0x98(r1)"),
    "restfpr_14": (bytes.fromhex("c9ccff70"),
                   "__restfpr_14   lfd f14, -0x90(r12)"),
    "savefpr_14": (bytes.fromhex("d9ccff70"),
                   "__savefpr_14   stfd f14, -0x90(r12)"),
    "restvmx_14": (bytes.fromhex("3960fee07dcb60ce"),
                   "__restvmx_14   li r11,-0x120; lvx v14"),
    "savevmx_14": (bytes.fromhex("3960fee07dcb61ce"),
                   "__savevmx_14   li r11,-0x120; stvx v14"),
    "restvmx_64": (bytes.fromhex("3960fc00100b60cb"),
                   "__restvmx_64   li r11,-0x400; lvx128 v64"),
    "savevmx_64": (bytes.fromhex("3960fc00100b61cb"),
                   "__savevmx_64   li r11,-0x400; stvx128 v64"),
}

SCN_CNT_CODE = 0x00000020


def be32(buf, off):
    return struct.unpack_from(">I", buf, off)[0]


def parse_sections(image):
    e_lfanew = struct.unpack_from("<I", image, 0x3C)[0]
    coff = e_lfanew + 4
    num_sections = struct.unpack_from("<H", image, coff + 2)[0]
    size_opt = struct.unpack_from("<H", image, coff + 16)[0]
    opt = coff + 20
    pe_base = struct.unpack_from("<I", image, opt + 0x1C)[0]
    sec_off = opt + size_opt
    out = []
    for i in range(num_sections):
        s = sec_off + i * 40
        name = image[s:s + 8].rstrip(b"\x00").decode("ascii", "replace")
        vsize = struct.unpack_from("<I", image, s + 8)[0]
        vaddr = struct.unpack_from("<I", image, s + 12)[0]
        rawptr = struct.unpack_from("<I", image, s + 20)[0]
        chars = struct.unpack_from("<I", image, s + 36)[0]
        out.append((name, vaddr, vsize, rawptr, chars))
    return pe_base, out


def find_all(hay, needle):
    start = 0
    while True:
        i = hay.find(needle, start)
        if i < 0:
            return
        yield i
        start = i + 4  # instructions are 4-byte aligned


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("image")
    ap.add_argument("--all", action="store_true", help="show every hit, not just the first")
    args = ap.parse_args()

    with open(args.image, "rb") as f:
        image = f.read()

    pe_base, sections = parse_sections(image)
    print(f"image: {args.image}")
    print(f"PE base: 0x{pe_base:08X}\n")

    code_sections = [(n, va, vs, rp) for (n, va, vs, rp, c) in sections if c & SCN_CNT_CODE]
    for n, va, vs, rp in code_sections:
        print(f"  code section {n}: RVA 0x{va:08X} size 0x{vs:08X}")

    results = {}
    for name, (pat, desc) in PATTERNS.items():
        hits = []
        for n, va, vs, rp in code_sections:
            blob = image[rp:rp + vs]
            for off in find_all(blob, pat):
                rva = va + off
                hits.append((rva, pe_base + rva, n))
        results[name] = hits
        print(f"\n{name}  ({desc})")
        print(f"  pattern: {pat.hex(' ')}")
        if not hits:
            print("  NOT FOUND")
            continue
        shown = hits if args.all else hits[:3]
        for rva, va_, sec in shown:
            print(f"  RVA 0x{rva:08X}  VA 0x{va_:08X}  ({sec})")
        if not args.all and len(hits) > len(shown):
            print(f"  ... {len(hits) - len(shown)} more (use --all)")

    # Emit a TOML fragment the user can paste into the XenonRecomp config.
    print("\n--- TOML fragment ---")
    for name, hits in results.items():
        if hits:
            print(f"{name}_address = 0x{hits[0][1]:08X}")


if __name__ == "__main__":
    main()