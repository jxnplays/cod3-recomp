#include "xdvdfs.h"

#include <cstring>

#include <Windows.h>

namespace
{
    constexpr char kMagic[] = "MICROSOFT*XBOX*MEDIA";
    constexpr size_t kMagicLength = 20;

    // Where the filesystem starts, by disc format. A stripped or rebuilt image
    // has no lead-in at all, which is the 0 case.
    struct FormatOffset { uint64_t offset; const char* name; };
    constexpr FormatOffset kFormats[] = {
        { 0x00000000ull, "stripped or rebuilt image" },
        { 0x02080000ull, "XGD3" },
        { 0x0FD90000ull, "XGD2" },
        { 0x18300000ull, "XGD1" },
    };

    constexpr uint32_t kVolumeDescriptorSector = 32;
    constexpr uint8_t  kAttributeDirectory = 0x10;
    constexpr int      kMaxDepth = 64;

    uint16_t ReadU16(const uint8_t* p) { return uint16_t(p[0]) | (uint16_t(p[1]) << 8); }
    uint32_t ReadU32(const uint8_t* p)
    {
        return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
    }

    std::string LastErrorText()
    {
        DWORD code = GetLastError();
        char buffer[256] = {};
        FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code,
            0, buffer, sizeof(buffer) - 1, nullptr);
        for (char* p = buffer; *p; ++p) if (*p == '\r' || *p == '\n') *p = ' ';
        return std::string(buffer) + "(error " + std::to_string(code) + ")";
    }
}

XDvdFs::Image::~Image() { Close(); }

void XDvdFs::Image::Close()
{
    if (m_file != nullptr && m_file != INVALID_HANDLE_VALUE)
        CloseHandle(static_cast<HANDLE>(m_file));
    m_file = nullptr;
}

bool XDvdFs::Image::ReadAt(uint64_t offset, void* buffer, size_t size, std::string& error)
{
    if (offset + size > m_fileSize)
    {
        error = "read past the end of the image";
        return false;
    }

    LARGE_INTEGER position;
    position.QuadPart = static_cast<LONGLONG>(offset);
    if (!SetFilePointerEx(static_cast<HANDLE>(m_file), position, nullptr, FILE_BEGIN))
    {
        error = "seek failed: " + LastErrorText();
        return false;
    }

    auto* out = static_cast<uint8_t*>(buffer);
    size_t remaining = size;
    while (remaining > 0)
    {
        DWORD chunk = static_cast<DWORD>(remaining > 0x10000000 ? 0x10000000 : remaining);
        DWORD got = 0;
        if (!ReadFile(static_cast<HANDLE>(m_file), out, chunk, &got, nullptr))
        {
            error = "read failed: " + LastErrorText();
            return false;
        }
        if (got == 0)
        {
            error = "unexpected end of file";
            return false;
        }
        out += got;
        remaining -= got;
    }
    return true;
}

