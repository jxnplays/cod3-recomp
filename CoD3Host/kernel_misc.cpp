// Xbox 360 kernel imports: the leftovers.
//
// String and time conversion, module queries, memory statistics, and the C
// runtime entry points the title imports from the kernel rather than linking
// itself. Also the exception entry points, which cannot work: XenonRecomp does
// not translate exception handling at all, so anything that actually throws is
// unrecoverable here and says so rather than corrupting state quietly.

#include "kernel.h"
#include "coroutines.h"
#include "modules.h"
#include "sampler.h"
#include <map>
#include <mutex>

#include <csetjmp>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

#include <Windows.h>

namespace
{
    constexpr uint32_t X_STATUS_SUCCESS           = 0x00000000;
    constexpr uint32_t X_STATUS_INVALID_PARAMETER = 0xC000000D;
    constexpr uint32_t X_STATUS_NOT_FOUND         = 0xC0000225;

    std::string GuestString(const uint8_t* base, uint32_t address, size_t limit = 4096)
    {
        if (address == 0) return {};
        std::string out;
        for (size_t i = 0; i < limit; i++)
        {
            const char c = static_cast<char>(*(base + address + i));
            if (c == '\0') break;
            out += c;
        }
        return out;
    }

    // Pulls the variadic arguments of a guest call. On this ABI the first
    // arguments arrive in r3 upward and the rest sit in the caller's parameter
    // save area, which starts 0x38 into the frame r1 points at.
    struct GuestArguments
    {
        const PPCContext& ctx;
        const uint8_t* base;
        uint32_t index;            // next register, 3 through 10

        uint32_t NextWord()
        {
            if (index <= 10)
            {
                const PPCRegister* registers[] = {
                    nullptr, nullptr, nullptr,
                    &ctx.r3, &ctx.r4, &ctx.r5, &ctx.r6,
                    &ctx.r7, &ctx.r8, &ctx.r9, &ctx.r10,
                };
                return registers[index++]->u32;
            }
            const uint32_t stackSlot = ctx.r1.u32 + 0x38 + (index - 11) * 4;
            index++;
            return Guest::Read32(base, stackSlot);
        }
    };

    // Formats a guest printf call. Only the conversions a title realistically
    // uses are handled; anything else is copied through so the output shows
    // what was asked for instead of silently losing it.
    std::string FormatGuest(const uint8_t* base, const std::string& format,
                            GuestArguments& arguments)
    {
        std::string out;
        char scratch[512];

        for (size_t i = 0; i < format.size(); i++)
        {
            if (format[i] != '%') { out += format[i]; continue; }
            if (i + 1 >= format.size()) { out += '%'; break; }
            if (format[i + 1] == '%') { out += '%'; i++; continue; }

            // Copy the whole conversion specification.
            size_t end = i + 1;
            while (end < format.size() &&
                   strchr("diouxXeEfgGcspn", format[end]) == nullptr)
                end++;
            if (end >= format.size()) { out += format.substr(i); break; }

            std::string spec = format.substr(i, end - i + 1);
            const char conversion = format[end];
            i = end;

            // Length modifiers are meaningless once the value is a 32 bit
            // guest word, and passing them to the host printf would read the
            // wrong width.
            std::string cleaned;
            for (char c : spec)
                if (c != 'l' && c != 'h' && c != 'I' && c != '6' && c != '4')
                    cleaned += c;

            switch (conversion)
            {
            case 'd': case 'i':
                snprintf(scratch, sizeof(scratch), cleaned.c_str(),
                    static_cast<int>(arguments.NextWord()));
                out += scratch;
                break;
            case 'u': case 'o': case 'x': case 'X':
                snprintf(scratch, sizeof(scratch), cleaned.c_str(),
                    static_cast<unsigned int>(arguments.NextWord()));
                out += scratch;
                break;
            case 'c':
                snprintf(scratch, sizeof(scratch), cleaned.c_str(),
                    static_cast<int>(arguments.NextWord() & 0xFF));
                out += scratch;
                break;
            case 'p':
                snprintf(scratch, sizeof(scratch), "0x%08X", arguments.NextWord());
                out += scratch;
                break;
            case 's':
            {
                const std::string value = GuestString(base, arguments.NextWord());
                snprintf(scratch, sizeof(scratch), cleaned.c_str(), value.c_str());
                out += scratch;
                break;
            }
            case 'e': case 'E': case 'f': case 'g': case 'G':
                // Floating point arguments travel in the floating point
                // registers, not the general ones, and reading them correctly
                // needs the full variadic layout. Marked rather than guessed.
                out += "<float>";
                break;
            default:
                out += spec;
                break;
            }
        }
        return out;
    }
}

