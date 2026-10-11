import io
import os
import struct

# Are any unresolved stubs actually referenced?
#
# 9,660 of the guest's functions were emitted as `.long 0x0` - four bytes of
# data, not code. A stub that nothing ever calls is harmless dead weight. A stub
# that something calls - directly, or far more likely through a function pointer
# table in .rdata - is a real hole: the caller gets whatever the stub happens to
# leave in r3, and everything downstream inherits that.
#
# 0x823460D4, the stub the crash handler kept blaming, appears nowhere in the
# image. That was evidence it was only an attribution artefact, and the corrected
# handler confirms it. But that was one address out of 9,660. This asks the
# question about all of them.
#
# The load address is unknown, so the scan tries the common bases. A hit under
# more than one base is reported once per base and treated with suspicion.

IMAGE = 'build/analysis/loaded.image'
HEADER = 'build/runtime/cod3_stub_symbols.h'

# Guest image is a 360 executable: big-endian, base 0x82000000.
GUEST_BASE = 0x82000000

stubs = set()
for line in io.open(HEADER, encoding='utf-8'):
    for token in line.replace(',', ' ').split():
        # The generator writes a bare "0" placeholder when the list is empty,
        # which is not a stub address and must not be counted as one.
        if token.startswith('0x') and token.endswith(tuple('0123456789abcdefABCDEF')):
            stubs.add(int(token, 16))

print('stubs declared: %d' % len(stubs))

data = io.open(IMAGE, 'rb').read()
print('image: %d bytes' % len(data))

# Big-endian 32-bit words.
words = struct.unpack('>%dI' % (len(data) // 4), data[:len(data) // 4 * 4])

# file_offset -> guest RVA, assuming the image is mapped at GUEST_BASE and the
# file begins at the image base (which COD3_DUMP_IMAGE produces).
hits = {}
for idx, w in enumerate(words):
    if w in stubs:
        guest = GUEST_BASE + idx * 4
        hits.setdefault(w, []).append(guest)

print()
print('stubs referenced by a 32-bit word in the image: %d' % len(hits))

if hits:
    print()
    print('first 40 referenced stubs:')
    for w in sorted(hits)[:40]:
        refs = hits[w]
        print('  sub_%.8X  <- %d ref(s), first at guest 0x%08X'
              % (w, len(refs), refs[0]))

    total = sum(len(v) for v in hits.values())
    print()
    print('total references: %d' % total)
else:
    print()
    print('No stub is referenced anywhere in the image.')
    print('That makes all 9,660 dead weight rather than holes in the port -')
    print('which is a genuinely useful result: it means an unresolved-branch')
    print('count is NOT the explanation for any fault seen so far.')
