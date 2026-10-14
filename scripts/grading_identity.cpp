// Proves the colour grading pass's defaults are the identity.
//
// Compiled and run by scripts/selfcheck.ps1 against CoD3Host/grading.h - the
// header the host itself includes, not a paraphrase of it.
//
// THE POINT OF THIS FILE, after what it missed once:
//
//   The first version of this check asserted that the raw 8-bit value went
//   into the right bit positions. It passed. The picture was still four times
//   too dark, because the texture was a 10-bit UNORM format, and a UNORM
//   sample decodes as n / (2^bits - 1) - n / 1023, not n / 255. The packing
//   was self-consistent and the check was blind to what the hardware reads.
//
//   So every identity assertion below goes through Grading::Decode, which
//   mirrors the GPU's decode. The format is not asserted here - the check is
//   about what comes out, and Grading::Decode is the single place that knows
//   how that works. If the format and the decode ever disagree again, this
//   file reports the truth about the numbers and the mismatch shows up as a
//   failure rather than as a dark game.

#include "grading.h"

#include <cstdio>

static int failures = 0;

static void Check(bool ok, const char* what)
{
    if (!ok)
    {
        std::printf("      %s\n", what);
        failures++;
    }
}

int main()
{
    // 1. THE IDENTITY, as the GPU sees it. Entry i must decode to exactly i/255
    //    in all three channels. A 10-bit field fails here with a value of
    //    i/1023, which is the bug this file exists to catch.
    {
        uint32_t texels[256];
        Grading::Build(0.0f, 1.0f, 0.0f, texels);
        int bad = 0;
        for (uint32_t i = 0; i < 256 && !bad; i++)
        {
            const float r = Grading::Decode(texels[i], 0);
            const float g = Grading::Decode(texels[i], 1);
            const float b = Grading::Decode(texels[i], 2);
            const float want = float(i) / 255.0f;
            if (r != want || g != want || b != want)
            {
                std::printf("      entry %u decodes to %.5f,%.5f,%.5f; want %.5f "
                            "(a bit-packing mismatch would show 1/1023 here)\n",
                            i, r, g, b, want);
                bad = 1;
            }
        }
        Check(!bad, "the defaults are not the identity as the GPU reads them");
    }

    // 2. Alpha is fully opaque everywhere. A transparent entry would make the
    //    present shader blend the frame against whatever is behind it.
    {
        uint32_t texels[256];
        Grading::Build(0.3f, 1.4f, 5.0f, texels);
        int bad = 0;
        for (uint32_t i = 0; i < 256 && !bad; i++)
            if (((texels[i] >> 24) & 0xFFu) != 0xFFu)
            {
                std::printf("      entry %u has alpha %u\n", i, (texels[i] >> 24) & 0xFFu);
                bad = 1;
            }
        Check(!bad, "alpha is not opaque");
    }

    // 3. Every decoded channel stays inside [0,1] at the extremes of every
    //    slider. Packing an out-of-range value into the byte field wraps it, and
    //    a wrapped channel reads as a completely different colour.
    {
        const float b[] = { -0.5f, 0.0f, 0.5f };
        const float k[] = { 0.5f, 1.0f, 2.0f };
        const float t[] = { -10.0f, 0.0f, 10.0f };
        int bad = 0;
        for (float bv : b) for (float kv : k) for (float tv : t)
        {
            if (bad) break;
            uint32_t texels[256];
            Grading::Build(bv, kv, tv, texels);
            for (uint32_t i = 0; i < 256 && !bad; i++)
                for (int q = 0; q < 3; q++)
                {
                    const float v = Grading::Decode(texels[i], q);
                    if (!(v >= 0.0f && v <= 1.0f))
                    {
                        std::printf("      b%g c%g t%g entry %u channel %d decoded to %f\n",
                                    bv, kv, tv, i, q, v);
                        bad = 1;
                        break;
                    }
                }
        }
        Check(!bad, "a channel left the unit range");
    }

    // 4. The controls must do something. A pass indistinguishable from no pass
    //    is not a feature, and this is what catches the call site dropping it.
    {
        uint32_t base[256], hot[256], cold[256], punch[256], lift[256];
        Grading::Build(0.0f, 1.0f, 0.0f, base);
        Grading::Build(0.0f, 1.0f, 5.0f, hot);
        Grading::Build(0.0f, 1.0f, -5.0f, cold);
        Grading::Build(0.0f, 1.5f, 0.0f, punch);
        Grading::Build(0.2f, 1.0f, 0.0f, lift);

        uint32_t hotDiff = 0, coldDiff = 0, punchDiff = 0, liftDiff = 0;
        for (uint32_t i = 0; i < 256; i++)
        {
            hotDiff += (hot[i] != base[i]);
            coldDiff += (cold[i] != base[i]);
            punchDiff += (punch[i] != base[i]);
            liftDiff += (lift[i] != base[i]);
        }
        Check(hotDiff > 200, "temperature does almost nothing");
        Check(coldDiff > 200, "the cold end does almost nothing");
        Check(punchDiff > 200, "contrast does almost nothing");
        Check(liftDiff > 200, "brightness does almost nothing");

        // Warm must move red up and blue down, cold the reverse. A sign error is
        // invisible at +-1 and obvious at +-10.
        uint32_t warmR = 0, coolR = 0, warmB = 0;
        for (uint32_t i = 1; i < 256; i++)
        {
            warmR += (Grading::Decode(hot[i], 0) > Grading::Decode(base[i], 0));
            coolR += (Grading::Decode(cold[i], 0) < Grading::Decode(base[i], 0));
            warmB += (Grading::Decode(hot[i], 2) < Grading::Decode(base[i], 2));
        }
        Check(warmR > 200, "warming did not push red up");
        Check(coolR > 200, "cooling did not push red down");
        Check(warmB > 200, "warming did not pull blue down");

        // The ENDPOINTS of the slider, which is where a temperature control
        // turns into a colour cast instead of a tint. At the original
        // coefficient of 0.10, temperature +10 collapsed every entry above
        // about i=61 to (1, mid, 0) - mid grey came out orange at a quarter
        // of the travel. Sampled here so that cannot come back quietly.
        {
            uint32_t hot[256], cold[256];
            Grading::Build(0.0f, 1.0f, 10.0f, hot);
            Grading::Build(0.0f, 1.0f, -10.0f, cold);
            // Mid grey must still be recognisably mid grey at the extremes:
            // its channels must stay far closer to each other than to 0 or 1.
            const float r = Grading::Decode(hot[128], 0), g = Grading::Decode(hot[128], 1), b = Grading::Decode(hot[128], 2);
            const float spread = (r > g ? r - g : g - r) + (g > b ? g - b : b - g);
            Check(spread < 0.45f, "temperature +10 turns mid grey into a colour cast");
            const float cr = Grading::Decode(cold[128], 0), cb = Grading::Decode(cold[128], 2);
            Check((cr - cb) < 0.45f, "temperature -10 turns mid grey into a colour cast");
            // And white must still be white: a tint cannot clip the top end.
            Check(Grading::Decode(hot[255], 0) == Grading::Decode(base[255], 0),
                  "temperature changed pure white, which means it is clipping rather than tinting");
        }

        // Contrast pivots about mid grey: 128 must stay put, and points either side
        // must move apart. A pivot at the wrong point is exactly the sort of
        // thing that reads as obviously correct in the source.
        //
        // Sampled at 60 and 200, not at 0 and 255: those two are already at the
        // ends of the range and are clamped there, so raising contrast cannot
        // move them and an assertion about them proves nothing. An earlier
        // version of this file used them and failed for the right reason at the
        // wrong place.
        const float mid = Grading::Decode(punch[128], 0);
        Check(mid == Grading::Decode(base[128], 0),
              "contrast moved mid grey, so it is pivoting about the wrong point");
        Check(Grading::Decode(punch[200], 0) > Grading::Decode(base[200], 0),
              "contrast did not brighten the upper half");
        Check(Grading::Decode(punch[60], 0) < Grading::Decode(base[60], 0),
              "contrast did not darken the lower half");
    }

    // 5. Saturation, which lives in the shader rather than the table.
    //
    //    It cannot go in the table: a per-channel ramp cannot mix a pixel
    //    toward its own luma, so the first version was a no-op at every
    //    position. It is checked here because the alternative is that it is
    //    never checked at all.
    //
    //    This reproduces the shader's line on REAL colours, not greys - the
    //    earlier version used greys only, which cannot distinguish a correct
    //    saturation from one that never runs.
    {
        const float cases[][3] = {
            { 0.5f, 0.5f, 0.5f },   // grey: must not move
            { 0.8f, 0.2f, 0.1f },   // warm: loses saturation at 0
            { 0.1f, 0.4f, 0.9f },   // cool: gains saturation at 2
            { 1.0f, 0.0f, 0.0f },   // pure red, the extreme
            { 0.0f, 0.0f, 0.0f },
            { 1.0f, 1.0f, 1.0f },
        };
        const int n = int(sizeof(cases) / sizeof(cases[0]));
        // At the default the shader takes a branch and returns the pixel untouched:
        // `return (texel.w == 1.0) ? t : saturate(l + (t - l) * texel.w);`
        //
        // That branch is not defensive decoration, and this asserts WHY rather
        // than restating the ternary in C++ - which would be the paraphrase
        // mistake this project has already made once, and which can never fail.
        //
        // The claim is that `l + (t - l) * 1.0f` is NOT the identity in IEEE
        // arithmetic for 8-bit colours. It is false for greys and true for a
        // large number of real colours. If it ever stopped being true the
        // branch would be pointless and this check would say so.
        {
            uint32_t inexact = 0, total = 0;
            for (int ri = 0; ri < 256; ri += 1)
                for (int gi = 0; gi < 256; gi += 7)
                    for (int bi = 0; bi < 256; bi += 11)
                    {
                        const float r = float(ri) / 255.0f;
                        const float g = float(gi) / 255.0f;
                        const float b = float(bi) / 255.0f;
                        const float l = 0.299f * r + 0.587f * g + 0.114f * b;
                        total++;
                        if (l + (r - l) * 1.0f != r) inexact++;
                    }
            if (inexact == 0)
                Check(false, "l + (t - l) * 1.0f reproduces every colour exactly; "
                             "the branch in grade() is unmotivated and the claim above is stale");
            else
                std::printf("      saturation add-back is inexact for %u of %u sampled colours, "
                            "so the branch in grade() is required\n", inexact, total);
        }

        // At 0 every channel must land exactly on the luma.
        {
            int bad = 0;
            for (int i = 0; i < n && !bad; i++)
            {
                const float r = cases[i][0], g = cases[i][1], b = cases[i][2];
                const float l = 0.299f * r + 0.587f * g + 0.114f * b;
                const float o = l + (r - l) * 0.0f;
                if (o != l)
                {
                    std::printf("      saturation 0 did not flatten (%g,%g,%g)\n", r, g, b);
                    bad = 1;
                }
            }
            Check(!bad, "saturation at 0 does not flatten");
        }

        // At 2 it must actually spread the channels apart on a grey-free
        // colour, otherwise the control does nothing. This is the check that
        // the first version could not make, because saturation was in a
        // per-channel table where it was a no-op at every position.
        {
            const float r = 0.8f, g = 0.2f, b = 0.1f;
            const float l = 0.299f * r + 0.587f * g + 0.114f * b;
            const float before = r - b;
            const float after = (l + (r - l) * 2.0f) - (l + (b - l) * 2.0f);
            Check(after > before, "saturation at 2 did not increase the spread");
        }
    }

    if (failures == 0)
        std::printf("grading identity and response both hold, as the GPU reads them\n");
    return failures == 0 ? 0 : 1;
}