std::string Kernel::FormatGuestCall(const PPCContext& ctx, const uint8_t* base, uint32_t formatAddress, uint32_t firstRegister)
{
    GuestArguments arguments{ ctx, base, firstRegister };
    return FormatGuest(base, GuestString(base, formatAddress), arguments);
}

// --- C runtime -------------------------------------------------------------

// int sprintf(char* buffer, const char* format, ...)
PPC_FUNC(__imp__sprintf)
{
    Kernel::CountImport("sprintf");
    const uint32_t buffer = ctx.r3.u32;
    const std::string format = GuestString(base, ctx.r4.u32);
    if (buffer == 0) { ctx.r3.u32 = uint32_t(-1); return; }

    GuestArguments arguments{ ctx, base, 5 };
    const std::string result = FormatGuest(base, format, arguments);

    memcpy(Guest::Ptr(buffer), result.c_str(), result.size() + 1);
    Kernel::ReportGuestText(result);
    ctx.r3.u32 = static_cast<uint32_t>(result.size());
}

// int _vsnprintf(char* buffer, size_t count, const char* format, va_list args)
PPC_FUNC(__imp___vsnprintf)
{
    Kernel::CountImport("_vsnprintf");
    const uint32_t buffer = ctx.r3.u32;
    const uint32_t count = ctx.r4.u32;
    const std::string format = GuestString(base, ctx.r5.u32);
    const uint32_t vaList = ctx.r6.u32;

    if (buffer == 0 || count == 0) { ctx.r3.u32 = uint32_t(-1); return; }

    // A va_list here is a pointer into the caller's argument area, so the
    // arguments are read straight from guest memory rather than registers.
    struct ListArguments
    {
        const uint8_t* base;
        uint32_t cursor;
        uint32_t Next() { const uint32_t v = Guest::Read32(base, cursor); cursor += 4; return v; }
    };

    std::string out;
    char scratch[512];
    ListArguments arguments{ base, vaList };

    for (size_t i = 0; i < format.size(); i++)
    {
        if (format[i] != '%') { out += format[i]; continue; }
        if (i + 1 >= format.size()) { out += '%'; break; }
        if (format[i + 1] == '%') { out += '%'; i++; continue; }

        size_t end = i + 1;
        while (end < format.size() && strchr("diouxXeEfgGcspn", format[end]) == nullptr)
            end++;
        if (end >= format.size()) { out += format.substr(i); break; }

        const char conversion = format[end];
        std::string cleaned;
        for (char c : format.substr(i, end - i + 1))
            if (c != 'l' && c != 'h' && c != 'I' && c != '6' && c != '4')
                cleaned += c;
        i = end;

        if (conversion == 's')
        {
            const std::string value = GuestString(base, arguments.Next());
            snprintf(scratch, sizeof(scratch), cleaned.c_str(), value.c_str());
            out += scratch;
        }
        else if (conversion == 'e' || conversion == 'E' || conversion == 'f' ||
                 conversion == 'g' || conversion == 'G')
        {
            out += "<float>";
        }
        else if (conversion == 'p')
        {
            snprintf(scratch, sizeof(scratch), "0x%08X", arguments.Next());
            out += scratch;
        }
        else
        {
            snprintf(scratch, sizeof(scratch), cleaned.c_str(),
                static_cast<int>(arguments.Next()));
            out += scratch;
        }
    }

    const size_t copy = out.size() < count - 1 ? out.size() : count - 1;
    memcpy(Guest::Ptr(buffer), out.c_str(), copy);
    *(Guest::Ptr(buffer) + copy) = '\0';
    ctx.r3.u32 = (copy < out.size()) ? uint32_t(-1) : static_cast<uint32_t>(copy);
}

