// =============================================================================
//  COD3 Recomp - indirect call table override
//
//  Force-included ahead of every generated translation unit.
//
//  XenonRecomp expands indirect calls through a macro that hardcodes where the
//  table lives:
//
//      #define PPC_LOOKUP_FUNC(x, y) *(PPCFunc**)(x + PPC_IMAGE_BASE +
//          PPC_IMAGE_SIZE + (uint64_t(uint32_t(y) - PPC_CODE_BASE) * 2))
//
//  Those constants are baked in at translation time from ppc_config.h
//  (PPC_IMAGE_BASE 0x82000000, PPC_IMAGE_SIZE 0xC30000), so the generated code
//  always reads the table from 0x82C30000.
//
//  That address is inside the PE image - it is where .reloc lives - so the
//  table cannot be relocated to match. Instead this file redefines
//  PPC_CALL_INDIRECT_FUNC, which ppc_context.h guards with #ifndef, so the
//  generated code asks the runtime where the table actually is.
//
//  The expansion keeps the original shape (a function pointer, immediately
//  called), so no generated code needs to change. The slot arithmetic is
//  unchanged too: byte offset (guest - CODE_BASE) * 2.
// =============================================================================

#ifndef COD3_INDIRECT_OVERRIDE_H
#define COD3_INDIRECT_OVERRIDE_H

// Declared rather than included: the generated units are compiled before any of
// our own headers, and the real definition lives in ppc_context.h.
typedef void PPCFunc(struct PPCContext&, uint8_t*);

// Implemented in ppc_runtime.cpp.
PPCFunc* cod3_lookup_indirect(uint32_t guest);

// Suppresses the generated definition in ppc_context.h.
#define PPC_CALL_INDIRECT_FUNC(x) (cod3_lookup_indirect(x))(ctx, base)

#endif // COD3_INDIRECT_OVERRIDE_H