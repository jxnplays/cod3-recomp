// =============================================================================
//  COD3 Recomp - host launcher
//
//  Loads the Call of Duty 3 Xbox 360 executable into guest memory and transfers
//  control to its recompiled entry point. Everything the guest does from there
//  on runs through the recompiled PowerPC translation.
//
//  The recomp does not embed the game's own files, so it looks for them the way
//  other recompilations do: next to the executable, in a remembered location
//  from a previous run, and failing that it asks. If this build is run from a
//  console it prints the path it wants; if it is run by double-click it opens a
//  folder picker.
//
//  Usage:
//      COD3.exe [--xex <path>] [--game-dir <dir>] [--title "..."] [--no-window]
// =============================================================================

#include "cod3_runtime.h"

#include <windows.h>
#include <commdlg.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#pragma comment(lib, "comdlg32.lib")

namespace {

// Remembered between runs so the picker is only shown when it has to be.
// Narrow, because it is combined with a path built from GetModuleFileNameA.
constexpr char kIniPath[] = "cod3-recomp.ini";

// The single executable the recomp needs to boot. The rest of the game data is
// read later by the title itself, through the storage APIs.
constexpr char kDefaultXex[] = "default.xex";

// Multiplayer title. Selected rather than auto-detected, because both XEXs are
// valid inputs and guessing would silently pick the wrong one.
constexpr char kMultiplayerXex[] = "codmp_xenonf.xex";

std::wstring widen(const std::string& s)
{
    if (s.empty())
        return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(),
                                      static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0)
        return std::wstring();
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                        out.data(), n);
    return out;
}

