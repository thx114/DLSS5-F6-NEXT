// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi.h>
#include <array>
#include <cstdint>
#include <memory>
#include <string>

namespace DrawUiMask {
// Rules use the existing F5 FNV1a bytecode fingerprint, NOT 3DMigoto hashes.
// Semicolon separated PS:PS-byte-length:VS:VS-byte-length:element-count.
// All fields are mandatory, hashes hexadecimal, lengths/count decimal.
struct Settings {bool enabled=false,fg=false,nr=false,filters=false;std::string rules;};
enum class Consumer : unsigned { Nr, Filter, Fg };
class Frame;
using FramePtr=std::shared_ptr<Frame>;
void Configure(const Settings&) noexcept;
bool Enabled() noexcept;
// Called at the existing DX11 Present boundary BEFORE native upload/copy.
void Present(IDXGISwapChain*,ID3D11DeviceContext*) noexcept;
void Revoke() noexcept;
// Only an exact same-context, same-buffer, sealed current frame is returned.
// Import is ordered by the caller's EXISTING later DX11 upload Signal/Flush.
// This API never claims readiness from a prior SR fence and submits no work.
FramePtr Acquire(ID3D11DeviceContext*,ID3D11Texture2D*,ID3D12Device*) noexcept;
bool Selected(const FramePtr&,Consumer) noexcept;
// All preparation must succeed BEFORE recording Begin. No submit/wait/Flush.
// The caller retains Frame on its actual command-list completion/quarantine.
bool Begin(const FramePtr&,Consumer,ID3D12GraphicsCommandList*,ID3D12Resource*,D3D12_RESOURCE_STATES) noexcept;
bool End(const FramePtr&,Consumer,ID3D12GraphicsCommandList*,ID3D12Resource*,D3D12_RESOURCE_STATES) noexcept;
// The existing Nr Begin/End pair brackets the real NativeProducer callback.
// With the Filter subscription it also lends the SAME held frame to layer Look,
// independently of global NR protection. No acquisition, submit or new owner.
bool LayerLookAvailable(ID3D12GraphicsCommandList*,ID3D12Resource*,UINT width,UINT height) noexcept;
class LayerLookRead final {
    FramePtr _frame;
    ID3D12GraphicsCommandList* _list=nullptr;
public:
    LayerLookRead(ID3D12GraphicsCommandList*,ID3D12Resource*,UINT width,UINT height) noexcept;
    ~LayerLookRead();
    LayerLookRead(const LayerLookRead&)=delete;
    LayerLookRead& operator=(const LayerLookRead&)=delete;
    ID3D12Resource* Resource() const noexcept;
};
// Final processed scene stays unchanged outside the mask. Inside it, use the
// actual same-frame pre-UI scene. The game's PresentColor is never overwritten.
ID3D12Resource* BuildHudless(const FramePtr&,ID3D12GraphicsCommandList*,ID3D12Resource*,D3D12_RESOURCE_STATES) noexcept;
ID3D12Resource* Hudless(const FramePtr&) noexcept;
// Observations only: never used to admit work, release resources or query a fence.
// Pending means commands were recorded; Submitted requires the actual owner's
// successful submission tail. Completed is emitted only by its existing retire path.
enum class Lifecycle : unsigned { Unknown, Pending, Submitted, Completed, Quarantined };
struct Receipt {
    Lifecycle state=Lifecycle::Unknown;
    uint64_t capture=0,frame=0,epoch=0,configuration=0;
    uintptr_t context=0,owner=0;
    uint64_t generation=0,submission=0;
};
struct ConsumerStatus {Receipt latest,completed,quarantined;};
struct LifecycleStatus {
    bool readable=false,enabled=false;
    uint64_t configuration=0;
    std::array<ConsumerStatus,3> consumers{};
};
// The same frame + consumer + submission owner tuple must accompany every event.
// Unknown is explicit when recording/tagging lacks a reliable completion receipt.
// Owner/generation/submission identify the real consumer batch (FG uses its
// existing frame receipt, NR/Filter use the genuine signalled tail). Zero tail
// is allowed for quarantine; an abandoned recording may report Unknown with
// zero owner/generation/submission. Neither case can report Completed.
void Observe(const FramePtr&,Consumer,Lifecycle,uintptr_t owner,uint64_t generation,uint64_t submission) noexcept;
LifecycleStatus ReadLifecycle() noexcept;
const char* Status() noexcept;
} // namespace DrawUiMask
