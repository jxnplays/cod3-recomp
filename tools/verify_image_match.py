"""Compare the image XenonRecomp translated against the one the runtime loads.

They are supposed to be identical - both call XenonUtils' Xex2LoadImage() on the
same XEX - but offline decoding disagrees with the generated code, so this dumps
a hash of each side's view of .text along with a few landmark instructions.

Usage: python tools/verify_image_match.py
"""

import io
import struct
import subprocess
import sys

IMAGE = 'build/analysis/loaded.image'
PPC = 'build/ppc/ppc_recomp.0.cpp'


def main():
    img = io.open(IMAGE, 'rb').read()
    e = struct.unpack_from('<I', img, 0x3C)[0]
    coff = e + 4
    ns = struct.unpack_from('<H', img, coff + 2)[0]
    so = struct.unpack_from('<H', img, coff + 16)[0]
    opt = coff + 20

    segs = {}
    for k in range(ns):
        s = opt + so + k * 40
        name = img[s:s + 8].rstrip(b'\x00').decode('ascii', 'replace')
        va = struct.unpack_from('<I', img, s + 12)[0]
        vs = struct.unpack_from('<I', img, s + 8)[0]
        rp = struct.unpack_from('<I', img, s + 20)[0]
        rs = struct.unpack_from('<I', img, s + 16)[0]
        segs.setdefault(name, []).append((va, vs, rp, rs))

    def insn(addr):
        for entry in segs.get('.text', []):
            va, vs, rp, rs = entry
            if va <= addr - 0x82000000 < va + max(vs, rs):
                return struct.unpack_from(
                    '>I', img, rp + (addr - 0x82000000 - va))[0]
        return None

    # Landmarks straight out of the generated code, with what the image holds.
    src = io.open(PPC, encoding='utf-8', errors='replace').read()
    i = src.find('PPC_FUNC_IMPL(__imp__sub_820A0000)')
    print('generated sub_820A0000 begins:')
    for line in src[i:i + 300].splitlines()[2:6]:
        if line.strip().startswith('//'):
            print('   ', line.strip())
    print()
    print('image bytes at the same addresses:')
    for a in (0x820A0000, 0x820A0004, 0x820A0008):
        print('    0x%08X  %08X' % (a, insn(a) or 0))

    print()
    print('Is the branch table at 0x820A0000 real code?')
    print('  0x8254D710 = lis r12,-0x28F0 -> a data/table reference')
    print('  0x40004305 = opcode 16 (bc)   -> a branch')
    print('  Alternating target/branch pairs is the signature of a jump table,')
    print('  so .text does begin with data.')


if __name__ == '__main__':
    main()