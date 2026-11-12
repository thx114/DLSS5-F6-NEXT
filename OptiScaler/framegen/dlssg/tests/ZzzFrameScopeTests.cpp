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

void NativeCreationNeedsCurrentGameCommand()
{
    assert(!Scope::InputCommandCurrent(list));
    Scope game(OriginFor(),true);
    assert(Scope::InputCommandCurrent(list));
    assert(!Scope::InputCommandCurrent(0) && !Scope::InputCommandCurrent(list+1));
    assert(!std::async(std::launch::async,[] { return Scope::InputCommandCurrent(list); }).get());
    {
        Scope local(OriginFor(),false);
        assert(!Scope::InputCommandCurrent(list));
    }
    assert(Scope::InputCommandCurrent(list));
    {
        Scope wrongList({api,list+1,feature,9,4646,true,true},true);
        assert(!Scope::InputCommandCurrent(list));
    }
    {
        Scope failed(OriginFor(),true);
        failed.Finish(false);
        assert(!Scope::InputCommandCurrent(list));
    }
    {
        Scope completed(OriginFor(),true);
        completed.Finish(true);
        assert(!Scope::InputCommandCurrent(list));
    }
    gameApi.store(api+1);
    assert(!Scope::InputCommandCurrent(list));
    gameApi.store(api);
    {
        Selection selected(api,feature,0,list,featureOwner,provider);
        assert(Current() && !Scope::InputCommandCurrent(list));
    }
    assert(!Scope::InputCommandCurrent(list)); // A claimed game scope cannot authorize another input.
}

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

// Mirror the production virtual IFeature / DX12 plus backend hierarchy. The
// previous integer-only fixtures missed the real base-subobject adjustment.
struct FeatureBase { virtual ~FeatureBase() = default; uint64_t baseValue=1; };
struct FeatureBackend : virtual FeatureBase { uint64_t backendValue=2; };
struct FeatureDx12 : virtual FeatureBase { uint64_t dx12Value=3; };
struct SelectedFeature final : FeatureBackend, FeatureDx12 {};

