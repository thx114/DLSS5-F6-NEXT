// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <d3d11_4.h>
#include <array>
#include <atomic>
#include <cstring>
#include <cstdint>
#include <cstddef>

// CPU PS binding observation through the documented immediate-context API.
// No OS constructor, inline-object layout, signed RVA or GPU capability.
namespace GenshinLegacy::PublicPsBinding {
enum class Method : size_t { SetShader, Clear, Execute, Finish, Swap, Count };
inline constexpr size_t MethodCount=static_cast<size_t>(Method::Count);
inline constexpr size_t Capacity=8; // retained public dispatch variants per method
inline constexpr std::array<unsigned,MethodCount> Slots{9,110,58,114,131};
using Dispatch=std::array<void*,MethodCount>;

// The installer retains Context1 after checking all public Context0..4 aliases.
// Therefore reading its documented slots does not assume an inline native table.
inline Dispatch ReadDispatch(ID3D11DeviceContext* context) noexcept {
    Dispatch result{};
    if(!context)return result;
    const auto* table=*reinterpret_cast<void* const* const*>(context);
    if(table)for(size_t i=0;i<Slots.size();++i)result[i]=table[Slots[i]];
    return result;
}

// Exact bytes of OUR committed Detours edit, relay and trampoline, not a recipe
// for OS code. Kept for process lifetime along with each executing original.
// Only the installer fills these before publishing the immutable Coverage.
struct Endpoint final {
    void* requested=nullptr;
    void* hook=nullptr;
    void** executed=nullptr;
    unsigned char* entry=nullptr;
    unsigned char* trampoline=nullptr;
    unsigned size=0;
    std::array<unsigned char,30> edit{};
    std::array<unsigned char,96> code{};
    std::array<unsigned char,32> ownEntry{};
    struct Hop {
        const unsigned char* entry=nullptr;
        const void* const* indirect=nullptr;
        const void* target=nullptr;
        unsigned size=0;
        std::array<unsigned char,6> code{};
    };
    std::array<Hop,4> hops{};
    unsigned hopCount=0;
    bool SameReceipt(const Endpoint& o)const noexcept {
        if(requested!=o.requested || hook!=o.hook || executed!=o.executed || entry!=o.entry ||
           trampoline!=o.trampoline || size!=o.size || edit!=o.edit || code!=o.code || ownEntry!=o.ownEntry ||
           hopCount!=o.hopCount || hopCount>hops.size())return false;
        for(unsigned i=0;i<hopCount;++i){const auto& a=hops[i];const auto& b=o.hops[i];
            if(a.entry!=b.entry || a.indirect!=b.indirect || a.target!=b.target || a.size!=b.size || a.code!=b.code)return false;}
        return true;
    }
    bool Current()const noexcept {
        // Pinned code modules and never-detached owned trampolines make these
        // ordinary reads. No ReadProcessMemory/VirtualQuery/IO on a draw.
        if(!requested || !executed || !entry || !trampoline || !hook ||
           size<5 || size>edit.size() || hopCount>hops.size() || *executed!=trampoline ||
           std::memcmp(entry,edit.data(),size) ||
           std::memcmp(trampoline,code.data(),code.size()) ||
           std::memcmp(hook,ownEntry.data(),ownEntry.size()))return false;
        for(size_t i=0;i<hopCount;++i){const auto& h=hops[i];
            if(!h.entry || h.size>h.code.size() ||
               std::memcmp(h.entry,h.code.data(),h.size) ||
               (h.indirect && *h.indirect!=h.target))return false;
        }
        return true;
    }
};

// A changed dispatch disables ALL old tickets immediately. Only the existing
// Present owner can install a newly observed public variant and republish.
// Earlier variants remain intercepted for callers caching public method pointers.
// The five documented PS mutation roles invalidate before forwarding. Hooks
// also observe the returned public vector: unknown normalization cannot revive
// a pre-boundary ticket merely by later switching back. Native Draw and scope
// return do not mutate the documented PS state. No private OS ABI is admitted.
// One lock-free scalar: low bit pending, upper bits monotonic generation.
// A successful old publication can never clear a newer request. UINT64_MAX is terminal.
inline std::atomic<uint64_t> refreshState{0};
static_assert(std::atomic<uint64_t>::is_always_lock_free);
inline std::atomic<bool> stopped{false};
inline uint64_t RefreshToken() noexcept {return refreshState.load(std::memory_order_acquire);}
inline uint64_t RequestRefresh() noexcept {
    auto old=RefreshToken();
    for(;;){const auto next=old>=UINT64_MAX-2?UINT64_MAX:((old+2)|uint64_t(1));
        if(refreshState.compare_exchange_weak(old,next,std::memory_order_acq_rel))return next;}
}
inline bool AcknowledgeRefresh(uint64_t token) noexcept {
    if(token==UINT64_MAX)return false;
    return refreshState.compare_exchange_strong(token,token&~uint64_t(1),std::memory_order_acq_rel);
}
inline bool RefreshRequested() noexcept {return (RefreshToken()&1)!=0;}
} // namespace GenshinLegacy::PublicPsBinding
