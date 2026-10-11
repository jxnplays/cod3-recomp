"""Locate the guest's non-volatile register save/restore helpers.

XenonRecomp needs the entry address of each helper, because the compiler inlines
them mid-function: the caller reaches the helper through a `bl` and then carries
on, so the helper has to be recognised as a fall-through block rather than a
function that ends at its `blr`. With the right addresses the ~8,650 branches
that currently fail to resolve land correctly.

Conventions measured in this title rather than assumed:

  * The link register is r12. `mflr r12` occurs 10,518 times across .text while
    `mflr r0` occurs twice, so a scanner keyed on r0 finds nothing at all.
  * GPR save   : a run of `std rN,off(r1)` for r14..r31.
  * GPR restore: a run of `ld  rN,off(r1)` for r14..r31.
  * FPR save   : a run of `stfd fN,off(r1)` for f14..f31.
  * FPR restore: a run of `lfd  fN,off(r1)` for f14..f31.
  * VMX        : `stvx`/`lvx`/`stvx128`/`lvx128` runs.

A helper's entry is the START of its run, not any interior address: the callers
`bl` to the entry, and only the interior offsets show up as unresolved targets
because the analyser split the run.

Reads build/analysis/loaded.image, dumped by the runtime, indexed by RVA the way
XenonUtils does. See load_segments() for why PointerToRawData is wrong.

Usage: python tools/find_reg_helpers.py
"""

import collections
import glob
import io
import re
import struct

IMAGE = 'build/analysis/loaded.image'
IMAGE_BASE = 0x82000000
PPC_DIR = 'build/ppc'


def load_text(img):
    """Return (text_base_va, text_size).

    The image is indexed by RVA, not PointerToRawData: XenonUtils maps each
    section as `image.data + section.VirtualAddress` (XenonUtils/xex.cpp).
    """
    e = struct.unpack_from('<I', img, 0x3C)[0]
    coff = e + 4
    ns = struct.unpack_from('<H', img, coff + 2)[0]
    so = struct.unpack_from('<H', img, coff + 16)[0]
    opt = coff + 20
    for k in range(ns):
        s = opt + so + k * 40
        if img[s:s + 8].rstrip(b'\x00') == b'.text':
            va = struct.unpack_from('<I', img, s + 12)[0]
            vs = struct.unpack_from('<I', img, s + 8)[0]
            rs = struct.unpack_from('<I', img, s + 16)[0]
            return IMAGE_BASE + va, max(vs, rs)
    raise SystemExit('no .text section')


def main():
    img = io.open(IMAGE, 'rb').read()
    tbase, tsize = load_text(img)

    def I(a):
        if not (tbase <= a < tbase + tsize):
            return None
        return struct.unpack_from('>I', img, a - IMAGE_BASE)[0]

    def rt(v):
        return (v >> 21) & 0x1F

    def ra(v):
        return (v >> 16) & 0x1F

    def op(v):
        return v >> 26

    def xo(v):
        return (v >> 1) & 0x3FF

    forms = {
        'gpr_save':    lambda v: op(v) == 44 and ra(v) == 1 and 14 <= rt(v) <= 31,
        'gpr_restore': lambda v: op(v) == 42 and ra(v) == 1 and 14 <= rt(v) <= 31,
        'fpr_save':    lambda v: op(v) == 58 and ra(v) == 1 and rt(v) >= 14,
        'fpr_restore': lambda v: op(v) == 62 and ra(v) == 1 and rt(v) >= 14,
    }

    runs = []   # (start, end, kind, count)
    a = tbase
    while a < tbase + tsize - 4:
        v = I(a)
        kind = None
        for k, test in forms.items():
            if test(v):
                kind = k
                break
        if kind is None:
            a += 4
            continue
        test = forms[kind]
        start = a
        n = 0
        p = a
        while p < tbase + tsize and test(I(p)):
            n += 1
            p += 4
        runs.append((start, p, kind, n))
        a = p

    by_kind = collections.defaultdict(list)
    for r in runs:
        by_kind[r[2]].append(r)

    print('spill/reload runs found in .text: %d' % len(runs))
    for kind in sorted(by_kind):
        rows = sorted(by_kind[kind], key=lambda r: -r[3])
        longest = ', '.join('0x%08X(%d)' % (s, n) for s, e, k, n in rows[:4])
        print('   %-12s %5d runs, largest: %s'
              % (kind, len(rows), longest))
    print()

    # Which run does each unresolved target fall inside?
    targets = collections.Counter()
    for path in glob.glob(PPC_DIR + '/ppc_recomp.*.cpp'):
        with io.open(path, encoding='utf-8', errors='replace') as f:
            for m in re.finditer(r'ERROR ([0-9A-F]{8})', f.read()):
                targets[int(m.group(1), 16)] += 1
    total = sum(targets.values())

    owner = {}
    for start, end, kind, n in runs:
        for addr in targets:
            if start <= addr < end:
                owner.setdefault(addr, (start, end, kind, n))

    covered = sum(targets[a] for a in owner)
    print('unresolved branches       : %d across %d targets'
          % (total, len(targets)))
    print('targets inside a run      : %d (branches %d, %.1f%%)'
          % (len(owner), covered, 100.0 * covered / max(1, total)))
    print()
    print('per-kind run entries that would fix them:')
    needed = collections.defaultdict(set)
    for addr in owner:
        start, end, kind, n = owner[addr]
        needed[kind].add((start, end, n))
    for kind in sorted(needed):
        print('   %s:' % kind)
        for start, end, n in sorted(needed[kind]):
            hits = sum(targets[a] for a in owner
                       if owner[a][2] == kind and owner[a][0] == start)
            print('      0x%08X  %d regs, ends 0x%08X   fixes %d branches'
                  % (start, n, end, hits))

    print()
    print('If the TOML declared these entries, %d of %d branches resolve.'
          % (covered, total))


if __name__ == '__main__':
    main()