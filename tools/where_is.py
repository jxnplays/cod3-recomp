import io
import glob
import re
import sys

# Given a guest address, name the function containing it and show the register
# setup around the nearest call to an import.
#
# Used to chase a caller from a captured link register, rather than guessing at a
# signature.

addr = int(sys.argv[1], 16)
near = sys.argv[2] if len(sys.argv) > 2 else None

best = None
for path in sorted(glob.glob('build/ppc/ppc_recomp.*.cpp')):
    s = io.open(path, encoding='utf-8', errors='replace').read()
    for m in re.finditer(r'PPC_FUNC_IMPL\(__imp__(\w+)\)', s):
        name = m.group(1)
        try:
            start = int(name[4:], 16)
        except ValueError:
            continue
        # Approximate extent: to the next function header.
        nxt = s.find('PPC_FUNC_IMPL(', m.end())
        end = nxt if nxt > 0 else len(s)
        if start <= addr < start + 0x4000 and (best is None or start > best[0]):
            best = (start, name, path, m.start(), end)

if best is None:
    print('0x%08X: no enclosing function found' % addr)
    sys.exit(1)

start, name, path, body_start, body_end = best
print('0x%08X is inside %s (+0x%X), %s'
      % (addr, name, addr - start, path.split('ppc_recomp.')[-1]))

if near:
    s = io.open(path, encoding='utf-8', errors='replace').read()
    call = '__imp__%s(' % near
    idx = s.find(call)
    if idx < 0:
        print('%s: not called' % near)
        sys.exit(0)

    print()
    print('--- register setup at the %s call ---' % near)
    note = None
    for line in s[:idx].split('\n')[-40:]:
        t = line.strip()
        if not t:
            continue
        mn = re.match(r'//\s*(.*)', t)
        if mn:
            note = mn.group(1)
            continue
        asg = re.search(r'ctx\.(r\d+)\.(?:u\d+|s\d+) = (.*);', t)
        if asg and asg.group(1) in ('r3', 'r4', 'r5', 'r6', 'r7', 'r8', 'r9', 'r10'):
            print('  %-3s = %-32s // %s' % (asg.group(1), asg.group(2)[:32], note or ''))
            note = None
