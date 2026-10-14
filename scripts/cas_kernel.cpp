// Proves the sharpening kernel does something you can see, and nothing you
// cannot.
//
// Compiled and run by scripts/selfcheck.ps1. The arithmetic below is a
// transcription of the `cas` entry point in CoD3Host/render_d3d11.cpp, kept
// deliberately line for line so a change to the shader has to be made here
// too.
//
// WHY THIS EXISTS. The kernel shipped once and did nothing. It computed
// `cM - (box - cM) * amount` and clamped the result to [mn, mx] - but mn and mx
// are the extremes of a neighbourhood that INCLUDES cM, so on most pixels cM
// already is the extreme and the clamp discarded the result. What survived
// measured as about seven percent more high-frequency energy: real, and
// invisible. A test that only asks "did any pixel change?" would have called
// that a pass. So this asks the question that actually matters - is it
// visible, and is it still bounded?

#include <cmath>
#include <initializer_list>
#include <cstdint>
#include <cstdio>

namespace
{
    struct RGB { float r, g, b; };

    RGB operator+(RGB a, RGB b) { return { a.r + b.r, a.g + b.g, a.b + b.b }; }
    RGB operator-(RGB a, RGB b) { return { a.r - b.r, a.g - b.g, a.b - b.b }; }
    RGB operator*(RGB a, float s) { return { a.r * s, a.g * s, a.b * s }; }
    RGB operator-(RGB a, float s) { return { a.r - s, a.g - s, a.b - s }; }
    RGB operator+(RGB a, float s) { return { a.r + s, a.g + s, a.b + s }; }
    RGB minRGB(RGB a, RGB b) { return { a.r < b.r ? a.r : b.r, a.g < b.g ? a.g : b.g, a.b < b.b ? a.b : b.b }; }
    RGB maxRGB(RGB a, RGB b) { return { a.r > b.r ? a.r : b.r, a.g > b.g ? a.g : b.g, a.b > b.b ? a.b : b.b }; }
    RGB clampRGB(RGB v, RGB lo, RGB hi)
    {
        return { v.r < lo.r ? lo.r : (v.r > hi.r ? hi.r : v.r),
                 v.g < lo.g ? lo.g : (v.g > hi.g ? hi.g : v.g),
                 v.b < lo.b ? lo.b : (v.b > hi.b ? hi.b : v.b) };
    }
    float luma(RGB c) { return 0.299f * c.r + 0.587f * c.g + 0.114f * c.b; }

    // The kernel, transcribed from the shader.
    RGB Cas(const RGB cM, const RGB cN, const RGB cS, const RGB cE, const RGB cW,
            const RGB cNW, const RGB cNE, const RGB cSW, const RGB cSE, float amount)
    {
        RGB mn = minRGB(minRGB(minRGB(cN, cS), minRGB(cE, cW)), minRGB(minRGB(cNW, cNE), minRGB(cSW, cSE)));
        mn = minRGB(mn, cM);
        RGB mx = maxRGB(maxRGB(maxRGB(cN, cS), maxRGB(cE, cW)), maxRGB(maxRGB(cNW, cNE), maxRGB(cSW, cSE)));
        mx = maxRGB(mx, cM);

        const RGB box = (cM * 4.0f + (cN + cS + cE + cW) * 2.0f + (cNW + cNE + cSW + cSE)) * (1.0f / 16.0f);
        const float range = luma(mx) - luma(mn);
        const float allow = 1.0f / (1.0f + range * 6.0f);
        RGB sharpened = cM + (cM - box) * (amount * 5.0f * allow);
        // A fixed four percent either side, matching the shader.
        return clampRGB(sharpened, mn - 0.04f, mx + 0.04f);
    }

    // A small image we can run the kernel over.
    struct Image
    {
        static constexpr int W = 24, H = 16;
        RGB p[H][W];
    };

