// Portable CPU tests for the product's saved SDK input bracket and exact output
// binding/admission. No Windows/SDK/GPU, simulated completion or display proof.
// Build separately with C++20; do not add this fixture to the product project.
#include "../../../inputs/FG/GenshinFgTicket.h"
#include <cstdio>
#include <cstdlib>
#include <type_traits>

// Separate executable: only this provider-shaped fixture can mint the private
// input receipt. The production provider seals it after real SetConstants.
namespace GenshinTransport { class PreparedDepthPacket {}; }
class DLSSG_Dx12 {
public:
    static std::shared_ptr<const GenshinBridgeFg::SdkInputSource> Seal(
        const GenshinBridgeFg::Key& key,uint64_t frame,uint64_t token,
        uint64_t outputOwner=22,uint64_t epoch=1,bool packet=true) {
        std::shared_ptr<const GenshinTransport::PreparedDepthPacket> root;
        if(packet)root=std::make_shared<GenshinTransport::PreparedDepthPacket>();
        return std::shared_ptr<const GenshinBridgeFg::SdkInputSource>(
            new GenshinBridgeFg::SdkInputSource(key,frame,token,11,epoch,outputOwner,std::move(root)));
    }
};
namespace {
using namespace GenshinBridgeFg;
unsigned checks=0;
void Check(bool ok,const char* contract) {
    ++checks;
    if(!ok){std::fprintf(stderr,"FAILED: %s\n",contract);std::abort();}
}
Key Source() {
    Key key;
    key.device=1;key.context=2;key.deviceEpoch=3;key.ownerEpoch=4;key.configEpoch=5;
    key.instance=6;key.generation=7;key.sourceFrame=8;key.picture=9;key.present=10;
    key.selected=11;key.backbuffer=12;key.swapchain=13;key.thread=14;
    key.width=1920;key.height=1080;key.submitSerial=15;key.copyOrderedSerial=16;key.historyEpoch=17;
    return key;
}
void SavedSdkInput() {
    static_assert(!std::is_copy_constructible_v<SdkInputCall>);
    const auto key=Source();
    const auto source=DLSSG_Dx12::Seal(key,40,40);
    Check(key.Known() && source->Matches(key,40),"provider-sealed exact source");
    SdkInputCall premature;
    Check(!premature.Return(0) && !premature.Succeeded(),"S_OK without real entry is not input acceptance");
    Check(!premature.Enter(nullptr,key,40),"missing dispatch source");
    auto other=key;++other.picture;
    Check(!premature.Enter(source,other,40),"different returned picture");
    Check(!premature.Enter(source,key,41),"different SDK frame");
    Check(!premature.Enter(DLSSG_Dx12::Seal(key,40,41),key,40),"aliased FrameToken");
    Check(!premature.Enter(DLSSG_Dx12::Seal(key,40,40,0),key,40),"missing creation owner");
    Check(!premature.Enter(DLSSG_Dx12::Seal(key,40,40,22,1,false),key,40),"missing immutable packet");
    Check(!premature.Enter(DLSSG_Dx12::Seal(key,UINT64_C(0x100000000),0),key,UINT64_C(0x100000000)),"truncated SDK frame");
    // Neither marker sending nor a local marker token is a parameter. The same
    // real dispatch must reach the SDK bracket under either marker strategy.
    for(bool gameSendsMarkers:{false,true}) {
        SdkInputCall call;
        Check(call.Enter(source,key,40),gameSendsMarkers?"game markers: dispatch enters":"own markers: dispatch enters");
        Check(call.Entered() && !call.Returned() && !call.Succeeded(),"CPU entry cannot certify SDK acceptance");
        Check(call.Source()==source,"keep the immutable dispatch receipt, not a newest token");
        Check(!call.Enter(source,key,40),"one SDK entry per dispatch");
        Check(call.Return(0) && call.Succeeded() && !call.Entered(),"saved exact S_OK accepts the input");
        Check(!call.Return(-1) && call.Succeeded(),"later wrapper failure cannot rewrite saved SDK S_OK");
        Check(!call.Enter(source,key,40),"accepted input cannot be reused");
    }
    for(std::int32_t result:{1,static_cast<std::int32_t>(0x087A0001u),static_cast<std::int32_t>(0x887A000Au),-1}) {
        SdkInputCall call;
        Check(call.Enter(source,key,40) && call.Return(result),"save non-S_OK SDK result");
        Check(!call.Succeeded(),"success-status/occlusion/retry/failure is not S_OK");
        Check(!call.Return(0) && !call.Succeeded(),"no promotion of a saved non-S_OK");
    }
    const auto next=DLSSG_Dx12::Seal(other,41,41,22,2);
    Check(source->StreamOwner()==next->StreamOwner(),"input token/picture/input epoch are not output-stream identity");
}
void ExactPhysicalBinding() {
    const SdkStreamOwner owner{11,22,1,1920,1080};
    const PhysicalStream physical{33,1,44,55,66};
    SdkOutputBinding binding;
    Check(!binding.BindPhysical(22,physical),"raw cannot bind before an authenticated SDK owner");
    Check(!binding.BindOwner({},77) && !binding.BindOwner(owner,0),"unknown owner/device rejected");
    Check(binding.BindOwner(owner,77),"bind exact SDK creation owner and physical device");
    auto unknown=physical;unknown.fence=0;
    Check(!binding.BindPhysical(22,unknown),"missing actual physical fence");
    Check(!binding.BindPhysical(23,physical),"synchronous or async foreign output owner rejected");
    Check(binding.BindPhysical(22,physical) && binding.Matches(22,physical),"first exact captured output");
    Check(binding.BindPhysical(22,physical),"subsequent raw in the same physical lifetime");
    for(unsigned field=0;field<5;++field) {
        auto changed=physical;
        switch(field){case 0:++changed.stream;break;case 1:++changed.generation;break;
            case 2:++changed.chain;break;case 3:++changed.queue;break;default:++changed.fence;break;}
        Check(!binding.BindPhysical(22,changed),"no relabeling stream/generation/chain/queue/fence before retirement");
        Check(binding.Matches(22,physical),"failed rebinding preserves original identity");
    }
    Check(!binding.RetireBuffers(23,33,1),"foreign owner cannot retire buffers");
    Check(!binding.RetireBuffers(22,34,1),"other stream cannot retire buffers");
    Check(!binding.RetireBuffers(22,33,2),"other generation cannot retire buffers");
    auto resized=owner;resized.width=2560;resized.height=1440;
    Check(!binding.BindOwner(resized,77),"extent cannot change before buffer retirement");
    Check(binding.RetireBuffers(22,33,1),"exact drained buffer lifetime retired");
    auto foreign=owner;++foreign.outputOwner;
    Check(!binding.BindOwner(foreign,77),"buffer retirement does not retire provider");
    foreign=owner;++foreign.provider;
    Check(!binding.BindOwner(foreign,77),"buffer retirement cannot replace SDK provider");
    Check(!binding.BindOwner(owner,78),"buffer retirement cannot replace physical device");
    Check(binding.BindOwner(resized,77),"first exact input after resize may bind its new extent");
    auto next=physical;++next.generation;++next.queue;++next.fence;
    Check(binding.BindPhysical(22,next),"new generation binds only after exact old retirement");
    Check(!binding.RetireBuffers(22,33,1) && binding.Matches(22,next),"late old retirement cannot clear the new generation");
    Check(!binding.RetireProvider(23),"foreign provider retirement has no authority");
    Check(binding.RetireProvider(22) && !binding.Owner().Known(),"full provider drain removes its physical binding");
}
void BoundedRevealAdmission() {
    OutputRevealWindow window;
    Check(!window.BlocksRaw(100),"ordinary output stream remains admitted");
    Check(!window.Hold(0,100,350,350),"missing output receipt cannot arm a drain");
    Check(window.Hold(7,100,350,300),"drain begins between SDK calls for an accepted output");
    // Raw 7 is the saved candidate; a previously entered raw 8 may still finish.
    // Admission remains closed, so there cannot be an endless raw 9,10,... tail
    // chase. Fence readiness/Show still belong to the real consumer, not here.
    Check(window.BlocksRaw(120),"new worker raw paused while last admitted raw may finish");
    Check(window.Hold(7,180,900,900),"owner retry preserves its existing drain window");
    Check(window.BlocksRaw(299),"drain retains its original shorter input deadline");
    Check(!window.BlocksRaw(300),"stalled owner resumes raw/input admission at the deadline");
    Check(!window.Hold(7,301,900,900),"same receipt cannot rearm or extend an expired drain");
    Check(window.Hold(9,310,560,600),"a genuinely new output may get its own drain opportunity");
    Check(window.BlocksRaw(559) && !window.BlocksRaw(560),"output freshness also bounds admission pause");
    window.Release();
    Check(!window.BlocksRaw(320),"successful reveal or revocation immediately resumes admission");
    Check(window.Hold(1,600,700,750),"retired physical lifetime may begin a fresh sequence");
    window.Release();
    Check(!window.BlocksRaw(601),"no blocked worker survives explicit lifetime revocation");
}
void StreamLeaseAcrossSlowSdkCall() {
    SdkStreamLease lease;
    Check(!lease.Current(100) && !lease.Accepted(),"no input authority before real entry");
    Check(!lease.Accept(100,250),"S_OK cannot accept a scope without entry");
    Check(lease.Begin(100,250),"authenticated SDK entry opens one transaction");
    Check(!lease.Begin(200,250),"overlapping input cannot replace the open transaction");
    Check(lease.Current(1000),"slow SDK call outlives display freshness without revocation");
    Check(!lease.Accepted(),"an open CPU call alone cannot reveal output");
    Check(lease.Accept(1000,250),"saved SDK S_OK renews the bounded accepted stream");
    lease.End(true);
    Check(lease.Current(1249) && !lease.Current(1250),"closed call retains only its accepted freshness bound");
    Check(lease.Begin(1300,250),"a freshly authenticated dispatch renews the same creation owner");
    lease.Revoke();
    Check(!lease.Current(1301) && !lease.Accepted(),"real revocation overrides an in-progress SDK call");
    Check(!lease.Accept(1302,250),"a late S_OK cannot resurrect the revoked call");
    lease.End(false);
    Check(!lease.Current(1303),"late scope destruction cannot resurrect a stream");
    Check(!lease.Begin(UINT64_MAX-100,250) && !lease.Begin(100,0),"invalid lease bounds cannot grant authority");
    Check(lease.Begin(1400,250),"new scope may enter after real revocation");
    lease.End(false);
    Check(!lease.Current(1401) && !lease.Accept(1401,250),"failed or unwound call leaves neither open entry nor residual authorization");
    Check(lease.Begin(1500,250) && lease.Accept(1501,250),"next valid SDK call is not permanently refused");
    lease.End(true);
    Check(lease.Begin(1600,250) && lease.Accepted(),"next same-stream input retains prior accepted output authority");
    lease.End(false);
    Check(!lease.Current(1601) && !lease.Accepted(),"failure after earlier success revokes the whole old stream lease");
}
void NativeOnlyReturnedDrain() {
    auto key=Source();
    const auto discard=[&](Lifecycle& life,const Key& candidate) {
        return life.DiscardReturned(candidate,key.present,key.thread,key.swapchain,key.context);
    };
    Lifecycle life;
    Check(!discard(life,key),"empty scope cannot claim a drain receipt");
    Check(life.Record(key),"native capture records its exact source");
    Check(!discard(life,key),"recorded without native Return cannot be discarded as returned");
    Check(life.Advance(key,Phase::Recorded,Phase::Returned),"real native Return precedes output drain");
    auto different=key;++different.picture;
    Check(!discard(life,different),"foreign immutable picture cannot be stolen from ready slot");
    different=key;++different.sourceFrame;
    Check(!discard(life,different),"different source cannot alias an unused returned receipt");
    Check(!life.DiscardReturned(key,key.present+1,key.thread,key.swapchain,key.context),"later Present cannot consume old ready receipt");
    Check(!life.DiscardReturned(key,key.present,key.thread+1,key.swapchain,key.context),"foreign thread cannot finish native drain");
    Check(!life.DiscardReturned(key,key.present,key.thread,key.swapchain+1,key.context),"foreign chain cannot finish native drain");
    Check(!life.DiscardReturned(key,key.present,key.thread,key.swapchain,key.context+1),"foreign context cannot finish native drain");
    Check(life.State()==Phase::Returned,"refusals preserve the original unconsumed recording");
    Check(discard(life,key) && life.State()==Phase::Revoked,"drain revokes just the current unused SDK recording");
    Check(!discard(life,key),"same native receipt cannot be consumed a second time");
    Check(!life.Advance(key,Phase::Returned,Phase::Copied),"drained receipt cannot become a later SDK input");
    // Native camera/guide production continues for multiple drain frames. Only
    // the fresh next recording can reach Copied; no SDK result is fabricated.
    for(unsigned i=0;i<3;++i) {
        ++key.present;++key.picture;++key.generation;++key.sourceFrame;
        Lifecycle native;
        Check(native.Record(key) && native.Advance(key,Phase::Recorded,Phase::Returned),"each drain frame runs a new exact native transaction");
        Check(discard(native,key),"every unused native-only frame vacates its own ready slot");
    }
    ++key.present;++key.picture;++key.generation;++key.sourceFrame;
    Lifecycle resumed;
    Check(resumed.Record(key) && resumed.Advance(key,Phase::Recorded,Phase::Returned),"next fresh input is independently produced after drain");
    Check(resumed.Advance(key,Phase::Returned,Phase::Copied),"only the resumed current receipt reaches private copy");
    Check(!discard(resumed,key),"a copied provider transaction cannot be stolen by output drain");
    SdkInputCall noSdk;
    Check(!noSdk.Succeeded() && !noSdk.Return(0),"native-only drain cannot sign SDK acceptance or predecessor history");
}
void UnboundScopeRetirement() {
    const SdkStreamOwner owner{11,22,1,1920,1080};
    SdkOutputBinding binding;
    Check(binding.BindOwner(owner,77),"successful SDK input can precede every raw output");
    Check(!binding.Physical().Known(),"no fabricated physical stream for an input-only scope");
    Check(!binding.RetireBuffers(22,33,1),"unrelated buffer retirement cannot erase an unbound provider");
    auto next=owner;++next.outputOwner;
    Check(!binding.BindOwner(next,77),"unretired old input-only scope rejects replacement provider");
    Check(binding.RetireProvider(22),"real full-provider retirement clears an unbound scope");
    Check(binding.BindOwner(next,77),"new SDK creation owner no longer starves after genuine retirement");
}
}
int main() {
    SavedSdkInput();ExactPhysicalBinding();BoundedRevealAdmission();StreamLeaseAcrossSlowSdkCall();NativeOnlyReturnedDrain();UnboundScopeRetirement();
    std::printf("async-output CPU contracts: %u checks passed (no GPU/display claim)\n",checks);
}
