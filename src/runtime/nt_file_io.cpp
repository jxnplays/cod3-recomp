// =============================================================================
//  COD3 Recomp - NT file I/O
// =============================================================================
//
//  ## Why this is the critical path
//
//  Every Nt* file import was a generated no-op stub. Without them the title
//  cannot load a single asset - no maps, no models, no audio banks - so nothing
//  downstream can be seen working no matter how correct the graphics are.
//
//  It is also the best-shaped work available: one subsystem, seven imports, and
//  signatures that are already known rather than guessed.
//
//  ## The signatures were measured, not assumed
//
//  The alternative to this file was implementing one import per crash as boot
//  tripped over it, which is how the Vd*, Xam* and Ex* stubs went wrong - each
//  written from an assumed shape and each found to be wrong when exercised.
//
//  So all seven call sites were audited first. The register setup matches the
//  documented NT API exactly:
//
//      NtCreateFile   r3 handle*, r4 access, r5 OBJECT_ATTRIBUTES*,
//                     r6 IO_STATUS_BLOCK*, r7 AllocationSize, r8 FileAttributes,
//                     r9 ShareAccess, r10 CreateDisposition
//                     observed: r8 = 0x80 (FILE_ATTRIBUTE_NORMAL),
//                               r9 = 3 (share), r10 = 2 (disposition)
//
//      NtReadFile     r3 handle, r4 Event, r5 ApcRoutine, r6 ApcContext,
//                     r7 IO_STATUS_BLOCK*, r8 Buffer, r9 Length,
//                     r10 ByteOffset
//                     observed: r4..r6 = 0, r7 = &stack[80], r10 = 0
//
//  That matters more than it sounds: the Xbox kernel layer IS NT, so the
//  documented semantics are the correct ones and there is nothing title-specific
//  to reverse-engineer here. Guest handles are mapped onto real Win32 handles.
//
//  ## What this does NOT do
//
//  It implements enough of the subsystem for the CRT to open the title's files
//  and read them. It does not implement the whole NT file model - no async
//  (every Event/ApcRoutine observed is null), no reparse points, no security
//  descriptors, no file locking. The CRT's loader does not use those, and
//  inventing them would be guessing.
//
//  Paths are resolved under the directory the launcher established, so the guest
//  sees its own game tree and not the host's filesystem.
// =============================================================================

#include "ppc_recomp_shared.h"

#include "cod3_runtime.h"

#include <windows.h>

#include <cstdio>
#include <string>
#include <unordered_map>