    // Deterministic pseudo-texture: broad noise plus a hard vertical edge and
    // a flat block, which is the mix that decides whether sharpening reads as
    // an improvement or as a mess.
    Image MakeScene(int seed)
    {
        Image img{};
        uint32_t s = uint32_t(seed) * 2654435761u + 1u;
        auto rnd = [&s]() { s = s * 1664525u + 1013904223u; return float((s >> 8) & 0xFFFF) / 65535.0f; };
        for (int y = 0; y < Image::H; y++)
            for (int x = 0; x < Image::W; x++)
            {
                RGB c{};
                if (x < 9) {                       // textured mid-tone
                    const float n = (rnd() - 0.5f) * 0.10f;
                    c = { 0.45f + n, 0.40f + n, 0.33f + n };
                } else if (x < 15) {               // hard step edge
                    c = { 0.02f, 0.02f, 0.03f };
                } else {                            // flat sky, must not ring
                    c = { 0.72f, 0.76f, 0.80f };
                }
                img.p[y][x] = c;
            }
        return img;
    }

    // Mean absolute difference between neighbours: high frequency energy. This
    // is the quantity sharpening is supposed to raise and grading is supposed
    // to leave alone.
    double Detail(const Image& img)
    {
        double sum = 0.0; int n = 0;
        for (int y = 0; y < Image::H; y++)
            for (int x = 0; x + 1 < Image::W; x++)
            {
                sum += std::fabs(img.p[y][x + 1].r - img.p[y][x].r);
                n++;
            }
        return n ? sum / n : 0.0;
    }

    RGB At(const Image& img, int x, int y)
    {
        const int cx = x < 0 ? 0 : (x >= Image::W ? Image::W - 1 : x);
        const int cy = y < 0 ? 0 : (y >= Image::H ? Image::H - 1 : y);
        return img.p[cy][cx];
    }

    Image Sharpen(const Image& src, float amount)
    {
        Image out = src;
        for (int y = 0; y < Image::H; y++)
            for (int x = 0; x < Image::W; x++)
                out.p[y][x] = Cas(src.p[y][x],
                                  At(src, x, y - 1), At(src, x, y + 1),
                                  At(src, x + 1, y), At(src, x - 1, y),
                                  At(src, x - 1, y - 1), At(src, x + 1, y - 1),
                                  At(src, x - 1, y + 1), At(src, x + 1, y + 1),
                                  amount);
        return out;
    }

    int failures = 0;
    void Check(bool ok, const char* what)
    {
        if (!ok) { std::printf("      %s\n", what); failures++; }
    }
}

