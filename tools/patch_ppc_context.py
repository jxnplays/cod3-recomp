"""Redirect XenonRecomp's indirect-call table lookup at the runtime table.

XenonRecomp expands indirect calls through a macro that hardcodes where the
table lives:

    #define PPC_LOOKUP_FUNC(x, y) *(PPCFunc**)(x + PPC_IMAGE_BASE +
        PPC_IMAGE_SIZE + (uint64_t(uint32_t(y) - PPC_CODE_BASE) * 2))

Those constants are baked in at translation time from ppc_config.h
(PPC_IMAGE_BASE 0x82000000, PPC_IMAGE_SIZE 0xC30000), so generated code always
reads the table from 0x82C30000.

That address is inside the PE image (it is where .reloc lives), so the runtime
cannot place a table there. This script rewrites the generated ppc_context.h so
indirect calls go through cod3_lookup_indirect(), which asks the runtime where
the table actually is.

Idempotent: running it twice makes no further change.
"""

import io
import os
import sys

MARKER = 'cod3_lookup_indirect'

OLD = (
    '#ifndef PPC_CALL_INDIRECT_FUNC\n'
    '#define PPC_CALL_INDIRECT_FUNC(x) (PPC_LOOKUP_FUNC(base, x))(ctx, base)\n'
    '#endif\n'
    '\n'
    'typedef void PPCFunc(struct PPCContext& __restrict__ ctx, uint8_t* base);\n'
)

NEW = (
    'typedef void PPCFunc(struct PPCContext& __restrict__ ctx, uint8_t* base);\n'
    '\n'
    '// --- COD3 Recomp -------------------------------------------------------\n'
    '// XenonRecomp hardcodes the indirect-call table at\n'
    '// PPC_IMAGE_BASE + PPC_IMAGE_SIZE (0x82C30000), which is inside the PE\n'
    '// image. The runtime keeps the table elsewhere, so ask it where the real\n'
    '// one is. The slot arithmetic is unchanged:\n'
    '//     byte offset = (guest - PPC_CODE_BASE) * 2\n'
    'PPCFunc* cod3_lookup_indirect(uint32_t guest);\n'
    '\n'
    '#ifndef PPC_CALL_INDIRECT_FUNC\n'
    '#define PPC_CALL_INDIRECT_FUNC(x) (cod3_lookup_indirect(x))(ctx, base)\n'
    '#endif\n'
)


def main(path):
    if not os.path.isfile(path):
        sys.stderr.write('patch_ppc_context: %s not found\n' % path)
        return 1

    with io.open(path, 'r', encoding='utf-8', errors='surrogateescape') as f:
        text = f.read()

    if MARKER in text:
        print('patch_ppc_context: already patched')
        return 0

    if OLD not in text:
        sys.stderr.write(
            'patch_ppc_context: could not find PPC_CALL_INDIRECT_FUNC in %s\n'
            % path)
        return 1

    text = text.replace(OLD, NEW)

    with io.open(path, 'w', encoding='utf-8', errors='surrogateescape',
                 newline='') as f:
        f.write(text)

    print('patch_ppc_context: redirected indirect calls to %s' % MARKER)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1
                  else 'build/ppc/ppc_context.h'))