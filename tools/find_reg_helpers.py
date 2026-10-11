"""Locate the guest's non-volatile register save/restore helper routines.

XenonRecomp needs the entry address of each of these, because the compiler
inlines them mid-function: a `bl` to the helper is followed by more code in the
caller, so the helper must be recognised as a fall-through block rather than a
function that ends at the `blr`.

The helpers have a very regular shape. A restore helper ends with:

    lwz   r0, 8(r1)      # 8101 0008
    mtlr  r0             # 7C08 0003
    addi  r1, r1, N      # 3821 xxxx   (N = frame size)
    blr                  # 4E80 0020

and a save helper begins with:

    mflr  r0             # 7C08 02A6
    stw   r0, 8(r1)      # 9001 0008
    stwu  r1, -N(r1)     # 9421 xxxx

This scans .text for both signatures and reports the candidate entry points,
grouped by the number of GPRs (or FPRs) they touch.

Usage: python tools/find_reg_helpers.py
"""

import io
import re
import struct
import collections

IMAGE = 'build/analysis/default.image'
IMAGE_BASE = 0x82000000


def sections(img):
    e = struct.unpack_from('<I', img, 0x3C)[0]
    coff = e + 4
    ns = struct.unpack_from('<H', img, coff + 2)[0]
    so = struct.unpack_from('<H', img, coff + 16)[0]
    opt = coff + 20
    out = {}
    for k in range(ns):
        s = opt + so + k * 40
        name = img[s:s + 8].rstrip(b'\x00').decode()
        out[name] = (struct.unpack_from('<I', img, s + 12)[0],
                     struct.unpack_from('<I', img, s + 8)[0],
                     struct.unpack_from('<I', img, s + 20)[0])
    return out


def main():
    img = io.open(IMAGE, 'rb').read()
    secs = sections(img)
    tva, tvs, trp = secs['.text']
    base = IMAGE_BASE + tva

    def I(a):
        return struct.unpack_from('>I', img, trp + (a - base))[0]

    size = tvs

    # Collect every address XenonRecomp failed to resolve, so we can report
    # coverage rather than a raw scan.
    targets = collections.Counter()
    import glob
    for path in glob.glob('build/ppc/ppc_recomp.*.cpp'):
        with io.open(path, encoding='utf-8', errors='replace') as f:
            for m in re.finditer(r'ERROR ([0-9A-F]{8})', f.read()):
                targets[int(m.group(1), 16)] += 1

    # GPR restore helper: a run of `ld rN,off(r1)` then lwz r0,8(r1); mtlr r0;
    # addi r1,r1,N; blr
    restores = []
    for off in range(0, size - 16, 4):
        a = base + off
        # locate the blr
        if I(a + 12) != 0x4E800020:
            continue
        # addi r1,r1,N  (3821 xxxx)
        if (I(a + 8) & 0xFFFF0000) != 0x38210000:
            continue
        # mtlr r0 (7C08 0003)
        if I(a + 4) != 0x7C080003:
            continue
        # lwz r0,8(r1)
        if I(a) != 0x81010008:
            continue
        # count preceding ld rN,off(r1) instructions
        n = 0
        p = a - 4
        while p >= base:
            v = I(p)
            if (v & 0xFC0007FF) == 0xE8000000 or (v & 0xFC00003F) == 0xE8000000:
                rt = (v >> 21) & 0x1F
                if rt < 14:  # r14..r31 are the volatile-saved set here
                    break
                n += 1
                p -= 4
                continue
            break
        if n:
            restores.append((p + 4, n))

    print('total unresolved branches : %d' % sum(targets.values()))
    print('distinct unresolved addrs: %d' % len(targets))
    print()
    print('GPR restore helpers found (ld rN.. / lwz r0,8(r1) / mtlr / addi / blr):')
    for addr, n in sorted(restores, key=lambda x: -x[1])[:20]:
        hit = targets.get(addr, 0)
        print('   0x%08X  regs=%-3d  %s' %
              (addr, n, ('<-- %d branches land here' % hit) if hit else ''))

    print()
    covered = sum(targets.get(a, 0) for a, _ in restores)
    print('branches landing on a discovered restore helper: %d of %d (%.1f%%)'
          % (covered, sum(targets.values()),
             100.0 * covered / max(1, sum(targets.values()))))


if __name__ == '__main__':
    main()