// --- String conversion -----------------------------------------------------

// NTSTATUS RtlMultiByteToUnicodeN(wchar_t* destination, ULONG destinationBytes,
//                                 ULONG* written, const char* source,
//                                 ULONG sourceBytes)
PPC_FUNC(__imp__RtlMultiByteToUnicodeN)
{
    Kernel::CountImport("RtlMultiByteToUnicodeN");
    const uint32_t destination = ctx.r3.u32;
    const uint32_t destinationBytes = ctx.r4.u32;
    const uint32_t writtenPtr = ctx.r5.u32;
    const uint32_t source = ctx.r6.u32;
    const uint32_t sourceBytes = ctx.r7.u32;

    if (destination == 0 || source == 0) { ctx.r3.u32 = X_STATUS_INVALID_PARAMETER; return; }

    const uint32_t characters =
        (sourceBytes < destinationBytes / 2) ? sourceBytes : destinationBytes / 2;

    // Guest wide characters are big endian, so each one is written through the
    // byte swapping helper rather than copied.
    for (uint32_t i = 0; i < characters; i++)
        Guest::Write16(base, destination + i * 2, *(base + source + i));

    if (writtenPtr != 0)
        Guest::Write32(base, writtenPtr, characters * 2);
    ctx.r3.u32 = X_STATUS_SUCCESS;
}

// --- Time conversion -------------------------------------------------------
// TIME_FIELDS is { year, month, day, hour, minute, second, millisecond,
// weekday }, each a 16 bit field.

// VOID RtlTimeToTimeFields(LARGE_INTEGER* time, TIME_FIELDS* fields)
PPC_FUNC(__imp__RtlTimeToTimeFields)
{
    Kernel::CountImport("RtlTimeToTimeFields");
    const uint32_t timePtr = ctx.r3.u32;
    const uint32_t fields = ctx.r4.u32;
    if (timePtr == 0 || fields == 0) return;

    FILETIME fileTime;
    const uint64_t value = Guest::Read64(base, timePtr);
    fileTime.dwLowDateTime = static_cast<DWORD>(value);
    fileTime.dwHighDateTime = static_cast<DWORD>(value >> 32);

    SYSTEMTIME system{};
    if (!FileTimeToSystemTime(&fileTime, &system))
        return;

    Guest::Write16(base, fields + 0x00, system.wYear);
    Guest::Write16(base, fields + 0x02, system.wMonth);
    Guest::Write16(base, fields + 0x04, system.wDay);
    Guest::Write16(base, fields + 0x06, system.wHour);
    Guest::Write16(base, fields + 0x08, system.wMinute);
    Guest::Write16(base, fields + 0x0A, system.wSecond);
    Guest::Write16(base, fields + 0x0C, system.wMilliseconds);
    Guest::Write16(base, fields + 0x0E, system.wDayOfWeek);
}

