import io
import glob
import re

# sub_82347340 and sub_823473D8 write BOTH +1408 and +1412 - the only writers that
# touch the pair, and therefore the most likely real method-table setup.
#
# sub_82347340 was previously dismissed as "never runs during boot" on the basis
# of an earlier misidentification. That dismissal is worth re-testing, because
# if these two functions are what install the heap descriptor's methods, then the
# reason +1412 was zero may be that they were called wrongly - not that they were
# never called.
#
# What is stored, and from what.

for fn in ('sub_82347340', 'sub_823473D8'):
    for path in sorted(glob.glob('build/ppc/ppc_recomp.*.cpp')):
        s = io.open(path, encoding='utf-8', errors='replace').read()
        i = s.find('PPC_FUNC_IMPL(__imp__%s)' % fn)
        if i < 0:
            continue

        j = s.find('\n}\n', i)
        body = s[i:j]
        lines = [l.strip() for l in body.splitlines() if l.strip()]

        print('=' * 72)
        print('%s   (%d lines)' % (fn, len(lines)))
        print('=' * 72)

        # Show every store to +1404..+1420 with the 12 lines of context before,
        # so the source of the stored value is visible.
        for n, l in enumerate(lines):
            if re.search(r'\+ (1404|1408|1412|1416|1420)\b', l) and 'STORE' in l:
                print('  ---')
                for q in range(max(0, n - 10), min(len(lines), n + 2)):
                    mark = '>>' if q == n else '  '
                    print('  %s %s' % (mark, lines[q][:68]))
                print()
        break
