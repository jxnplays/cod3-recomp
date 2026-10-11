import io
import struct

# The control addresses cluster: 0x8208C710, 0x8208C738, 0x8208C758 - an 0x48
# byte span holding pointers to sub_823460D8, sub_82346D98 and sub_823478F0.
# That is a dispatch or vtable table, and it sits in the same neighbourhood as
# the heap code that faults.
#
# Dump it. A table of heap functions called through a pointer is exactly where a
# bad descriptor would turn into a bad call, and it would explain why the fault
# address is a small integer rather than a wild pointer.

IMAGE = 'build/analysis/loaded.image'
GUEST_BASE = 0x82000000

data = io.open(IMAGE, 'rb').read()

# Which section is 0x8C710 in? Read the dump's own idea if it has one, else infer.
print('table region around file offset 0x8C600..0x8C800')
print('-' * 68)

# Which generated functions exist at each pointer, so the table reads as code
# rather than as hex.
import glob
import re
known = {}
for f in sorted(glob.glob('build/ppc/ppc_recomp.*.cpp')):
    for m in re.finditer(r'PPC_FUNC_IMPL\(__imp__(sub_\w+)\)', io.open(f, encoding='utf-8', errors='replace').read()):
        known[int(m.group(1)[4:], 16)] = m.group(1)

stub = set()
for line in io.open('build/runtime/cod3_stub_symbols.h', encoding='utf-8'):
    for t in line.replace(',', ' ').split():
        if t.startswith('0x') and t.endswith(tuple('0123456789abcdefABCDEF')):
            stub.add(int(t, 16))

for off in range(0x8C600, 0x8C800, 4):
    v = struct.unpack_from('>I', data, off)[0]
    guest = GUEST_BASE + off

    if v in known:
        tag = 'sub_%.8X' % v
        if v in stub:
            tag += '   <-- UNRESOLVED STUB'
    elif 0x82000000 <= v < 0x8C00000:
        tag = '(in-image data/rodata)'
    else:
        tag = ''

    if tag:
        print('0x%08X  ->  0x%08X  %s' % (guest, v, tag))