// BOOLEAN RtlTimeFieldsToTime(TIME_FIELDS* fields, LARGE_INTEGER* time)
PPC_FUNC(__imp__RtlTimeFieldsToTime)
{
    Kernel::CountImport("RtlTimeFieldsToTime");
    const uint32_t fields = ctx.r3.u32;
    const uint32_t timePtr = ctx.r4.u32;
    if (fields == 0 || timePtr == 0) { ctx.r3.u32 = 0; return; }

    SYSTEMTIME system{};
    system.wYear = Guest::Read16(base, fields + 0x00);
    system.wMonth = Guest::Read16(base, fields + 0x02);
    system.wDay = Guest::Read16(base, fields + 0x04);
    system.wHour = Guest::Read16(base, fields + 0x06);
    system.wMinute = Guest::Read16(base, fields + 0x08);
    system.wSecond = Guest::Read16(base, fields + 0x0A);
    system.wMilliseconds = Guest::Read16(base, fields + 0x0C);

    FILETIME fileTime{};
    if (!SystemTimeToFileTime(&system, &fileTime)) { ctx.r3.u32 = 0; return; }

    Guest::Write64(base, timePtr,
        (uint64_t(fileTime.dwHighDateTime) << 32) | fileTime.dwLowDateTime);
    ctx.r3.u32 = 1;
}

// --- Module and memory queries ---------------------------------------------

// VOID* RtlImageXexHeaderField(VOID* header, ULONG field)
PPC_FUNC(__imp__RtlImageXexHeaderField)
{
    Kernel::CountImport("RtlImageXexHeaderField");
    // The loaded image's optional headers are not kept in guest memory, so no
    // field can be produced. A title that needs one will show up as a missing
    // value rather than a wrong one.
    ctx.r3.u32 = 0;
}

// NTSTATUS XexGetProcedureAddress(HANDLE module, ULONG ordinal, VOID** out)
PPC_FUNC(__imp__XexGetProcedureAddress)
{
    Kernel::CountImport("XexGetProcedureAddress");
    // By ordinal only: the levels export one function, by ordinal, and a
    // name (a pointer, above the ordinal range) finds nothing.
    const uint32_t address = ctx.r4.u32 < 0x10000 ? Modules::Export(ctx.r3.u32, ctx.r4.u32) : 0;
    printf("XexGetProcedureAddress: module 0x%08X ordinal %u -> 0x%08X\n",
        ctx.r3.u32, ctx.r4.u32, address);
    fflush(stdout);
    if (ctx.r5.u32 != 0) Guest::Write32(base, ctx.r5.u32, address);
    ctx.r3.u32 = address != 0 ? X_STATUS_SUCCESS : X_STATUS_NOT_FOUND;
}

// NTSTATUS XexLoadImage(const char* name, ULONG flags, ULONG version, HANDLE* out)
PPC_FUNC(__imp__XexLoadImage)
{
    Kernel::CountImport("XexLoadImage");
    const std::string name = GuestString(base, ctx.r3.u32);
    printf("XexLoadImage: %s (flags 0x%X, version 0x%X)\n",
        name.empty() ? "<unnamed>" : name.c_str(), ctx.r4.u32, ctx.r5.u32);
    fflush(stdout);
    const uint32_t out = ctx.r6.u32;
    const uint32_t handle = name.empty() ? 0 : Modules::Load(ctx, base, name);
    if (out != 0) Guest::Write32(base, out, handle);
    ctx.r3.u32 = handle != 0 ? X_STATUS_SUCCESS : X_STATUS_NOT_FOUND;
}

// NTSTATUS XexUnloadImage(HANDLE module)
PPC_FUNC(__imp__XexUnloadImage)
{
    Kernel::CountImport("XexUnloadImage");
    Modules::Unload(ctx, base, ctx.r3.u32);
    ctx.r3.u32 = X_STATUS_SUCCESS;
}

