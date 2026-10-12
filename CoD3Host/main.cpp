#include "log.h"
#include "installer.h"
#include "kernel.h"
#include "settings.h"
#include <cstdlib>
#include "scheduler.h"
#include "sampler.h"
#include "window.h"
#include "xdvdfs.h"

#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include <Windows.h>
#include <timeapi.h>
#pragma comment(lib, "winmm.lib")

namespace fs = std::filesystem;

namespace
{
    fs::path ExecutableDirectory()
    {
        wchar_t buffer[MAX_PATH * 4] = {};
        const DWORD length = GetModuleFileNameW(nullptr, buffer, DWORD(std::size(buffer)));
        if (length == 0) return fs::current_path();
        return fs::path(buffer).parent_path();
    }

    void Banner()
    {
        printf("Call of Duty 3 - static recompilation\n");
        printf("Recompiled from the Xbox 360 executable with XenonRecomp.\n");
        printf("\n");
    }

    void Usage()
    {
        printf("Usage: CoD3 [options]\n");
        printf("\n");
        printf("With no options, the game starts if it is already installed next to\n");
        printf("this program, and first time setup runs if it is not.\n");
        printf("\n");
        printf("  --iso <file>      install from this disc image, no file picker\n");
        printf("  --game <folder>   use this extracted folder where it is\n");
        printf("  --copy <folder>   copy this extracted folder into game/\n");
        printf("  --list-iso <file> show what is inside a disc image and exit\n");
        printf("  --extract-one <iso> <path in iso> <output file>\n");
        printf("                    pull a single file out of a disc image\n");
        printf("  --install-only    run setup but do not start the game\n");
        printf("  --help            this text\n");
        printf("\n");
    }

    // Reads a disc image and prints its contents. Useful on its own for
    // checking an image before committing to a multi-gigabyte extraction.
    int ListIso(const fs::path& isoPath)
    {
        XDvdFs::Image image;
        std::string error;
        if (!image.Open(isoPath, error))
        {
            fprintf(stderr, "%s\n", error.c_str());
            return 1;
        }

        printf("%s\n", isoPath.string().c_str());
        printf("  filesystem  %s, starting at offset 0x%llX\n",
            image.FormatName(), (unsigned long long)image.BaseOffset());

        std::vector<XDvdFs::Entry> files;
        if (!image.ListFiles(files, error))
        {
            fprintf(stderr, "  %s\n", error.c_str());
            return 1;
        }

        uint64_t total = 0;
        bool hasXex = false;
        for (const auto& file : files)
        {
            total += file.size;
            if (file.path == "default.xex") hasXex = true;
        }

        printf("  contents    %zu files, %.2f GB\n", files.size(), total / 1073741824.0);
        printf("  default.xex %s\n\n", hasXex ? "present" : "MISSING, this is not the game");

        for (const auto& file : files)
            printf("  %12llu  %s\n", (unsigned long long)file.size, file.path.c_str());

        return hasXex ? 0 : 1;
    }

    // Pulls one file out of a disc image and writes it where asked. Reading a
    // known file and comparing it byte for byte is how the reader gets checked
    // without writing a whole disc to disk.
    int ExtractOne(const fs::path& isoPath, const std::string& insidePath,
                   const fs::path& destination)
    {
        XDvdFs::Image image;
        std::string error;
        if (!image.Open(isoPath, error))
        {
            fprintf(stderr, "%s\n", error.c_str());
            return 1;
        }

        std::vector<XDvdFs::Entry> files;
        if (!image.ListFiles(files, error))
        {
            fprintf(stderr, "%s\n", error.c_str());
            return 1;
        }

        for (const auto& file : files)
        {
            if (file.path != insidePath) continue;

            std::vector<uint8_t> data;
            if (!image.ReadEntry(file, data, error))
            {
                fprintf(stderr, "%s\n", error.c_str());
                return 1;
            }

            FILE* out = nullptr;
            if (_wfopen_s(&out, destination.c_str(), L"wb") != 0 || out == nullptr)
            {
                fprintf(stderr, "cannot write %s\n", destination.string().c_str());
                return 1;
            }
            fwrite(data.data(), 1, data.size(), out);
            fclose(out);

            printf("%s -> %s (%zu bytes)\n",
                insidePath.c_str(), destination.string().c_str(), data.size());
            return 0;
        }

        fprintf(stderr, "%s is not in the image\n", insidePath.c_str());
        return 1;
    }

    void ExplainIncompleteRuntime()
    {
        printf("\n");
        printf("Note: this boots, renders, and reaches the loading screen. It does\n");
        printf("not reach the menu: the title streams part of its archive and then\n");
        printf("waits. What it waits for is reported as it runs. See STATUS.md.\n");
        printf("\n");
    }
}

namespace
{
    int Run(int argc, char** argv);
}

