import io
import glob
import re
import sys

# Dump the mnemonics of a guest function, taken from XenonRecomp's own
# `// mnemonic` comments rather than decoded by hand.
#
# Hand-decoding produced three wrong answers earlier in this project. The
# generated comments are authoritative.

TARGETS = sys.argv[1:] or ['sub_82437108']

for target in TARGETS:
    found = False
    for path in sorted(glob.glob('build/ppc/ppc_recomp.*.cpp')):
        s = io.open(path, encoding='utf-8', errors='replace').read()
        i = s.find('PPC_FUNC_IMPL(__imp__%s)' % target)
        if i < 0:
            continue

        j = s.find('\n}\n', i)
        body = s[i:j]

        print('=' * 74)
        print('%s   (%s, %d lines)' % (target, path.split('ppc_recomp.')[-1],
                                       body.count('\n')))
        print('=' * 74)

        for line in body.splitlines():
            t = line.strip()
            m = re.match(r'//\s*(.*)', t)
            if m:
                print('    %s' % m.group(1))
                continue
            # Show the store/load that implements each mnemonic, so the mapping
            # between the two is never in doubt.
            a = re.search(r'(ctx\.r\d+\.(?:u|s)\d+|PPC_(?:STORE|LOAD)_\w+)\s*=\s*'
                          r'(.*?);?$', t)
            if a and 'PPC_FUNC_PROLOGUE' not in t and 'ctx.r12.u64 = ctx.lr' not in t:
                print('        %-34s = %s' % (a.group(1), a.group(2)[:34]))
        print()
        found = True
        break

    if not found:
        print('%s: not found' % target)
