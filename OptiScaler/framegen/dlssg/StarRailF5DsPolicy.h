#pragma once
#include <Config.h>
#include <State.h>
#include <Util.h>
#include <native/OpticalFgNrPolicy.h>
#include <upscalers/FeatureProvider_Dx11.h>

// R957: behavior taken from the physical F5-DS ffa41cfe snapshot, not AdaMFG.
// This is policy inside the existing provider/interop, never a second pipeline.
namespace StarRailF5Ds
{
inline bool Executable()
{
    static const bool result = _wcsicmp(Util::ExePath().filename().c_str(), L"StarRail.exe") == 0;
    return result;
}
inline bool Selected(ID3D11Device* sourceDevice=nullptr)
{
    if (!Executable()) return false;
    const auto& state = State::Instance();
    return FeatureProvider_Dx11::ActiveBackend(sourceDevice) == Upscaler::DLSS_on12 &&
        state.swapchainInteropApi == SwapchainInteropApi::Dx11wDx12 &&
        state.activeFgInput == FGInput::Upscaler && state.activeFgOutput == FGOutput::DLSSG &&
        !state.externalFrameGeneration && !state.gameCalledSlInit && !native::OpticalFgNrEnabled();
}
}
