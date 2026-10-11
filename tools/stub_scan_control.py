import io
import struct

# Positive control for stub_references.py.
#
# That tool reported that none of the 9,660 unresolved stubs is referenced
# anywhere in the image. A negative like that is only worth anything if the scan
# can find references that DO exist, so this checks the method against addresses
# that are certainly referenced: real, non-stub functions.
#
# If the control finds nothing either, the scan's assumption about how the dump
# is laid out is wrong and the negative result is worthless.

IMAGE = 'build/analysis/loaded.image'
GUEST_BASE = 0x82000000

data = io.open(IMAGE, 'rb').read()
n = len(data) // 4
words = struct.unpack('>%dI' % n, data[:n * 4])

print('image %d bytes = %d big-endian words' % (len(data), n))
print('scanning for GUEST_BASE = 0x%08X' % GUEST_BASE)
print()

# Control set: addresses of functions that are definitely real and, being
# non-stub, are certainly not `.long 0x0`.
CONTROL = {
    0x822E8108: 'sub_822E8108 (real, seen on the boot path)',
    0x823460D8: 'sub_823460D8 (the real function behind the blamed stub)',
    0x82346D98: 'sub_82346D98 (caller of the faulting walk)',
    0x823478F0: 'sub_823478F0 (the heap core, runs during boot)',
}

for addr, why in CONTROL.items():
    refs = [i for i, w in enumerate(words) if w == addr]
    print('0x%08X  %d reference(s)   %s' % (addr, len(refs), why))
    if refs:
        print('        first at file offset 0x%08X  (guest 0x%08X)'
              % (refs[0] * 4, GUEST_BASE + refs[0] * 4))

print()
found_any = any(
    any(w == a for w in words) for a in CONTROL
)
print('CONTROL %s' % ('PASSED - the scan finds real pointers'
                      if found_any else 'FAILED - the scan finds nothing at all'))

if not found_any:
    print()
    print('The scan is not working. Likely causes:')
    print('  - the dump is NOT laid out with file offset == RVA')
    print('  - the values are stored little-endian')
    print('  - the guest base is not 0x82000000')
    print('In any of those cases the "0 stubs referenced" result is meaningless.')
