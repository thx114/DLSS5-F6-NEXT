#pragma once

// Which of NR's surfaces are stale when a format changes (DlssNr_Dx12.cpp, ReleaseSurfacesIfFormatChanged).
//
// The model surfaces follow the canonical game colour, or RGBA16F while Compress screen edges packs the picture.
// Internal colour surfaces (colorCopy, colorSmall, outputNative) also use the canonical format. Shader views keep
// each resource's own channel order, so an RGBA8/BGRA8 swap does not require new model features or history.
// Model and colour formats are checked separately: compression can keep the model at RGBA16F while the game's
// colour changes to a genuinely different family, which still invalidates the internal set.
//
// Canonical colour families are NOT D3D12 copy-compatible families. activeColor, lutScratch, heldColor, guidePreview
// and hdrCopy when copied back to a non-UAV game target retain the exact game format and check it independently.
// CopyResource / CopyTextureRegion do not convert channel order; only the shader paths cross that boundary.

namespace DlssNr
{
// DXGI_FORMAT values as ints, keeping this helper independent of Windows headers.
constexpr int kR8G8B8A8Typeless = 27, kR8G8B8A8Unorm = 28, kR8G8B8A8UnormSrgb = 29;
constexpr int kB8G8R8A8Unorm = 87, kB8G8R8A8Typeless = 90, kB8G8R8A8UnormSrgb = 91;

// Only BGRA8 changes channel order. Preserve typeless/UNORM/sRGB distinctions; other formats are unchanged.
constexpr int CanonicalColourFormat(int format)
{
    switch (format)
    {
    case kB8G8R8A8Typeless:
        return kR8G8B8A8Typeless;
    case kB8G8R8A8Unorm:
        return kR8G8B8A8Unorm;
    case kB8G8R8A8UnormSrgb:
        return kR8G8B8A8UnormSrgb;
    default:
        return format;
    }
}

// NR's internal colour equivalence only; never use this predicate to authorize a GPU copy.
constexpr bool SameColourFamily(int a, int b) { return CanonicalColourFormat(a) == CanonicalColourFormat(b); }

// Compare actual internal allocations to the desired canonical formats exactly. 0 means no surface yet.
constexpr bool SurfacesStale(int haveModel, int wantModel, int haveColour, int wantColour)
{
    return (haveModel != 0 && haveModel != wantModel) || (haveColour != 0 && haveColour != wantColour);
}

// A surface made at the ordinary (uncompressed) size: stale when that size is not the one wanted now. Under Compress
// screen edges the change check sees only the packed size, which can stay put while the ordinary size moves by a 16-pixel
// step (1920x1080, Balanced, scale 0.50 to 0.504: 960x544 -> 976x544, packed 864x496 either way), so the surface has to
// be asked for itself.
constexpr bool SizeStale(unsigned haveW, unsigned haveH, unsigned wantW, unsigned wantH)
{
    return haveW != 0 && (haveW != wantW || haveH != wantH);
}
} // namespace DlssNr
