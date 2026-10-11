import io
import glob
import re

# These are IMPORTS, so there is no branch to them. XenonRecomp emits a direct
# call to the __imp__ wrapper instead:
#
#     ctx.r3.u64 = ...;
#     ctx.lr = 0x8234XXXX;
#     __imp__XamGetSystemVersion(ctx, base);
#
# Searching for the branch target finds nothing, which is why the previous
# version reported zero call sites for all six.

TARGETS = [
    'VdSetDisplayMode',
    'VdPersistDisplay',
    'VdQueryVideoFlags',
    'XamGetSystemVersion',
    'XamLoaderGetLaunchData',
    'NetDll_WSAStartup',
]

for path in sorted(glob.glob('build/ppc/ppc_recomp.*.cpp')):
    s = io.open(path, encoding='utf-8', errors='replace').read()

    func_starts = [(m.start(), m.group(1))
                   for m in re.finditer(r'PPC_FUNC_IMPL\(__imp__(\w+)\)', s)]

    for t in TARGETS:
        call = '__imp__%s(' % t
        if call not in s:
            continue

        sites = []
        for m in re.finditer(re.escape(call), s):
            i = m.start()
            fn = None
            for pos, name in func_starts:
                if pos < i:
                    fn = name
                else:
                    break

            # Walk back over the preceding lines, pairing each "// mnemonic"
            # comment with the register assignment that implements it.
            lines = s[:i].split('\n')
            tail = [l.strip() for l in lines[-45:] if l.strip()]

            pairs = []
            note = None
            for l in tail:
                mn = re.match(r'//\s*(.*)', l)
                if mn:
                    note = mn.group(1)
                    continue
                asg = re.search(r'ctx\.(r\d+)\.(?:u\d+|s\d+) = (.*);', l)
                if asg:
                    val = asg.group(2).strip()
                    if note:
                        pairs.append('%s = %s   [%s]' % (asg.group(1), val, note))
                        note = None
                    else:
                        pairs.append('%s = %s' % (asg.group(1), val))
            sites.append((fn, pairs[-8:]))

        print('=' * 72)
        print('%s   %d call site(s)' % (t, len(sites)))
        print('=' * 72)
        seen = set()
        shown = 0
        for fn, pairs in sites:
            key = tuple(pairs)
            if key in seen:
                continue
            seen.add(key)
            print('  in %s' % fn)
            for p in pairs:
                print('      %s' % p)
            print()
            shown += 1
            if shown >= 2:
                break
        print()
