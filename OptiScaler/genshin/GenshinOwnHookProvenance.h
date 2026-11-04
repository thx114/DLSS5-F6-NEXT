// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "GenshinNativeQueryProvenance.h"

namespace GenshinNativeQuery::Own {
struct Slot final {
    PVOID transaction=nullptr,entry=nullptr,hook=nullptr;
    PVOID* executed=nullptr;PVOID* mirror=nullptr;
    bool live=false,detachReady=false;
    detail::Patch detachReceipt{};
    constexpr Slot()noexcept=default;Slot(const Slot&)=delete;Slot& operator=(const Slot&)=delete;
};
template<class Function> inline Function& Original(Slot& slot)noexcept {
    static_assert(sizeof(Function)==sizeof(PVOID));
    return reinterpret_cast<Function&>(slot.transaction);
}
class Batch final {
    struct Item {Slot* slot=nullptr;PVOID prior=nullptr;};
    std::array<Item,16> items_{};unsigned count_=0;
    detail::FactoryProtectionLease protection_;
    const detail::FactoryPredecessor* predecessors_=nullptr;unsigned predecessorCount_=0;
    LONG error_=NO_ERROR;bool begun_=false,finished_=false;
    bool ReleaseProtection(bool committed)noexcept {
        bool good=detail::FactoryLeaseAccess::Restore(protection_);
        for(unsigned i=0;predecessors_ && i<(std::min)(predecessorCount_,3u);++i){const auto& r=predecessors_[i];if(!r.captured)continue;
            unsigned ownSize=0;
            if(committed)for(unsigned j=0;j<count_;++j){const auto& p=items_[j].slot->detachReceipt;
                if(p.predecessor.captured && p.predecessor.nodes[0].address==r.nodes[0].address)ownSize=p.size;
            }
            if(!detail::FactoryPredecessorIntact(r,ownSize))good=false;
        }
        if(!good)GenshinNativeQuery::Revoke();return good;
    }
    void AbortBatch()noexcept {
        const auto result=DetourTransactionAbort();
        if(result!=NO_ERROR)GenshinNativeQuery::Revoke();
        ReleaseProtection(false);GenshinNativeQuery::EndTransaction(false);Restore();
    }
    void Restore()noexcept {
        for(unsigned i=0;i<count_;++i){auto& s=*items_[i].slot;
            InterlockedExchangePointer(s.executed,items_[i].prior);
            s.entry=nullptr;s.hook=nullptr;s.executed=nullptr;s.mirror=nullptr;
        }
    }
public:
    explicit Batch(const detail::FactoryPredecessor* predecessors=nullptr,unsigned predecessorCount=0,
                  InstallRequirement requirement=InstallRequirement::Mandatory)noexcept
        :predecessors_(predecessors),predecessorCount_(predecessorCount) {
        error_=GenshinNativeQuery::Begin(requirement);begun_=error_==NO_ERROR;
        if(begun_)error_=DetourUpdateThread(GetCurrentThread());
        if(error_==NO_ERROR && !detail::FactoryLeaseAccess::Prepare(protection_,this,predecessors,predecessorCount)){
            GenshinNativeQuery::Revoke();error_=ERROR_INVALID_DATA;
        }
    }
    ~Batch(){if(begun_ && !finished_)AbortBatch();}
    Batch(const Batch&)=delete;Batch& operator=(const Batch&)=delete;
    LONG Add(Slot& slot,PVOID* executed,PVOID* mirror,PVOID entry,PVOID hook,
             HMODULE module=nullptr,const char* symbol=nullptr,bool enroll=true,
             const std::source_location& caller=std::source_location::current(),
             const detail::FactoryPredecessor* predecessor=nullptr)noexcept {
        if(error_!=NO_ERROR)return error_;
        if(!executed || !entry || !hook || count_==items_.size())return error_=ERROR_INVALID_PARAMETER;
        if(predecessor && predecessor->captured){bool prepared=false;
            for(unsigned i=0;predecessors_ && i<predecessorCount_;++i)
                if(predecessor==&predecessors_[i])prepared=true;
            if(!prepared){GenshinNativeQuery::Revoke();return error_=ERROR_INVALID_DATA;}
        }
        if(slot.live){
            if(slot.executed!=executed || slot.mirror!=mirror || slot.entry!=entry || slot.hook!=hook ||
               (mirror && *mirror!=*executed)){
                GenshinNativeQuery::Revoke();return error_=ERROR_INVALID_OPERATION;
            }return NO_ERROR;
        }
        for(unsigned i=0;i<count_;++i)if(items_[i].slot==&slot)return error_=ERROR_INVALID_PARAMETER;
        items_[count_++]={&slot,*executed};
        slot.entry=entry;slot.hook=hook;slot.executed=executed;slot.mirror=mirror;
        // Stage only an unhooked entry. No valid trampoline/certificate is
        // published until Detours commits. Abort restores the previous slot.
        *executed=entry;
        PDETOUR_TRAMPOLINE trampoline=nullptr;PVOID target=nullptr,detour=nullptr;
        error_=enroll?GenshinNativeQuery::Attach(executed,hook,executed,mirror,module,symbol,true,
            &target,&trampoline,&detour,caller,nullptr,nullptr,predecessor):DetourAttachEx(executed,hook,&trampoline,&target,&detour);
        auto& p=slot.detachReceipt;p={};p.original=executed;p.executed=executed;p.mirror=mirror;
        if(predecessor && predecessor->captured)p.predecessor=*predecessor;
        p.entry=static_cast<unsigned char*>(target);p.trampoline=reinterpret_cast<unsigned char*>(trampoline);p.hook=hook;
        slot.detachReady=error_==NO_ERROR && detour==hook && target && trampoline &&
            detail::Memory(trampoline,p.code.data(),p.code.size()) &&
            detail::Memory(hook,p.ownEntry.data(),p.ownEntry.size());
        if(slot.detachReady){p.size=p.code[62];
            slot.detachReady=p.size>=5 && p.size<=24 && detail::Equal(target,p.code.data()+32,p.size);
        }
        return error_;
    }
    LONG Commit()noexcept {
        if(!begun_)return error_;
        finished_=true;
        if(error_!=NO_ERROR){AbortBatch();return error_;}
        // Last pre-write check belongs to this exact transaction, not a later
        // setup sample. Never commit over a changed predecessor or own preimage.
        for(unsigned i=0;i<count_;++i){const auto& p=items_[i].slot->detachReceipt;
            if(p.predecessor.captured && (!detail::FactoryPredecessorIntact(p.predecessor,0,&protection_) ||
               !detail::Equal(p.entry,p.code.data()+32,p.size))){
                GenshinNativeQuery::Revoke();AbortBatch();
                return error_=ERROR_INVALID_DATA;
            }
        }
        error_=DetourTransactionCommit();
        // Restore even on a failed commit: Detours' saved protection may be
        // our temporary RWX. Native publication below always sees frozen RX.
        const bool restored=ReleaseProtection(error_==NO_ERROR);
        if(error_==NO_ERROR){
            // Detours has already published each CANONICAL handler original.
            // Proxy forwarding mirrors publish only on native commit success.
            // Optional provenance failure must not strand an installed hook.
            for(unsigned i=0;i<count_;++i){auto& s=*items_[i].slot;
                if(s.mirror && s.mirror!=s.executed)InterlockedExchangePointer(s.mirror,*s.executed);
                s.live=true;s.detachReady=s.detachReady && detail::FinalizeOwnPatch(s.detachReceipt);
            }
        }
        // This verifies the exact postcommit edit/slot graph before the ledger
        // becomes even/usable. Revoked provenance never publishes authority.
        GenshinNativeQuery::EndTransaction(error_==NO_ERROR);
        if(error_!=NO_ERROR)Restore();
        if(error_==NO_ERROR && !restored)error_=ERROR_INVALID_DATA;
        return error_;
    }
};
inline LONG Install(Slot& slot,PVOID* mirror,PVOID entry,PVOID hook,
                    HMODULE module=nullptr,const char* symbol=nullptr,bool enroll=true,
                    const std::source_location& caller=std::source_location::current())noexcept {
    Batch batch;batch.Add(slot,&slot.transaction,mirror,entry,hook,module,symbol,enroll,caller);return batch.Commit();
}
inline LONG Detach(Slot& slot)noexcept {
    if(!slot.live)return NO_ERROR;
    // Validate the exact owned entry/relay/trampoline first. Do not restore over
    // a foreign patch installed after us, even when optional trust is revoked.
    if(!slot.detachReady || !detail::PatchIntact(slot.detachReceipt)){
        GenshinNativeQuery::Revoke();return ERROR_INVALID_OPERATION;
    }
    GenshinNativeQuery::Revoke(); // Retire all certificates before trampoline free.
    auto result=GenshinNativeQuery::Begin();if(result!=NO_ERROR)return result;
    result=DetourUpdateThread(GetCurrentThread());
    if(result==NO_ERROR)result=DetourDetach(slot.executed,slot.hook);
    if(result!=NO_ERROR){GenshinNativeQuery::Abort();return result;}
    result=DetourTransactionCommit();
    if(result==NO_ERROR){
        if(slot.mirror && slot.mirror!=slot.executed)InterlockedExchangePointer(slot.mirror,*slot.executed);
        slot.live=false;
    }
    GenshinNativeQuery::EndTransaction(result==NO_ERROR);return result;
}
} // namespace GenshinNativeQuery::Own