// NTSTATUS MmQueryStatistics(X_MM_STATISTICS* statistics)
PPC_FUNC(__imp__MmQueryStatistics)
{
    Kernel::CountImport("MmQueryStatistics");
    const uint32_t statistics = ctx.r3.u32;
    if (statistics == 0) { ctx.r3.u32 = X_STATUS_INVALID_PARAMETER; return; }

    // The console has 512 MB shared between the CPU and the GPU. Reporting
    // that, mostly free, is closer to right than reporting the host's memory.
    constexpr uint32_t TotalPages = (512u << 20) / 4096;
    constexpr uint32_t AvailablePages = TotalPages / 2;

    memset(Guest::Ptr(statistics), 0, 0x34);
    Guest::Write32(base, statistics + 0x00, 0x34);            // structure size
    Guest::Write32(base, statistics + 0x04, TotalPages);
    Guest::Write32(base, statistics + 0x08, AvailablePages);
    Guest::Write32(base, statistics + 0x0C, AvailablePages);
    ctx.r3.u32 = X_STATUS_SUCCESS;
}

// NTSTATUS NtQueryVirtualMemory(VOID* address, X_MEMORY_BASIC_INFORMATION* out)
PPC_FUNC(__imp__NtQueryVirtualMemory)
{
    Kernel::CountImport("NtQueryVirtualMemory");
    const uint32_t address = ctx.r3.u32;
    const uint32_t information = ctx.r4.u32;
    if (information == 0) { ctx.r3.u32 = X_STATUS_INVALID_PARAMETER; return; }

    memset(Guest::Ptr(information), 0, 0x1C);
    Guest::Write32(base, information + 0x00, address & ~0xFFFu);   // base address
    Guest::Write32(base, information + 0x04, address & ~0xFFFu);   // allocation base
    Guest::Write32(base, information + 0x08, 0x04);                // PAGE_READWRITE
    Guest::Write32(base, information + 0x0C, 0x1000);              // region size
    Guest::Write32(base, information + 0x10, 0x1000);              // MEM_COMMIT
    Guest::Write32(base, information + 0x14, 0x04);
    Guest::Write32(base, information + 0x18, 0x20000);             // MEM_PRIVATE
    ctx.r3.u32 = X_STATUS_SUCCESS;
}

// --- Exceptions ------------------------------------------------------------
// XenonRecomp does not translate exception handling: the link register and the
// arbitrary jumps a handler performs have no equivalent in the generated C++.
// These cannot be made to work without changes to the recompiler itself.

PPC_FUNC(__imp__RtlCaptureContext)
{
    Kernel::CountImport("RtlCaptureContext");
    // A CONTEXT the title could unwind from does not exist. Zeroing it is at
    // least deterministic.
    if (ctx.r3.u32 != 0)
        memset(Guest::Ptr(ctx.r3.u32), 0, 0x2B0);
}

// The title's longjmp, checked. Declared in cod3_mmio.h, which the host
// cannot include: its macros are for the translated code alone.
namespace Mmio { void LongJump(::PPCContext& ctx, uint8_t* base, jmp_buf& buffer, int value); }

void Mmio::LongJump(PPCContext& ctx, uint8_t* base, jmp_buf& buffer, int value)
{
    // A script thread yielding: the engine's fiber does the longjmp.
    if (Coroutines::YieldIfCoroutine(buffer, value)) return;

    const auto* frame = reinterpret_cast<const _JUMP_BUFFER*>(&buffer);
    ULONG_PTR low = 0, high = 0;
    GetCurrentThreadStackLimits(&low, &high);
    const bool stackOk = frame->Rsp >= low && frame->Rsp < high;
    MEMORY_BASIC_INFORMATION info{};
    const bool codeOk = VirtualQuery(reinterpret_cast<void*>(frame->Rip), &info, sizeof(info)) != 0
        && (info.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)) != 0;
    if (stackOk && codeOk)
        longjmp(buffer, value);

    printf("\nlongjmp: the title longjmp'd with %d through the buffer at 0x%08X, which no "
           "host setjmp filled in this frame:\n  host stack pointer %p (this thread's stack is %p..%p), "
           "return address %p%s\n",
        value, uint32_t(reinterpret_cast<uint8_t*>(&buffer) - base), (void*)frame->Rsp,
        (void*)low, (void*)high, (void*)frame->Rip, codeOk ? "" : ", not code");
    printf("  the buffer's first words:");
    for (int i = 0; i < 8; i++)
        printf(" %08X", Guest::Read32(base, uint32_t(reinterpret_cast<uint8_t*>(&buffer) - base) + i * 4));
    printf("\n  lr 0x%08X, r1 0x%08X\n", uint32_t(ctx.lr), ctx.r1.u32);
    uint32_t functions[16] = {};
    const int count = Sampler::FunctionsOnStack(functions, 16);
    if (count > 0)
    {
        printf("  guest call chain, innermost first:");
        for (int i = 0; i < count; i++) printf("%ssub_%08X", i == 0 ? " " : " <- ", functions[i]);
        printf("\n");
    }
    fflush(stdout);
    Kernel::Exit(1);
}

