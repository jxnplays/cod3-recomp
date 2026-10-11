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

// Pointer to the first byte of the loaded guest image. PPC_LOAD_U32(x) and
// friends index off this directly, so it must remain stable for process life.
extern uint8_t* g_base;

// Load `default.xex` into guest memory, building it through XenonUtils so the
// image is byte-for-byte identical to the one XenonRecomp translated.
// Returns false and prints a diagnostic on failure.
bool cod3_load_image(const char* xex_path);

// Entry point of the loaded guest image (absolute guest virtual address).
extern uint32_t g_guest_entry_point;

// Top of the guest stack region, as a guest virtual address. The region is
// reserved immediately after the image, so guest addresses above
// PPC_IMAGE_SIZE address the stack rather than the image itself.
extern uint32_t g_guest_stack_top;

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

// Xbox 360 pool allocations back the game's entire heap. These forward to a
// simple tagged allocator so that tag/usage reporting works and so leaks are
// visible at shutdown.
void* cod3_pool_alloc(uint32_t bytes, uint32_t tag, uint32_t flags);
void cod3_pool_free(void* ptr);

// Expose the number of live allocations (used by the leak check on shutdown).
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