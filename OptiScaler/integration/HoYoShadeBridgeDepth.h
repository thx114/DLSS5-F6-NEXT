#pragma once
#include "HoYoShadeDepth.h"
#include <with_dx12/SharedInputRead.h>

namespace HoYoShade::Depth {
class BridgeReader final : public Reader {
    interop::SharedInputReadRef input;
public:
    explicit BridgeReader(interop::SharedInputReadRef value) : input(std::move(value)) {}
    bool Validate(const Access& a) const noexcept override {
        return input && input->Current() && a.resource == input->depth.Get() &&
            a.queue == input->queue.Get() && a.readyFence == input->ready.Get() &&
            a.readyValue == input->readyValue && a.owner == input->stream &&
            a.generation == input->generation && a.frame == input->frame && a.epoch == input->cacheFrame;
    }
    bool BindCompletion(ID3D12Fence* fence, uint64_t value) noexcept override {
        if (input && input->BindCompletion(fence, value)) return true;
        Quarantine(); return false;
    }
    void Quarantine() noexcept override { if (input) input->Quarantine(); }
};
inline Input FromBridge(const interop::SharedInputReadRef& input, uint32_t width, uint32_t height,
                        bool reversed) noexcept
{
    try {
        if (!input || !input->Current() || !input->depth) return {};
        const auto desc = input->depth->GetDesc();
        // Shared simultaneous-access input decays to COMMON after the completed
        // DX11 upload batch. No state inference for an arbitrary game texture.
        if (!(desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS)) return {};
        Access a;
        a.resource = input->depth.Get(); a.width = width; a.height = height;
        switch (desc.Format) {
        case DXGI_FORMAT_R32_TYPELESS: a.typedFormat = DXGI_FORMAT_R32_FLOAT; break;
        // R16 typeless may carry FLOAT or UNORM. The NGX parameter has no
        // typed-view contract here, so leave that case without depth.
        case DXGI_FORMAT_R16_TYPELESS: return {};
        default: a.typedFormat = desc.Format; break;
        }
        a.stateKnown = a.directionKnown = true; a.reversed = reversed;
        // NGX DX11 color/depth inputs share texture coordinates. Their common
        // top-left origin is preserved; no camera/FOV/linearization is invented.
        a.yFlipped = false;
        a.queue = input->queue.Get(); a.readyFence = input->ready.Get(); a.readyValue = input->readyValue;
        a.owner = input->stream; a.generation = input->generation; a.frame = input->frame; a.epoch = input->cacheFrame;
        a.reader = std::make_shared<BridgeReader>(input);
        return Compatible(desc, a) && a.reader->Validate(a) ? a : Access{};
    } catch (...) { return {}; }
}
}
