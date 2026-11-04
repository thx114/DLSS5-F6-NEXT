// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <d3d11.h>
#include <fsr2/ffx_fsr2.h>
#include <genshin/GenshinSrReceipt.h>
#include "GenshinFsrContract.h"
namespace GenshinFsrInternal {
struct DispatchResult {
    FfxErrorCode status=FFX_ERROR_BACKEND_API_ERROR;
    GenshinFsr::Transition transition=GenshinFsr::Transition::None;
};
}
// C++ internal provider only. Never exported/detoured and never calls original executable FFX entry points.
FfxErrorCode RegisterGenshinFsr2Dx11(FfxFsr2Context* key, const FfxFsr2ContextDescription& desc,
                                  uint64_t instance, uint64_t epoch, bool& cleanupOwned);
GenshinFsrInternal::DispatchResult DispatchGenshinFsr2Dx11(FfxFsr2Context* key, const FfxFsr2DispatchDescription& desc,
                                  const GenshinSrReceipt::Identity& captureIdentity,
                                  std::shared_ptr<const GenshinGuides::Receipt> guides={});
FfxErrorCode DestroyGenshinFsr2Dx11(FfxFsr2Context* key);
namespace GenshinFsrInternal {
// Set only by DestroyGenshinFsr2Dx11 after Dx11Owner::CanDestroy. It is not a
// game-global release shortcut and never applies to public/upstream handles.
inline thread_local bool providerReleaseQuiesced=false;
}
