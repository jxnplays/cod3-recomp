import io
import glob
import re

# sub_82347340 writes r21 into the descriptor's +1408, so tracing where r21 is
# set identifies what that pointer should be.
#
# Follow the chain backwards: find every assignment to r21 before the store, and
# if r21 is copied from another register or loaded, follow that too.

for path in sorted(glob.glob('build/ppc/ppc_recomp.*.cpp')):
    s = io.open(path, encoding='utf-8', errors='replace').read()
    i = s.find('PPC_FUNC_IMPL(__imp__sub_82347340)')
    if i < 0:
        continue
    j = s.find('\n}\n', i)
    lines = [l.strip() for l in s[i:j].splitlines() if l.strip()]

    # Locate the store to +1408
    store_at = None
    for n, l in enumerate(lines):
        if '+ 1408' in l and 'STORE' in l:
            store_at = n
            break
    assert store_at is not None

    print('=' * 74)
    print('sub_82347340: where does r21 come from?')
    print('=' * 74)

    # Every r21 assignment before the store, with its mnemonic.
    note = None
    for n in range(store_at):
        t = lines[n]
        m = re.match(r'//\s*(.*)', t)
        if m:
            note = m.group(1)
            continue
        if re.search(r'ctx\.r21\.\w+ = ', t):
            print('  line %-5d %-40s // %s' % (n, t[:40], note))
    print()
    print('  --- the store itself, with 6 lines of context ---')
    for n in range(max(0, store_at - 6), store_at + 1):
        t = lines[n]
        m = re.match(r'//\s*(.*)', t)
        print('  %-44s %s' % (t[:44], ('// ' + m.group(1)) if m else ''))
    break
