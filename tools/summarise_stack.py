import collections
import io
import re
import sys

# Summarise a guest call stack dump: how many frames are distinct, and which
# host addresses repeat.
#
# Usage: python tools/summarise_stack.py build/run.log

path = sys.argv[1] if len(sys.argv) > 1 else 'build/run.log'

with io.open(path, encoding='utf-8', errors='replace') as f:
    lines = f.read().splitlines()

frames = []
capture = False
for line in lines:
    if 'guest call stack' in line:
        capture = True
        continue
    if capture:
        m = re.search(r'#(\d+)\s+(\S+)\s+\(host\s+(\S+?),\s*\+(\d+)\)', line)
        if not m:
            if line.strip().startswith('==='):
                break
            continue
        frames.append((int(m.group(1)), m.group(2), m.group(3),
                       int(m.group(4))))

if not frames:
    print('no frames parsed from %s' % path)
    sys.exit(1)

print('frames parsed          : %d' % len(frames))

by_host = collections.Counter(f[2] for f in frames)
print('distinct host addresses: %d' % len(by_host))
print()

print('most repeated frames:')
for host, n in by_host.most_common(8):
    guest = next(f[1] for f in frames if f[2] == host)
    print('   %-18s x%-4d  host %s' % (guest, n, host))

print()
by_guest = collections.Counter(f[1] for f in frames)
print('most repeated guest symbols:')
for guest, n in by_guest.most_common(8):
    print('   %-18s x%d' % (guest, n))