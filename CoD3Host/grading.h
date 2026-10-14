#pragma once

#include <cstdint>

// The colour grading table, as plain arithmetic with no graphics API in it.
//
// This lives on its own, and takes plain floats rather than the settings
// struct, for one reason: so the identity claim can be measured rather than
// argued. The host's single most important property about grading is that with
// every control at its default it is a straight copy of its input, and the only
// way to know that is to run it and compare 256 entries against their own
// indices. `scripts/selfcheck.ps1` compiles `scripts/grading_identity.cpp`,
// which calls this exact function, so the property cannot rot unnoticed.
//
// Why a table at all, rather than arithmetic in the pixel shader:
//
//   - it costs one texture lookup instead of four multiplies,
//   - the shader needs no grading constants and no new constant buffer,
//   - and it cannot change the picture unless a control is actually moved.
//
// SATURATION IS NOT IN HERE, and that is not an oversight.
//
// This table is indexed per channel: the shader looks up red, green and blue
// independently, each at its own value. Saturation needs to know all three
// channels at once, because it mixes a pixel toward its own luma. Building it
// into a per-channel ramp cannot work, and the first attempt proved it: entry
// i is a grey, so its luma is i, and `luma + (r - luma) * saturation` is `r`
// whatever the setting is. The slider did nothing at any position, and the
// self-check caught it by asserting that saturation 0 changes all 256 entries
// when it changes none.
//
// So saturation is applied in the shader instead, from texel.w, and only
// brightness, contrast and temperature live in the table.
//
// The order is the order that behaves:
//
//   brightness  a lift, before anything else
//   temperature a warm/cool push along the blue-yellow axis
//   (saturation happens in the shader, after the lookup)
//   contrast    about mid grey, so 1.0 leaves the pivot alone
//
// The table is applied BEFORE the console's own display gamma in the present
// shader, so the title's table still has the last word on the display
// transform. Grading after a gamma curve is where grading usually goes wrong.
namespace Grading {

// Bits per channel in the table, and therefore the divisor the GPU applies on
// read-back.
//
// This constant exists so the format and the decode cannot drift apart
// silently. `Decode` below is derived from it, and `scripts/selfcheck.ps1`
// asserts that the texture created for this table really is an 8-bit-per-
// channel format - because when the table was first built with a 10-bit
// format and an 8-bit packing, both the packing and this decode were
// self-consistent and every check still passed while the entire game rendered
// four times too dark.
//
// A test that cannot see the format is not a test of the format.
inline constexpr int kBitsPerChannel = 8;
inline constexpr float kChannelMax = float((1 << kBitsPerChannel) - 1);

inline uint32_t To8(float v)
{
    const float c = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
    return uint32_t(c * 255.0f + 0.5f);
}

// Fills 256 R8G8B8A8 texels: red low, then green, then blue, alpha high.
//
// The format is eight bits per channel and NOT the 10-bit R10G10B10A2 the
// display table next door uses. That mistake was made here first and it is
// worth writing down, because it is invisible in a code review and total in
// its effect:
//
//   a UNORM texture is decoded by the GPU as n / (2^bits - 1).
//   R8G8B8A8  -> n / 255
//   R10G10B10A2 -> n / 1023
//
// So packing an 8-bit value into a 10-bit field and letting the shader read it
// back scales the entire picture by 255/1023 = 0.2493. Every pixel, including
// at the identity. White lands on gamma entry 63 and the game looks like it
// was rendered in a cave.
//
// The 10-bit table gets away with it only because it expands on the way in,
// `(i << 2) | (i >> 6)`, which this one does not and must not - see Decode.
inline void Build(float brightness, float contrast, float temperature, uint32_t texels[256])
{
    // Warm pushes red up and blue down.
    //
    // The 0.02 is measured, not chosen by taste. At 0.10, which is what this
    // started at, the top of the slider turned every entry above about i=61
    // into pure (1, mid, 0): mid grey came out orange at 25% of the travel,
    // and the comment here claiming the control was "deliberately gentle" was
    // simply false. At 0.02 the extremes of the slider are a clear tint and
    // nothing is destroyed.
    const float warm = temperature * 0.02f;

    for (int i = 0; i < 256; i++)
    {
        float r = float(i) / 255.0f, g = r, b = r;

        r += brightness; g += brightness; b += brightness;
        r += warm; b -= warm;

        // About mid grey rather than about zero, so raising contrast does not
        // darken the whole picture the way a naive multiply does.
        r = 0.5f + (r - 0.5f) * contrast;
        g = 0.5f + (g - 0.5f) * contrast;
        b = 0.5f + (b - 0.5f) * contrast;

        // R8G8B8A8: red low, green, blue, alpha high and fully opaque.
        texels[i] = To8(r) | (To8(g) << 8) | (To8(b) << 16) | (0xFFu << 24);
    }
}

// What the GPU will read back out of a texel.
//
// This exists so the self-check can assert the identity against what the
// hardware actually sees, rather than against the bits that went in. That
// distinction is precisely the defect described above: the packing was
// self-consistent and the identity check passed, and the picture was still
// four times too dark.
inline float Decode(uint32_t texel, int channel)
{
    // Bounds checked: an out-of-range channel would shift past the word, which
    // is undefined rather than merely wrong.
    if (channel < 0 || channel * kBitsPerChannel >= 32) return 0.0f;
    const uint32_t mask = (1u << kBitsPerChannel) - 1u;
    return float((texel >> (channel * kBitsPerChannel)) & mask) / kChannelMax;
}

} // namespace Grading