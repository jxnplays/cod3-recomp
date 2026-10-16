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
    // The stick the title reads saturates at 32767, and it is fed COUNTS
    // rather than a position. So the gain is really "stick units per mouse
    // count", and what matters is how many counts it takes to reach full
    // deflection.
    //
    // It used to be 1300 per count, which sounds harmless and is not: with
    // the frame normalisation in ReadMouse, full deflection arrived after
    // about 25 counts in a sixtieth of a second - 1500 counts a second. At
    // 1600 DPI that is 24 mm per second of hand movement for full turn speed.
    // No mouse can be moved that slowly, so every ordinary flick pinned the
    // stick at 32767 and the output was a square wave: full speed or nothing,
    // with no fine aim in between. That is what "the aiming is not fluid"
    // turned out to be - not too slow, but saturated.
    //
    // Even the bottom of the old 0.1-10 slider could not rescue it: at 0.1
    // it still saturated at about 240 mm/s, which is below a normal flick.
    //
    // The anchor is now 150 per count, which is roughly 4000 counts per
    // sixtieth of a second for full deflection - a turn across a desk rather
    // than across a fingernail. The slider floor drops to 0.01 so a high-DPI
    // mouse still has somewhere to go.
    //
    // The piecewise shape stays, and for the same reason: a stick saturates,
    // so the useful range is bounded, and a plain square root anchored at 1
    // moves every setting below the point it was meant to affect - which is
    // how a 0.3 setting once became nearly twice as fast as it had been.
    inline double MouseGain(double sensitivity)
    {
        if (sensitivity <= 3.0) return 150.0 * sensitivity;
        return 450.0 * std::sqrt(sensitivity / 3.0);
    }

    // The lowest the sensitivity slider may go. Matched by the menu, the ini
    // validation and ReadMouse; they were three separate copies of the same
    // range and all three have to move together.
    inline constexpr double kMinMouseSensitivity = 0.01;

    // The highest. It was 10, and 10 was reported as "still far too slow" -
    // which is consistent with the gain below rather than surprising: at 10
    // the gain is 450*sqrt(10/3) = 821 counts per pixel, and a 1000 dpi mouse
    // covers 1000 counts per inch, so 10 is still under one screen-width per
    // 1.2 inches of hand movement. Somebody who wants to flick across a room
    // needs the headroom, and the curve already grows slowly enough that the
    // extra range does not make the low end unusable.
    //
    // 100 is the top because past that the curve's square root makes the
    // top of the slider feel no different from the middle of it.
    inline constexpr double kMaxMouseSensitivity = 100.0;
}

#endif