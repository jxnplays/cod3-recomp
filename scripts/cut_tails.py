"""Functions the analyser ended too early: their last instruction is not one
that leaves (a return, an unconditional branch, a jump through a register),
so the generated function falls off its end and returns where the original
went on to the next instruction.

    python scripts/cut_tails.py [--level name]

Found this way: the title's memset (sub_8234EBA0) ended at its first
`bdzlr` of the byte tail, so a length of 2 or 3 past a word boundary left
the last one or two bytes unwritten. Prints each function, its last
instruction and the address after it; exit status 0 either way.
"""
import glob
import os
import re
import sys

root = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'CoD3RecompLib')
level = sys.argv[sys.argv.index('--level') + 1] if '--level' in sys.argv else None
folder = os.path.join(root, 'levels', level) if level else os.path.join(root, 'ppc')

func_re = re.compile(r'^PPC_FUNC_IMPL\(__imp__(\w+?)_?([0-9A-F]{8})\)')
insn_re = re.compile(r'^\t// (\S+)(.*)$')
leaves = re.compile(r'^(blr|b|bctr|rfi|ERROR)$')

found = 0
for path in sorted(glob.glob(os.path.join(folder, 'ppc_recomp.*.cpp'))):
    name, last, count = None, None, 0
    def report():
        global found
        if name is None or last is None:
            return
        op = last[0].rstrip('+-.')
        if leaves.match(op):
            return
        # a tail call through the count register or an unconditional bl to a
        # routine that never returns cannot be told from here; list them all
        found += 1
        print('%s  last %s%s  (%d instructions)' % (name, last[0], last[1], count))
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            m = func_re.match(line)
            if m:
                report()
                name, last, count = m.group(1) + '_' + m.group(2), None, 0
                continue
            m = insn_re.match(line)
            if m and name:
                last = (m.group(1), m.group(2))
                count += 1
    report()
print('%d functions end on an instruction that does not leave' % found, file=sys.stderr)
