import io
import glob
import re
from collections import Counter

# RECOUNT. The first version of this reported 9,854 unresolved branches, which is
# absurd next to the 184 previously believed. It was wrong.
#
# Almost all of them targeted 0x8234F2B0..0x8234F2EC - a sixty-byte cluster
# called thousands of times, which is the floating-point save/restore helper
# table (__restfpr_N and friends). Those ARE implemented; XenonRecomp emits them
# as a direct call:
#
#     // bl 0x8234f28c
#     __restfpr_25(ctx, base);
#
# The previous version only accepted `if (`, `goto` or `ctx.lr = ` as evidence of
# an implementation, so every helper call counted as unresolved.
#
# The lesson is the one that keeps recurring on this project: a number that looks
# alarming is more often a broken measurement than a broken port. Both this and
# the earlier heap work produced wrong readings from a plausible-looking filter.
#
# This version accepts ANY emitted statement after the comment as evidence.

helpers = Counter()
unresolved = []
implemented = 0

for path in sorted(glob.glob('build/ppc/ppc_recomp.*.cpp')):
    s = io.open(path, encoding='utf-8', errors='replace').read()

    func_starts = [(m.start(), m.group(1))
                   for m in re.finditer(r'PPC_FUNC_IMPL\(__imp__(\w+)\)', s)]

    for m in re.finditer(r'// (bl|b|blr) (0x[0-9a-fA-F]+)?', s):
        i = m.start()

        # The next non-blank, non-comment line is what the compiler emitted.
        rest = s[m.end():m.end() + 300]
        nxt = None
        for line in rest.split('\n')[1:]:
            t = line.strip()
            if not t or t.startswith('//'):
                continue
            nxt = t
            break

        if nxt is None:
            continue

        is_helper = bool(re.match(r'__\w+\(ctx', nxt)) or bool(
            re.match(r'(if\s*\(|goto\s+|ctx\.lr = |ctx\.r\d+\.\w+ = )', nxt))

        if is_helper:
            implemented += 1
            h = re.match(r'(__\w+)\(', nxt)
            if h:
                helpers[h.group(1)] += 1
            continue

        fn = None
        for pos, name in func_starts:
            if pos < i:
                fn = name
            else:
                break

        unresolved.append((m.group(2), fn, m.group(1), nxt[:60]))

print('branch comments with an implementation after them: %d' % implemented)
print('genuinely unresolved: %d' % len(unresolved))
print()

if unresolved:
    by_target = Counter(t for t, _, _, _ in unresolved if t)
    print('distinct unresolved targets: %d' % len(by_target))
    print()
    print('top unresolved targets:')
    for t, n in by_target.most_common(20):
        fns = sorted({f for x, f, _, _ in unresolved if x == t and f})
        print('  %-12s %4d   from %s' % (t, n, ', '.join(fns[:2]) or '-'))

    print()
    print('what follows them (should be nothing, or a comment):')
    kinds = Counter(n.split('(')[0][:30] for _, _, _, n in unresolved)
    for k, n in kinds.most_common(10):
        print('  %-32s %4d' % (k, n))
