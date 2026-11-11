#pragma once

#include "GimiDx11Resources.h"
#include <menu/input/input_system.h>

namespace gimi
{
struct NativeOverlayResult
{
    bool success = false, consumed = false;
    const char* reason = "Native overlay unavailable";
};

// Overlay component of ONE native PRE packet. The enclosing executor preflights
// the complete ordered text/overlay tape, then reserves ONE Session reader.
// This component owns its actual backbuffer/private inputs/immutable t120 pages;
// it has no independent submission, reservation or completion. Only the enclosing
// packet's NEW post-final-reader Signal can retire either component.
class NativeOverlayPacket final
{
    struct Storage;
    std::unique_ptr<Storage> state_;
public:
    NativeOverlayPacket(std::uint32_t actions, ID3D11Device*, std::shared_ptr<const Dx11StaticResources>);
    ~NativeOverlayPacket();
    NativeOverlayPacket(const NativeOverlayPacket&) = delete;
    NativeOverlayPacket& operator=(const NativeOverlayPacket&) = delete;
    bool BeginPrepare(ID3D11DeviceContext*, IDXGISwapChain*, HWND, std::uint64_t nativeEpoch,
                      const BatchView&, const char*& reason) noexcept;
    // Called in tape order only for output/binding/overlay actions. Text scopes
    // restore their exact entry state, so they leave this OM/PS/viewport cursor
    // unchanged. The enclosing preflight proves their resources and CS->GS pair.
    bool PrepareAction(const Action&, std::uint32_t immutableParameters, Dx11ResourceProvider*,
                       UINT& step, const char*& reason) noexcept;
    bool PrepareMouse(const Action&, const float* completeImage, const OptiInput::GameCursorSnapshot&,
                      Dx11ResourceProvider*, UINT& step, const char*& reason) noexcept;
    bool EndPrepare(ID3D11DeviceContext*, const char*& reason) noexcept;
    bool BeginSubmit(ID3D11DeviceContext*, std::uint64_t nativeEpoch) noexcept; // getters only
    NativeOverlayResult Execute(ID3D11DeviceContext*, UINT step) noexcept;
    void Restore(ID3D11DeviceContext*) const noexcept;
    std::uint64_t Epoch() const noexcept;
    ID3D11Texture2D* Backbuffer() const noexcept; // borrowed from this retained component only
    void Retire() noexcept; // cancellation before any GPU effect or actual completion only
};
} // namespace gimi
