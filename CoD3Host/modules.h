#pragma once

// The levels' own code.
//
// Every single player level of this title ships its script code compiled to
// PowerPC in a XEX DLL of its own (sp\<level>\<level>.dll), which the title
// loads with XexLoadImage when the level starts and asks for one export from.
// Each DLL is recompiled like the title itself, into a library named for the
// level, and this loads the DLL's image into guest memory when the title asks
// and switches the indirect call table to that level's functions.
//
// All the DLLs share one image base, so at most one is loaded at a time,
// which is also how the title uses them.

#include <cstdint>
#include <string>

#include <ppc_context.h>

namespace Modules
{
    // XexLoadImage: loads the DLL at the guest path, runs its entry point on
    // the calling thread, and returns the module handle, or nought with the
    // reason in the log.
    uint32_t Load(PPCContext& ctx, uint8_t* base, const std::string& guestPath);

    // XexUnloadImage: the module's entry point runs with DLL_PROCESS_DETACH
    // on the calling thread, as the loader runs it, and then the module is
    // forgotten. Its memory stays until the next module lands on it.
    void Unload(PPCContext& ctx, uint8_t* base, uint32_t handle);

    // XexGetProcedureAddress: the address of an export by ordinal, or nought.
    uint32_t Export(uint32_t handle, uint32_t ordinal);

    // The recompiled function at a guest address inside the loaded module,
    // or null: what the indirect call table is for the title's own code.
    PPCFunc* Lookup(uint32_t address);

    // Whether the address is inside the loaded module's image.
    bool Contains(uint32_t address);

    // The loaded module's name for diagnostics, empty when none is loaded.
    const char* LoadedName();
}
