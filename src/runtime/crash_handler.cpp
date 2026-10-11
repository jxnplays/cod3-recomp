// =============================================================================
//  COD3 Recomp - crash attribution
//
//  When the recompiled game code faults, the CPU is executing native x86 inside
//  our binary, so the faulting PC says nothing on its own. This maps a host
//  address back to the guest PowerPC address it was translated from, using the
//  same PPCFuncMappings table the dispatcher uses.
//
//  That turns "access violation somewhere in 24 MB of generated code" into
//  "faulted at guest 0x8217B2C4", which is the difference between a debuggable
//  port and a guessing game.
// =============================================================================

#include "cod3_runtime.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "ppc_recomp_shared.h"

namespace {

// Host-pointer -> guest address index.
//
// The linker is free to lay the generated functions out in any order, so the
// "last mapping below the PC" heuristic is not reliable. Instead we sort a copy
// of the mapping table by host address once and binary search it, which makes
// attribution exact rather than approximate.
struct IndexEntry
{
    const char* host;
    uint32_t guest;
};

const std::vector<IndexEntry>& host_index()
{
    static const std::vector<IndexEntry> index = []() {
        std::vector<IndexEntry> v;
        for (size_t i = 0; PPCFuncMappings[i].host != nullptr; ++i)
        {
            v.push_back({
                reinterpret_cast<const char*>(PPCFuncMappings[i].host),
                static_cast<uint32_t>(PPCFuncMappings[i].guest),
            });
        }
        std::sort(v.begin(), v.end(),
                  [](const IndexEntry& a, const IndexEntry& b) {
                      return a.host < b.host;
                  });
        return v;
    }();
    return index;
}

// Attribute a host code address to the guest function containing it.
//
// Returns false if the address falls outside every translated function, which
// means the fault was in our own runtime or the CRT rather than in guest code.
bool attribute_host_pc(const void* pc, uint32_t* guest, size_t* distance)
{
    const auto& index = host_index();
    if (index.empty())
        return false;

    const auto* target = static_cast<const char*>(pc);

    auto it = std::upper_bound(
        index.begin(), index.end(), target,
        [](const char* key, const IndexEntry& e) { return key < e.host; });

    if (it == index.begin())
        return false; // faulted before the first translated function

    --it;
    *guest = it->guest;
    *distance = static_cast<size_t>(target - it->host);

    return true;
}

// Dump the indexed neighbourhood around a PC. Useful whenever attribution looks
// wrong: the index is built from PPCFuncMappings sorted by host address, and a
// PC in a gap is attributed to the preceding start. Seeing the neighbouring host
// addresses makes that visible instead of having to infer it from the symbol.
void dump_index_neighbourhood(const void* pc, size_t count = 8)
{
    const auto& index = host_index();
    const auto* target = static_cast<const char*>(pc);
    auto it = std::lower_bound(
        index.begin(), index.end(), target,
        [](const IndexEntry& e, const char* key) { return e.host < key; });

    std::fprintf(stderr, "  [cod3]   index around host %p:\n", pc);
    for (auto n = (it > index.begin() ? it - 1 : it);
         n < index.end() && n < it + static_cast<ptrdiff_t>(count); ++n)
    {
        std::fprintf(stderr, "  [cod3]     %p  sub_%08X  (%+td)\n",
                     static_cast<const void*>(n->host), n->guest,
                     static_cast<const char*>(target) - n->host);
    }
}

const char* describe_exception(DWORD code)
{
    switch (code)
    {
    case EXCEPTION_ACCESS_VIOLATION:      return "access violation";
    case EXCEPTION_INT_DIVIDE_BY_ZERO:   return "integer divide by zero";
    case EXCEPTION_ILLEGAL_INSTRUCTION:   return "illegal instruction (__builtin_debugtrap?)";
    case EXCEPTION_STACK_OVERFLOW:       return "stack overflow";
    case EXCEPTION_PRIV_INSTRUCTION:      return "privileged instruction";
    case EXCEPTION_FLT_DIVIDE_BY_ZERO:    return "floating point divide by zero";
    case EXCEPTION_DATATYPE_MISALIGNMENT: return "misaligned data access";
    default:                              return "unhandled exception";
    }
}

// Walk the host call stack and map each frame back to the guest function it
// belongs to.
//
// This works because recompiled guest functions are ordinary C++ functions that
// call each other directly, so the host return addresses on the stack are
// exactly the frames of the guest call chain. It is the only practical way to
// recover "how did we get here" without a source-level debugger.
void print_guest_call_stack()
{
    constexpr int kMaxFrames = 96;
    void* frames[kMaxFrames] = {};

    const USHORT captured =
        RtlCaptureStackBackTrace(0, kMaxFrames, frames, nullptr);

    std::fprintf(stderr, "\n[cod3] guest call stack (%u frame(s)):\n",
                 static_cast<unsigned>(captured));

    int printed = 0;
    for (USHORT i = 0; i < captured; ++i)
    {
        uint32_t guest = 0;
        size_t distance = 0;
        if (!attribute_host_pc(frames[i], &guest, &distance))
            continue;

        // The recompiled code tail-calls aggressively, so consecutive host
        // frames often resolve to the same guest function and the symbol alone
        // does not say who called. The guest link register does: each `bl` sets
        // it to the return address immediately before the call, so it identifies
        // the call site even when the caller's own frame has been replaced.
        uint32_t lr = 0;
        cod3_frame_link_register(frames[i], &lr);

        if (lr != 0)
        {
            std::fprintf(stderr,
                "    #%-2d sub_%.8X  called from 0x%08X  (host %p, +%zu)\n",
                printed++, guest, lr, frames[i], distance);
        }
        else
        {
            std::fprintf(stderr, "    #%-2d sub_%.8X  (host %p, +%zu)\n",
                         printed++, guest, frames[i], distance);
        }

        if (printed >= 40)
        {
            std::fprintf(stderr, "    ... (truncated)\n");
            break;
        }
    }

    if (printed == 0)
    {
        std::fprintf(stderr,
            "    (no frames fell inside a translated function; the fault is "
            "in the runtime itself)\n");
    }

    // Guest globals only acquire meaningful values once the CRT has been
    // running, so the heap and descriptor state is dumped here rather than at
    // load time, when it would always read as zero.
    cod3_dump_heap_slots();
}

} // namespace

