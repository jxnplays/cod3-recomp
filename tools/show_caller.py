"""Show the generated C++ around a call to a guest function.

Usage: python tools/show_caller.py 0x82348B9C
       python tools/show_caller.py sub_8234BE28

Accepts either a guest address (matched against the `ctx.lr = <addr>;` that
precedes the call) or a generated symbol name.
"""

import glob
import io
import re
import sys

target = sys.argv[1] if len(sys.argv) > 1 else None
if target is None:
    sys.exit('usage: show_caller.py <addr|symbol> [context lines]')

context = int(sys.argv[2]) if len(sys.argv) > 2 else 14

if target.startswith('sub_'):
    call = re.compile(re.escape(target) + r'\(ctx, base\)')
else:
    call = re.compile(r'ctx\.lr = ' + re.escape(target) + r';')

for path in sorted(glob.glob('build/ppc/ppc_recomp.*.cpp')):
    src = io.open(path, encoding='utf-8', errors='replace').read()
    for m in call.finditer(src):
        # Enclosing function: the last PPC_FUNC* before the call.
        name = None
        st = 0
        for mm in re.finditer(r'PPC_FUNC(?:_IMPL)?\((\w+)\)', src):
            if mm.start() < m.start():
                st = mm.start()
                name = mm.group(1)
        print('%s  in %s()  [%s]' % (target, name, path.split('/')[-1]))
        end = src.find('\n}\n', m.start())
        lines = src[st:end].splitlines()
        for k, l in enumerate(lines):
            if call.search(l):
                for x in lines[max(0, k - context):k + 4]:
                    print('   ', x.rstrip())
                sys.exit(0)
print('no call site found for %s' % target)
sys.exit(1)