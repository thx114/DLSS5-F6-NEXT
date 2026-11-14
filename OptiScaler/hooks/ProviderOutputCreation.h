// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "SwapchainCreateRule.h"
#include <framegen/FinalOutputEndpoint.h>
#include <runtime/NativeObjectContracts.h>
#include <wrl/client.h>

namespace ProviderOutputCreation
{
// Routed factory policy runs once at the outer game call. Its nested SDK
// factories still return physical outputs that need their own Present owner.
// Observe that return without repeating source/FG creation or unwrapping a
// third-party proxy. The wrapper takes the returned reference, just as on the
// ordinary factory path; no extra reference is created or discarded here.
template<class Wrapper, class Interface, class Authenticate>
void Observe(HRESULT result, Interface** output, IUnknown* queue, HWND window,
             UINT flags, bool uwp, bool composition, Authenticate authenticate)
{
    if (!framegen::final_output::CreationScope::Active() || !SwapchainCreate::Created(result, output)) return;
    Microsoft::WRL::ComPtr<Wrapper> existing;
    if ((*output)->QueryInterface(IID_PPV_ARGS(&existing)) != E_NOINTERFACE || existing) return;
    for (const auto& contract : { NativeObjectContracts::ReShade, NativeObjectContracts::Streamline })
    {
        Microsoft::WRL::ComPtr<IUnknown> proxy;
        if ((*output)->QueryInterface(contract, reinterpret_cast<void**>(proxy.GetAddressOf())) != E_NOINTERFACE || proxy)
            return; // In particular, the SDK input proxy is never a physical output.
    }
    if (!authenticate(*output, queue)) return;
    *output = new Wrapper(*output, queue, window, flags, uwp, composition, flags);
}
}
