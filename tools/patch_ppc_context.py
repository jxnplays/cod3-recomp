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

It also redirects MMIO stores, for the same reason and independently:
PPC_MM_STORE_U32 is an ordinary store, but the Xenos GPU command processor
lives at 0x7FC80000 and there is no such device on a PC, so the store faults
and the command stream is lost. Routing it through the runtime captures the
whole Xenos command sequence, which is what a D3D backend has to translate.

Idempotent: running it twice makes no further change.
"""

import io
import os
import sys

MARKER = 'cod3_lookup_indirect'
MMIO_MARKER = 'cod3_mmio_store_u32'

MMIO_OLD = (
    '#ifndef PPC_MM_STORE_U32\n'
    '#define PPC_MM_STORE_U32(x, y)  PPC_STORE_U32(x, y)\n'
    '#endif\n'
)

MMIO_NEW = (
    '// --- COD3 Recomp -------------------------------------------------------\n'
    '// Guest MMIO goes to devices that do not exist on a PC. The Xenos GPU\n'
    "// command processor is at 0x7FC80000, so a plain store there faults and\n"
    '// the command stream - the whole Xenos command sequence a D3D backend\n'
    '// would have to translate - is lost. Route it through the runtime.\n'
    '//\n'
    '// Catching the macro centrally covers all eleven sites at once; they sit\n'
    '// in nine separate functions and maintaining a list of them would break\n'
    '// the moment the recompiler output changed.\n'
    'void cod3_mmio_store_u32(uint8_t* base, uint32_t addr, uint32_t value);\n'
    '\n'
    '#ifndef PPC_MM_STORE_U32\n'
    '#define PPC_MM_STORE_U32(x, y) \\\n'
    '    cod3_mmio_store_u32(base, (uint32_t)(x), (uint32_t)(y))\n'
    '#endif\n'
)

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
    '//\n'
    '// The call-taking variant also reports each indirect call when\n'
    '// COD3_TRACE_ENTRIES is set. That is the only reliable way to see the\n'
    '// guest call chain: the generated code tail-calls freely, so a host\n'
    '// stack walk collapses many distinct callers onto one symbol.\n'
    'PPCFunc* cod3_lookup_indirect(uint32_t guest);\n'
    'PPCFunc* cod3_lookup_indirect_call(uint32_t guest, const PPCContext& ctx);\n'
    '\n'
    '#ifndef PPC_CALL_INDIRECT_FUNC\n'
    '#define PPC_CALL_INDIRECT_FUNC(x) \\\n'
    '    (cod3_lookup_indirect_call(x, ctx))(ctx, base)\n'
    '#endif\n'
)


def main(path):
    if not os.path.isfile(path):
        sys.stderr.write('patch_ppc_context: %s not found\n' % path)
        return 1

    with io.open(path, 'r', encoding='utf-8', errors='surrogateescape') as f:
        text = f.read()

    if MARKER in text:
        print('patch_ppc_context: indirect calls already patched')
    elif OLD not in text:
        sys.stderr.write(
            'patch_ppc_context: could not find PPC_CALL_INDIRECT_FUNC in %s\n'
            % path)
        return 1
    else:
        text = text.replace(OLD, NEW)
        print('patch_ppc_context: redirected indirect calls to %s' % MARKER)

    if MMIO_MARKER in text:
        print('patch_ppc_context: MMIO stores already patched')
    elif MMIO_OLD not in text:
        sys.stderr.write(
            'patch_ppc_context: could not find PPC_MM_STORE_U32 in %s\n'
            % path)
        return 1
    else:
        text = text.replace(MMIO_OLD, MMIO_NEW)
        print('patch_ppc_context: redirected MMIO stores to %s' % MMIO_MARKER)

    with io.open(path, 'w', encoding='utf-8', errors='surrogateescape',
                 newline='') as f:
        f.write(text)

    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1
                  else 'build/ppc/ppc_context.h'))