// Exposed so the runtime can recover a frame's guest link register; see
// cod3_frame_link_register() in ppc_runtime.cpp. This has to live outside the
// anonymous namespace or it gets internal linkage and will not link.
bool cod3_attribute_host_pc(const void* pc, uint32_t* guest, uint64_t* distance)
{
    return attribute_host_pc(pc, guest, distance);
}

void cod3_install_crash_handler()
{
    SetUnhandledExceptionFilter([](EXCEPTION_POINTERS* info) -> LONG {
        const EXCEPTION_RECORD* rec = info->ExceptionRecord;

        std::fflush(stdout);
        std::fprintf(stderr, "\n");
        std::fprintf(stderr,
            "================================================================\n"
            "[cod3] FATAL: %s (0x%08X)\n",
            describe_exception(rec->ExceptionCode), rec->ExceptionCode);

        if (rec->ExceptionAddress != nullptr)
        {
            uint32_t guest = 0;
            size_t distance = 0;
            std::fprintf(stderr, "[cod3]   host PC     : 0x%016llX\n",
                static_cast<unsigned long long>(
                    reinterpret_cast<uintptr_t>(rec->ExceptionAddress)));

            if (attribute_host_pc(rec->ExceptionAddress, &guest, &distance))
            {
                std::fprintf(stderr,
                    "[cod3]   guest addr  : 0x%08X  (sub_%.8X, +%zu bytes)\n",
                    guest, guest, distance);

                // If the reported offset is implausibly large for this symbol,
                // say so rather than leaving a wrong name to be trusted. The
                // index resolves a PC to the last function start at or below it,
                // so a PC past a function's end is attributed to that function.
                // See dump_index_neighbourhood() below.
                dump_index_neighbourhood(rec->ExceptionAddress);

                // The guest register state is what actually identifies a bad
                // pointer. Reporting r1/r3/r11/r26-r31 lets the faulting store be
                // matched against the instruction that made it, instead of the
                // base register being guessed at from the address alone.
                if (g_crash_ctx != nullptr)
                {
                    const PPCContext& c = *g_crash_ctx;
                    std::fprintf(stderr,
                        "[cod3]   guest regs: r1=0x%08X r3=0x%08X r4=0x%08X"
                        " r11=0x%08X\n"
                        "[cod3]               r26=0x%08X r27=0x%08X r28=0x%08X"
                        " r29=0x%08X\n"
                        "[cod3]               r30=0x%08X r31=0x%08X r5=0x%08X"
                        " r6=0x%08X\n",
                        c.r1.u32, c.r3.u32, c.r4.u32, c.r11.u32,
                        c.r26.u32, c.r27.u32, c.r28.u32, c.r29.u32,
                        c.r30.u32, c.r31.u32, c.r5.u32, c.r6.u32);
                }
            }
            else
            {
                std::fprintf(stderr,
                    "[cod3]   guest addr  : not inside a translated function\n");
            }
        }

        // Identify which module the fault landed in. A PC inside COD3.exe means
        // the recompiled code or our runtime did it; anything else is the OS.
        HMODULE module = nullptr;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(rec->ExceptionAddress), &module) != 0)
        {
            char name[MAX_PATH] = {};
            if (GetModuleFileNameA(module, name, MAX_PATH) != 0)
            {
                std::fprintf(stderr, "[cod3]   module      : %s\n", name);
            }
        }

        // If the faulting address looks like a guest virtual address, dump the
        // PowerPC instruction stream around the corresponding guest location.
        // This pinpoints the faulting store/load instead of only naming the
        // enclosing function, which matters because a single guest function can
        // fault for many different reasons.
        if (rec->NumberParameters >= 2)
        {
            const uintptr_t bad = rec->ExceptionInformation[1];
            if (bad >= kGuestImageBase && bad < kGuestImageBase + 0x10000000ull)
            {
                std::fprintf(stderr,
                    "[cod3]   guest VA    : 0x%08X (inside the title's memory)\n",
                    static_cast<uint32_t>(bad));
            }
            else
            {
                std::fprintf(stderr,
                    "[cod3]   fault addr  : 0x%llX - NOT a valid guest address\n",
                    static_cast<unsigned long long>(bad));
            }
        }

        if (rec->NumberParameters >= 2)
        {
            const bool isWrite = rec->ExceptionInformation[0] != 0;
            std::fprintf(stderr,
                "[cod3]   %s of address 0x%016llX\n",
                isWrite ? "write" : "read",
                static_cast<unsigned long long>(rec->ExceptionInformation[1]));
        }

        std::fprintf(stderr,
            "[cod3]   guest image : mapped at 0x%08X, size 0x%08X "
            "(entry 0x%08X)\n"
            "[cod3]   pool allocs : %zu live\n"
            "================================================================\n",
            kGuestImageBase, g_guest_heap_base,
            g_guest_entry_point, cod3_pool_live_allocations());

        print_guest_call_stack();
        cod3_dump_import_trace();
        std::fflush(stderr);
        return EXCEPTION_EXECUTE_HANDLER;
    });

    // A vectored handler runs before the unhandled one and, crucially, before
    // the thread's stack has been exhausted. A guest stack overflow walks off
    // the bottom of the host stack, and by the time an unhandled-exception
    // filter runs there is no stack left to format the report from - which is
    // exactly the silent death this was added to diagnose.
    AddVectoredExceptionHandler(1, [](EXCEPTION_POINTERS* info) -> LONG {
        const EXCEPTION_RECORD* rec = info->ExceptionRecord;

        const uintptr_t bad =
            rec->NumberParameters >= 2
                ? static_cast<uintptr_t>(rec->ExceptionInformation[1])
                : 0;

        // A fault whose target is a plausible guest address is the guest's
        // problem and is left for the full report.
        if (bad >= kGuestImageBase && bad < kGuestImageBase + 0x10000000ull)
            return EXCEPTION_CONTINUE_SEARCH;

        // Only a stack overflow means "ran off the bottom of the host stack".
        // Saying so for every fault outside the guest address space was
        // misleading - it led to chasing a non-existent recursion when the
        // fault was an ordinary access violation at a bad pointer.
        const bool isStackOverflow =
            rec->ExceptionCode == EXCEPTION_STACK_OVERFLOW;

        std::fflush(stdout);
        std::fprintf(stderr,
            "\n"
            "================================================================\n"
            "[cod3] FATAL (vectored): %s (0x%08X)\n"
            "[cod3]   host PC     : 0x%016llX\n"
            "[cod3]   fault addr  : 0x%llX%s\n",
            describe_exception(rec->ExceptionCode),
            static_cast<uint32_t>(rec->ExceptionCode),
            static_cast<unsigned long long>(
                reinterpret_cast<uintptr_t>(rec->ExceptionAddress)),
            static_cast<unsigned long long>(bad),
            isStackOverflow
                ? "  (stack overflow: the guest ran off the bottom of the\n"
                  "[cod3]   host stack, so the unhandled filter could not run)"
                : "  (not a guest address - a bad host-side pointer)");

        // Attribute the host PC even here. The unhandled filter normally does
        // this, but it cannot run once the stack is gone, and without it the
        // report names no guest address at all - which is the whole point of
        // having one.
        {
            uint32_t guest = 0;
            uint64_t distance = 0;
            if (attribute_host_pc(rec->ExceptionAddress, &guest, &distance))
            {
                std::fprintf(stderr,
                    "[cod3]   guest addr  : 0x%08X  (sub_%.8X, +%llu bytes)\n",
                    guest, guest, static_cast<unsigned long long>(distance));
            }
            else
            {
                std::fprintf(stderr,
                    "[cod3]   guest addr  : not inside a translated function\n");
            }
        }

        print_guest_call_stack();
        cod3_report_guest_entries();
        cod3_dump_heap_slots();
        std::fflush(stderr);

        // Terminate rather than continue: once the stack is gone there is
        // nothing sensible left to return to.
        return EXCEPTION_EXECUTE_HANDLER;
    });
}