import io

# [0x9CC21E00] holds 0x80 where a pointer belongs. The reservation is zeroed, so
# the guest wrote it. Find what writes offset +12180 (0x2F84) relative to the
# base it is given.
p = 'tools/scan_offset.py'

import glob
import re

TARGET = 12180

hits = {}
for f in sorted(glob.glob('build/ppc/ppc_recomp.*.cpp')):
    s = io.open(f, encoding='utf-8', errors='replace').read()
    # stores to <reg> + 12180, and loads from the same, so both sides are visible
    for m in re.finditer(r'ctx\.r(\w+)\.u32 \+ %d' % TARGET, s):
        i = m.start()
        j = s.rfind('PPC_FUNC_IMPL(', 0, i)
        nm = re.search(r'__imp__?(\w+)\)', s[j:j + 120])
        ctx = s[max(0, i - 300):i + 300]
        kind = 'STORE' if 'PPC_STORE' in ctx.split('ctx.r%s.u32 + %d' % (m.group(1), TARGET))[0][-200:] else '?'
        hits.setdefault(nm.group(1) if nm else '?', []).append(
            (m.group(1), 'STORE' if 'PPC_STORE_U32(ctx.r%s.u32 + %d,' % (m.group(1), TARGET) in ctx else 'LOAD'))

for k in sorted(hits):
    print('%-18s %s' % (k, sorted(set(hits[k]))))