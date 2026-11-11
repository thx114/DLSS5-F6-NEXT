#pragma once

#include <cstdint>

// Observations of the current synchronous menu call, never GPU ownership or
// draw-data storage. Identity tokens are compared, not dereferenced.
namespace MenuHotpaths
{
enum class Backend { Other, Dx11, Dx12 };

struct FrameContext
{
    Backend Path = Backend::Other;
    std::uintptr_t DeviceIdentity = 0;
    std::uint64_t BackendGeneration = 0;
    std::uint64_t OutputGeneration = 0;
    bool MenuVisible = false;
    bool FpsOverlay = false;
    bool NotificationDemand = false;
    bool OtherOverlayDemand = false;
};

inline thread_local FrameContext* Current = nullptr;

// The borrow ends on every exit, including unwinding. A nested call restores
// its still-live parent; no TLS context pointer survives the outer call.
class FrameScope final
{
    FrameContext* Previous;

  public:
    explicit FrameScope(FrameContext& frame) noexcept : Previous(Current) { Current = &frame; }
    ~FrameScope() { Current = Previous; }
    FrameScope(const FrameScope&) = delete;
    FrameScope& operator=(const FrameScope&) = delete;
};
} // namespace MenuHotpaths
