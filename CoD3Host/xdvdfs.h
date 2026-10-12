#pragma once

// Reader for XDVDFS, the filesystem on Xbox 360 game discs.
//
// A disc image is a plain sector image whose filesystem starts at one of a few
// fixed offsets, depending on which disc format the title shipped on. Sector 32
// of that region holds the volume descriptor, which points at the root
// directory. Directories are binary trees of variable length entries, not flat
// lists, so listing one means walking the tree.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace XDvdFs
{
    inline constexpr uint32_t SectorSize = 2048;

    struct Entry
    {
        std::string path;      // full path inside the image, '/' separated
        uint32_t    sector = 0;
        uint32_t    size = 0;
        bool        isDirectory = false;
    };

    struct Progress
    {
        uint64_t bytesDone = 0;
        uint64_t bytesTotal = 0;
        uint32_t filesDone = 0;
        uint32_t filesTotal = 0;
        std::string current;
    };

    class Image
    {
    public:
        ~Image();

        // Opens the image and locates its filesystem. Returns false and fills
        // `error` if the file is not an Xbox 360 disc image.
        bool Open(const std::filesystem::path& isoPath, std::string& error);
        void Close();

        // Every file in the image, directories excluded.
        bool ListFiles(std::vector<Entry>& out, std::string& error);

        // Writes the whole tree to `destination`. `onProgress` is called as
        // files complete, so a caller can show progress without polling.
        bool ExtractAll(const std::filesystem::path& destination,
                        const std::function<void(const Progress&)>& onProgress,
                        std::string& error);

        // Reads one file out of the image. Used to check the executable before
        // committing to a multi-gigabyte extraction, and to test the reader
        // without writing the whole disc to disk.
        bool ReadEntry(const Entry& entry, std::vector<uint8_t>& out, std::string& error);

        uint64_t BaseOffset() const { return m_baseOffset; }
        const char* FormatName() const { return m_formatName; }

    private:
        bool ReadAt(uint64_t offset, void* buffer, size_t size, std::string& error);
        bool WalkDirectory(uint32_t sector, uint32_t size, const std::string& prefix,
                           std::vector<Entry>& out, std::string& error, int depth);

        void*       m_file = nullptr;   // HANDLE
        uint64_t    m_fileSize = 0;
        uint64_t    m_baseOffset = 0;
        const char* m_formatName = "unknown";
        uint32_t    m_rootSector = 0;
        uint32_t    m_rootSize = 0;
    };
}