void FeatureIdentitySurvivesVirtualBaseAdjustment()
{
    SelectedFeature object, other;
    FeatureDx12* selected=&object;
    FeatureBase* published=selected;
    assert(reinterpret_cast<uintptr_t>(selected)!=reinterpret_cast<uintptr_t>(published));
    const auto selectedId=SelectedFeatureIdentity<FeatureBase>(selected);
    const auto publishedId=SelectedFeatureIdentity<FeatureBase>(published);
    assert(selectedId && selectedId==publishedId);
    assert(SelectedFeatureIdentity<FeatureBase>(static_cast<FeatureDx12*>(nullptr))==0);
    Scope scope(OriginFor(),true);
    Selection entry(api,feature,0,list,selectedId,provider);
    auto receipt=Selection::Current(list,publishedId,0,provider);
    assert(receipt && receipt->origin.frame==4646);
    // Raw alternate-interface addresses and a different object still fail.
    assert(!Selection::Current(list,reinterpret_cast<uintptr_t>(selected),0,provider));
    assert(!Selection::Current(list,SelectedFeatureIdentity<FeatureBase>(&other),0,provider));
    // Publication receives the actual selected caller, not a global that may
    // point to this different feature during a nested backend invocation.
    FeatureBase* changedGlobal=&other;
    assert(SelectedFeatureIdentity<FeatureBase>(changedGlobal)!=publishedId);
    assert(Selection::Current(list,SelectedFeatureIdentity<FeatureBase>(selected),0,provider)==receipt);
    scope.Finish(true);
    assert(receipt->Consumable(true));
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
    NativeCreationNeedsCurrentGameCommand();
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
    for (unsigned failure=0;failure!=3;++failure)
    {
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
        assert(receipt->origin.frame==4646); // GPU owners/fences are checked at actual call sites, not mocked here.
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

void UnattemptedPresentPreservesPassThrough()
{
    // Actual FG Off/paused/warmup/non-FG/TEST admission is wired in FGPresent,
    // checked statically below Windows. This tests its real shared decision,
    // not a loop giving different names to the same false argument.
    unsigned beginCalls=0, offCalls=0, baseCalls=0;
    const auto action=ResolvePresent(false,false,BasePresentSafe(false,false,false),[&] { ++beginCalls; return false; },
        [&] { ++offCalls; return false; });
    assert(action==PresentAction::PassThrough && beginCalls==0 && offCalls==0);
    const int result=RunPresent(action,-1,[&] { ++baseCalls; return -37; });
    assert(result==-37 && baseCalls==1); // Preserve actual base failure too, not a synthetic Success.
}

dlssg::InputOwner OffOwner()
{
    dlssg::InputOwner owner {};
    owner.device=0x600; owner.queue=0x700; owner.epoch=8; owner.width=1280; owner.height=800;
    return owner;
}

void SafeGapCallsActualOffAndBaseThenSourceRecovers()
{
    GapOffProof proof;
    auto owner=OffOwner();
    bool sdkOn=true;
    unsigned offCalls=0, beginCalls=0, baseCalls=0;
    auto off=[&] { ++offCalls; sdkOn=false; return true; }; // External SDK seam, called by production Ensure.
    auto safe=[] { return true; };
    auto ensure=[&] { return proof.Ensure(owner,0,off,safe); };
    assert(!proof.Current(owner,0)); // Neither a new provider nor no-running-options is an Off receipt.
    const auto gap=ResolvePresent(true,false,true,[&] { ++beginCalls; return false; },ensure);
    assert(gap==PresentAction::KnownOffSafeBasePresent && offCalls==1 && beginCalls==0 && !sdkOn);
    assert(RunPresent(gap,-1,[&] { ++baseCalls; assert(!sdkOn); return 29; })==29 && baseCalls==1);
    assert(ensure() && offCalls==1); // Exact owner/viewport proof may be checked again without inventing a result.

    Scope scope(OriginFor(),true);
    Selection selected(api,feature,0,list,featureOwner,provider);
    const auto receipt=Current();
    assert(receipt);
    scope.Finish(true);
    proof.Invalidate(); // Production Dispatch does this BEFORE its actual On/options call.
    assert(!proof.Current(owner,0));
    sdkOn=true;
    bool presented=false;
    auto begin=[&] { ++beginCalls; return receipt->Consumable(true) && ConsumePresentOnce(presented); };
    const auto recovered=ResolvePresent(true,true,true,begin,ensure);
    assert(recovered==PresentAction::Consume && sdkOn && presented && beginCalls==1 && offCalls==1);
    assert(RunPresent(recovered,-1,[&] { ++baseCalls; return 31; })==31);
    const auto duplicate=ResolvePresent(true,true,true,begin,ensure);
    assert(duplicate==PresentAction::KnownOffSafeBasePresent && offCalls==2 && !sdkOn && presented);
    assert(RunPresent(duplicate,-1,[&] { ++baseCalls; return 33; })==33 && baseCalls==3);
}

void OffFailureOrUnknownSubmissionBlocksBase()
{
    GapOffProof proof;
    const auto owner=OffOwner();
    unsigned offCalls=0, baseCalls=0;
    bool sdkAcceptsOff=false, submissionKnown=true;
    auto ensure=[&] {
        return proof.Ensure(owner,0,[&] { ++offCalls; return sdkAcceptsOff; },
            [&] { return submissionKnown; });
    };
    auto action=ResolvePresent(true,false,true,[] { return true; },ensure);
    assert(action==PresentAction::UnsafeFailure && offCalls==1 && !proof.Current(owner,0));
    assert(RunPresent(action,-11,[&] { ++baseCalls; return 0; })==-11 && baseCalls==0);
    sdkAcceptsOff=true;
    submissionKnown=false; // External safety seam; this does NOT pretend to test GPU/fence lifetime.
    action=ResolvePresent(true,false,true,[] { return true; },ensure);
    assert(action==PresentAction::UnsafeFailure && offCalls==1); // Unsafe Execute/Signal state cannot be hidden by Off.
    assert(RunPresent(action,-12,[&] { ++baseCalls; return 0; })==-12 && baseCalls==0);
    submissionKnown=true;
    assert(ensure() && offCalls==2);
    submissionKnown=false;
    assert(!ensure() && !proof.Current(owner,0) && offCalls==2); // Existing Off is no authority over unsafe submissions.
}

void OnAttemptAndReentryRevokeOldOffProof()
{
    GapOffProof proof;
    auto owner=OffOwner();
    unsigned offCalls=0;
    auto success=[&] { ++offCalls; return true; };
    auto safe=[] { return true; };
    assert(proof.Ensure(owner,0,success,safe) && offCalls==1);
    proof.Invalidate();
    auto actualOn=[&] { assert(!proof.Current(owner,0)); return false; };
    assert(!actualOn()); // A failed/partial On attempt still supersedes the old successful Off.
    const auto action=ResolvePresent(true,false,true,[] { return false; },[&] {
        return proof.Ensure(owner,0,[&] { ++offCalls; return false; },safe);
    });
    assert(action==PresentAction::UnsafeFailure && offCalls==2 && !proof.Current(owner,0));
    assert(proof.Ensure(owner,0,success,safe) && offCalls==3); // Only a fresh actual Off may restore proof.
    proof.Invalidate();
    assert(!proof.Ensure(owner,0,[&] { proof.Invalidate(); return true; },safe)); // Recursive On during Off invalidates publication.
    assert(!proof.Current(owner,0));
    assert(proof.Ensure(owner,0,success,safe));
    auto replacement=owner; ++replacement.epoch;
    assert(!proof.Current(replacement,0) && !proof.Current(owner,1));
    assert(proof.Ensure(replacement,1,success,safe)); // New owner/viewport can recover only via real Off.
    assert(proof.Current(replacement,1) && !proof.Current(owner,0));
    proof.Invalidate();
    auto currentOwner=replacement;
    assert(!proof.Ensure(replacement,1,[&] { ++currentOwner.epoch; return true; },
        [&] { return currentOwner==replacement; })); // Owner changed during vendor callback.
}

void InvalidSourceNeedsFreshOffBeforeBasePresent()
{
    enum class Failure { Missing, Stale, Upstream, Selected, Pending };
    for (auto failure : { Failure::Missing,Failure::Stale,Failure::Upstream,Failure::Selected,Failure::Pending })
    {
        Scope scope(OriginFor(),true);
        Selection selected(api,feature,0,list,featureOwner,provider);
        auto receipt=Current();
        assert(receipt);
        if (failure!=Failure::Pending) scope.Finish(failure!=Failure::Upstream);
        const dlssg::NativeSource expected { 1,23,8,4,4646,list,0x400,api,9,0,true,provider };
        auto candidate=expected;
        if (failure==Failure::Missing) candidate={};
        if (failure==Failure::Stale) ++candidate.serial;
        bool presented=false;
        GapOffProof proof;
        const auto owner=OffOwner();
        unsigned offCalls=0, baseCalls=0;
        auto begin=[&] {
            return candidate && candidate==expected && receipt->Consumable(failure!=Failure::Selected) &&
                ConsumePresentOnce(presented);
        };
        const auto action=ResolvePresent(true,true,true,begin,[&] {
            return proof.Ensure(owner,0,[&] { ++offCalls; return true; },[] { return true; });
        });
        assert(action==PresentAction::KnownOffSafeBasePresent && !presented && offCalls==1);
        assert(RunPresent(action,-1,[&] { ++baseCalls; return 41; })==41 && baseCalls==1);
        assert(action!=PresentAction::Consume); // Real hook clears source/End/Sleep before the base call (wiring checked).
    }
}

void FailedOffStaysUnsafeAcrossInactivePresents()
{
    GapOffProof proof;
    const auto owner=OffOwner();
    bool sdkAcceptsOff=false, active=true, teardownPending=false, teardownOffAccepted=false;
    bool ownerUnsafe=false;
    unsigned offCalls=0, beginCalls=0, baseCalls=0;
    auto setOff=[&] { ++offCalls; return sdkAcceptsOff; }; // External SDK result seam only.
    auto baseSafe=[&] { return BasePresentSafe(teardownPending,teardownOffAccepted,ownerUnsafe); };
    auto begin=[&] { ++beginCalls; return false; };
    auto ensure=[&] {
        const bool off=proof.Ensure(owner,0,setOff,[] { return true; });
        if (!off)
        {
            // Mirror only Deactivate's logical state transition and actual Off
            // result, not its GPU resources. Static wiring binds the real fields.
            proof.Invalidate();
            active=false;
            teardownPending=true;
            teardownOffAccepted=setOff();
        }
        return off;
    };
    auto base=[&] { ++baseCalls; return 53; };
    const auto failed=ResolvePresent(active,false,baseSafe(),begin,ensure);
    assert(failed==PresentAction::UnsafeFailure && !active && teardownPending && !teardownOffAccepted);
    assert(offCalls==2 && beginCalls==0 && RunPresent(failed,-19,base)==-19 && baseCalls==0);

    auto nextInactivePresent=[&] {
        return ResolvePresent(active,false,baseSafe(),begin,ensure);
    };
    const auto next=nextInactivePresent(); // Actual next Present: attempted=false after Deactivate.
    assert(next==PresentAction::UnsafeFailure && offCalls==2 && beginCalls==0);
    assert(RunPresent(next,-20,base)==-20 && baseCalls==0);
    teardownOffAccepted=setOff(); // A later existing teardown retry also fails.
    assert(!teardownOffAccepted && nextInactivePresent()==PresentAction::UnsafeFailure && offCalls==3);

    sdkAcceptsOff=true;
    teardownOffAccepted=setOff(); // Only a real accepted Off changes the next decision.
    const auto offSafe=nextInactivePresent();
    assert(teardownPending && offSafe==PresentAction::PassThrough && offCalls==4 && beginCalls==0);
    assert(RunPresent(offSafe,-21,base)==53 && baseCalls==1);
    ownerUnsafe=true; // Actual provider wires failed/unknown submission, owner change and quarantine here.
    const auto quarantined=nextInactivePresent();
    assert(quarantined==PresentAction::UnsafeFailure && RunPresent(quarantined,-22,base)==-22 && baseCalls==1);
    teardownPending=false;
    teardownOffAccepted=false;
    assert(nextInactivePresent()==PresentAction::UnsafeFailure); // Off/completed flags cannot clear quarantine.
    ownerUnsafe=false; // External seam for the already-proven existing safe teardown boundary.
    const auto completed=nextInactivePresent();
    assert(completed==PresentAction::PassThrough && RunPresent(completed,-23,base)==53 && baseCalls==2);
    assert(offCalls==4 && beginCalls==0); // No stale source, no extra SDK Off on unattempted paths.
}

int main()
{
    gameApi.store(api);
    FeatureIdentitySurvivesVirtualBaseAdjustment();
    Actual4646IsNotIndependentOrdinal1();
    NestedScopesShadowAndRestore();
    ExactApiFeatureViewportListAndOwner();
    ZeroAndGameFrameRestart();
    ChainedRealViewportParsing();
    MissingCrossThreadAndUnregisteredOrigin();
    FailedUpstreamOrSelectedRevokesOnlyConsumption();
    VendorCallbacksCannotRelabelSourceOrReviveScope();
    UnattemptedPresentPreservesPassThrough();
    SafeGapCallsActualOffAndBaseThenSourceRecovers();
    OffFailureOrUnknownSubmissionBlocksBase();
    OnAttemptAndReentryRevokeOldOffProof();
    InvalidSourceNeedsFreshOffBeforeBasePresent();
    FailedOffStaysUnsafeAcrossInactivePresents();
    std::puts("PASS 16 ZZZ production-frame-scope suites: native Create origin, virtual-base selected feature identity, original 8 identity/outcome suites, unattempted pass-through, real Off/base/recovery, unsafe submission or Off failure, On/reentrant stale-proof invalidation, invalid-source safe fallback, cross-Present failed-Off persistence/recovery; CPU does not prove GPU lifecycle");
}
