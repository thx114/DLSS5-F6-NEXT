#include "../ZzzFrameScope.h"
#include "../DlssgInputContinuity.h"
#include <sl.h>
#include <cassert>
#include <cstdio>
#include <future>

using namespace dlssg::zzz;
constexpr uintptr_t api=0x100, list=0x200, featureOwner=0x300, provider=0x500;
constexpr uint32_t feature=static_cast<uint32_t>(sl::kFeatureDLSS);
Origin OriginFor(uint32_t frame=4646) { return { api,list,feature,9,frame,true,true }; }
std::shared_ptr<Receipt> Current() { return Selection::Current(list,featureOwner,0,provider); }

// Use the real SDK token interface, with only the external SDK function fake.
// No test implementation of Scope/Selection/token validation exists.
struct Token final : sl::FrameToken
{
    uint32_t value=0;
    operator uint32_t() const override { return value; }
} tokenStorage;
uint32_t requestedFrame=0;
bool aliasToken=false, nullToken=false, failToken=false;
using TokenCallback=void (*)();
TokenCallback callback=nullptr;
sl::Result GetToken(sl::FrameToken*& token, const uint32_t* frame)
{
    requestedFrame=*frame;
    tokenStorage.value=aliasToken?1:*frame;
    token=nullToken?nullptr:&tokenStorage;
    if (callback) callback();
    return failToken?sl::Result::eErrorInvalidState:sl::Result::eOk;
}

void Actual4646IsNotIndependentOrdinal1()
{
    Scope scope(OriginFor(),true);
    std::shared_ptr<Receipt> receipt;
    {
        Selection selected(api,feature,0,list,featureOwner,provider);
        receipt=Current();
        assert(receipt && receipt->origin.frame==4646 && !receipt->Consumable(true));
    }
    const dlssg::NativeSource source { 1,23,1,1,receipt->origin.frame,list,0x400,api,9,0,true,provider };
    assert(source.serial==1 && source.frame==23 && source.sdkFrame==4646);
    assert(source.upstreamViewport==9 && source.providerViewport==0 && source.provider==provider);
    sl::FrameToken* token=nullptr;
    assert(TokenFor(source.sdkFrame,token,&GetToken) && requestedFrame==4646);
    assert(static_cast<uint32_t>(*token)==4646);
    aliasToken=true;
    assert(!TokenFor(source.sdkFrame,token,&GetToken)); // SDK ring returns1 -> reject, never relabel source
    assert(source.sdkFrame==4646 && source.serial==1);
    aliasToken=false;
    scope.Finish(true);
    assert(receipt->Consumable(true) && !receipt->Consumable(false));
    Selection again(api,feature,0,list,featureOwner,provider);
    assert(!Current()); // nested backend cannot mint a second selected authorization
}

void NestedScopesShadowAndRestore()
{
    Scope parent(OriginFor(),true);
    std::shared_ptr<Receipt> parentReceipt;
    {
        Selection outer(api,feature,0,list,featureOwner,provider);
        parentReceipt=Current();
        assert(parentReceipt);
        for (unsigned kind=0;kind!=3;++kind)
        {
            Origin invalid=OriginFor();
            if (kind==0) invalid.frameKnown=false;
            if (kind==1) invalid.feature=static_cast<uint32_t>(sl::kFeatureDLSS_G);
            Scope child(invalid,kind==0);
            assert(!Current()); // never walk past the active invalid/non-game scope
            Selection selected(api,feature,0,list,featureOwner,provider);
            assert(!Current());
        }
        assert(Current()==parentReceipt);
        {
            Origin nestedOrigin=OriginFor(4647);
            nestedOrigin.list=list+1; nestedOrigin.viewport=27;
            Scope nested(nestedOrigin,true);
            assert(!Current()); // outer Selection must not authorize the inner origin
            Selection inner(api,feature,0,list+1,featureOwner+1,provider+1);
            auto receipt=Selection::Current(list+1,featureOwner+1,0,provider+1);
            assert(receipt && receipt!=parentReceipt && receipt->origin.frame==4647);
            assert(receipt->origin.viewport==27); // game namespace27, provider namespace0
            nested.Finish(true);
            assert(receipt->Consumable(true));
        }
        assert(Current()==parentReceipt); // active outer Selection restored
        {
            Selection duplicate(api,feature,0,list,featureOwner,provider);
            assert(!Current()); // same-scope claimed, even after a valid nested scope
        }
        assert(Current()==parentReceipt);
    }
    Selection again(api,feature,0,list,featureOwner,provider);
    assert(!Current()); // outer Scope cannot be claimed again after outer Selection expires
    parent.Finish(true);
}

