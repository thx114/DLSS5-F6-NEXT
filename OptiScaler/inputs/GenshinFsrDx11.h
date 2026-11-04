// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "GenshinFsrContract.h"
#include <genshin/GenshinTypedGuides.h>
#include <memory>

namespace GenshinFsr
{
struct CapturedFrame
{
    Parameters parameters;
    ID3D11Texture2D* color = nullptr;
    ID3D11Texture2D* depth = nullptr;
    // Prepared signed-square decoded UV motion, R16G16_FLOAT; raw R10 input is rejected.
    ID3D11Texture2D* motion = nullptr;
    ID3D11Texture2D* output = nullptr;
    ID3D11Texture2D* exposure = nullptr;
    ID3D11Texture2D* reactive = nullptr;
    ID3D11Texture2D* transparency = nullptr;
    // Capture owner must establish that native replay is safe after a partial backend write.
    bool replaySafe = false;
    std::shared_ptr<const GenshinGuides::Receipt> guides;
};
struct RouteStatus
{
    Reason reason = Reason::CaptureUnproven;
    bool captureProven = false, providerRegistered = false, srAccepted = false;
    // NR/FG need separately proven downstream capability and resource contracts.
    bool nr = false, fg = false, pending = false;
};
class Dx11Owner
{
    struct Storage;
    std::unique_ptr<Storage> storage_;
public:
    Dx11Owner(ID3D11DeviceContext* context, Discovery evidence);
    ~Dx11Owner();
    Dx11Owner(const Dx11Owner&) = delete;
    Dx11Owner& operator=(const Dx11Owner&) = delete;
    Result Submit(const CapturedFrame& frame);
    Reason Poll();
    // Resize/scene recreation requires invalidation, completion, then a new strictly newer epoch owner.
    Reason Invalidate();
    // Revoke the immutable consumer receipt only; do not query or release GPU/provider state.
    void RevokeGuides() noexcept;
    RouteStatus Status() const noexcept;
    static void CollectRetired();
};
}
