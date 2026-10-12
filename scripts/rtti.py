"""The virtual function tables of the classes whose RTTI names match.

    python scripts/rtti.py <image.bin> <name fragment> [...]

The image is the title's memory from 0x82000000 (COD3_IMAGEDUMP). An MSVC
type descriptor is a vtable pointer, a spare word and the decorated name;
a complete object locator is five words whose fourth is the descriptor's
address; a vtable is preceded by a pointer to its locator. This walks
from each matching name to its locators and from those to the tables, and
prints the first entries of each (big endian, as the console stores them).
"""
import re
import struct
import sys

BASE = 0x82000000
image = open(sys.argv[1], 'rb').read()
wanted = [w.encode() for w in sys.argv[2:]]


def words_equal(value):
    needle = struct.pack('>I', value)
    at = 0
    while True:
        at = image.find(needle, at)
        if at < 0:
            return
        if at % 4 == 0:
            yield at
        at += 1


for m in re.finditer(rb'\.\?AV[\x20-\x7e]{2,160}?@@\x00', image):
    name = m.group()[:-1]
    if not any(w in name for w in wanted):
        continue
    descriptor = BASE + m.start() - 8
    print('%s  descriptor %08X' % (name.decode(), descriptor))
    for at in words_equal(descriptor):
        locator = BASE + at - 12
        signature, offset = struct.unpack_from('>II', image, at - 12)
        if signature != 0:
            continue
        for vat in words_equal(locator):
            table = BASE + vat + 4
            entries = struct.unpack_from('>8I', image, vat + 4)
            print('    locator %08X (offset %u) -> vtable %08X: %s' % (
                locator, offset, table, ' '.join('%08X' % e for e in entries)))
