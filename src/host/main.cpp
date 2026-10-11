// =============================================================================
//  COD3 Recomp - host launcher
//
//  Loads the Call of Duty 3 Xbox 360 executable into guest memory and transfers
//  control to its recompiled entry point. Everything the guest does from there
//  on runs through the recompiled PowerPC translation.
//
//  Usage:
//      COD3.exe [--xex <path>] [--title "Call of Duty 3"] [--no-window]
// =============================================================================

#include "cod3_runtime.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace {

// Path to default.xex, resolved relative to this executable so the game can be
// launched from anywhere as long as the dump sits next to the binary.
std::string executable_directory()
{
    char buffer[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameA(nullptr, buffer, MAX_PATH);
    if (n == 0)
        return ".";

    std::string path(buffer, n);
    const size_t slash = path.find_last_of("\\/");
    if (slash == std::string::npos)
        return ".";
    return path.substr(0, slash);
}

bool file_exists(const std::string& path)
{
    const DWORD attrs = GetFileAttributesA(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

void print_usage()
{
    std::printf(
        "COD3 - Call of Duty 3 (Xbox 360) static recompilation\n"
        "\n"
        "Usage: COD3.exe [options]\n"
        "\n"
        "Options:\n"
        "  --xex <path>     Path to default.xex. Defaults to looking next to the\n"
        "                   executable, then at the original dump location.\n"
        "  --title <text>   Window title. Defaults to the game name.\n"
        "  --no-window      Do not create a host window (headless bring-up).\n"
        "  --help           Show this message.\n"
        "\n"
        "Environment:\n"
        "  COD3_QUIET_IMPORTS=1   Suppress the per-call trace of kernel APIs that\n"
        "                         are not yet implemented in the runtime.\n");
}

} // namespace

int main(int argc, char** argv)
{
    // Install the crash attributor first, so even a failure during load is
    // reported against the right guest address.
    cod3_install_crash_handler();

    const char* xexOverride = nullptr;
    std::string windowTitle = "Call of Duty 3 (Xbox 360 recomp)";
    bool createWindow = true;

    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        if (arg == "--xex" && i + 1 < argc)
        {
            xexOverride = argv[++i];
        }
        else if (arg == "--title" && i + 1 < argc)
        {
            windowTitle = argv[++i];
        }
        else if (arg == "--no-window")
        {
            createWindow = false;
        }
        else if (arg == "--help" || arg == "-h")
        {
            print_usage();
            return 0;
        }
        else
        {
            std::fprintf(stderr, "unknown option: %s\n\n", arg.c_str());
            print_usage();
            return 2;
        }
    }

    // ---------------------------------------------------------------------
    // Locate the XEX.
    // ---------------------------------------------------------------------
    std::string xexPath;
    if (xexOverride != nullptr)
    {
        xexPath = xexOverride;
    }
    else
    {
        const std::string exeDir = executable_directory();
        const char* candidates[] = {
            "default.xex",
            "..\\..\\Call of Duty 3\\default.xex",
            "..\\..\\..\\Call of Duty 3\\default.xex",
        };
        for (const char* candidate : candidates)
        {
            std::string full = exeDir;
            full += "\\";
            full += candidate;
            if (file_exists(full))
            {
                xexPath = full;
                break;
            }
        }
    }

    if (xexPath.empty() || !file_exists(xexPath))
    {
        std::fprintf(stderr,
            "[cod3] could not find default.xex.\n"
            "        Pass it explicitly: COD3.exe --xex \"<path to default.xex>\"\n");
        return 3;
    }

    std::fprintf(stderr, "[cod3] executable : %s\n", xexPath.c_str());

    // ---------------------------------------------------------------------
    // Load the guest image.
    // ---------------------------------------------------------------------
    if (!cod3_load_image(xexPath.c_str()))
    {
        std::fprintf(stderr, "[cod3] failed to load the guest image\n");
        return 4;
    }

    // ---------------------------------------------------------------------
    // Host window.
    // ---------------------------------------------------------------------
    // The Xbox 360's GPU is an Xenos, and its command processor is not
    // emulated by this runtime. The window is created up front anyway so that
    // a graphics backend can be attached to it as that work lands; right now it
    // simply hosts the guest's title bar and message loop.
    HWND hwnd = nullptr;
    if (createWindow)
    {
        WNDCLASSA wc = {};
        wc.lpfnWndProc = DefWindowProcA;
        wc.hInstance = GetModuleHandleA(nullptr);
        wc.hCursor = LoadCursorA(nullptr, IDC_ARROW);
        wc.lpszClassName = "COD3RecompHost";
        RegisterClassA(&wc);

        RECT rect = { 0, 0, 1280, 720 };
        AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);

        hwnd = CreateWindowA(
            wc.lpszClassName, windowTitle.c_str(), WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT, CW_USEDEFAULT,
            rect.right - rect.left, rect.bottom - rect.top,
            nullptr, nullptr, wc.hInstance, nullptr);

        if (hwnd != nullptr)
        {
            ShowWindow(hwnd, SW_SHOW);
            UpdateWindow(hwnd);
        }
    }

    // ---------------------------------------------------------------------
    // Enter the recompiled title.
    // ---------------------------------------------------------------------
    std::fprintf(stderr,
        "\n[cod3] entering recompiled entry point 0x%08X\n"
        "[cod3] (this is where unimplemented kernel APIs and the missing GPU\n"
        "[cod3]  command processor will surface)\n\n",
        g_guest_entry_point);

    // The Xbox 360 loader hands the title a context pointer and a reason code.
    PPCContext ctx = {};
    ctx.r3.u32 = 0;            // context
    ctx.r4.u32 = 0;            // reason / reason code
    ctx.r5.u32 = 0;
    ctx.r6.u32 = 0;
    ctx.r2.u32 = 0;

    // r1 is the guest stack pointer. The runtime reserved a dedicated stack
    // region past the image; see cod3_load_image().
    ctx.r1.u32 = g_guest_stack_top;

    // The entry point is a normal PowerPC function reached by `bl`, so the link
    // register must look like a return address. Nothing returns through it, but
    // `mflr r12` at the top of _xstart reads it and stores r12 to the stack, so
    // leaving it as zero fills the guest stack with a bogus return address.
    // Point it at the image entry so the value is at least a real guest VA.
    ctx.lr = g_guest_entry_point;

    // PowerPC requires r1 to be 16-byte aligned. The stack top is page aligned
    // minus 0x10000, so it already satisfies this.
    ctx.r1.u32 &= ~0xFu;

    // Diagnostic dump of the launch state. Cheap, and it settles questions about
    // what the CRT actually receives immediately rather than by inference.
    if (std::getenv("COD3_TRACE_LAUNCH") != nullptr)
    {
        std::fprintf(stderr,
            "[cod3] launch state: r1=0x%08X r2=0x%08X r3=0x%08X r4=0x%08X\n"
            "[cod3]              r5=0x%08X r6=0x%08X r12=0x%08X lr=0x%08X\n"
            "[cod3]   image 0x%08X  stack 0x%08X..0x%08X  heap 0x%08X\n",
            ctx.r1.u32, ctx.r2.u32, ctx.r3.u32, ctx.r4.u32,
            ctx.r5.u32, ctx.r6.u32, ctx.r12.u32, static_cast<uint32_t>(ctx.lr),
            kGuestImageBase, g_guest_stack_base, g_guest_stack_top,
            g_guest_heap_base);
        std::fflush(stderr);
    }

    cod3_run_guest_function(g_guest_entry_point, ctx);

    std::fprintf(stderr, "\n[cod3] entry point returned 0x%08X\n", ctx.r3.u32);
    cod3_dump_import_trace();
    std::fprintf(stderr, "[cod3] live pool allocations at exit: %zu\n",
                 cod3_pool_live_allocations());

    if (hwnd != nullptr)
    {
        MessageBoxA(hwnd, "The recomp reached the end of the entry point.\n\n"
                          "See the console output for the kernel APIs it used.",
                    "COD3 Recomp", MB_OK | MB_ICONINFORMATION);
    }

    return 0;
}