PPC_FUNC(__imp__RtlRaiseException)
{
    Kernel::CountImport("RtlRaiseException");
    printf("\n");

    // What was raised, and from where. Which exception it is decides whether
    // this is the title reporting a problem or using exceptions as ordinary
    // control flow, and those need different answers.
    const uint32_t record = ctx.r3.u32;
    if (record != 0)
    {
        const uint32_t code = Guest::Read32(base, record);
        const uint32_t flags = Guest::Read32(base, record + 4);
        const uint32_t address = Guest::Read32(base, record + 12);
        const uint32_t count = Guest::Read32(base, record + 16);

        printf("Exception 0x%08X raised at 0x%08X, flags 0x%08X", code, address, flags);
        if (code == 0xE06D7363) printf(", which is a C++ throw");
        printf("\n");

        if (count > 0 && count <= 15)
        {
            printf("  arguments:");
            for (uint32_t i = 0; i < count; i++)
                printf(" %08X", Guest::Read32(base, record + 20 + i * 4));
            printf("\n");
        }

        // What was thrown.
        //
        // A C++ throw carries a description of the thrown type with it, and
        // that description ends in the type's own name. Following it turns
        // "the game raised an exception" into the name the programmer gave it,
        // which is the difference between guessing at the cause and reading it.
        if (code == 0xE06D7363 && count >= 3)
        {
            auto inImage = [](uint32_t value) {
                return value >= 0x82000000u && value < 0x82C30000u;
            };

            const uint32_t throwInfo = Guest::Read32(base, record + 20 + 8);
            const uint32_t array = inImage(throwInfo)
                ? Guest::Read32(base, throwInfo + 12) : 0;
            const uint32_t types = inImage(array)
                ? Guest::Read32(base, array + 0) : 0;

            for (uint32_t i = 0; i < types && i < 4; i++)
            {
                const uint32_t catchable = Guest::Read32(base, array + 4 + i * 4);
                if (!inImage(catchable)) continue;
                const uint32_t descriptor = Guest::Read32(base, catchable + 4);
                if (!inImage(descriptor)) continue;

                char text[160];
                uint32_t at = 0;
                for (; at + 1 < sizeof(text); at++)
                {
                    const uint8_t byte = Guest::Read8(base, descriptor + 8 + at);
                    if (byte == 0) break;
                    text[at] = char(byte);
                }
                text[at] = 0;
                if (at > 0) printf("  what was thrown: %s\n", text);
            }
        }

        // What this thread asked the kernel for just before it threw. A throw
        // about a file is nearly always the answer to one of these.
        // The thrown object itself. IdvFileError derives from std::string,
        // so the object is the message: a word of allocator, sixteen bytes of
        // inline text or a pointer, then the size and the capacity.
        if (code == 0xE06D7363 && count >= 2)
        {
            const uint32_t object = Guest::Read32(base, record + 20 + 4);
            if (object >= 0x10000 && object < 0xC0000000)
            {
                printf("  the thrown object at 0x%08X:", object);
                for (int i = 0; i < 12; i++)
                    printf(" %08X", Guest::Read32(base, object + i * 4));
                printf("\n");

                for (uint32_t skip = 0; skip <= 4; skip += 4)
                {
                    const uint32_t size = Guest::Read32(base, object + skip + 16);
                    const uint32_t capacity = Guest::Read32(base, object + skip + 20);
                    if (size == 0 || size > 512 || capacity < size) continue;
                    const uint32_t text = capacity >= 16
                        ? Guest::Read32(base, object + skip) : object + skip;
                    if (text < 0x10000 || text >= 0xC0000000) continue;
                    printf("  the message: \"");
                    for (uint32_t i = 0; i < size && i < 200; i++)
                    {
                        const uint8_t c = Guest::Read8(base, text + i);
                        printf("%c", (c >= 32 && c < 127) ? char(c) : '.');
                    }
                    printf("\"\n");
                    break;
                }
            }
        }

        Kernel::ReportRecentCalls(GetCurrentThreadId());
    }

    {
        uint32_t functions[12] = {};
        const int walked = Sampler::FunctionsOnStack(functions, 12);
        if (walked > 0)
        {
            printf("  guest call chain, innermost first:");
            for (int i = 0; i < walked; i++)
                printf("%ssub_%08X", i == 0 ? " " : " <- ", functions[i]);
            printf("\n");
        }
    }

    printf("The game raised an exception, which this recompilation cannot handle.\n");
    printf("XenonRecomp does not translate exception handling at all, so there is\n");
    printf("no handler to run and no stack to unwind. See STATUS.md.\n");
    fflush(stdout);
    Kernel::Exit(1);
}