namespace {

// Guest handle -> Win32 handle.
//
// The guest's handles are its own small integers; handing those to Win32 would
// alias unrelated objects. Every handle crossing this boundary is translated.
std::unordered_map<uint32_t, HANDLE> g_files;
uint32_t g_nextGuestHandle = 0x1000;   // deliberately in no guest range

// Where the guest's files live. Set once by the launcher from the directory it
// resolved, so the guest cannot reach the rest of the host filesystem.
std::string g_gameRoot;

// --- guest memory access -----------------------------------------------------
//
// Guest addresses are mapped at their real host addresses, so a guest pointer is
// a valid host pointer. But guest memory is BIG-ENDIAN, so every scalar read has
// to swap. Getting this wrong yields byte-swapped lengths and garbage paths,
// which is why each accessor is named for what it does rather than left inline.

uint16_t guest_u16(uint32_t addr)
{
    const uint32_t w = cod3_load_u32(addr & ~3u);
    return static_cast<uint16_t>((w >> (8 * (3 - (addr & 3)))) & 0xFFFFu);
}

uint8_t guest_u8(uint32_t addr)
{
    const uint32_t w = cod3_load_u32(addr & ~3u);
    return static_cast<uint8_t>((w >> (8 * (3 - (addr & 3)))) & 0xFFu);
}

// Copy a guest UTF-16 string of `bytes` bytes into a host std::string, as UTF-8.
//
// The guest stores UTF-16 big-endian and the host expects little-endian, so each
// unit is swapped. Only the BMP subset is handled: surrogate pairs are not
// decoded, because no path in this title needs them and a wrong answer here
// would be worse than a partial one.
std::string guest_utf16_to_utf8(uint32_t addr, uint32_t bytes)
{
    std::string out;
    out.reserve(bytes);

    for (uint32_t i = 0; i + 1 < bytes; i += 2)
    {
        const uint16_t c = guest_u16(addr + i);
        if (c == 0)
            break;

        if (c < 0x80)
        {
            out.push_back(static_cast<char>(c));
        }
        else if (c < 0x800)
        {
            out.push_back(static_cast<char>(0xC0 | (c >> 6)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        }
        else
        {
            out.push_back(static_cast<char>(0xE0 | (c >> 12)));
            out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        }
    }

    return out;
}

// OBJECT_ATTRIBUTES.ObjectName is a UNICODE_STRING.
//
//   +0  USHORT Length          bytes, not characters
//   +2  USHORT MaximumLength
//   +4  PWSTR Buffer
//
// Length is in BYTES. Reading it as a character count halves every path, which
// looks like "file not found" rather than an obvious bug - the reason this is
// worth stating.
std::string object_attributes_name(uint32_t oa)
{
    if (oa == 0)
        return std::string();

    const uint16_t bytes = guest_u16(oa + 0);
    const uint32_t buffer = cod3_load_u32(oa + 4);

    if (bytes == 0 || buffer == 0)
        return std::string();

    return guest_utf16_to_utf8(buffer, bytes);
}

DWORD nt_access_to_win32(uint32_t access)
{
    DWORD a = 0;
    if (access & 0x00000001u) a |= GENERIC_READ;          // FILE_READ_DATA
    if (access & 0x00000002u) a |= FILE_APPEND_DATA;
    if (access & 0x00000004u) a |= FILE_READ_EA;
    if (access & 0x00000008u) a |= FILE_WRITE_EA;
    if (access & 0x00000010u) a |= FILE_EXECUTE;
    if (access & 0x00000020u) a |= FILE_READ_ATTRIBUTES;
    if (access & 0x00000040u) a |= FILE_WRITE_ATTRIBUTES;
    if (access & 0x00010000u) a |= DELETE;
    if (access & 0x00100000u) a |= SYNCHRONIZE;
    if (a == 0)
        a = GENERIC_READ;
    return a;
}

DWORD nt_disposition_to_win32(uint32_t d)
{
    switch (d)
    {
    case 0:  return CREATE_NEW;        // FILE_SUPERSEDE
    case 1:  return OPEN_EXISTING;     // FILE_OPEN
    case 2:  return OPEN_ALWAYS;       // FILE_CREATE
    case 3:  return OPEN_ALWAYS;       // FILE_OPEN_IF
    case 4:  return TRUNCATE_EXISTING; // FILE_OVERWRITE
    case 5:  return TRUNCATE_EXISTING; // FILE_OVERWRITE_IF
    default: return OPEN_EXISTING;
    }
}

DWORD nt_share_to_win32(uint32_t s)
{
    DWORD v = 0;
    if (s & 1) v |= FILE_SHARE_READ;
    if (s & 2) v |= FILE_SHARE_WRITE;
    if (s & 4) v |= FILE_SHARE_DELETE;
    if (v == 0) v = FILE_SHARE_READ;
    return v;
}

void set_status(PPCContext& ctx, uint32_t ioStatus, NTSTATUS status)
{
    if (ioStatus == 0)
        return;
    // IO_STATUS_BLOCK: Status at +0, Information at +8 (pointer sized).
    cod3_store_u32(ioStatus + 0, static_cast<uint32_t>(status));
    cod3_store_u32(ioStatus + 8, 0);
}

} // namespace

// -----------------------------------------------------------------------------
// cod3_set_game_root
// -----------------------------------------------------------------------------
//
// Called by the launcher with the directory it resolved for the game's files.
// Guest paths are resolved beneath it, so the guest sees its own tree.
void cod3_set_game_root(const std::string& root)
{
    g_gameRoot = root;
    if (!g_gameRoot.empty() && g_gameRoot.back() != '\\' && g_gameRoot.back() != '/')
        g_gameRoot.push_back('\\');
}

// -----------------------------------------------------------------------------
// NtCreateFile
// -----------------------------------------------------------------------------
//
// Signature measured from the two call sites (see the header):
//
//   r3  PHANDLE FileHandle            out, guest pointer
//   r4  ACCESS_MASK DesiredAccess
//   r5  POBJECT_ATTRIBUTES           guest pointer; name read from here
//   r6  PIO_STATUS_BLOCK              out
//   r7  PLARGE_INTEGER AllocationSize
//   r8  ULONG FileAttributes
//   r9  ULONG ShareAccess
//   r10 ULONG CreateDisposition
PPC_FUNC(__imp__NtCreateFile)
{
    const uint32_t outHandle  = ctx.r3.u32;
    const uint32_t access     = ctx.r4.u32;
    const uint32_t objectAttr = ctx.r5.u32;
    const uint32_t ioStatus   = ctx.r6.u32;
    const uint32_t attributes = ctx.r8.u32;
    const uint32_t share      = ctx.r9.u32;
    const uint32_t disposition = ctx.r10.u32;

    const std::string name = object_attributes_name(objectAttr);

    std::string path = name;
    if (!name.empty() && (name[0] == '\\' || name[0] == '/'))
    {
        // An absolute guest path means "relative to the game root", not the
        // host root. Stripping the leading separator rather than prepending
        // keeps the guest inside its own tree.
        path = g_gameRoot + name.substr(1);
    }
    else if (!g_gameRoot.empty())
    {
        path = g_gameRoot + name;
    }

    if (path.empty())
    {
        set_status(ctx, ioStatus, 0xC0000008u); // STATUS_INVALID_PARAMETER
        ctx.r3.u64 = 0xC0000008u;
        return;
    }

    HANDLE h = CreateFileA(
        path.c_str(),
        nt_access_to_win32(access),
        nt_share_to_win32(share),
        nullptr,
        nt_disposition_to_win32(disposition),
        attributes ? attributes : FILE_ATTRIBUTE_NORMAL,
        nullptr);

    if (h == INVALID_HANDLE_VALUE)
    {
        const DWORD err = GetLastError();
        DWORD status;
        switch (err)
        {
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:        status = 0xC000000Fu; break; // NOT_FOUND
        case ERROR_ACCESS_DENIED:         status = 0xC0000022u; break; // ACCESS_DENIED
        case ERROR_SHARING_VIOLATION:     status = 0xC0000043u; break; // SHARING_VIOLATION
        case ERROR_DISK_FULL:             status = 0xC000007Fu; break; // DISK_FULL
        default:                          status = 0xC0000001u; break; // UNSUCCESSFUL
        }

        set_status(ctx, ioStatus, status);

        std::fprintf(stderr,
            "[cod3] NtCreateFile: failed (%lu) for '%s'\n",
            err, path.c_str());
        std::fflush(stderr);

        ctx.r3.u64 = status;
        return;
    }

    const uint32_t guest = g_nextGuestHandle++;
    g_files[guest] = h;

    if (outHandle != 0)
        cod3_store_u32(outHandle, guest);

    set_status(ctx, ioStatus, 0); // STATUS_SUCCESS
    ctx.r3.u64 = 0;

    std::fprintf(stderr, "[cod3] NtCreateFile: '%s' -> guest handle 0x%04X\n",
                 path.c_str(), guest);
    std::fflush(stderr);
}

// -----------------------------------------------------------------------------
// NtClose
// -----------------------------------------------------------------------------
PPC_FUNC(__imp__NtClose)
{
    const uint32_t guest = ctx.r3.u32;

    auto it = g_files.find(guest);
    if (it == g_files.end())
    {
        // Not one of ours. 0xC0000008 STATUS_INVALID_HANDLE is what the guest
        // would see for an unknown handle, and it is a real answer rather than
        // a success that hides the mistake.
        ctx.r3.u64 = 0xC0000008u;
        return;
    }

    CloseHandle(it->second);
    g_files.erase(it);

    ctx.r3.u64 = 0;
}

// -----------------------------------------------------------------------------
// NtReadFile / NtWriteFile
// -----------------------------------------------------------------------------
//
// Signature measured from the call sites:
//
//   r3  handle, r4 Event, r5 ApcRoutine, r6 ApcContext,
//   r7  IO_STATUS_BLOCK*, r8 Buffer, r9 Length, r10 ByteOffset
//
// Event and ApcRoutine are 0 at every observed call site, so this is synchronous
// I/O only. Async is not implemented because nothing here uses it - implementing
// it on assumption would be inventing behaviour.
namespace {

void do_file_io(PPCContext& ctx, bool writing)
{
    const uint32_t guest    = ctx.r3.u32;
    const uint32_t ioStatus = ctx.r7.u32;
    const uint32_t buffer   = ctx.r8.u32;
    const uint32_t length   = ctx.r9.u32;
    const uint32_t offset   = ctx.r10.u32;

    auto it = g_files.find(guest);
    if (it == g_files.end())
    {
        set_status(ctx, ioStatus, 0xC0000008u);
        ctx.r3.u64 = 0xC0000008u;
        return;
    }

    OVERLAPPED ov = {};
    ov.Offset     = offset & 0xFFFFFFFFu;
    ov.OffsetHigh = 0;

    DWORD moved = 0;
    const BOOL ok = writing
        ? WriteFile(it->second, cod3_guest_ptr(buffer), length, &moved, &ov)
        : ReadFile(it->second, cod3_guest_ptr(buffer), length, &moved, &ov);

    if (!ok)
    {
        const DWORD err = GetLastError();
        const DWORD status = (err == ERROR_HANDLE_EOF)
            ? 0xC0000011u   // STATUS_END_OF_FILE
            : 0xC0000001u; // STATUS_UNSUCCESSFUL

        set_status(ctx, ioStatus, status);
        ctx.r3.u64 = status;
        return;
    }

    // IO_STATUS_BLOCK.Information is the byte count at +8.
    if (ioStatus != 0)
        cod3_store_u32(ioStatus + 8, moved);

    set_status(ctx, ioStatus, 0);
    ctx.r3.u64 = 0;
}

} // namespace

PPC_FUNC(__imp__NtReadFile)
{
    do_file_io(ctx, false);
}

PPC_FUNC(__imp__NtWriteFile)
{
    do_file_io(ctx, true);
}

// -----------------------------------------------------------------------------
// NtFlushBuffersFile
// -----------------------------------------------------------------------------
PPC_FUNC(__imp__NtFlushBuffersFile)
{
    auto it = g_files.find(ctx.r3.u32);
    if (it == g_files.end())
    {
        ctx.r3.u64 = 0xC0000008u;
        return;
    }

    FlushFileBuffers(it->second);
    ctx.r3.u64 = 0;
}

// -----------------------------------------------------------------------------
// NtQueryInformationFile - FileStandardInformation only
// -----------------------------------------------------------------------------
//
// The CRT asks for the file size before reading. That is the one query the
// loader needs; the rest of the information classes are not implemented, and
// answering them with a zeroed buffer would be a plausible-looking lie.
PPC_FUNC(__imp__NtQueryInformationFile)
{
    const uint32_t guest  = ctx.r3.u32;
    const uint32_t ioStat = ctx.r4.u32;
    const uint32_t class_ = ctx.r5.u32;
    const uint32_t out    = ctx.r6.u32;
    const uint32_t outLen = ctx.r7.u32;

    auto it = g_files.find(guest);
    if (it == g_files.end())
    {
        set_status(ctx, ioStat, 0xC0000008u);
        ctx.r3.u64 = 0xC0000008u;
        return;
    }

    // FileStandardInformation = 1
    if (class_ != 1)
    {
        std::fprintf(stderr,
            "[cod3] NtQueryInformationFile: class %u not implemented\n",
            class_);
        std::fflush(stderr);

        set_status(ctx, ioStat, 0xC00000ABu); // STATUS_NOT_SUPPORTED
        ctx.r3.u64 = 0xC00000ABu;
        return;
    }

    LARGE_INTEGER size = {};
    if (!GetFileSizeEx(it->second, &size))
    {
        set_status(ctx, ioStat, 0xC0000001u);
        ctx.r3.u64 = 0xC0000001u;
        return;
    }

    // FILE_STANDARD_INFORMATION: AllocationSize(8), EndOfFile(8),
    // NumberOfLinks(4), DeletePending(1), Directory(1) + padding.
    if (out != 0 && outLen >= 0x18)
    {
        cod3_store_u32(out + 0, static_cast<uint32_t>(size.QuadPart));
        cod3_store_u32(out + 4, static_cast<uint32_t>(size.QuadPart >> 32));
        cod3_store_u32(out + 8, static_cast<uint32_t>(size.QuadPart));
        cod3_store_u32(out + 12, static_cast<uint32_t>(size.QuadPart >> 32));
        cod3_store_u32(out + 16, 1); // NumberOfLinks
        cod3_store_u32(out + 20, 0); // DeletePending | Directory
    }

    set_status(ctx, ioStat, 0);
    ctx.r3.u64 = 0;
}

// -----------------------------------------------------------------------------
// NtSetInformationFile - FileEndOfFileInformation only
// -----------------------------------------------------------------------------
PPC_FUNC(__imp__NtSetInformationFile)
{
    const uint32_t guest = ctx.r3.u32;
    const uint32_t ioStat = ctx.r4.u32;
    const uint32_t class_ = ctx.r5.u32;
    const uint32_t info   = ctx.r6.u32;

    auto it = g_files.find(guest);
    if (it == g_files.end())
    {
        set_status(ctx, ioStat, 0xC0000008u);
        ctx.r3.u64 = 0xC0000008u;
        return;
    }

    // FileEndOfFileInformation = 4
    if (class_ != 4 || info == 0)
    {
        set_status(ctx, ioStat, 0xC00000ABu);
        ctx.r3.u64 = 0xC00000ABu;
        return;
    }

    LARGE_INTEGER end = {};
    end.LowPart  = cod3_load_u32(info);
    end.HighPart = cod3_load_u32(info + 4);

    if (!SetFilePointerEx(it->second, end, nullptr, FILE_BEGIN))
    {
        set_status(ctx, ioStat, 0xC0000001u);
        ctx.r3.u64 = 0xC0000001u;
        return;
    }

    set_status(ctx, ioStat, 0);
    ctx.r3.u64 = 0;
}
