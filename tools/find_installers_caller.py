import io
import glob
import re

# sub_82347340 and sub_823473D8 install the heap descriptor's method slots and
# never run during boot, while sub_823478F0 - a consumer of those slots - runs six
# times. Something upstream that should have run them did not.
#
# So find the callers. These are ordinary functions, not imports, so they appear
# as __imp__sub_82347340(ctx, base) calls.
#
# The caller matters because it tells us what has to run first. The installers
# read [r31+80] for the descriptor and [r31+164] for the grow method, so whatever
# builds the owner struct has to have run before them.

TARGETS = ('sub_82347340', 'sub_823473D8')

for path in sorted(glob.glob('build/ppc/ppc_recomp.*.cpp')):
    s = io.open(path, encoding='utf-8', errors='replace').read()
    func_starts = [(m.start(), m.group(1))
                   for m in re.finditer(r'PPC_FUNC_IMPL\(__imp__(\w+)\)', s)]

    for t in TARGETS:
        call = '__imp__%s(' % t
        if call not in s:
            continue

        for m in re.finditer(re.escape(call), s):
            i = m.start()
            fn = '?'
            for pos, name in func_starts:
                if pos < i:
                    fn = name
                else:
                    break

            print('=' * 72)
            print('%s called from %s' % (t, fn))
            print('=' * 72)

            lines = s[:i].split('\n')[-24:] + ['  ---- call ----'] + \
                    s[i:].split('\n')[:6]
            note = None
            for line in lines:
                t2 = line.strip()
                if not t2:
                    continue
                mn = re.match(r'//\s*(.*)', t2)
                if mn:
                    note = mn.group(1)
                    continue
                if 'call' in t2:
                    print('  ---- call ----')
                    continue
                asg = re.search(r'ctx\.(r\d+)\.(\w+) = (.*);', t2)
                if asg and note:
                    print('  %-3s = %-26s // %s'
                          % (asg.group(1), asg.group(3)[:26], note))
                    note = None
                elif asg:
                    print('  %-3s = %s' % (asg.group(1), asg.group(3)[:40]))
                else:
                    print('      %s' % t2[:66])
            print()
