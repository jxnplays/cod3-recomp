import struct

# Decode one PowerPC instruction word, carefully.
#
# Written after hand-decoding produced three wrong answers in a row, all of
# which sent debugging in the wrong direction. Getting opcode 21 right matters:
# it covers add, subf, neg, cmpw, cmplw, cntlzw, slw and sraw depending on the
# extended opcode and the S bit, so a wrong decode turns a comparison into an
# add and makes a branch look like an unconditional call.

def sign16(v):
    return v - 0x10000 if v & 0x8000 else v


def decode(v):
    op = (v >> 26) & 0x3F
    rt = (v >> 21) & 0x1F
    ra = (v >> 16) & 0x1F
    rb = (v >> 11) & 0x1F
    d = v & 0x3FF
    si = sign16(v & 0xFFFF)
    xo = (v >> 1) & 0x3FF
    rc = v & 1
    oe = (v >> 10) & 1
    S = (v >> 20) & 1

    # cmplw / cmpw: opcode 21 with the extended opcode field zero.
    # Bit layout: 31 crD rA rB 0 0 0 1 1 0 rc
    if op == 21 and (v & 0x000001FC) == 0x000000C8:
        return 'cmp%s cr%d,r%d,r%d' % ('lw' if S else 'w', rt, ra, rb)
    if op == 21 and (v & 0x000003FC) == 0x000002C8:
        return 'cmplwi cr%d,r%d,%d' % (rt, ra, si)
    if op == 11 and (v & 0x000001FC) == 0x00000008:
        return 'cmpwi cr%d,r%d,%d' % (rt, ra, si)
    if op == 10 and (v & 0x000003FC) == 0x00000288:
        return 'cmplwi cr%d,r%d,%d' % (rt, ra, si)

    # add/subf: opcode 21 with S=1, extended opcode in {8,10,40,104,235}
    if op == 21 and S == 1 and (v & 0x7C) != 0:
        names = {8: 'subfc', 10: 'addc', 40: 'subfe', 104: 'adde',
                 235: 'mulhwu', 459: 'divwe', 491: 'divweo'}
        nm = names.get((v >> 1) & 0x3FF)
        if nm:
            if 'div' in nm:
                return '%s r%d,r%d,r%d' % (nm, rt, ra, rb)
            return '%s r%d,r%d,r%d' % (nm, rt, ra, rb)

    # cntlzw / extsb / extsh: opcode 31
    if op == 31:
        names = {26: 'cntlzw', 922: 'extsb', 982: 'extsh', 339: 'mfspr',
                 467: 'mtspr', 23: 'lwzx', 87: 'stbx', 151: 'stwx',
                 533: 'lfsx', 661: 'stfsx', 597: 'lswx', 725: 'stswx',
                 535: 'lfsdx', 699: 'stfsdx', 598: 'lsdx', 726: 'stdsx',
                 536: 'lfdup', 695: 'stfdup'}
        # XO 444 is the record form of `or`, which is `mr` when RB is zero.
        if xo == 444:
            return 'mr r%d,r%d' % (ra, rt) if rb == 0 else \
                   'or r%d,r%d,r%d' % (ra, rt, rb)
        nm = names.get(xo)
        if nm == 'mfspr':
            spr = (v >> 16) & 0x1F
            n = {8: 'lr', 9: 'ctr'}.get(spr, 'spr%d' % spr)
            return 'mf%s r%d' % (n, rt)
        if nm == 'mtspr':
            spr = (v >> 16) & 0x1F
            n = {8: 'lr', 9: 'ctr'}.get(spr, 'spr%d' % spr)
            return 'mt%s r%d' % (n, rt)
        if nm and nm.endswith('dup'):
            return '%s f%d' % (nm, rt)
        if nm and ('zx' in nm or 'wx' in nm):
            return '%s r%d,r%d,r%d' % (nm, rt, ra, rb)
        if nm and nm.startswith('ext'):
            return '%s r%d,r%d' % (nm, rt, ra)
        if nm == 'cntlzw':
            return 'cntlzw r%d,r%d' % (rt, ra)

    if v == 0x4E800020:
        return 'blr'
    if v == 0x4E800420:
        return 'bctr'
    if v == 0x4E800421:
        return 'bctrl'
    if op == 18:
        li = v & 1
        aa = (v >> 1) & 1
        disp = (v >> 2) & 0xFFFFFF
        if disp & 0x800000:
            disp -= 0x1000000
        return 'b%s%s 0x%08X' % ('l' if li else '', 'a' if aa else '',
                                 (v & 0xFFFF0000) + d + (disp << 2) if False else 0)
    if op == 16:
        disp = sign16(v & 0xFFFC)
        return 'bc%s %+d' % ('l' if (v & 1) else '', disp)
    if op == 14:
        return 'addi r%d,r%d,%d' % (rt, ra, si)
    if op == 15:
        return 'addis r%d,r%d,%d' % (rt, ra, si)
    if op == 7:
        return 'mulli r%d,%d' % (rt, si)
    if op == 32:
        return 'lwz r%d,%d(r%d)' % (rt, si, ra)
    if op == 36:
        return 'stw r%d,%d(r%d)' % (rt, si, ra)
    if op == 37:
        return 'stwu r%d,%d(r%d)' % (rt, si, ra)
    if op == 42:
        return 'ld r%d,%d(r%d)' % (rt, si, ra)
    if op == 44:
        return 'std r%d,%d(r%d)' % (rt, si, ra)
    if op == 58:
        return 'stfd f%d,%d(r%d)' % (rt, si, ra)
    if op == 62:
        return 'lfd f%d,%d(r%d)' % (rt, si, ra)
    return 'op%d 0x%08X' % (op, v)


if __name__ == '__main__':
    for w in [0x547E1838, 0x7D8802A6, 0x7FC3F378, 0x4E800020, 0x409A0018,
              0x2B0B0000, 0x7D7EF82E, 0x3BEB7110, 0x7C6B1B78, 0x91470210]:
        print('0x%08X  %s' % (w, decode(w)))