PPC_FUNC(__imp__RtlUnwind)
{
    Kernel::CountImport("RtlUnwind");
    printf("\n");
    printf("The game tried to unwind the stack after an exception, which this\n");
    printf("recompilation cannot do. See STATUS.md.\n");
    fflush(stdout);
    Kernel::Exit(1);
}

PPC_FUNC(__imp____C_specific_handler)
{
    Kernel::CountImport("__C_specific_handler");
    // Reached while an exception is already being dispatched, which the line
    // above has already ruled out.
    ctx.r3.u32 = 1;   // ExceptionContinueSearch
}

namespace
{
    std::mutex g_textMutex;
    uint64_t g_textLines = 0;
}

void Kernel::ReportGuestText(const std::string& text)
{
    // The title formats its own diagnostics through these, so this is the game
    // saying in its own words what it is doing. Only the first few hundred are
    // printed: after that it is repeating itself.
    if (text.size() < 2) return;

    std::lock_guard<std::mutex> lock(g_textMutex);
    if (g_textLines++ >= 400) return;

    std::string line = text;
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
        line.pop_back();
    if (line.empty()) return;

    printf("game: %s\n", line.c_str());
    fflush(stdout);
}

// A value probe for temporarily instrumented recompiled code: prints a tag
// and a register once per call, so a chain of direct calls inside the title
// can say which of them returned the failure.
void CoD3TraceValue(const char* tag, uint32_t value)
{
    printf("probe: %s = 0x%08X\n", tag, value);
    fflush(stdout);
}

// The same, printed only when the value differs from the last one seen
// under that tag: for a word polled every frame.
void CoD3TraceChange(const char* tag, uint32_t value)
{
    static std::mutex mutex;
    static std::map<std::string, uint32_t> last;
    std::lock_guard<std::mutex> lock(mutex);
    auto found = last.find(tag);
    if (found != last.end() && found->second == value) return;
    last[tag] = value;
    printf("probe: %s -> 0x%08X\n", tag, value);
    fflush(stdout);
}

// The script thread object's destructor, sub_824A3040 in the title: the
// host fiber the thread ran on goes with it. The recompiled function is
// reached through its weak alias, which this stronger definition replaces.
extern "C" PPC_FUNC(__imp__sub_824A3040);
PPC_FUNC(sub_824A3040)
{
    Coroutines::Destroy(ctx.r3.u32);
    __imp__sub_824A3040(ctx, base);
}
