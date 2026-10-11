"""Explain where each unresolved branch target actually sits.

XenonRecomp could not resolve 9,270 branches to 92 distinct addresses. With the
image now decoded correctly (indexed by RVA, as XenonUtils does), this reports
what each target is, and - the point of the exercise - whether it lies INSIDE a
recognisable register save/restore helper. If it does, the fix is simply to give
XenonRecomp the right helper entry address in the TOML; the branch then resolves
because the compiler treats the helper as a fall-through block.

Usage: python tools/explain_targets.py
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
            rp = struct.unpack_from('<I', img, s + 20)[0]
            rs = struct.unpack_from('<I', img, s + 16)[0]
            return IMAGE_BASE + va, max(vs, rs)
    raise SystemExit('no .text')


def main():
    img = io.open(IMAGE, 'rb').read()
    tbase, tsize = load_text(img)

    def I(a):
        if not (tbase <= a < tbase + tsize):
            return None
        return struct.unpack_from('>I', img, a - IMAGE_BASE)[0]

    def is_std_r(v):      # std rN,off(r1)
        return (v >> 26) == 44 and ((v >> 16) & 0x1F) == 1 and \
            14 <= ((v >> 21) & 0x1F) <= 31

    def is_ld_r(v):       # ld rN,off(r1)
        return (v >> 26) == 42 and ((v >> 16) & 0x1F) == 1 and \
            14 <= ((v >> 21) & 0x1F) <= 31

    def is_stfd(v):       # stfd fN,off(r1)
        return (v >> 26) == 58 and ((v >> 16) & 0x1F) == 1 and \
            ((v >> 21) & 0x1F) >= 14

    def is_lfd(v):        # lfd fN,off(r1)
        return (v >> 26) == 62 and ((v >> 16) & 0x1F) == 1 and \
            ((v >> 21) & 0x1F) >= 14

    # Build every maximal run of the four spill/reload forms, recording where
    # each run starts.
    runs = []   # (start, end, kind, count)
    a = tbase
    while a < tbase + tsize - 4:
        v = I(a)
        if is_std_r(v) or is_stfd(v):
            kind = 'save'
            test = (is_std_r, is_stfd)
        elif is_ld_r(v) or is_lfd(v):
            kind = 'restore'
            test = (is_ld_r, is_lfd)
        else:
            a += 4
            continue
        start = a
        n = 0
        p = a
        while p < tbase + tsize:
            w = I(p)
            if any(t(w) for t in test):
                n += 1
                p += 4
                continue
            break
        runs.append((start, p, kind, n))
        a = p

    targets = collections.Counter()
    for path in glob.glob(PPC_DIR + '/ppc_recomp.*.cpp'):
        with io.open(path, encoding='utf-8', errors='replace') as f:
            for m in re.finditer(r'ERROR ([0-9A-F]{8})', f.read()):
                targets[int(m.group(1), 16)] += 1

    total = sum(targets.values())
    inside = {}
    for addr, count in targets.items():
        for start, end, kind, n in runs:
            if start <= addr < end:
                inside[addr] = (start, end, kind, n, addr - start)
                break

    print('unresolved branches      : %d' % total)
    print('distinct targets         : %d' % len(targets))
    print('spill/reload runs in .text: %d' % len(runs))
    print('targets inside such a run: %d  (branches %d, %.1f%%)'
          % (len(inside), sum(inside[a][0] and targets[a] for a in inside),
             100.0 * sum(targets[a] for a in inside) / max(1, total)))
    print()
    print('targets landing inside a spill/reload run:')
    for addr in sorted(inside, key=lambda x: -targets[x]):
        start, end, kind, n, off = inside[addr]
        print('   target 0x%08X  %5d branches  -> run 0x%08X..0x%08X '
              '(%s, %d regs), +%d bytes in'
              % (addr, targets[addr], start, end, kind, n, off))

    outside = [a for a in targets if a not in inside]
    print()
    print('targets NOT in a spill/reload run: %d (branches %d)'
          % (len(outside), sum(targets[a] for a in outside)))
    for addr in sorted(outside, key=lambda x: -targets[x])[:20]:
        v = I(addr) or 0
        print('   0x%08X  %5d branches  insn=0x%08X' % (addr, targets[addr], v))

    covered = sum(targets[a] for a in inside)
    print()
    print('If XenonRecomp were given these run starts, %d of %d branches '
          '(%d distinct targets) would resolve.'
          % (covered, total, len(inside)))


if __name__ == '__main__':
    main()