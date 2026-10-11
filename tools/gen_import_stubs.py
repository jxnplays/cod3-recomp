#!/usr/bin/env python3
"""Generate the Xbox 360 kernel import table implementation for the COD3 runtime.

XenonRecomp emits a call to `__imp__<Name>` for every function the XEX imports
from xboxkrnl.exe / xam.xex. This project owns those definitions: the runtime
resolves them to real Win32 behaviour where it matters and to logging stubs
everywhere else, so the recomp can be brought up incrementally instead of all
at once.

The list of required symbols is derived from the recompiler's own output
(ppc_recomp_shared.h), so it can never drift from the generated code.

Usage:
    python gen_import_stubs.py <build/ppc> <output.cpp> [--dry-run]
"""

import argparse
import os
import re
import sys

# Functions implemented by hand in kernel_imports_core.cpp. They are excluded
# from the generated stub file so there are no duplicate definitions.
HAND_IMPLEMENTED = {
    # memory
    "ExAllocatePool", "ExAllocatePoolWithTag", "ExAllocatePoolTypeWithTag",
    "ExFreePool",
    # critical sections
    "RtlInitializeCriticalSection", "RtlInitializeCriticalSectionAndSpinCount",
    "RtlEnterCriticalSection", "RtlLeaveCriticalSection",
    "RtlTryEnterCriticalSection", "RtlDeleteCriticalSection",
    # IRQL
    "KeGetCurrentIrql", "KeRaiseIrql", "KfRaiseIrql",
    "KeLowerIrql", "KfLowerIrql",
    # events / mutexes / semaphores / waits
    "KeInitializeEvent", "KeSetEvent", "KeResetEvent", "KePulseEvent",
    "KeInitializeMutex", "KeReleaseMutex",
    "KeWaitForSingleObject", "KeWaitForMultipleObjects",
    "KeInitializeSemaphore", "KeReleaseSemaphore",
    # threads
    "ExCreateThread", "KeCreateThread", "KeResumeThread",
    "KeTerminateThread", "KeSetBasePriorityThread", "KeSetPriorityThread",
    # thread local storage (the CRT allocates slots during boot)
    "KeTlsAlloc", "KeTlsFree", "KeTlsGetValue", "KeTlsSetValue",
    # object manager
    "ObReferenceObjectByHandle", "ObDereferenceObject",
    "ObDereferenceObjectByPointer", "ObReferenceObjectByPointer",
    # memory descriptors
    "MmGetPhysicalAddress", "MmMapIoSpace", "MmUnmapIoSpace", "MmMapIoSpaceEx",
    # Implemented in src/runtime/mm_physical.cpp. The guest calls this 60 times
    # during boot and the generated stub returns 0, so every call failed and the
    # structures behind them were never built.
    "MmAllocatePhysicalMemoryEx",
    # virtual memory
    "NtAllocateVirtualMemory", "NtFreeVirtualMemory", "NtQueryVirtualMemory",
    # Implemented in src/runtime/kernel_imports_core.cpp. Real Win32 objects
    # underneath, so a guest that waits on an event really blocks. These were
    # being called ten times each and returning nothing.
    "NtCreateEvent", "NtCreateMutant", "NtWaitForSingleObjectEx",
    "NtReleaseMutant", "NtResumeThread", "KeSetAffinityThread",
    # module / export resolution (the game resolves most of D3D dynamically)
    "XexGetModuleHandle", "XexGetProcedureAddress", "XexGetXEntryInfo",
    "XexGetOptionalXEntryInfo", "XexGetTitleId",
    # XEX header fields: the CRT cannot set up its heap without these
    "RtlImageXexHeaderField",
    # misc
    "DbgPrint", "RtlNtStatusToDosError",
    "ExRegisterTitleTerminateNotification",
    # video display
    "XGetVideoMode", "VdGetCurrentDisplayInformation", "VdSetDisplayMode",
    "VdQueryVideoMode", "VdQueryVideoFlags", "VdPersistDisplay",
    "VdGetCurrentDisplayGamma", "VdSetDisplayGamma",
}

PREAMBLE = """// =============================================================================
//  GENERATED FILE - DO NOT EDIT BY HAND
//  Produced by tools/gen_import_stubs.py from the XenonRecomp output in
//  build/ppc/ppc_recomp_shared.h. Regenerate with tools/build.ps1.
// =============================================================================
//
//  Every symbol here corresponds to an Xbox 360 kernel API that Call of Duty 3
//  imports from xboxkrnl.exe / xam.xex. The recompiled game code calls through
//  these as `__imp__<Name>(ctx, base)` with the PowerPC calling convention:
//
//      r3 .. r10   integer arguments
//      f1 .. f12   floating point arguments
//      r3          integer return value
//      f1          floating point return value
//
//  NOTE ON LINKAGE: the recompiler declares these with PPC_EXTERN_FUNC, which
//  is a plain C++ declaration. They must therefore be *defined* with C++
//  linkage (PPC_FUNC), not with PPC_FUNC_IMPL/extern "C" - otherwise the
//  definitions mangle differently from the references and the link fails with
//  undefined __imp__ symbols. PPC_FUNC_IMPL is only for the recompiler's own
//  weak internal aliases.
//
//  Stubs return 0 (NTSTATUS_SUCCESS / NULL / 0), which is the safe default for
//  the majority of these APIs: the caller sees "it worked" and continues. Each
//  one is registered in the import trace table so that the runtime can report
//  exactly which APIs the title actually reaches as it boots, which is how the
//  hand-written implementations in kernel_imports_core.cpp are prioritised.
// =============================================================================

#include "ppc_context.h"
#include "cod3_runtime.h"

"""

# Returns a value that makes the guest see success.
RETURN_STMT = "\tctx.r3.u64 = 0;\n"


def collect_imports(ppc_dir):
    header = os.path.join(ppc_dir, "ppc_recomp_shared.h")
    if not os.path.isfile(header):
        sys.exit(f"missing {header}; run XenonRecomp first")
    with open(header, "r", encoding="utf-8", errors="replace") as f:
        text = f.read()
    names = []
    for m in re.finditer(r"PPC_EXTERN_FUNC\(__imp__(\w+)\);", text):
        names.append(m.group(1))
    seen = set()
    ordered = []
    for n in names:
        if n not in seen:
            seen.add(n)
            ordered.append(n)
    return ordered


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("ppc_dir")
    ap.add_argument("out")
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    imports = collect_imports(args.ppc_dir)
    stubs = [n for n in imports if n not in HAND_IMPLEMENTED]

    if args.dry_run:
        print(f"{len(imports)} imports total")
        print(f"{len(HAND_IMPLEMENTED & set(imports))} hand-implemented "
              f"(of {len(HAND_IMPLEMENTED)} declared)")
        print(f"{len(stubs)} generated stubs")
        for n in imports:
            kind = "hand" if n in HAND_IMPLEMENTED else "stub"
            print(f"  {kind:5} {n}")
        return

    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)

    with open(args.out, "w", encoding="utf-8", newline="\n") as f:
        f.write(PREAMBLE)
        for name in stubs:
            f.write(
                f"PPC_FUNC(__imp__{name})\n"
                f"{{\n"
                f"\tcod3_trace_unimplemented(\"{name}\");\n"
                f"{RETURN_STMT}"
                f"}}\n\n"
            )

    print(f"wrote {args.out}: {len(stubs)} stubs "
          f"({len(imports) - len(stubs)} hand-implemented)")


if __name__ == "__main__":
    main()