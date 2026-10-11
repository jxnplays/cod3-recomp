"""Disassemble guest code using XenonRecomp's own decoder and its own addresses.

tools/decode_at.py decodes instructions by hand, and hand-decoding went wrong
repeatedly - opcode 21 alone covers add, subf, neg, cmpw, cmplw, cntlzw, slw and
sraw depending on the extended opcode and the S bit, and 0x82357E60 read as a
compare is in fact `rlwinm r30,r3,3,0,28`.

The generated C++ carries a `// <mnemonic>` comment above every translated
instruction, from the same decoder that emitted the running code, so it is
authoritative by construction. Addresses are recovered from two things the
generator emits:

  * `ctx.lr = 0xXXXXXXXX;` before a call - 0xXXXXXXXX is the return address, so
    it names the instruction *after* the call.
  * `loc_XXXXXXXX:` labels.

Between anchors the instructions are in program order and every one is four
bytes, so a run can be walked forward once its start is known.

Usage: python tools/disasm_guest.py <start> [end]

Output matches tools/decode_at.py so the two are interchangeable.
"""

import glob
import io
import re
import sys

FUNC = re.compile(r'PPC_FUNC(?:_IMPL)?\((\w+)\)')
LR = re.compile(r'ctx\.lr = (0x[0-9A-Fa-f]+);')
LOC = re.compile(r'^\s*loc_([0-9A-F]{8}):')
COMMENT = re.compile(r'^\s*// (.+)$')

SKIP = ('nop', '.long 0x0')


def collect():
    """Return (address -> (symbol, mnemonic)) covering every translated insn."""
    table = {}
    order = []

    for path in sorted(glob.glob('build/ppc/ppc_recomp.*.cpp')):
        src = io.open(path, encoding='utf-8', errors='replace').read()
        current = None

        # Split into function bodies so each one is walked independently.
        for fm in FUNC.finditer(src):
            name = fm.group(1)
            start = fm.end()
            end = src.find('\n}\n', start)
            if end < 0:
                end = len(src)

            # Pass 1: find every address the generator states outright.
            anchors = {}
            pending = None
            for line in src[start:end].splitlines():
                m = LOC.match(line)
                if m:
                    anchors[int(m.group(1), 16)] = True
                    pending = None
                    continue
                m = LR.search(line)
                if m:
                    pending = int(m.group(1), 16)
                    continue
                m = COMMENT.match(line)
                if m and pending is not None:
                    anchors[pending] = True
                    pending = None

            if not anchors:
                continue

            base = min(anchors)
            is_call = False

            # Pass 2: walk from the lowest anchor, filling in program order.
            for line in src[start:end].splitlines():
                m = LOC.match(line)
                if m:
                    base = int(m.group(1), 16)
                    is_call = False
                    continue
                m = LR.search(line)
                if m:
                    # `ctx.lr = X` names the address after the call that
                    # follows, so the `bl` itself sits at X-4.
                    after = int(m.group(1), 16)
                    base = after - 4
                    is_call = True
                    continue
                m = COMMENT.match(line)
                if not m:
                    continue
                text = m.group(1).strip()
                if text in SKIP:
                    continue
                if is_call:
                    # This comment annotates the call instruction itself.
                    addr = base
                    is_call = False
                else:
                    addr = base
                if addr not in table:
                    table[addr] = (name, text)
                    order.append(addr)
                base = addr + 4

    order.sort()
    return table, order


def main():
    if len(sys.argv) < 2:
        sys.exit('usage: disasm_guest.py <start> [end]')

    start = int(sys.argv[1], 0)
    end = int(sys.argv[2], 0) if len(sys.argv) > 2 else start + 0x60

    table, order = collect()
    if not table:
        sys.exit('no generated code found under build/ppc')

    printed = 0
    for addr in order:
        if addr < start:
            continue
        if addr >= end:
            break
        sym, mnem = table[addr]
        print('0x%08X: %-26s  ; %s' % (addr, mnem, sym))
        printed += 1
        if printed > 400:
            break

    if printed == 0:
        print('nothing emitted at or after 0x%08X' % start)


if __name__ == '__main__':
    main()