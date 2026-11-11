// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "GenshinOutputContract.h"
#include <cstdint>

namespace GenshinCaller {
// Passive evidence only. Neither a record nor a passing comparison grants admission.
struct FgBindingRecord {
    GenshinGuides::Owner owner{};
    uint64_t provider=0, providerFrame=0, queue=0, commandList=0, device=0;
    uint64_t leasedInput=0, acceptedInput=0, acceptedDestination=0;
    uint32_t providerIndex=0;
};
enum class FgObservationReason : unsigned char { None, MissingEndpoint, ResourceChanged, OwnerChanged };
inline FgObservationReason CompareFgBinding(const FgBindingRecord& r,
    const GenshinGuides::Owner& owner,uint64_t expectedInput) noexcept {
    if(!GenshinGuides::SameOwner(r.owner,owner) || !owner.device || !owner.context ||
       !owner.deviceEpoch || !owner.producerEpoch || !owner.configEpoch || !owner.instance ||
       !owner.generation || !owner.historyEpoch || !owner.thread)return FgObservationReason::OwnerChanged;
    if(!r.provider || !r.queue || !r.commandList || !r.device || !r.acceptedDestination)
        return FgObservationReason::MissingEndpoint;
    if(!expectedInput || r.leasedInput!=expectedInput || r.acceptedInput!=expectedInput)
        return FgObservationReason::ResourceChanged;
    return FgObservationReason::None;
}
class FgObservationBudget final {
    GenshinGuides::Owner key_{};
    uint64_t calls_=0;
    unsigned samples_=0;
public:
    bool Take(GenshinGuides::Owner owner) noexcept {
        // This is a sampling key, not a cross-frame receipt. Every emitted record
        // still carries its original sourceFrame and is never reused for admission.
        owner.sourceFrame=0;
        if(!GenshinGuides::SameOwner(key_,owner)){key_=owner;calls_=0;samples_=0;}
        const auto call=calls_++;
        if(samples_>=16 || call%256!=0)return false;
        ++samples_;return true;
    }
};
}