bool XDvdFs::Image::Open(const std::filesystem::path& isoPath, std::string& error)
{
    Close();

    HANDLE handle = CreateFileW(isoPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
    {
        error = "cannot open the image: " + LastErrorText();
        return false;
    }
    m_file = handle;

    LARGE_INTEGER size;
    if (!GetFileSizeEx(handle, &size))
    {
        error = "cannot read the file size: " + LastErrorText();
        Close();
        return false;
    }
    m_fileSize = static_cast<uint64_t>(size.QuadPart);

    // Try each known lead-in offset and keep the one whose volume descriptor
    // carries the magic at both ends of the sector.
    uint8_t sector[SectorSize];
    for (const auto& format : kFormats)
    {
        const uint64_t descriptor = format.offset + uint64_t(kVolumeDescriptorSector) * SectorSize;
        if (descriptor + SectorSize > m_fileSize)
            continue;

        std::string ignored;
        if (!ReadAt(descriptor, sector, SectorSize, ignored))
            continue;

        if (memcmp(sector, kMagic, kMagicLength) != 0)
            continue;
        if (memcmp(sector + SectorSize - kMagicLength, kMagic, kMagicLength) != 0)
            continue;

        m_baseOffset = format.offset;
        m_formatName = format.name;
        m_rootSector = ReadU32(sector + 0x14);
        m_rootSize = ReadU32(sector + 0x18);

        if (m_rootSize == 0)
        {
            error = "the image has an empty root directory";
            Close();
            return false;
        }
        return true;
    }

    error = "not an Xbox 360 disc image: no XDVDFS volume descriptor at any known offset";
    Close();
    return false;
}

bool XDvdFs::Image::WalkDirectory(uint32_t sector, uint32_t size, const std::string& prefix,
                                  std::vector<Entry>& out, std::string& error, int depth)
{
    if (depth > kMaxDepth)
    {
        error = "directory nesting is deeper than " + std::to_string(kMaxDepth) +
                ", the image is probably damaged";
        return false;
    }
    if (size == 0)
        return true;

    std::vector<uint8_t> table(size);
    if (!ReadAt(m_baseOffset + uint64_t(sector) * SectorSize, table.data(), size, error))
        return false;

    // Entries form a binary tree addressed by 32-bit-word offsets from the
    // start of the table, so walk it with an explicit stack. Offsets already
    // visited are rejected: a damaged image must not send us in circles.
    std::vector<uint32_t> pending{ 0 };
    std::vector<bool> visited(size / 4 + 1, false);
    std::vector<Entry> subdirectories;

    while (!pending.empty())
    {
        const uint32_t wordOffset = pending.back();
        pending.pop_back();

        const uint64_t byteOffset = uint64_t(wordOffset) * 4;
        if (byteOffset + 14 > size)
            continue;
        if (visited[wordOffset])
            continue;
        visited[wordOffset] = true;

        const uint8_t* entry = table.data() + byteOffset;
        const uint16_t left = ReadU16(entry + 0);
        const uint16_t right = ReadU16(entry + 2);
        const uint32_t entrySector = ReadU32(entry + 4);
        const uint32_t entrySize = ReadU32(entry + 8);
        const uint8_t attributes = entry[12];
        const uint8_t nameLength = entry[13];

        if (nameLength == 0 || byteOffset + 14 + nameLength > size)
            continue;   // padding between sectors, not a real entry

        std::string name(reinterpret_cast<const char*>(entry + 14), nameLength);

        Entry item;
        item.path = prefix.empty() ? name : prefix + "/" + name;
        item.sector = entrySector;
        item.size = entrySize;
        item.isDirectory = (attributes & kAttributeDirectory) != 0;

        if (item.isDirectory)
            subdirectories.push_back(item);
        else
            out.push_back(item);

        // 0 and 0xFFFF both mean "no child" in images seen in the wild.
        if (left != 0 && left != 0xFFFF) pending.push_back(left);
        if (right != 0 && right != 0xFFFF) pending.push_back(right);
    }

    for (const auto& directory : subdirectories)
    {
        if (!WalkDirectory(directory.sector, directory.size, directory.path, out, error, depth + 1))
            return false;
    }
    return true;
}

bool XDvdFs::Image::ListFiles(std::vector<Entry>& out, std::string& error)
{
    out.clear();
    return WalkDirectory(m_rootSector, m_rootSize, std::string(), out, error, 0);
}

bool XDvdFs::Image::ReadEntry(const Entry& entry, std::vector<uint8_t>& out, std::string& error)
{
    out.assign(entry.size, 0);
    if (entry.size == 0)
        return true;
    return ReadAt(m_baseOffset + uint64_t(entry.sector) * SectorSize, out.data(), entry.size, error);
}

bool XDvdFs::Image::ExtractAll(const std::filesystem::path& destination,
                               const std::function<void(const Progress&)>& onProgress,
                               std::string& error)
{
    std::vector<Entry> files;
    if (!ListFiles(files, error))
        return false;

    Progress progress;
    progress.filesTotal = static_cast<uint32_t>(files.size());
    for (const auto& file : files)
        progress.bytesTotal += file.size;

    std::error_code ec;
    std::filesystem::create_directories(destination, ec);
    if (ec)
    {
        error = "cannot create " + destination.string() + ": " + ec.message();
        return false;
    }

    std::vector<uint8_t> buffer(4u << 20);

    for (const auto& file : files)
    {
        const std::filesystem::path target = destination / std::filesystem::path(file.path);

        std::filesystem::create_directories(target.parent_path(), ec);
        if (ec)
        {
            error = "cannot create " + target.parent_path().string() + ": " + ec.message();
            return false;
        }

        HANDLE out = CreateFileW(target.c_str(), GENERIC_WRITE, 0, nullptr,
            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (out == INVALID_HANDLE_VALUE)
        {
            error = "cannot write " + target.string() + ": " + LastErrorText();
            return false;
        }

        uint64_t offset = m_baseOffset + uint64_t(file.sector) * SectorSize;
        uint64_t remaining = file.size;
        bool failed = false;

        while (remaining > 0)
        {
            const size_t chunk = static_cast<size_t>(
                remaining < buffer.size() ? remaining : buffer.size());

            if (!ReadAt(offset, buffer.data(), chunk, error))
            {
                error = "while reading " + file.path + ": " + error;
                failed = true;
                break;
            }

            DWORD written = 0;
            if (!WriteFile(out, buffer.data(), static_cast<DWORD>(chunk), &written, nullptr) ||
                written != chunk)
            {
                error = "cannot write " + target.string() + ": " + LastErrorText();
                failed = true;
                break;
            }

            offset += chunk;
            remaining -= chunk;
            progress.bytesDone += chunk;
        }

        CloseHandle(out);
        if (failed)
            return false;

        progress.filesDone++;
        progress.current = file.path;
        if (onProgress)
            onProgress(progress);
    }

    return true;
}
