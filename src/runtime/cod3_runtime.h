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
#include <string>

#include <cstddef>
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

// Guest call tracing.
//
// Enable with COD3_TRACE_GUEST=1 to record the guest addresses of recompiled
// functions as they are entered, and dump the tail of that history when the
// runtime faults. This is the cheapest way to see how far the boot path got.
void cod3_set_guest_trace(bool enabled);
void cod3_record_guest_call(uint32_t address);

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

// -----------------------------------------------------------------------------
// Guest memory access
// -----------------------------------------------------------------------------
//
// THE GUEST IS BIG-ENDIAN. PowerPC on the Xbox 360 stores multi-byte values
// most-significant byte first, and XenonRecomp's generated code byte-swaps every
// load and store to compensate on a little-endian host.
//
// The runtime is NOT generated code, so it must do the same thing by hand.
// Writing a uint32_t natively into guest memory and letting the guest read it
// back silently yields the byteswapped value - 0x83CA0000 comes back as
// 0x0000CA83 - which produces a wrong pointer far from the real cause.
//
// Always use these helpers for guest memory. They are deliberately the only
// sanctioned way to touch it.

// Store a 32-bit value in guest memory, byte-swapped as the guest expects.
inline void cod3_store_u32(uint32_t address, uint32_t value)
{
    auto* p = reinterpret_cast<volatile uint8_t*>(static_cast<uintptr_t>(address));
    p[0] = static_cast<uint8_t>(value >> 24);
    p[1] = static_cast<uint8_t>(value >> 16);
    p[2] = static_cast<uint8_t>(value >> 8);
    p[3] = static_cast<uint8_t>(value);
}

// Load a 32-bit value from guest memory.
inline uint32_t cod3_load_u32(uint32_t address)
{
    const auto* p = reinterpret_cast<const volatile uint8_t*>(
        static_cast<uintptr_t>(address));
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) |
           static_cast<uint32_t>(p[3]);
}

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

// Recover the guest link register saved in the host frame at `returnAddress`.
//
// Each recompiled guest function spills its incoming link register to a fixed
// offset in its own frame, so the value identifies the guest call site. This is
// what makes the crash report's call chain usable: the generated code
// tail-calls, so the symbol a host frame maps to is often not the function that
// made the call, but the link register always is.
void cod3_frame_link_register(const void* returnAddress, uint32_t* outLr);

// Map a host code address back to the guest function it belongs to.
bool cod3_attribute_host_pc(const void* pc, uint32_t* guest, uint64_t* distance);

// Record the guest stack frame size a translated function opens with, so its
// saved link register can be recovered from a host stack frame.
void cod3_record_frame_size(uint32_t guest, int32_t size);

// Reserve a thread-local slot, or return 0xFFFFFFFF when the pool is full.
//
// Exposed so the runtime can reserve the CRT's own slot at load time rather
// than only when the title happens to call KeTlsAlloc.
uint32_t cod3_reserve_tls_slot();

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

// Look up one of the XEX optional header fields (XEX_HEADER_* ids) captured
// from the container at load time.
//
// The CRT queries these through RtlImageXexHeaderField() during boot and gives
// up if the call fails, so the runtime has to answer them. Returns false when
// the field is genuinely absent from this title.
bool cod3_xex_header_field(uint32_t field, uint32_t* outValue);

// Runtime service identifiers. These are functions the kernel APIs must return
// as guest-callable pointers.
enum
{
    Cod3RuntimeService_TlsDispatcher = 0,
};

// Optional hook run by the crash reporter before it prints.
//
// The CRT's heap bookkeeping lives in guest globals that only acquire values
// once the title has run for a while, so the interesting state is not
// observable at load time. Installing a dumper lets the crash report show it.
void cod3_set_heap_slot_dumper(void (*fn)());

// Run the dumper installed above, if any. Called by the crash reporter.
void cod3_dump_heap_slots();

// Print how many times the instrumented guest functions were entered. Safe to
// call from a crash reporter: it only reads counters.
void cod3_report_guest_entries();

// Reserve guest addresses for runtime-supplied functions that kernel APIs hand
// back to the guest as pointers. Called once during image load.
void cod3_register_runtime_services();

// Guest address the guest should call for a runtime service, or 0 if the
// service is unavailable.
uint32_t cod3_runtime_service_address(int service);

// Publish the loader-populated globals in the title's .rdata that the CRT walks
// during boot (the XEX loader context chain). Without these the title gives up
// at the first instruction of heap setup.
void cod3_populate_loader_globals();

// The CRT per-thread block that r13 points at. PowerPC on the 360 reserves r13
// for this, and nothing on the boot path ever assigns it - only two thread
// trampolines write it, and neither runs during boot - so the loader has to
// build the block before calling the guest entry point.
// See src/runtime/crt_thread_block.cpp for the layout and how it was derived.
void cod3_populate_crt_thread_block();
uint32_t cod3_crt_thread_block();

