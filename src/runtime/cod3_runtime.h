// =============================================================================
//  COD3 Recomp - runtime interface
//
//  Shared declarations for the Xbox 360 kernel surface that the recompiled
//  Call of Duty 3 code links against. Written from scratch for this project.
//
//  The recompiled code has the signature `void fn(PPCContext&, uint8_t* base)`
//  where `base` points at the start of the guest image (the address the Xbox
//  360 loader would have mapped at PPC_IMAGE_BASE, 0x82000000).
// =============================================================================

#pragma once

#include "ppc_context.h"

#include <cstdint>

// -----------------------------------------------------------------------------
// Guest memory
// -----------------------------------------------------------------------------

// Pointer to the first byte of the loaded guest image, or NULL.
//
// IMPORTANT: this is always NULL by design. The recompiled code computes every
// memory reference as `base + guest_va`, where guest_va is a full Xbox 360
// virtual address (e.g. 0x82BF0000). To make that resolve, the guest image is
// mapped at its real virtual address (0x82000000) so that guest addresses and
// host addresses coincide and base == 0.
//
// Runtime code that dereferences a guest pointer must therefore use the guest
// address directly (or the cod3_guest_ptr helpers), NOT `g_base + addr`.
extern uint8_t* g_base;

// Map the XEX into guest memory at its real virtual address, using the same
// XenonUtils loader the recompiler used. Returns false on failure.
bool cod3_load_image(const char* xex_path);

// Entry point of the loaded guest image (guest virtual address).
extern uint32_t g_guest_entry_point;

// Guest memory layout, established by cod3_load_image().
extern uint32_t g_guest_stack_base;
extern uint32_t g_guest_stack_top;
extern uint32_t g_guest_heap_base;
extern uint32_t g_guest_heap_size;

// The address the title is linked to load (XEX IMAGE_BASE_ADDRESS).
extern uint32_t kGuestImageBase;

// -----------------------------------------------------------------------------
// Import tracing
// -----------------------------------------------------------------------------

// Called by every generated import stub. Records that the title reached the
// given kernel API and, the first time it does, prints it. This is how the
// hand-written implementations get prioritised as the port is brought up.
void cod3_trace_unimplemented(const char* name);

// Print a one-line summary of every kernel API the guest has called so far,
// sorted by call count. Called on shutdown.
void cod3_dump_import_trace();

// When true (default) every unimplemented import is reported on first use.
// Set COD3_QUIET_IMPORTS=1 to suppress the per-call print.
bool cod3_import_trace_enabled();

// -----------------------------------------------------------------------------
// Xbox 360 kernel constants used by the hand-written implementations
// -----------------------------------------------------------------------------

// NTSTATUS values are signed 32-bit; success is 0 and warnings are positive.
using NTSTATUS = int32_t;

constexpr NTSTATUS STATUS_SUCCESS = 0x00000000;
constexpr NTSTATUS STATUS_PENDING = 0x00000103;
constexpr NTSTATUS STATUS_WAIT_TIMEOUT = 0x00000102;
constexpr NTSTATUS STATUS_INVALID_HANDLE = 0xC0000008;
constexpr NTSTATUS STATUS_NOT_IMPLEMENTED = 0xC0000002;

// Interrupt Request Levels. The recompiled code raises and lowers these
// constantly; we track the value so spin-lock code sees plausible IRQLs.
constexpr uint32_t PASSIVE_LEVEL = 0;
constexpr uint32_t APC_LEVEL = 1;
constexpr uint32_t DISPATCH_LEVEL = 2;
constexpr uint32_t DPC_LEVEL = 3;

extern uint32_t g_irql;

// -----------------------------------------------------------------------------
// Pool allocator
// -----------------------------------------------------------------------------

// Xbox 360 pool allocations back the game's entire heap. These serve memory
// from the guest heap region and return guest virtual addresses, because the
// guest stores pointers in 32-bit registers and hands them back to us later.
void* cod3_pool_alloc(uint32_t bytes, uint32_t tag, uint32_t flags);
void cod3_pool_free(void* ptr);

// Number of live pool allocations (used by the leak check on shutdown).
size_t cod3_pool_live_allocations();

// -----------------------------------------------------------------------------
// Guest execution
// -----------------------------------------------------------------------------

// Call a recompiled guest function at `address` with the supplied register
// context, and leave the context's r3 holding the return value on completion.
// `address` is a guest virtual address (e.g. the XEX entry point).
void cod3_run_guest_function(uint32_t address, PPCContext& ctx);

// Resolve a private export ordinal (>= 0x10000) against the recompiled function
// table, returning the guest virtual address of the target, or 0 if unknown.
// The Xbox 360 title resolves its own statically linked modules this way.
uint32_t cod3_resolve_private_export(uint32_t ordinal);

// -----------------------------------------------------------------------------
// Host display
// -----------------------------------------------------------------------------

// Report the host desktop mode (width, height, refresh, pixel format) so the
// recomp can present in a window at native PC resolution instead of being
// pinned to the original console modes.
void cod3_host_display_mode(uint32_t* width, uint32_t* height,
                            uint32_t* refreshHz, uint32_t* pixelFormat);

// -----------------------------------------------------------------------------
// Diagnostics
// -----------------------------------------------------------------------------

// Install a top-level exception handler that attributes a fault back to the
// guest PowerPC address it was translated from. Without this, a crash in the
// recompiled code reports only a host address inside a 24 MB binary, which is
// not actionable.
void cod3_install_crash_handler();