std::string narrow(const std::wstring& s)
{
    if (s.empty())
        return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(),
                                      static_cast<int>(s.size()), nullptr, 0,
                                      nullptr, nullptr);
    if (n <= 0)
        return std::string();
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                        out.data(), n, nullptr, nullptr);
    return out;
}

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
    if (path.empty())
        return false;
    const DWORD attrs = GetFileAttributesA(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES &&
           !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

std::string ini_path()
{
    char buffer[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameA(nullptr, buffer, MAX_PATH);
    std::string path(buffer, n);
    const size_t slash = path.find_last_of("\\/");
    if (slash != std::string::npos)
        path.resize(slash + 1);
    path += kIniPath;
    return path;
}

std::string read_saved_xex()
{
    char value[MAX_PATH] = {};
    GetPrivateProfileStringA("game", "xex", "", value, sizeof(value),
                             ini_path().c_str());
    return value[0] != '\0' ? std::string(value) : std::string();
}

void save_xex(const std::string& xex)
{
    WritePrivateProfileStringA("game", "xex", xex.c_str(), ini_path().c_str());
}

bool looks_like_xex(const std::string& path)
{
    // A XEX2 container starts with the magic "XEX2". Checking it means a wrong
    // file - a .xex from a different game, or an unrelated binary - is rejected
    // immediately rather than failing much later inside the loader.
    HANDLE f = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                           nullptr);
    if (f == INVALID_HANDLE_VALUE)
        return false;

    char magic[4] = {};
    DWORD read = 0;
    const BOOL ok = ReadFile(f, magic, sizeof(magic), &read, nullptr);
    CloseHandle(f);

    return ok && read == sizeof(magic) &&
           std::memcmp(magic, "XEX2", 4) == 0;
}

// Ask the user to point at the game folder. Returns the chosen XEX path, or an
// empty string if they cancelled.
std::string ask_for_xex(bool interactive)
{
    const std::wstring title = L"Select the Call of Duty 3 (Xbox 360) folder";

    std::wstring buffer(MAX_PATH, L'\0');

    // These must outlive the dialog call: OPENFILENAME stores raw pointers, and
    // binding a temporary here leaves lpstrInitialDir dangling.
    const std::wstring initialDir = widen(executable_directory());

    // An .xex filter keeps the dialog showing the right file type, and
    // OFN_FILEMUSTEXIST means the user cannot hand back a path that is not there.
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = L"Xbox 360 executables (*.xex)\0*.xex\0All files (*.*)\0*.*\0\0";
    ofn.lpstrFile = buffer.data();
    ofn.nMaxFile = static_cast<DWORD>(buffer.size());
    ofn.lpstrTitle = title.c_str();
    ofn.lpstrInitialDir = initialDir.c_str();
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER |
                OFN_NOCHANGEDIR;

    if (interactive)
    {
        if (GetOpenFileNameW(&ofn))
        {
            std::string chosen = narrow(std::wstring(buffer.data()));
            if (file_exists(chosen) && looks_like_xex(chosen))
                return chosen;

            std::fprintf(stderr,
                "[cod3] %s does not look like an Xbox 360 executable "
                "(expected a XEX2 file).\n", chosen.c_str());
        }
        return std::string();
    }

    // Non-interactive (a console launch, or a test harness): ask on stderr
    // instead of opening a dialog that nobody can answer.
    std::fprintf(stderr,
        "\n"
        "  COD3 Recomp needs the original Call of Duty 3 (Xbox 360) files.\n"
        "\n"
        "  Point it at the default.xex from your Xbox 360 dump, for example:\n"
        "\n"
        "      COD3.exe --xex \"<path>\\\\default.xex\"\n"
        "\n"
        "  or pass the folder containing it:\n"
        "\n"
        "      COD3.exe --game-dir \"<path>\"\n"
        "\n");
    return std::string();
}

// Search order: explicit override, then a remembered path, then the usual
// places a dump tends to live relative to the executable.
std::string locate_xex(const std::string& overridePath,
                       const std::string& gameDir,
                       const std::string& wanted)
{
    if (!overridePath.empty())
        return file_exists(overridePath) ? overridePath : std::string();

    if (!gameDir.empty())
    {
        std::string full = gameDir;
        if (!full.empty() && full.back() != '\\' && full.back() != '/')
            full += '\\';
        full += wanted;
        if (file_exists(full))
            return full;
    }

    const std::string saved = read_saved_xex();
    if (!saved.empty() && file_exists(saved))
        return saved;

    const std::string exeDir = executable_directory();
    const char* relatives[] = {
        "",
        "game\\",
        "..\\..\\Call of Duty 3\\",
        "..\\..\\..\\Call of Duty 3\\",
    };
    for (const char* rel : relatives)
    {
        std::string full = exeDir + "\\" + rel + wanted;
        if (file_exists(full))
            return full;
    }
    return std::string();
}

void print_usage()
{
    std::printf(
        "COD3 - Call of Duty 3 (Xbox 360) static recompilation\n"
        "\n"
        "Usage: COD3.exe [options]\n"
        "\n"
        "Options:\n"
        "  --xex <path>     Path to default.xex. If omitted, the game folder is\n"
        "                   looked up next to this executable and in the location\n"
        "                   used on a previous run, and finally asked for.\n"
        "  --game-dir <dir> Folder containing the Xbox 360 dump.\n"
        "  --multiplayer    Load codmp_xenonf.xex instead of default.xex.\n"
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

    std::string xexOverride;
    std::string gameDir;
    std::string windowTitle = "Call of Duty 3 (Xbox 360 recomp)";
    bool createWindow = true;
    bool interactive = true;
    bool multiplayer = false;

    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        if (arg == "--xex" && i + 1 < argc)
        {
            xexOverride = argv[++i];
        }
        else if (arg == "--game-dir" && i + 1 < argc)
        {
            gameDir = argv[++i];
        }
        else if (arg == "--multiplayer")
        {
            // Both titles are supported inputs; the choice is explicit so the
            // recomp never silently boots the wrong one.
            xexOverride.clear();
            multiplayer = true;
            windowTitle = "Call of Duty 3 - Multiplayer (Xbox 360 recomp)";
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

    // A non-interactive run (no console attached, or a test harness) must not
    // block on a modal dialog.
    interactive = GetConsoleWindow() != nullptr;

    // ---------------------------------------------------------------------
    // Locate the XEX.
    // ---------------------------------------------------------------------
    const std::string wanted = multiplayer ? kMultiplayerXex : kDefaultXex;

    std::string xexPath = locate_xex(xexOverride, gameDir, wanted);

    if (xexPath.empty())
    {
        xexPath = ask_for_xex(interactive);
        if (!xexPath.empty())
            save_xex(xexPath);
    }

    if (xexPath.empty())
    {
        std::fprintf(stderr, "[cod3] no game files found; cannot start.\n");
        return 3;
    }

    if (!looks_like_xex(xexPath))
    {
        std::fprintf(stderr,
            "[cod3] %s is not an Xbox 360 executable (no XEX2 header).\n",
            xexPath.c_str());
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
    // The Xbox 360's graphics stack is not emulated by this runtime, so the
    // window is created up front to host the guest's message loop and to give
    // a graphics backend somewhere to attach as that work lands.
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
        "\n[cod3] entering recompiled entry point 0x%08X\n\n",
        g_guest_entry_point);

    PPCContext ctx = {};
    ctx.r1.u32 = g_guest_stack_top;

    // r13 is the PowerPC per-thread data register. On the 360 the XEX loader
    // builds the CRT thread block and passes it to the title before _xstart
    // runs; nothing in the guest assigns r13 on the boot path, so it has to
    // arrive set. Left at zero, the first CRT call that follows [r13+256] faults
    // on address 0x100. See src/runtime/crt_thread_block.cpp.
    ctx.r13.u32 = cod3_crt_thread_block();
    if (ctx.r13.u32 == 0)
    {
        std::fprintf(stderr,
            "[cod3] FATAL: the CRT thread block is not built; r13 would be 0 and"
            " the CRT will fault at 0x100. Was cod3_load_image() run?\n");
        return 1;
    }

    // The entry point is a normal PowerPC function reached by `bl`, so the link
    // register must look like a return address. Nothing returns through it, but
    // `mflr r12` at the top of _xstart reads it and stores r12 to the stack, so
    // leaving it as zero fills the guest stack with a bogus return address.
    // Point it at the image entry so the value is at least a real guest VA.
    ctx.lr = g_guest_entry_point;

    // PowerPC requires r1 to be 16-byte aligned.
    ctx.r1.u32 &= ~0xFu;

    if (std::getenv("COD3_TRACE_LAUNCH") != nullptr)
    {
        std::fprintf(stderr,
            "[cod3] launch state: r1=0x%08X r2=0x%08X r3=0x%08X r4=0x%08X\n"
            "[cod3]              r5=0x%08X r6=0x%08X r12=0x%08X lr=0x%08X\n"
            "[cod3]   image 0x%08X  stack 0x%08X..0x%08X  heap 0x%08X\n"
            "[cod3]   r13 (CRT thread block) 0x%08X\n",
            ctx.r1.u32, ctx.r2.u32, ctx.r3.u32, ctx.r4.u32,
            ctx.r5.u32, ctx.r6.u32, ctx.r12.u32, static_cast<uint32_t>(ctx.lr),
            kGuestImageBase, g_guest_stack_base, g_guest_stack_top,
            g_guest_heap_base, ctx.r13.u32);
        std::fflush(stderr);
    }

    cod3_run_guest_function(g_guest_entry_point, ctx);

    std::fprintf(stderr, "\n[cod3] entry point returned 0x%08X\n", ctx.r3.u32);
    cod3_dump_import_trace();
    std::fprintf(stderr, "[cod3] live pool allocations at exit: %zu\n",
                 cod3_pool_live_allocations());

    if (hwnd != nullptr)
    {
        MessageBoxA(hwnd,
                    "The recomp reached the end of the entry point.\n\n"
                    "See the console output for the kernel APIs it used.",
                    "COD3 Recomp", MB_OK | MB_ICONINFORMATION);
    }

    return 0;
}