// Published by cod3_populate_crt_thread_block(); 0 until it has run.
extern uint32_t g_crt_thread_block;

// The [r13+256] word, recorded when COD3_TRACE_TLS=2. The crash handler reports
// its current value alongside the r13 the faulting code saw, which distinguishes
// "the pointer was cleared" from "r13 moved" without having to instrument every
// store that could be responsible.
extern uint32_t* g_thread_io_word;

// The value cod3_populate_crt_thread_block() stored into that word, so a later
// change is reported as a change rather than as a bare absolute.
extern uint32_t g_thread_io_block_expected;

// Bump allocator over the guest heap region, for loader structures that must be
// reachable by the guest at real guest virtual addresses.
//
// This exists because VirtualAlloc is the wrong tool here. The loader builds
// structures the guest then dereferences with its own load/store macros, so the
// pointer has to be a guest VA inside the mapped heap, not a host address.
// Returns nullptr if the region is exhausted.
//
// cod3_pool_free() is a no-op for anything obtained from here: the region is
// owned by the loader for the life of the process, which matches how the real
// loader's boot structures behave.
void* cod3_heap_alloc(uint32_t bytes, uint32_t tag);

// Size of the loader's private region at the base of the guest heap.
// NtAllocateVirtualMemory starts handing memory out above it, so the two never
// overlap: the CRT zeroes what it allocates, which would destroy loader
// structures if they shared a region.
extern const size_t kLoaderRegionBytes;

// The live guest context, published by the launcher before the entry point runs.
// The crash handler reads it to report register state: the faulting instruction
// alone rarely identifies which register held the bad pointer.
// Null until the launcher has started the guest.
extern PPCContext* g_crash_ctx;

// The next free guest virtual address for kernel-memory allocation, shared by
// NtAllocateVirtualMemory and MmAllocatePhysicalMemoryEx. Both carve from the one
// guest address space, so they must share a single bump point or they will
// eventually hand out the same bytes twice.
uint32_t* g_vm_brk_slot();

// Whether kernel-memory allocation chatter reaches stderr. COD3_QUIET_VM=1
// silences it. A function rather than a global so there is no cross-translation
// unit initialisation order to get wrong.
bool cod3_vm_quiet();

// The CRT heap grow method, callable by the guest through `bctrl`.
// See src/runtime/heap_grow_method.cpp for why the loader owns it.
void cod3_heap_grow(PPCContext& ctx, uint8_t* base);
uint32_t cod3_heap_grow_service_address();
void cod3_install_heap_methods(uint32_t heap);

// Install the critical section a heap descriptor keeps at +1408.
// See src/runtime/kernel_imports_core.cpp for why the loader owns it.
void cod3_heap_install_lock(uint32_t heap);

// Guest pointer as a host pointer. Guest addresses are mapped at their real
// host addresses, so this is a widening cast and nothing more - but the
// indirection is named so no caller assumes `g_base + addr` arithmetic.
inline void* cod3_guest_ptr(uint32_t addr)
{
    return reinterpret_cast<void*>(static_cast<uintptr_t>(addr));
}

// Directory the guest's files are resolved beneath. Set by the launcher.
void cod3_set_game_root(const std::string& root);

// Print the display subsystem's state. Called on a crash so a fault in the
// display layer can be read against what it had actually set up.
void cod3_display_state_summary();

// Allocate guest-visible memory from the guest heap bump. Distinct from
// cod3_heap_alloc, which serves the loader's private 64 KiB region.
void* cod3_guest_alloc(uint32_t bytes, const char* what);

// Guest MMIO stores. Redirected centrally by tools/patch_ppc_context.py so all
// eleven sites are captured at once; see src/runtime/gpu_command_processor.cpp.
void cod3_mmio_store_u32(uint8_t* base, uint32_t addr, uint32_t value);
void cod3_report_gpu_stream();

// Print how many CRT heap allocations were skipped by the fault tolerance. Safe
// to call from a crash reporter.
void cod3_report_heap_tolerance();

// The first address NtAllocateVirtualMemory returned, or 0 if it has not been
// called. Used to inspect a descriptor the guest actually created, rather than
// one inferred from where the heap is expected to start - the loader's own
// structures occupy the low part of the heap, so a computed guess reads them.
uint32_t cod3_first_vm_page();

// Allocate (once per field) a zeroed guest page to stand in for an XEX header
// field this title does not carry. The Xbox 360 kernel answers such queries with
// a pointer to a default value rather than failing, and the CRT dereferences the
// result unconditionally.
uint32_t cod3_xex_default_field_slot(uint32_t field);

// -----------------------------------------------------------------------------
// Diagnostics
// -----------------------------------------------------------------------------

// Install a top-level exception handler that attributes a fault back to the
// guest PowerPC address it was translated from. Without this, a crash in the
// recompiled code reports only a host address inside a 24 MB binary, which is
// not actionable.
void cod3_install_crash_handler();