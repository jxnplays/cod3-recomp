#pragma once

// First run setup.
//
// The recompiled executable needs the game's own data files, which cannot ship
// with it. On first launch there is no `game` folder next to the executable, so
// the user is asked for their disc image or an already extracted copy, and the
// files are put in place. On every later launch the folder is already there and
// the game starts straight away.

#include <filesystem>
#include <string>

namespace Installer
{
    struct GameLocation
    {
        std::filesystem::path root;      // folder holding default.xex
        std::filesystem::path executable;// <root>/default.xex
    };

    // Size of the default.xex this build was recompiled from. A different size
    // means a different region or revision, and the recompiled code will not
    // match it.
    inline constexpr uint64_t ExpectedXexSize = 7254016;

    // A source named on the command line, so setup can run without the
    // interactive menu. Both empty means ask the user.
    struct Source
    {
        std::filesystem::path iso;     // extract this disc image
        std::filesystem::path folder;  // use this extracted folder
        bool copyFolder = false;       // copy it in rather than using it in place
    };

    // True when `folder` looks like an extracted Call of Duty 3 disc.
    bool IsGameFolder(const std::filesystem::path& folder);

    // Finds the game, running first time setup if it is not there yet.
    // Returns false if the user cancelled or setup failed; `error` is empty on
    // a deliberate cancellation.
    bool Locate(const std::filesystem::path& exeDirectory,
                const Source& source,
                GameLocation& out,
                std::string& error);
}
