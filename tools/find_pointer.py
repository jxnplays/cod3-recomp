import io
import struct
import sys

# Search the loaded image for a guest word that matches a truncated pointer.
#
# A fault address like 0x4B677B8 is what you get when a 64-bit host pointer
# (0x00007FF7...) is stored into a 32-bit guest field, or when a guest pointer
# is truncated. Finding where the value was written narrows it to one function.
#
# Usage: python tools/find_pointer.py <fault-addr> [image]

addr = int(sys.argv[1], 0) if len(sys.argv) > 1 else 0
path = sys.argv[2] if len(sys.argv) > 2 else 'build/analysis/loaded.image'

if addr == 0:
    sys.exit('usage: find_pointer.py <fault-address> [image]')

with io.open(path, 'rb') as f:
    img = f.read()

# Guest words are big-endian.
needle = struct.pack('>I', addr)
print('searching for guest word 0x%08X in %s (%d bytes)'
      % (addr, path, len(img)))

hits = []
pos = 0
while True:
    j = img.find(needle, pos)
    if j < 0:
        break
    hits.append(j)
    pos = j + 1

if not hits:
    # Also try the value with plausible high bytes restored.
    for cand in (addr | 0x82000000, addr | 0x84000000, addr | 0x83000000,
                 addr | 0x84600000):
        needle = struct.pack('>I', cand)
        pos = 0
        while True:
            j = img.find(needle, pos)
            if j < 0:
                break
            hits.append(j)
            pos = j + 1
    print('  (also tried 0x82/0x83/0x84/0x84600000 prefixes)')

print('occurrences: %d' % len(hits))
for j in hits[:20]:
    va = 0x82000000 + j
    print('   file 0x%08X  ->  guest VA 0x%08X' % (j, va))