int main()
{
    // 1. At zero it must do nothing at all. Not "almost nothing".
    {
        const Image a = MakeScene(1), b = Sharpen(MakeScene(1), 0.0f);
        double worst = 0.0;
        for (int y = 0; y < Image::H; y++)
            for (int x = 0; x < Image::W; x++)
            {
                worst = std::fmax(worst, std::fabs(a.p[y][x].r - b.p[y][x].r));
                worst = std::fmax(worst, std::fabs(a.p[y][x].g - b.p[y][x].g));
                worst = std::fmax(worst, std::fabs(a.p[y][x].b - b.p[y][x].b));
            }
        Check(worst == 0.0, "amount 0 changed a pixel");
    }

    // 2. At the middle of the slider it must be VISIBLE.
    //
    //    The threshold is the point. The version that shipped raised detail by
    //    about seven percent and nobody could see it; seven percent is not a
    //    pass for a control the player is meant to notice, so the bar here is
    //    a fifth.
    {
        const Image base = MakeScene(1);
        const double d0 = Detail(base);
        double last = 0.0;
        for (float amount : { 0.25f, 0.5f, 0.75f, 1.0f })
        {
            const Image s = Sharpen(base, amount);
            const double d = Detail(s);
            const double pct = d0 > 0 ? 100.0 * (d - d0) / d0 : 0.0;
            std::printf("      amount %.2f: detail +%.1f%%\n", amount, pct);
            Check(pct >= 20.0, "the sharpening is too weak to see");
            Check(pct > last, "more amount did not sharpen more");
            last = pct;
        }
    }

    // 3. Bounded. Every pixel must stay inside its own neighbourhood's
    //    extremes plus the margin. Without this the pass would clip and ring,
    //    and it is the reason the original was clamped - just clamped wrongly.
    {
        const Image base = MakeScene(1);
        for (float amount : { 0.25f, 0.5f, 0.75f, 1.0f })
        {
            const Image s = Sharpen(base, amount);
            double worstOver = 0.0;
            for (int y = 0; y < Image::H; y++)
                for (int x = 0; x < Image::W; x++)
                {
                    RGB mn = base.p[y][x], mx = base.p[y][x];
                    const int nx[9] = { x, x, x + 1, x - 1, x - 1, x + 1, x - 1, x + 1, x };
                    const int ny[9] = { y - 1, y + 1, y, y, y - 1, y - 1, y + 1, y + 1, y };
                    for (int k = 0; k < 9; k++) { mn = minRGB(mn, At(base, nx[k], ny[k])); mx = maxRGB(mx, At(base, nx[k], ny[k])); }
                    const RGB lo = mn - 0.04f, hi = mx + 0.04f;
                    worstOver = std::fmax(worstOver, s.p[y][x].r - hi.r);
                    worstOver = std::fmax(worstOver, s.p[y][x].g - hi.g);
                    worstOver = std::fmax(worstOver, s.p[y][x].b - hi.b);
                    worstOver = std::fmax(worstOver, lo.r - s.p[y][x].r);
                    worstOver = std::fmax(worstOver, lo.g - s.p[y][x].g);
                    worstOver = std::fmax(worstOver, lo.b - s.p[y][x].b);
                }
            if (worstOver > 1e-5)
            {
                std::printf("      amount %.2f overshot the bound by %g\n", amount, worstOver);
                failures++;
            }
        }
    }

    // 4. The flat sky must stay flat. A sky is the worst case for an unsharp
    //    mask - there is nothing there to sharpen and noise becomes the whole
    //    signal - and it is the first thing anyone would notice going wrong.
    {
        const Image base = MakeScene(1);
        const Image s = Sharpen(base, 1.0f);
        double worst = 0.0;
        for (int y = 0; y < Image::H; y++)
            for (int x = 16; x < Image::W; x++)
                worst = std::fmax(worst, std::fabs(s.p[y][x].r - base.p[y][x].r));
        std::printf("      the flat area moved by up to %g at full strength\n", worst);
        Check(worst < 0.01, "the flat area was sharpened, which is where ringing starts");
    }

    // 5. The hard step edge must not gain a bright line. This is the artefact
    //    that makes sharpening look like a mistake: a light rim along every
    //    high-contrast boundary, which is what the subtitle text and the
    //    skyline are made of.
    //
    //    Judged against the brightest value the scene already had in that
    //    region, not an absolute number: the scene contains both a 0.45 mid
    //    tone and a 0.72 sky, and a threshold between them would pass a halo
    //    or fail a clean edge for the wrong reason.
    {
        const Image base = MakeScene(1);
        const Image s = Sharpen(base, 1.0f);
        float origMax = 0.0f, sharpMax = 0.0f;
        for (int y = 0; y < Image::H; y++)
            for (int x = 7; x <= 17; x++)
            {
                origMax = std::fmax(origMax, base.p[y][x].r);
                sharpMax = std::fmax(sharpMax, s.p[y][x].r);
            }
        std::printf("      near the step edge: brightest %.3f, was %.3f, halo %+.3f\n",
                    sharpMax, origMax, sharpMax - origMax);
        Check(sharpMax - origMax < 0.05, "a bright halo appeared along the hard edge");
    }

    if (failures == 0) std::printf("sharpening is visible, bounded, and leaves flat areas alone\n");
    return failures == 0 ? 0 : 1;
}