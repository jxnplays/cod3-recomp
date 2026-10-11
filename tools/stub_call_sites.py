import io
import glob
import re
import sys

# Read the register setup at each call site of an import.
#
# The ExCreateThread and KeCreateThread stubs read the documented registers
# (r8 and r5 respectively) and still receive an ADDRESS where stackSize belongs.
# The documented shape is therefore not the one this title uses, and guessing a
# third shape is how the last two wrong answers happened. Read it instead.
#
# These are imports, so the emitted call is `__imp__NAME(ctx, base);` - there is
# no branch to them.

TARGETS = sys.argv[1:] or ['ExCreateThread', 'KeCreateThread']

for target in TARGETS:
    call = '__imp__%s(' % target
    hits = []

    for path in sorted(glob.glob('build/ppc/ppc_recomp.*.cpp')):
        s = io.open(path, encoding='utf-8', errors='replace').read()
        if call not in s:
            continue

        func_starts = [(m.start(), m.group(1))
                       for m in re.finditer(r'PPC_FUNC_IMPL\(__imp__(\w+)\)', s)]

        for m in re.finditer(re.escape(call), s):
            i = m.start()
            fn = '?'
            for pos, name in func_starts:
                if pos < i:
                    fn = name
                else:
                    break
            hits.append((fn, path, s, i))

    print('=' * 74)
    print('%s: %d call site(s)' % (target, len(hits)))
    print('=' * 74)

    shown = 0
    seen = set()
    for fn, path, s, i in hits:
        lines = s[:i].split('\n')[-40:]
        pairs = []
        note = None
        for line in lines:
            t = line.strip()
            if not t:
                continue
            mn = re.match(r'//\s*(.*)', t)
            if mn:
                note = mn.group(1)
                continue
            asg = re.search(r'ctx\.(r\d+)\.(?:u\d+|s\d+) = (.*);', t)
            if asg:
                val = asg.group(2).strip()
                # Only register arguments matter, and only those set close to
                # the call. Skip callee-saved shuffling.
                reg = asg.group(1)
                if reg in ('r3', 'r4', 'r5', 'r6', 'r7', 'r8', 'r9', 'r10'):
                    pairs.append('%s = %-30s // %s' % (reg, val[:30], note or ''))
                    note = None

        key = tuple(pairs[-6:])
        if key in seen:
            continue
        seen.add(key)

        print('  in %s  (%s)' % (fn, path.split('ppc_recomp.')[-1]))
        for p in pairs[-8:]:
            print('      %s' % p)
        print()
        shown += 1
        if shown >= 2:
            break
