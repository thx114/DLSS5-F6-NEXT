// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "GenshinRuntimeCapture.h"
#include <memory>

namespace GenshinLegacy::RuntimeCapture {
// Observes actual API bindings and optional raw payloads. No observation grants
// producer authentication, route admission, resource semantics or dispatch.
void RawShaderCreated(ID3D11DeviceChild*,const void*,size_t,uint64_t,unsigned)noexcept;
class InputObserver final {
    struct Storage;
    std::unique_ptr<Storage> storage_;
public:
    InputObserver();
    ~InputObserver();
    InputObserver(const InputObserver&)=delete;
    InputObserver& operator=(const InputObserver&)=delete;
    void Revoke()noexcept;
    void CheckGeneration(uint64_t(*)(uint64_t))noexcept;
    void Queue(ID3D11Device*,ID3D11DeviceContext*,const DrawFact&,
               const std::array<ID3D11ShaderResourceView*,3>&)noexcept;
    bool Poll(ID3D11Device*,ID3D11DeviceContext*)noexcept;
    bool Idle()const noexcept;
};
}
