// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <runtime/GuideCapture.h>
#include <wrl/client.h>

namespace MenuPreview {
// Display-only storage on the menu's exact ordered output queue. It contains
// the preview before ImGui, never the game/NR/FG input or any UI pixels.
// The owner must retain this object until its actual output fence retires;
// invalidation below changes eligibility, never resource lifetime.
class OutputCache final
{
    Microsoft::WRL::ComPtr<ID3D12Resource> Texture;
    GuideCapture::PreviewDemand Demand {};
    GuideCapture::PreviewSnapshot Displayed {}; // metadata only, no producer lease
    GuideCapture::PreviewSnapshot LastSubmitted {}; // identity only, never GPU completion proof
    bool Valid = false;
    bool Enabled = false;

    static bool SameView(const GuideCapture::PreviewDemand& a, const GuideCapture::PreviewDemand& b) noexcept
    {
        // Freezing stops acquisition, but keeps the selected immutable image.
        return a.mode == b.mode && a.raw == b.raw && a.flipY == b.flipY && a.gain == b.gain;
    }
    static void Transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource,
                           D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
    {
        D3D12_RESOURCE_BARRIER b {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = resource;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = before;
        b.Transition.StateAfter = after;
        list->ResourceBarrier(1, &b);
    }

  public:
    void Select(const GuideCapture::PreviewDemand& demand, bool sealedOutput) noexcept
    {
        Enabled = sealedOutput && demand.mode != GuideCapture::PreviewMode::Off;
        if (!Enabled || !SameView(Demand, demand))
            Valid = false;
        Demand = demand;
    }
    bool HasImage() const noexcept { return Valid; }
    bool NeedsRefresh(const GuideCapture::PreviewSnapshot& snapshot) noexcept
    {
        if (Valid && (snapshot.revision != Displayed.revision || snapshot.owner != Displayed.owner ||
            snapshot.epoch != Displayed.epoch || snapshot.feature != Displayed.feature ||
            snapshot.configuration != Displayed.configuration))
            Valid = false; // Never cross a known input domain or demand revision.
        return !Valid || snapshot.serial != Displayed.serial;
    }
    // A failed refresh leaves the previous image eligible only in the SAME
    // view/domain. Transient try_lock/reader pressure must not expose game frames.
    // target enters/leaves RENDER_TARGET; Texture enters/leaves COPY_SOURCE.
    bool Record(ID3D12Device* device, ID3D12GraphicsCommandList* list, ID3D12Resource* target,
                const GuideCapture::PreviewSnapshot* fresh, bool& readerRecorded)
    {
        readerRecorded = false;
        if (!Enabled || !device || !list || !target) return false;
        const auto desc = target->GetDesc();
        if (Texture)
        {
            const auto stored = Texture->GetDesc();
            if (stored.Width != desc.Width || stored.Height != desc.Height || stored.Format != desc.Format ||
                stored.Dimension != desc.Dimension || stored.DepthOrArraySize != desc.DepthOrArraySize ||
                stored.MipLevels != desc.MipLevels || stored.SampleDesc.Count != desc.SampleDesc.Count ||
                stored.SampleDesc.Quality != desc.SampleDesc.Quality)
            {
                Valid = false;
                return false;
            }
        }
        if (fresh && NeedsRefresh(*fresh))
        {
            if (!Texture)
            {
                auto copy = desc;
                copy.Flags = D3D12_RESOURCE_FLAG_NONE;
                D3D12_HEAP_PROPERTIES heap {};
                heap.Type = D3D12_HEAP_TYPE_DEFAULT;
                if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &copy,
                    D3D12_RESOURCE_STATE_COPY_SOURCE, nullptr, IID_PPV_ARGS(&Texture)))) return false;
            }
            // Resize/format changes require the owner's fence-based generation
            // cleanup. Never replace storage still referenced by recordings.
            readerRecorded = GuideCapture::DrawPreview(*fresh, list, desc.Format,
                static_cast<unsigned>(desc.Width), desc.Height);
            if (readerRecorded)
            {
                Transition(list, target, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
                Transition(list, Texture.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
                list->CopyResource(Texture.Get(), target);
                Transition(list, Texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
                Transition(list, target, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
                Displayed = *fresh;
                Displayed.lease.reset();
                Displayed.texture = nullptr;
                Valid = true;
                return true;
            }
        }
        if (!Valid) return false;
        Transition(list, target, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_DEST);
        list->CopyResource(target, Texture.Get());
        Transition(list, target, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET);
        return true;
    }
    void NoteSubmitted() noexcept
    {
        if (Valid)
        {
            GuideCapture::NoteDisplayed(Displayed);
            LastSubmitted = Displayed;
        }
    }
    void DiscardRecording() noexcept { Valid = false; }
    // ONLY after the owner's actual queue/fence has completed all recordings.
    void Retire() noexcept
    {
        // A discarded refresh may differ from the last submitted display. This
        // conditional metadata clear neither proves completion nor retires a use.
        GuideCapture::ClearDisplayed(LastSubmitted);
        Valid = false;
        Displayed = {};
        LastSubmitted = {};
        Texture.Reset();
    }
};
} // namespace MenuPreview
