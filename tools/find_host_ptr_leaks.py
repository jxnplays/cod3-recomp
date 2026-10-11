import io
import glob
import re

# The guest holds 0x7FC80000 in r11 and dereferences it + 0x714. That is a
# Windows-range address, so a host pointer has leaked into guest state.
#
# Every import that hands the guest memory has to hand it GUEST memory. Find the
# ones that cast a host pointer down to 32 bits and return it - each is a
# candidate for the leak.
#
# Deliberately flags the pattern rather than judging each one: a few are correct
# (handing back an address the guest itself supplied), and the rest are leaks.
# Which is which needs reading, but the list makes that a per-site decision
# instead of a search.

SUSPECT = re.compile(
    r'(?:static_cast<uint32_t>|static_cast<unsigned>)\s*'
    r'\(\s*reinterpret_cast<(?:uintptr_t|std::uintptr_t|unsigned long long|SIZE_T)>'
    r'\s*\(')

for path in sorted(glob.glob('src/runtime/*.cpp')) + sorted(glob.glob('src/host/*.cpp')):
    s = io.open(path, encoding='utf-8').read()

    funcs = [(m.start(), m.group(1))
             for m in re.finditer(r'^(?:extern "C" )?(?:void|PPCContext[\w:<>& ]*|[\w:<>& ]+?)\s+'
                                  r'(cod3_\w+|__imp__\w+)\s*\(', s, re.M)]

    for m in SUSPECT.finditer(s):
        i = m.start()
        fn = '?'
        for pos, name in funcs:
            if pos < i:
                fn = name
            else:
                break

        line_no = s[:i].count('\n') + 1
        snippet = s[i:i + 130].split('\n')[0]

        print('%-34s %s:%d' % (fn, path.replace('\\', '/'), line_no))
        print('      %s' % snippet.strip()[:120])
