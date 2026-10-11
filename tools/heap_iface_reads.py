import io
import glob
import re

# The heap descriptor's method table, from sub_824C2640:
#
#     stw r30,1404(r31)
#     stw r30,1408(r31)
#     stw r30,1412(r31)
#     stw r30,1416(r31)
#
# Four slots, all initialised to one value. That is a sub-interface: four
# consecutive method slots. Only +1412 was known to be read (sub_82345E48 tests
# and calls it), and only +1412 was filled with a trampoline.
#
# If the other three are also called and are still zero, then the descriptor has
# a one-quarter-populated method table and the fault is simply "called a null
# method". That would be a much better explanation than anything tried so far,
# and it is cheap to check: find every READ of these four offsets.
#
# Only reads matter here. Writes were already surveyed - nothing in the guest
# writes them for a heap descriptor, which is why the loader installs them.

OFFSETS = (1404, 1408, 1412, 1416, 1420)

reads = {o: {} for o in OFFSETS}
writes = {o: {} for o in OFFSETS}

for path in sorted(glob.glob('build/ppc/ppc_recomp.*.cpp')):
    s = io.open(path, encoding='utf-8', errors='replace').read()

    func_starts = [(m.start(), m.group(1))
                   for m in re.finditer(r'PPC_FUNC_IMPL\(__imp__(\w+)\)', s)]

    def owner(i):
        fn = '?'
        for pos, name in func_starts:
            if pos < i:
                fn = name
            else:
                break
        return fn

    for o in OFFSETS:
        for m in re.finditer(r'ctx\.r(\w+)\.u32 \+ %d\b' % o, s):
            i = m.start()
            # Look back a little for the store/load wrapper.
            before = s[max(0, i - 120):i]
            if 'PPC_STORE_U32(' in before[-60:]:
                writes[o].setdefault(owner(i), 0)
                writes[o][owner(i)] += 1
            else:
                reads[o].setdefault(owner(i), 0)
                reads[o][owner(i)] += 1

for o in OFFSETS:
    print('=' * 66)
    print('offset +%d' % o)
    print('  READS : %s' % (', '.join('%s x%d' % (k, v)
                                      for k, v in sorted(reads[o].items()))
                            or 'none'))
    print('  WRITES: %s' % (', '.join('%s x%d' % (k, v)
                                      for k, v in sorted(writes[o].items()))
                            or 'none'))
