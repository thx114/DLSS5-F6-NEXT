// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "TaaSelection.h"
#include <d3d11.h>
#include <atomic>
#include <filesystem>
#include <memory>
#include <vector>

namespace taa {
struct Invocation;
struct Status {
    Mode requested=Mode::Off;
    std::string profilePath, profileError, detail, evidenceDirectory;
    uint64_t matches=0, evaluations=0, outputs=0;
    bool ownerReady=false;
    CandidateKey selected{},active{};
    std::vector<Candidate> candidates;
};
// Configuration/reload boundaries publish one immutable request. Hooks never
// race a mutable Config string or perform profile I/O in a draw callback.
void Configure(Mode mode, const std::string& path, const std::filesystem::path& base = {}, std::string_view candidate = {});
void RememberShader(ID3D11DeviceChild* shader,const void* bytes,size_t size) noexcept;
void DepthCleared(ID3D11DeviceContext* context,ID3D11DepthStencilView* view,UINT flags,float depth) noexcept;
bool NeedsHooks() noexcept;
namespace detail { inline std::atomic_bool inputOwned{false}; }
inline bool OwnsInput() noexcept {return detail::inputOwned.load(std::memory_order_acquire);}
Status ReadStatus();
void OnPresent(ID3D11DeviceContext* context, bool writesCovered) noexcept;
class Scope final {
    struct Transaction;
    std::unique_ptr<Transaction> transaction_;
    bool outer_=false, entered_=false;
public:
    Scope(ID3D11DeviceContext* context, const Invocation& call, bool eligible) noexcept;
    ~Scope();
    Scope(const Scope&)=delete;
    Scope& operator=(const Scope&)=delete;
    // Called only after the original command returned. A GIMI-consumed or
    // abandoned command gets no evaluation/copyback; its input copies retire.
    void Returned() noexcept;
    bool Capturing() const noexcept {return bool(transaction_);}
};
// A physical-command scope, created only after optional command consumers have
// declined the draw. Diagnostic observers never gain SR mutation authority.
struct ConsumerCommand {bool indexed=false;unsigned count=0,first=0,instances=1,firstInstance=0;int base=0;};
class ConsumerScope final {
    struct Transaction;std::unique_ptr<Transaction> transaction_;
public:
    ConsumerScope(ID3D11DeviceContext*,const ConsumerCommand&,bool eligible) noexcept;
    ~ConsumerScope();
    void Returned() noexcept;
};
void ConsumerWrite(ID3D11DeviceContext*,ID3D11Resource*,std::string_view reason="untracked resource write") noexcept;
void ConsumerDispatch(ID3D11DeviceContext*) noexcept;
void RevokeOwner() noexcept;
void ConsumerCopy(ID3D11DeviceContext*,ID3D11Resource* destination,ID3D11Resource* source,bool full) noexcept;
} // namespace taa