void ExactApiFeatureViewportListAndOwner()
{
    Scope scope(OriginFor(),true);
    for (unsigned wrong=0;wrong!=5;++wrong)
    {
        Selection entry(wrong==0?api+1:api,wrong==1?feature+1:feature,
                        0,wrong==2?list+1:list,wrong==3?0:featureOwner,wrong==4?0:provider);
        assert(!Current());
    }
    Selection selected(api,feature,0,list,featureOwner,provider);
    assert(Current());
    assert(!Selection::Current(list+1,featureOwner,0,provider));
    assert(!Selection::Current(list,featureOwner+1,0,provider));
    assert(!Selection::Current(list,featureOwner,1,provider));
    assert(!Selection::Current(list,featureOwner,0,provider+1));
    assert(Current()->origin.viewport==9); // real nonzero game viewport bound to local viewport0
    scope.Finish(true);
}

void ZeroAndGameFrameRestart()
{
    std::shared_ptr<Receipt> previous;
    dlssg::NativeSource previousSource {};
    // A game clock may restart or repeat in a new real invocation. Existing
    // source serial/generation and provider presented flag own replay checks.
    for (uint32_t frame : { 0u,4646u,1u,0u,4646u })
    {
        {
            Scope scope(OriginFor(frame),true);
            Selection selected(api,feature,0,list,featureOwner,provider);
            auto receipt=Current();
            assert(receipt && receipt!=previous && receipt->origin.frameKnown && receipt->origin.frame==frame);
            assert(receipt->origin.viewport==9);
            const dlssg::NativeSource source { previousSource.serial+1,23,2,1,frame,list,0x400,
                api,9,0,true,provider };
            assert(source!=previousSource); // repeated raw ID is still a distinct existing source identity
            previousSource=source;
            sl::FrameToken* token=nullptr;
            assert(TokenFor(receipt->origin.frame,token,&GetToken) && requestedFrame==frame);
            scope.Finish(true);
            assert(receipt->Consumable(true));
            previous=receipt;
        }
        assert(!Current()); // expired TLS cannot authorize using the retained outcome
    }
    {
        gameApi.store(api+1); // successfully installed replacement API, no lifetime refusal
        auto origin=OriginFor(0); origin.api=api+1;
        Scope scope(origin,true);
        Selection selected(api+1,feature,0,list,featureOwner,provider);
        assert(Current() && Current()->origin.frame==0);
        scope.Finish(true);
    }
    gameApi.store(api);
    for (unsigned missing=0;missing!=2;++missing)
    {
        auto invalid=OriginFor();
        if (missing==0) invalid.frameKnown=false;
        else invalid.viewportKnown=false;
        Scope scope(invalid,true);
        Selection selected(api,feature,0,list,featureOwner,provider);
        assert(!Current());
    }
}

void ChainedRealViewportParsing()
{
    sl::Constants constants;
    sl::ViewportHandle viewport(19u), other(22u);
    constants.next=&viewport;
    const sl::BaseStructure* inputs[] { &constants };
    auto result=ParseGameViewport(inputs,1);
    assert(result.known && result.value==19);
    viewport.next=&constants;
    assert(!ParseGameViewport(inputs,1).known); // cycle after a valid viewport still invalidates
    viewport.next=nullptr;
    const sl::BaseStructure* ambiguous[] { &constants,&other };
    assert(!ParseGameViewport(ambiguous,2).known);
    constants.next=nullptr;
    assert(!ParseGameViewport(inputs,1).known); // missing is not implicit viewport0
    sl::Constants second;
    second.next=&viewport;
    const sl::BaseStructure* secondRoot[] { &constants,&second };
    assert(ParseGameViewport(secondRoot,2).known && ParseGameViewport(secondRoot,2).value==19);
    assert(!ParseGameViewport(nullptr,0).known);
    const sl::BaseStructure* top[] { &viewport };
    assert(ParseGameViewport(top,1).known);
    sl::ViewportHandle zero(0u);
    constants.next=&zero;
    assert(ParseGameViewport(inputs,1).known && ParseGameViewport(inputs,1).value==0);
    std::array<sl::Constants,129> tooLong;
    for (size_t i=1;i<tooLong.size();++i) tooLong[i-1].next=&tooLong[i];
    tooLong.back().next=&viewport;
    const sl::BaseStructure* bounded[] { &tooLong.front() };
    assert(!ParseGameViewport(bounded,1).known);
}

