#pragma once

// The allocation recipe, not a guessed bytes-per-pixel multiplier. The caller
// supplies committed-resource allocation sizes (including device alignment).
// No route, guide authority, GPU ownership or history admission lives here.
namespace DlssNrReuseBudget
{
constexpr unsigned long long Limit = 512ull * 1024 * 1024;
constexpr unsigned long long Invalid = ~0ull;
constexpr unsigned long long Add(unsigned long long a, unsigned long long b)
{
    return a == Invalid || b == Invalid || b >= Invalid - a ? Invalid : a + b;
}
constexpr bool Fits(unsigned long long required, unsigned long long retired)
{
    return Add(required, retired) <= Limit;
}
enum class Kind { Detail, ColourDepth, Motion, Answer, Coverage, Readback };
struct Layout
{
    unsigned width = 0, height = 0, motionWidth = 0, motionHeight = 0;
    unsigned coverageTiles = 0, coverageSlots = 0, coverageBytes = 0;
    // Only authenticated interval-1 picture feedback consumes its reads before same-set capture.
    bool singleHistory = false;
    bool secondMotion = false, estimate = false, steadied = false;
    bool fused = false, shown = false, debug = false, measure = false;
    bool fusedRgba16 = false; // same fused resource, promoted to signed scratch when needed
    bool operator==(const Layout& r) const
    {
        return width == r.width && height == r.height && motionWidth == r.motionWidth &&
            motionHeight == r.motionHeight && coverageTiles == r.coverageTiles &&
            coverageSlots == r.coverageSlots && coverageBytes == r.coverageBytes &&
            singleHistory == r.singleHistory && secondMotion == r.secondMotion &&
            estimate == r.estimate && steadied == r.steadied &&
            fused == r.fused && shown == r.shown && debug == r.debug && measure == r.measure &&
            fusedRgba16 == r.fusedRgba16;
    }
};
template<class AllocationSize> unsigned long long Required(const Layout& l, AllocationSize&& size)
{
    if (!l.width || !l.height || !l.motionWidth || !l.motionHeight ||
        l.width > 16384 || l.height > 16384 || l.motionWidth > 16384 || l.motionHeight > 16384)
        return Invalid;
    unsigned long long bytes = 0;
    auto add = [&](Kind kind, unsigned w, unsigned h, unsigned count)
    {
        if (!count || bytes == Invalid) return;
        const auto allocation = size(kind, w, h);
        if (!allocation || allocation == Invalid || allocation > (Invalid - bytes) / count)
            bytes = Invalid;
        else bytes += allocation * count;
    };
    add(Kind::Detail, l.width, l.height, l.singleHistory ? 1 : 2);
    add(Kind::ColourDepth, l.width, l.height, l.singleHistory ? 1 : 2);
    if (!l.singleHistory)
    {
        add(Kind::Motion, l.width, l.height, l.secondMotion ? 2 : 1);
        add(Kind::Motion, l.motionWidth, l.motionHeight, 1);
    }
    add(Kind::Detail, l.width, l.height, l.estimate ? 1 : 0);
    add(Kind::Answer, l.width, l.height, l.steadied ? 1 : 0);
    add(l.fusedRgba16 ? Kind::Detail : Kind::Answer, l.width, l.height, l.fused ? 1 : 0);
    add(Kind::Detail, l.width, l.height, l.shown ? 1 : 0);
    add(Kind::Answer, l.width, l.height, l.debug ? 1 : 0);
    if (l.measure)
    {
        if (!l.coverageTiles || !l.coverageSlots || !l.coverageBytes) return Invalid;
        add(Kind::Coverage, l.coverageTiles, l.coverageTiles, 1);
        add(Kind::Readback, l.coverageBytes, 1, l.coverageSlots);
    }
    return bytes;
}
}
