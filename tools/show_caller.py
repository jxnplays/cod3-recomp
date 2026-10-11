import glob
import io
import re

# Show the generated C++ around a guest call site, with its enclosing function.
#
# Usage: python tools/show_caller.py 0x82348B9C

target = '0x82348B9C'

for path in sorted(glob.glob('build/ppc/ppc_recomp.*.cpp')):
    src = io.open(path, encoding='utf-8', errors='replace').read()
    i = src.find(target)
    if i < 0:
        continue
    name = None
    st = 0
    for m in re.finditer(r'PPC_FUNC(?:_IMPL)?\((\w+)\)', src):
        if m.start() < i:
            st = m.start()
            name = m.group(1)
    print('call site %s in %s()  [%s]'
          % (target, name, path.split('/')[-1]))
    j = src.find('\n}\n', i)
    lines = src[st:j].splitlines()
    for k, l in enumerate(lines):
        if target in l or 'ERROR' in l:
            lo = max(0, k - 14)
            for x in lines[lo:k + 5]:
                print('   ', x.rstrip())
            break
    break
else:
    print('call site %s not found' % target)