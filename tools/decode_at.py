"""Decode the guest image around the addresses XenonRecomp failed to resolve.

Reads build/analysis/loaded.image, which is dumped by the runtime and therefore
comes from the same loader XenonRecomp used. See tools/find_reg_helpers.py for
why the standalone extractor is not used.

Usage: python tools/decode_at.py 0x8234F2B0 0x8234F310
"""

import io
import struct
import sys

IMAGE = 'build/analysis/loaded.image'
IMAGE_BASE = 0x82000000

BLR = 0x4E800020


def load_segments(img):
    """Parse the PE section table.

    IMPORTANT: the address of an instruction is its RVA, not its file offset.
    XenonUtils maps each section as `image.data + section.VirtualAddress`
    (XenonUtils/xex.cpp), so the guest byte for VA X lives at file offset X in
    the loaded image. PointerToRawData is NOT how the image is indexed, and
    using it decodes the wrong bytes for every address.
    """
    e = struct.unpack_from('<I', img, 0x3C)[0]
    coff = e + 4
    ns = struct.unpack_from('<H', img, coff + 2)[0]
    so = struct.unpack_from('<H', img, coff + 16)[0]
    opt = coff + 20
    segs = []
    for k in range(ns):
        s = opt + so + k * 40
        name = img[s:s + 8].rstrip(b'\x00').decode('ascii', 'replace')
        va = struct.unpack_from('<I', img, s + 12)[0]
        vs = struct.unpack_from('<I', img, s + 8)[0]
        rp = struct.unpack_from('<I', img, s + 20)[0]
        rs = struct.unpack_from('<I', img, s + 16)[0]
        segs.append((name, va, vs, rp, rs))
    return segs


def decode(ins, pc):
    """Decode one guest instruction.

    Kept deliberately small. A full decoder was tried and produced three wrong
    answers that each sent debugging in the wrong direction - opcode 21 covers
    add, subf, neg, cmpw, cmplw, cntlzw, slw and sraw depending on the extended
    opcode and the S bit, so guessing at it is not safe.

    Where this is unsure it says so. The authoritative text for any address is
    the `// <mnemonic>` comment XenonRecomp emitted above the corresponding line
    in build/ppc/ppc_recomp.*.cpp - that is the same decoder that produced the
    code actually running, so prefer it.
    """
    op = ins >> 26
    rt = (ins >> 21) & 0x1F
    ra = (ins >> 16) & 0x1F
    rb = (ins >> 11) & 0x1F
    si = ins & 0xFFFF
    if si & 0x8000:
        si -= 0x10000

    if ins == BLR:
        return 'blr'
    if op == 19:
        bo = (ins >> 21) & 0x1F
        bi = (ins >> 21) & 0x3F & 0x1F
        # bclr / bcctr: BO bit 0 selects the CTR branch
        if bo & 0x04 == 0:
            return 'bctr' if (bo & 0x10) == 0 else 'bctrl'
        return 'bcond (BO=%d BI=%d)' % (bo, (ins >> 21) & 0x1F)
    if op == 18:
        d = (ins >> 2) & 0xFFFFFF
        if d & 0x800000:
            d -= 0x1000000
        return 'b%s 0x%08X' % ('l' if (ins & 1) else '', pc + (d << 2))
    if op == 16:
        d = ins & 0xFFFC
        if d & 0x8000:
            d -= 0x10000
        return 'bc%s +%d' % ('l' if (ins & 1) else '', d)
    if op == 14:
        return 'addi r%d,r%d,%d' % (rt, ra, si)
    if op == 15:
        return 'addis r%d,r%d,%d' % (rt, ra, si)
    if op in (32, 33, 34, 35, 36, 37, 38, 40, 41, 42, 43, 44, 45, 46, 47):
        names = {32: 'lwz', 33: 'lwzu', 34: 'lbz', 36: 'stw', 37: 'stwu',
                 40: 'stb', 42: 'ld', 43: 'ldu', 44: 'stb_', 45: 'stbu',
                 46: 'lmw', 47: 'stmw'}
        nm = names.get(op, 'op%d' % op)
        if op == 37:
            return 'stwu r%d,%d(r%d)' % (rt, si, ra)
        return '%s r%d,%d(r%d)' % (nm, rt, si, ra)
    if op == 58:
        return 'stfd f%d,%d(r%d)' % (rt, si, ra)
    if op == 62:
        return 'lfd f%d,%d(r%d)' % (rt, si, ra)
    if op == 63:
        return 'stfs/stfdup'
    if op == 31:
        xo = (ins >> 1) & 0x3FF
        if xo == 444:
            spr = (ins >> 16) & 0x1F
            return 'mt%s r%d' % ('lr' if spr == 8 else 'ctr' if spr == 9
                                else 'spr%d' % spr, rt)
        if xo == 339:
            spr = (ins >> 16) & 0x1F
            return 'mf%s r%d' % ('lr' if spr == 8 else 'ctr' if spr == 9
                                else 'spr%d' % spr, rt)
        if xo == 23:
            return 'lwzx r%d,r%d,r%d' % (rt, ra, rb)
        if xo == 87:
            return 'stbx'
        if xo == 151:
            return 'stwx r%d,r%d,r%d' % (rt, ra, rb)
        if xo == 21:
            return 'lhzx'
        if xo == 533:
            return 'lfsx'
        if xo == 597:
            return 'lswx'
        if xo == 661:
            return 'stfsx'
        if xo == 725:
            return 'stswx'
        return 'op31 XO=%d' % xo
    if op == 7:
        return 'mulli r%d,%d' % (rt, si)
    if op == 10:
        return 'cmpli cr%d,r%d,%d' % (rt >> 2, rt & 3, si)
    if op == 11:
        return 'cmpi cr%d,r%d,%d' % (rt >> 2, rt & 3, si)
    if op == 24:
        return 'ori r%d,r%d,0x%X' % (rt, ra, ins & 0xFFFF)
    if op == 25:
        return 'oris r%d,r%d,0x%X' % (rt, ra, ins & 0xFFFF)
    if op == 26:
        return 'xori'
    if op == 27:
        return 'xoris'
    if op == 3:
        return 'twi'
    return 'op%d' % op


def main():
    start = int(sys.argv[1], 0) if len(sys.argv) > 1 else 0x8234F2B0
    end = int(sys.argv[2], 0) if len(sys.argv) > 2 else start + 4
    if end <= start:
        end = start + 4

    img = io.open(IMAGE, 'rb').read()
    segs = load_segments(img)

    def off_for(va):
        # See load_segments(): the image is indexed by RVA.
        for name, sva, svs, srp, srs in segs:
            if sva <= va - IMAGE_BASE < sva + max(svs, srs):
                return va - IMAGE_BASE, name
        return None, None

    a = start
    while a < end:
        off, name = off_for(a)
        if off is None:
            print('0x%08X  <no section>' % a)
            a += 4
            continue
        ins = struct.unpack_from('>I', img, off)[0]
        print('0x%08X: %08X   %-28s  ; %s' % (a, ins, decode(ins, a), name))
        a += 4


if __name__ == '__main__':
    main()