// The runner inserts actual producer methods; only SDK/API boundaries are mocked.
#include <framegen/FgRuntimeStatus.h>
#include <framegen/DlssgStateSample.h>
#include <iostream>
#include <thread>
#include <cstdlib>
using UINT=unsigned; using HRESULT=int32_t;
constexpr HRESULT S_OK=0, DXGI_STATUS_OCCLUDED=1;
constexpr UINT DXGI_PRESENT_TEST=1;
#define FAILED(value) ((value)<0)
struct IDXGISwapChain {};
unsigned checks=0;
void Check(bool v,const char* msg) {++checks; if(!v){std::cerr<<msg<<'\n';std::exit(1);}}
struct IFGFeature { virtual ~IFGFeature()=default; uint64_t padding=0; };
struct IFGFeature_Dx12 : virtual IFGFeature {
    const uint64_t _runtimeInstance=framegen::NextRuntimeInstance();
    bool _waitingNewFrameData=false,_isActive=true; uint64_t _targetFrame=0,_frameCount=4;
    IDXGISwapChain* _swapChain=nullptr;
    framegen::RuntimeSnapshot RuntimeIdentity(HRESULT) const;
    framegen::RuntimeSnapshot RuntimeObservation(UINT,HRESULT) const;
};
struct Owner { uint64_t lifecycle=1; bool known=true; bool Known()const{return known;} bool operator==(const Owner&)const=default; };
struct Continuity {bool held=false; bool Held()const{return held;}};
struct DLSSG_Dx12 : virtual IFGFeature_Dx12 {
    Owner _owner,_runtimeObservedOwner; uint64_t _runtimeRateFloorNs=1; unsigned viewport=0;
    Continuity _inputContinuity; bool _depthSourceGap=false;
    Owner CurrentProviderOwner()const{return _owner;}
    void ObservePresentReturned(IDXGISwapChain*,UINT,HRESULT);
};
framegen::DlssgStateSample sdkSample;
unsigned dlssgReads=0,xefgReads=0;
namespace StreamlineProxy {uintptr_t DLSSGGetState(){return 1;}}
namespace StreamlineHooks {bool ReadDlssgStateSample(uintptr_t,unsigned,framegen::DlssgStateSample& s){++dlssgReads;s=sdkSample;return true;}}
using xefg_swapchain_handle_t=void*;
enum xefg_swapchain_result_t {XEFG_SWAPCHAIN_RESULT_SUCCESS=0,XEFG_SWAPCHAIN_RESULT_WARNING_MISSING_PRESENT_STATUS=5,XEFG_FAILURE=-1};
struct xefg_swapchain_present_status_t {uint32_t framesPresented=0;xefg_swapchain_result_t frameGenResult=XEFG_SWAPCHAIN_RESULT_SUCCESS;uint32_t isFrameGenEnabled=0;};
xefg_swapchain_present_status_t xeStatus;
xefg_swapchain_result_t xeQueryResult=XEFG_SWAPCHAIN_RESULT_SUCCESS;
bool xeQueryAvailable=true;
xefg_swapchain_result_t QueryXe(void*,xefg_swapchain_present_status_t* s){++xefgReads;*s=xeStatus;return xeQueryResult;}
namespace XeFGProxy {auto GetLastPresentStatus(){return xeQueryAvailable?&QueryXe:nullptr;}}
struct XeFG_Dx12 : virtual IFGFeature_Dx12 {
    xefg_swapchain_handle_t _swapChainContext=reinterpret_cast<void*>(2);
    std::recursive_mutex _presentStatusMutex;
    framegen::OutputRateWindow _outputRate;
    uint64_t _statusEpoch=1,_statusSequence=0,_statusEndNs=0;
    void ObservePresentReturned(IDXGISwapChain*,UINT,HRESULT);
};
// INSERT_PRODUCTION
framegen::RuntimeSnapshot Read(IFGFeature* provider) {
    framegen::RuntimeSnapshot s;
    Check(framegen::RuntimeState().Read(reinterpret_cast<uintptr_t>(provider),framegen::RuntimeNowNs(),s),"canonical provider read");return s;
}
int main(){
    IDXGISwapChain chain,other;
    DLSSG_Dx12 dlssg; dlssg._swapChain=&chain;
    const auto now=framegen::RuntimeNowNs();
    sdkSample.api=1; sdkSample.queryBeginNs=now-2000000;sdkSample.queryEndNs=now-1000000;sdkSample.succeeded=true;
    sdkSample.outputRate={1,0,now-1000000000,now-1000000,120,60,120,true};
    dlssg.ObservePresentReturned(&chain,0,S_OK);auto s=Read(&dlssg);
    Check(s.status==framegen::RuntimeStatus::OutputObserved && s.outputRate.known,"fresh observed output");
    const auto reads=dlssgReads;
    for(int n=0;n<50;++n)Read(&dlssg);
    Check(dlssgReads==reads,"menu reads cannot consume SDK query counters");
    dlssg.ObservePresentReturned(&other,0,S_OK);dlssg.ObservePresentReturned(&chain,DXGI_PRESENT_TEST,S_OK);
    Check(dlssgReads==reads,"wrong chain and TEST excluded");
    sdkSample.queryBeginNs=now-4000000001ull;sdkSample.queryEndNs=now-4000000000ull;
    dlssg.ObservePresentReturned(&chain,0,S_OK);s=Read(&dlssg);
    Check(!s.outputRate.known && s.status==framegen::RuntimeStatus::EnabledUnconfirmed,"continued Present cannot renew stale SDK sample");
    sdkSample.succeeded=false;sdkSample.status=9;
    dlssg.ObservePresentReturned(&chain,0,S_OK);s=Read(&dlssg);
    Check(s.status!=framegen::RuntimeStatus::SdkError,"stale SDK error is not renewed");
    sdkSample.queryBeginNs=now-2000000;sdkSample.queryEndNs=now-1000000;
    dlssg.ObservePresentReturned(&chain,0,S_OK);s=Read(&dlssg);
    Check(s.status==framegen::RuntimeStatus::SdkError && !s.outputRate.known,"fresh SDK error clears rate");
    sdkSample.succeeded=true;sdkSample.status=0;sdkSample.outputRate.frames=0;sdkSample.outputRate.hz=0;
    dlssg.ObservePresentReturned(&chain,0,S_OK);s=Read(&dlssg);
    Check(s.outputRate.known && s.outputRate.hz==0 && s.status!=framegen::RuntimeStatus::OutputObserved,"zero FPS is not observed output");
    dlssg._inputContinuity.held=true;dlssg.ObservePresentReturned(&chain,0,S_OK);
    Check(Read(&dlssg).status==framegen::RuntimeStatus::InputGap,"input gap precedes SDK activity");
    dlssg._inputContinuity.held=false;dlssg._owner.lifecycle++;
    dlssg.ObservePresentReturned(&chain,0,S_OK);s=Read(&dlssg);
    Check(!s.outputRate.known && s.epoch==dlssg._owner.lifecycle,"new owner cannot inherit old count window");
    dlssg.ObservePresentReturned(&chain,0,DXGI_STATUS_OCCLUDED);s=Read(&dlssg);
    Check(s.status==framegen::RuntimeStatus::Waiting && !s.outputRate.known,"occlusion is no output");
    XeFG_Dx12 xe;xeStatus={60,XEFG_SWAPCHAIN_RESULT_SUCCESS,1};
    xe._statusEndNs=framegen::RuntimeNowNs()-300000000ull;
    xe.ObservePresentReturned(&chain,0,S_OK);s=Read(&xe);
    Check(s.outputRate.known && s.outputRate.frames==60 && s.status==framegen::RuntimeStatus::OutputObserved,"XeFG actual last Present output count");
    auto xeBefore=xefgReads;xe.ObservePresentReturned(&chain,DXGI_PRESENT_TEST,S_OK);
    Check(xefgReads==xeBefore,"XeFG TEST does not query");
    xe.ObservePresentReturned(&chain,0,DXGI_STATUS_OCCLUDED);s=Read(&xe);
    Check(xefgReads==xeBefore && !s.outputRate.known,"XeFG occluded excludes count");
    xeStatus.isFrameGenEnabled=0;xe.ObservePresentReturned(&chain,0,S_OK);
    Check(Read(&xe).status==framegen::RuntimeStatus::SdkDisabled,"XeLL-only/SDK disabled never reports FG active");
    xeQueryResult=XEFG_SWAPCHAIN_RESULT_WARNING_MISSING_PRESENT_STATUS;xe.ObservePresentReturned(&chain,0,S_OK);
    Check(!Read(&xe).outputRate.known,"missing present status warning invalidates count window");
    xeQueryResult=XEFG_SWAPCHAIN_RESULT_SUCCESS;xeStatus.frameGenResult=XEFG_FAILURE;xe.ObservePresentReturned(&chain,0,S_OK);
    Check(Read(&xe).status==framegen::RuntimeStatus::SdkError,"interpolation error");
    xeQueryAvailable=false;xe.ObservePresentReturned(&chain,0,S_OK);
    Check(!Read(&xe).outputRate.known,"optional SDK entry absent");
    xeQueryAvailable=true;xe._presentStatusMutex.lock();
    xeBefore=xefgReads;std::thread retired([&]{xe.ObservePresentReturned(&chain,0,S_OK);});retired.join();
    Check(xefgReads==xeBefore,"context lifetime lock excludes observation without blocking");
    xe._swapChainContext=nullptr;xe._presentStatusMutex.unlock();
    xe.ObservePresentReturned(&chain,0,S_OK);Check(xefgReads==xeBefore,"retired context is not queried");
    std::cout<<"Runtime output observation: PASS "<<checks<<" checks (actual producer methods; mocked SDK)\n";
}
