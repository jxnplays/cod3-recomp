import io
import glob
import re

# What does the caller DO with XamGetSystemVersion's six out-parameters?
#
# The signature is settled from the call site - six pointers in r3..r8 - but the
# values are not, and writing zeroes is a guess. Reading what the caller
# compares them against turns it into a measurement instead.

path = 'build/ppc/ppc_recomp.0.cpp'
target = '__imp__XamGetSystemVersion('

for p in sorted(glob.glob('build/ppc/ppc_recomp.*.cpp')):
    s = io.open(p, encoding='utf-8', errors='replace').read()
    if target not in s:
        continue

    for m in re.finditer(re.escape(target), s):
        i = m.start()
        # Which function?
        fn = '?'
        for fm in re.finditer(r'PPC_FUNC_IMPL\(__imp__(\w+)\)', s):
            if fm.start() < i:
                fn = fm.group(1)
            else:
                break

        print('=' * 74)
        print('CALL in %s  (%s)' % (fn, p.split('ppc_recomp.')[-1]))
        print('=' * 74)

        # 16 instructions before, 40 after.
        seg = s[:i].split('\n')[-16:] + ['   ---- call ----'] + \
              s[i:].split('\n')[:40]
        note = None
        for line in seg:
            t = line.strip()
            if not t:
                continue
            mn = re.match(r'//\s*(.*)', t)
            if mn:
                note = mn.group(1)
                continue
            if 'call' in t:
                print('   ---- call ----')
                continue
            if note:
                print('   %-44s // %s' % (t[:44], note))
                note = None
            else:
                print('   %s' % t[:70])
        print()
        break
    break