void MissingCrossThreadAndUnregisteredOrigin()
{
    {
        Selection selected(api,feature,0,list,featureOwner,provider);
        assert(!Current());
    }
    {
        Origin other=OriginFor(); other.api=api+1;
        Scope scope(other,true);
        Selection selected(api+1,feature,0,list,featureOwner,provider);
        assert(!Current());
    }
    Scope scope(OriginFor(),true);
    auto worker=std::async(std::launch::async,[] {
        Selection selected(api,feature,0,list,featureOwner,provider);
        return !Current();
    });
    assert(worker.get());
    Selection synchronous(api,feature,0,list,featureOwner,provider);
    assert(Current()); // async miss cannot consume synchronous authorization
    scope.Finish(true);
}

void FailedUpstreamOrSelectedRevokesOnlyConsumption()
{
    struct Resources { int value=99; };
    for (unsigned failure=0;failure!=3;++failure)
    {
        auto owner=std::make_unique<Resources>();
        auto* retained=owner.get();
        uint64_t realFenceTarget=77;
        std::shared_ptr<Receipt> receipt;
        {
            Scope scope(OriginFor(),true);
            Selection selected(api,feature,0,list,featureOwner,provider);
            receipt=Current();
            assert(receipt && !receipt->Consumable(true)); // no Present while upstream pending
            if (failure!=2) scope.Finish(failure!=0); // failed, success+SR failure, missing completion
        }
        const bool srSucceeded=failure!=1;
        assert(!receipt->Consumable(srSucceeded));
        assert(owner.get()==retained && retained->value==99 && realFenceTarget==77);
        assert(receipt->origin.frame==4646); // revocation does not rewrite frame/owners/fence
    }
    std::shared_ptr<Receipt> receipt;
    {
        Scope scope(OriginFor(),true);
        Selection selected(api,feature,0,list,featureOwner,provider);
        receipt=Current();
        scope.Finish(false);
        scope.Finish(true); // failure cannot be overwritten by a later callback
    }
    assert(!receipt->Consumable(true));
}

std::shared_ptr<Receipt> callbackReceipt;
void NestedCallback()
{
    Scope nested(OriginFor(9999),true);
    Selection selected(api,feature,0,list,featureOwner,provider);
    assert(Current() && Current()!=callbackReceipt && Current()->origin.frame==9999);
    nested.Finish(true);
    assert(callbackReceipt->origin.frame==4646);
}

void VendorCallbacksCannotRelabelSourceOrReviveScope()
{
    dlssg::NativeSource source { 1,23,8,4,4646,list,0x400,api,9,0,true,provider };
    const auto before=source;
    {
        Scope scope(OriginFor(),true);
        Selection selected(api,feature,0,list,featureOwner,provider);
        callbackReceipt=Current();
        callback=&NestedCallback;
        sl::FrameToken* token=nullptr;
        assert(TokenFor(source.sdkFrame,token,&GetToken));
        assert(source==before && requestedFrame==4646 && Current()==callbackReceipt);
        callback=nullptr;
        nullToken=true;
        assert(!TokenFor(source.sdkFrame,token,&GetToken));
        nullToken=false; failToken=true;
        assert(!TokenFor(source.sdkFrame,token,&GetToken));
        failToken=false;
        assert(!TokenFor(source.sdkFrame,token,static_cast<decltype(&GetToken)>(nullptr)));
        scope.Finish(true);
    }
    assert(!Current() && source==before && callbackReceipt->Consumable(true));
    callbackReceipt.reset();
}

int main()
{
    gameApi.store(api);
    Actual4646IsNotIndependentOrdinal1();
    NestedScopesShadowAndRestore();
    ExactApiFeatureViewportListAndOwner();
    ZeroAndGameFrameRestart();
    ChainedRealViewportParsing();
    MissingCrossThreadAndUnregisteredOrigin();
    FailedUpstreamOrSelectedRevokesOnlyConsumption();
    VendorCallbacksCannotRelabelSourceOrReviveScope();
    std::puts("PASS 8 ZZZ production-frame-scope suites: 4646 not 1, nested shadows/restoration, exact joins, nonzero game/local viewport namespaces, zero/restart, real SL chains, cross-thread, failures, immutable callbacks");
}