int main(int argc, char** argv)
{
    Log::Start();
    // Every exit goes through Kernel::Exit, which waits for a key when this
    // program owns the console. Started from Explorer that is the difference
    // between a readable window and one that vanishes instantly.
    Kernel::Exit(Run(argc, argv));
}

namespace
{

int Run(int argc, char** argv)
{
    // --- Command line ------------------------------------------------------
    Installer::Source source;
    bool installOnly = false;

    for (int i = 1; i < argc; i++)
    {
        const std::string option = argv[i];
        const bool hasValue = (i + 1) < argc;

        if (option == "--help" || option == "-h" || option == "/?")
        {
            Usage();
            return 0;
        }
        if (option == "--install-only")
        {
            installOnly = true;
        }
        else if (option == "--list-iso" && hasValue)
        {
            return ListIso(fs::path(argv[++i]));
        }
        else if (option == "--extract-one" && (i + 3) < argc)
        {
            const fs::path iso = argv[i + 1];
            const std::string inside = argv[i + 2];
            const fs::path destination = argv[i + 3];
            return ExtractOne(iso, inside, destination);
        }
        else if (option == "--iso" && hasValue)
        {
            source.iso = fs::path(argv[++i]);
        }
        else if (option == "--game" && hasValue)
        {
            source.folder = fs::path(argv[++i]);
            source.copyFolder = false;
        }
        else if (option == "--copy" && hasValue)
        {
            source.folder = fs::path(argv[++i]);
            source.copyFolder = true;
        }
        else
        {
            fprintf(stderr, "Unrecognised option: %s\n\n", option.c_str());
            Usage();
            return 1;
        }
    }

    Banner();

    const fs::path exeDirectory = ExecutableDirectory();

    // --- Find the game, running first time setup if needed ----------------
    Installer::GameLocation game;
    std::string error;
    if (!Installer::Locate(exeDirectory, source, game, error))
    {
        if (error.empty())
        {
            printf("Cancelled. Nothing was changed.\n");
            return 0;
        }
        fprintf(stderr, "Setup failed: %s\n", error.c_str());
        return 1;
    }

    printf("Game: %s\n\n", game.root.string().c_str());

    // Useful while the runtime is still missing: install the game without
    // watching the boot stop at the first kernel call.
    if (installOnly)
    {
        printf("Setup done, not booting (--install-only).\n");
        return 0;
    }

    ExplainIncompleteRuntime();

    // --- Bring up the guest -----------------------------------------------
    // The file imports resolve guest paths under the installed game, and put
    // anything the title writes in a saves folder next to this program.
    Guest::GameRoot = game.root;
    Settings::Load(exeDirectory);
    Kernel::InitializeFileSystem(exeDirectory);

    if (!Guest::Initialize(game.executable.string().c_str()))
    {
        fprintf(stderr, "\nCould not set up the guest address space.\n");
        Guest::Shutdown();
        return 1;
    }

    // A fresh context. The stack pointer is the one register the entry point
    // cannot supply for itself.
    PPCContext ctx{};
    ctx.r1.u32 = Guest::StackBase - 0x100;
    ctx.r13.u32 = Guest::CreateThreadPointer(1);
    ctx.fpscr.loadFromHost();
    Kernel::SetCurrentContext(&ctx);

    // Reports where the guest threads are, so a stall in translated code can be
    // located without a debugger.
    // Opened before the guest starts so there is something on screen from the
    // first moment, rather than a console and nothing else.
    Window::Open();

    Scheduler::Attach(Guest::CurrentProcessor(ctx));
    Kernel::RegisterEntryThread();
    Sampler::Start(10);
    Kernel::StartWatchdog();

    // A millisecond timer, for the vertical blank and every wait with a
    // timeout: the default is fifteen, and a runtime built out of short
    // sleeps runs at whatever the timer allows.
    timeBeginPeriod(1);

    // Whatever ends this process, say so. A run that simply stops says nothing
    // about whether the title asked to quit or something ended it.
    atexit([]() {
        printf("\nThe process is exiting.\n");
        fflush(stdout);
    });

    // A guest address to look at, named in the environment rather than built
    // in, so chasing a particular object costs a run and not a rebuild.
    // COD3_WATCH takes a hexadecimal guest address: its first four words are
    // printed here, before any guest code runs, and a hardware write watchpoint
    // is set on it so the next write to it says which guest function did it.
    // COD3_WATCH=hex,r watches reads as well as writes: who reads a string,
    // for instance. COD3_WATCH_SECOND=N arms it that many seconds in
    // instead (kernel_video.cpp), for something that is not there yet.
    if (const char* watch = getenv("COD3_WATCH"); watch != nullptr && getenv("COD3_WATCH_SECOND") == nullptr)
    {
        const uint32_t address = uint32_t(strtoul(watch, nullptr, 16));
        const bool reads = strchr(watch, ',') != nullptr && strchr(watch, 'r') != nullptr;
        if (*watch != 0)   // zero is a valid address to watch: the null object
        {
            printf("watch: 0x%08X holds %08X %08X %08X %08X before the guest starts\n",
                address,
                Guest::Read32(Guest::Base, address),
                Guest::Read32(Guest::Base, address + 4),
                Guest::Read32(Guest::Base, address + 8),
                Guest::Read32(Guest::Base, address + 12));
            Kernel::WatchWrite(address, reads);
            Kernel::ArmWatchpoints();
        }
    }

    // COD3_PEEK takes hexadecimal guest addresses separated by commas and
    // prints sixty four bytes of each, as words and as text, then exits: a
    // way to read a string or a table out of the image without running it.
    if (const char* peek = getenv("COD3_PEEK"))
    {
        for (const char* at = peek; *at != 0;)
        {
            char* end = nullptr;
            const uint32_t address = uint32_t(strtoul(at, &end, 16));
            if (end == at) break;
            printf("peek: 0x%08X\n", address);
            for (uint32_t row = 0; row < 64; row += 16)
            {
                printf("  %08X:", address + row);
                for (uint32_t w = 0; w < 16; w += 4) printf(" %08X", Guest::Read32(Guest::Base, address + row + w));
                printf("  ");
                for (uint32_t b = 0; b < 16; b++)
                {
                    const uint8_t c = Guest::Base[address + row + b];
                    printf("%c", c >= 32 && c < 127 ? c : '.');
                }
                printf("\n");
            }
            at = *end == ',' ? end + 1 : end;
        }
        fflush(stdout);
        return 0;
    }

    // COD3_IMAGEDUMP=path writes the loaded image, 0x82000000 on for sixteen
    // megabytes, to a file and exits: the strings and tables of the title
    // searched offline, with their guest addresses at their offsets.
    if (const char* imageDump = getenv("COD3_IMAGEDUMP"))
    {
        if (FILE* out = fopen(imageDump, "wb"))
        {
            fwrite(Guest::Base + 0x82000000u, 1, 16u << 20, out);
            fclose(out);
            printf("image: sixteen megabytes from 0x82000000 written to %s\n", imageDump);
        }
        fflush(stdout);
        return 0;
    }

    // COD3_MEMDUMP=hexaddress,hexsize,seconds,path writes that much guest
    // memory to a file once the run is that many seconds old, while the
    // title goes on: what a level holds once loaded (the archives are
    // compressed on the disc) searched offline. Several ranges may be given,
    // separated by semicolons.
    for (const char* memDump = getenv("COD3_MEMDUMP"); memDump != nullptr && *memDump != 0;)
    {
        const char* end = strchr(memDump, ';');
        const std::string spec(memDump, end != nullptr ? size_t(end - memDump) : strlen(memDump));
        memDump = end != nullptr ? end + 1 : nullptr;
        char path[512] = {};
        unsigned address = 0, size = 0, seconds = 0;
        if (sscanf(spec.c_str(), "%x,%x,%u,%511s", &address, &size, &seconds, path) == 4)
        {
            const std::string where = path;
            std::thread([address, size, seconds, where]() {
                Sleep(seconds * 1000);
                if (FILE* out = fopen(where.c_str(), "wb"))
                {
                    // Page by page, through ReadProcessMemory, which answers
                    // a page it cannot read with a failure rather than a
                    // fault for the guest's handlers: such a page reads as
                    // zeros.
                    std::vector<uint8_t> page(65536);
                    for (uint64_t at = address; at < uint64_t(address) + size; at += page.size())
                    {
                        SIZE_T got = 0;
                        if (!ReadProcessMemory(GetCurrentProcess(), Guest::Base + at, page.data(), page.size(), &got))
                        {
                            for (size_t part = 0; part < page.size(); part += 4096)
                                if (!ReadProcessMemory(GetCurrentProcess(), Guest::Base + at + part, page.data() + part, 4096, &got))
                                    std::fill(page.begin() + part, page.begin() + part + 4096, uint8_t(0));
                        }
                        fwrite(page.data(), 1, page.size(), out);
                    }
                    fclose(out);
                    printf("memdump: 0x%08X, 0x%X bytes, written to %s\n", address, size, where.c_str());
                    fflush(stdout);
                }
            }).detach();
        }
    }

    printf("\nEntering guest code at _xstart\n");
    fflush(stdout);

    _xstart(ctx, Guest::Base);

    // Reaching here would mean the guest returned from its entry point, which
    // it should not do while the kernel imports are still stubs.
    printf("\nGuest returned from _xstart with r3 = 0x%08X\n", ctx.r3.u32);
    Guest::Shutdown();
    return 0;
}

}   // namespace
