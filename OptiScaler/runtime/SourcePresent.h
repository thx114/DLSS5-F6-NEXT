#pragma once

struct IDXGISwapChain;

// A source wrapper and its nested FG proxy describe the same source picture.
// The SDK's physical output runs without this authority, including on workers.
namespace SourcePresent
{
class Scope final
{
    inline static thread_local bool active_ = false;
    bool owns_;
public:
    explicit Scope(bool source) noexcept : owns_(source && !active_)
    {
        if (owns_) active_ = true;
    }
    ~Scope() { if (owns_) active_ = false; }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
    bool Owns() const noexcept { return owns_; }
};

// For a native DX12 FG proxy with no outer source wrapper. Uses the same
// clocks/feature tick/epoch as LocalPresent; callers must own a source Scope.
void ObserveProxySource(IDXGISwapChain*, unsigned syncInterval, unsigned flags);
}
