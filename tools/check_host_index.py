import collections
import io
import re
import sys

# The crash reporter maps a host code address back to a guest function with
# upper_bound over (host address, guest) pairs. XenonRecomp emits each guest
# function as a weak alias of an __imp__ body, and where several functions share
# one body the index ends up with duplicate host addresses. upper_bound then
# picks whichever sorted last, which may not be the function that is actually
# running - which is how a frame can be reported as the wrong guest symbol.
#
# This reports the duplicates.
#
# Usage: python tools/check_host_index.py

path = 'build/ppc/ppc_func_mapping.cpp'
with io.open(path, encoding='utf-8', errors='replace') as f:
    text = f.read()

# The file maps guest -> symbol; the host pointer is not in it, so this can
# only report guests that share a symbol, which is the same condition.
pat = re.compile(r'\{\s*(0x[0-9A-Fa-f]+),\s*(\w+)\s*\}')
by_symbol = collections.defaultdict(list)
for m in pat.finditer(text):
    by_symbol[m.group(2)].append(int(m.group(1), 16))

dupes = {s: gs for s, gs in by_symbol.items() if len(gs) > 1}
print('guest symbols sharing one host body : %d' % len(dupes))
print('total guest functions               : %d'
      % sum(len(v) for v in by_symbol.values()))

worst = sorted(dupes.items(), key=lambda kv: -len(kv[1]))[:12]
print()
print('most shared:')
for sym, gs in worst:
    print('   %-20s %d guests' % (sym, len(gs)))
    print('        e.g. %s' % ', '.join('0x%08X' % g for g in gs[:5]))