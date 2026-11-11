// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "GenshinNativeQueryProvenance.h"

// Enroll existing system hooks only for the exact Genshin process identities.
// Other games keep the native Detours path and their selected render routes.
namespace GenshinSystemHooks {
inline bool Enabled()noexcept {
    static const bool enabled=[]()noexcept {
        wchar_t path[32768]{};const auto size=GetModuleFileNameW(nullptr,path,32768);
        if(!size || size>=32768)return false;
        const auto* slash=std::wcsrchr(path,L'\\');const auto* name=slash?slash+1:path;
        return !_wcsicmp(name,L"YuanShen.exe") || !_wcsicmp(name,L"GenshinImpact.exe");
    }();return enabled;
}
class Transaction final {
    bool owned_=Enabled(),begun_=false,finished_=false;
    LONG error_=NO_ERROR;
    struct Mirror {PVOID* executed=nullptr;PVOID* alias=nullptr;};
    std::array<Mirror,16> mirrors_{};unsigned mirrorCount_=0;
    bool StageMirror(PVOID* original,PVOID* mirror)noexcept {
        if(!mirror)return true;
        if(!original || mirror==original || mirrorCount_==mirrors_.size() || *mirror!=*original)return false;
        for(unsigned i=0;i<mirrorCount_;++i)
            if(mirrors_[i].alias==mirror || mirrors_[i].executed==mirror || mirrors_[i].alias==original)return false;
        mirrors_[mirrorCount_++]={original,mirror};return true;
    }
public:
    Transaction()noexcept {
        error_=owned_?GenshinNativeQuery::Begin():DetourTransactionBegin();
        begun_=error_==NO_ERROR;
        if(begun_)error_=DetourUpdateThread(GetCurrentThread());
    }
    ~Transaction(){if(begun_ && !finished_){
        if(owned_)GenshinNativeQuery::Abort();else DetourTransactionAbort();
    }}
    Transaction(const Transaction&)=delete;Transaction& operator=(const Transaction&)=delete;
    template<class Function> LONG Attach(PVOID* original,Function hook,PVOID* mirror=nullptr,
        const std::source_location& caller=std::source_location::current())noexcept {
        if(error_!=NO_ERROR)return error_;
        if(!StageMirror(original,mirror))return error_=ERROR_INVALID_PARAMETER;
        const auto target=reinterpret_cast<PVOID>(hook);
        error_=owned_?GenshinNativeQuery::Attach(original,target,original,mirror,nullptr,nullptr,true,nullptr,nullptr,nullptr,caller):
            DetourAttach(original,target);
        return error_;
    }
    template<class Function> LONG AttachInputMethod(PVOID* original,Function hook,IUnknown* object,unsigned index,
        const std::source_location& caller=std::source_location::current())noexcept {
        if(error_!=NO_ERROR)return error_;
        const auto target=reinterpret_cast<PVOID>(hook);
        error_=owned_?GenshinNativeQuery::AttachInputMethod(original,target,object,index,caller):DetourAttach(original,target);
        return error_;
    }
    template<class Function> LONG Detach(PVOID* original,Function hook,PVOID* mirror=nullptr)noexcept {
        if(error_!=NO_ERROR)return error_;
        if(!StageMirror(original,mirror))return error_=ERROR_INVALID_PARAMETER;
        // Retire certificates before Detours can restore code or free a trampoline.
        if(owned_)GenshinNativeQuery::Revoke();
        error_=DetourDetach(original,reinterpret_cast<PVOID>(hook));return error_;
    }
    LONG Commit()noexcept {
        if(!begun_ || finished_)return error_==NO_ERROR?ERROR_INVALID_OPERATION:error_;
        finished_=true;
        if(error_!=NO_ERROR){
            if(owned_)GenshinNativeQuery::Abort();else DetourTransactionAbort();
            return error_;
        }
        error_=DetourTransactionCommit();
        // Publish every executing alias before provenance checks the exact
        // committed slot graph. Abort/failed commit publishes no mirror.
        if(error_==NO_ERROR)for(unsigned i=0;i<mirrorCount_;++i)
            InterlockedExchangePointer(mirrors_[i].alias,*mirrors_[i].executed);
        if(owned_)GenshinNativeQuery::EndTransaction(error_==NO_ERROR);
        return error_;
    }
};
} // namespace GenshinSystemHooks
