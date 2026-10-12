"""Which recompiled functions form a given guest address.

    python scripts/xref.py 0x82061950 [0x82066838 ...] [--level chambois]

The generated code keeps the original PowerPC as comments; an address is
built as `lis rA,hi` then `addi rB,rA,lo` (or a load/store with rA as the
base) anywhere later in the same function. This follows the value of each
register's `lis` within a function and reports every instruction whose
effective address equals one asked for, with the function and the line.
It does not follow values through other registers or calls, so a reference
built another way (a table, a register copy) is not found.
"""
import glob
import os
import re
import sys

root = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'CoD3RecompLib')
args = [a for a in sys.argv[1:] if not a.startswith('--')]
level = sys.argv[sys.argv.index('--level') + 1] if '--level' in sys.argv else None
if level:
    args = [a for a in args if a != level]
targets = {int(a, 16) & 0xFFFFFFFF for a in args}
folder = os.path.join(root, 'levels', level) if level else os.path.join(root, 'ppc')

func_re = re.compile(r'^PPC_FUNC_IMPL\(__imp__(\w+)\)')
lis_re = re.compile(r'^\s*// lis r(\d+),(-?\d+)\s*$')
use_re = re.compile(r'^\s*// (\w+)\.? r(\d+),r(\d+),(-?\d+)\s*$')
mem_re = re.compile(r'^\s*// (\w+) [rf](\d+),(-?\d+)\(r(\d+)\)\s*$')

for path in sorted(glob.glob(os.path.join(folder, 'ppc_recomp.*.cpp'))):
    function = None
    high = {}
    with open(path, encoding='utf-8', errors='replace') as f:
        for number, line in enumerate(f, 1):
            m = func_re.match(line)
            if m:
                function, high = m.group(1), {}
                continue
            m = lis_re.match(line)
            if m:
                high[int(m.group(1))] = (int(m.group(2)) << 16) & 0xFFFFFFFF
                continue
            m = use_re.match(line)
            if m and m.group(1) in ('addi', 'addic', 'ori'):
                base = int(m.group(3))
                if base in high:
                    value = (high[base] + int(m.group(4))) & 0xFFFFFFFF if m.group(1) != 'ori' else high[base] | (int(m.group(4)) & 0xFFFF)
                    if value in targets:
                        print('%08X %-24s %s:%d  %s' % (value, function, os.path.basename(path), number, line.strip()))
                continue
            m = mem_re.match(line)
            if m:
                base = int(m.group(4))
                if base in high:
                    value = (high[base] + int(m.group(3))) & 0xFFFFFFFF
                    if value in targets:
                        print('%08X %-24s %s:%d  %s' % (value, function, os.path.basename(path), number, line.strip()))
