import io
import glob
import re
from collections import Counter

# Boot now writes to 0x7FC80000, the Xenos GPU command processor. That is not a
# bug to fix - it is the boundary the whole graphics port has to be built around.
#
# Size the job before starting it: how many distinct MMIO addresses does the
# guest touch, and through which instruction forms? XenonRecomp routes these
# through PPC_MM_STORE_* / PPC_MM_LOAD_* helpers rather than plain loads and
# stores, so the helpers are where interception has to hook.
#
# Distinct addresses matter more than total count: a handful of command
# processor registers plus ring buffer doorbells is tractable, thousands is not.

MMIO = re.compile(r'PPC_MM_(STORE|LOAD)_(U8|U16|U32|U64)\(')

locs = Counter()
forms = Counter()
total = 0
per_fn = Counter()

for path in sorted(glob.glob('build/ppc/ppc_recomp.*.cpp')):
    s = io.open(path, encoding='utf-8', errors='replace').read()
    func_starts = [(m.start(), m.group(1))
                   for m in re.finditer(r'PPC_FUNC_IMPL\(__imp__(\w+)\)', s)]

    for m in MMIO.finditer(s):
        i = m.start()
        total += 1
        forms[m.group(1) + '_' + m.group(2)] += 1

        fn = '?'
        for pos, name in func_starts:
            if pos < i:
                fn = name
            else:
                break
        per_fn[fn] += 1

        # Find the address expression: base register + displacement.
        seg = s[i:i + 160]
        d = re.search(r'ctx\.(\w+)\.u32 \+ (\d+)', seg)
        if d:
            locs[(d.group(1), int(d.group(2)))] += 1
        else:
            locs[('<computed>', 0)] += 1

print('total MMIO accesses in the recompiled guest: %d' % total)
print()
print('by instruction form:')
for k, v in forms.most_common():
    print('   PPC_MM_%-14s %6d' % (k, v))
print()
print('distinct (base register, displacement) pairs: %d' % len(locs))
for (reg, disp), v in locs.most_common(15):
    print('   %-12s + 0x%05X   %6d' % (reg, disp, v))
print()
print('functions containing MMIO accesses: %d' % len(per_fn))
for k, v in per_fn.most_common(10):
    print('   %-18s %6d' % (k, v))
