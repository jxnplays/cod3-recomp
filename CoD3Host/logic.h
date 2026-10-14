// The two pieces of logic that are easy to get subtly wrong, in one place so
// that the code and the check that guards them cannot drift apart.
//
// Both were once inline in the host, where they were read and believed rather
// than run. The .xex suffix test compared eleven characters against a
// four-character string for a whole session, because nothing ever executed it.
// The sensitivity curve was a plain square root across its whole range to raise
// its top end, which made a 0.3 setting 1.83 times faster and reached the
// player as "the aiming is janky" - again, because nothing ever ran it.
//
// scripts/selfcheck.ps1 compiles this header and runs both. That only means
// anything if the host uses these functions too, so: do not re-inline them.
#ifndef COD3_HOST_LOGIC_H
#define COD3_HOST_LOGIC_H

#include <cmath>
#include <cstdint>
#include <cctype>
#include <string>
#include <string_view>

namespace HostLogic
{
    // True when this is one of the titles the host may be asked to boot.
    //
    // Only ".xex" counts. This used to be written as a comparison of the last
    // 11 characters against "default.xex", and when it was widened to accept
    // any .xex the length was left at 11 while the string became four
    // characters long - so it compared 11 characters against 4 and could never
    // match. Every relaunch was refused, including the next-mission one that
    // had always worked.
    inline bool IsBootableXex(const std::string& name)
    {
        std::string lower = name;
        for (char& c : lower) c = char(tolower(static_cast<unsigned char>(c)));
        const std::string_view suffix = ".xex";
        if (lower.size() < suffix.size()) return false;
        return lower.compare(lower.size() - suffix.size(), suffix.size(), suffix) == 0;
    }

    // The mouse gain for a sensitivity setting.
    //
    // Piecewise on purpose. A stick reports an int16 and saturates at 32767,
    // so a plain linear gain made anything past 3.0 clip within a few pixels
    // of movement - a flick became a full-speed turn with nothing in between.
    // And a plain square root to fix that was worse: anchored anywhere it
    // moves every setting below the point it was meant to affect, which is
    // exactly how a 0.3 setting ended up nearly twice as fast as it had been.
    //
    // Below 3.0 this is bit for bit the gain the game has always had.
    inline double MouseGain(double sensitivity)
    {
        if (sensitivity <= 3.0) return 1300.0 * sensitivity;
        return 3900.0 * std::sqrt(sensitivity / 3.0);
    }
}

#endif