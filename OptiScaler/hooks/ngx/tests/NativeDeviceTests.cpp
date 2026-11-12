#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <Unknwn.h>
#include <wrl/client.h>
#include <atomic>
#include <cstdio>
#include <stdexcept>
#include <utility>
#include <vector>

using Microsoft::WRL::ComPtr;
unsigned checks=0;
void Check(bool value,const char* message) { ++checks; if (!value) throw std::runtime_error(message); }

// COM/device API mocks. Separate interface addresses may expose one canonical
// IUnknown. No D3D device, vendor SDK or GPU commands are executed.
struct __declspec(uuid("6B99B7C9-7356-4B1A-A61C-6D304579FF08")) Device : IUnknown
{
    ULONG references=1;
    Device* identity=this;
    bool queryAllowed=true;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** out) override
    {
        *out=nullptr;
        if (!queryAllowed || iid!=__uuidof(IUnknown)) return E_NOINTERFACE;
        *out=static_cast<IUnknown*>(identity); identity->AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
    ULONG STDMETHODCALLTYPE Release() override { return --references; }
};
using ID3D12Device=Device;
struct ID3D12GraphicsCommandList
{
    Device* device;
    bool getAllowed=true;
    unsigned getCalls=0;
    HRESULT GetDevice(REFIID iid,void** out)
    {
        ++getCalls;
        *out=nullptr;
        if (!getAllowed || !device) return E_FAIL;
        if (iid!=__uuidof(Device)) return E_NOINTERFACE;
        device->AddRef();*out=device;return S_OK;
    }
};
struct State
{
    bool isShuttingDown=false,nvngxDx12Inited=false;
    Device* currentD3D12Device=nullptr;
    int api=0;
    static State& Instance() { static State value; return value; }
};
constexpr int DX12=12;
std::atomic_bool shutdown{false};
Device *D3D12Device=nullptr,*nativeAttachedDevice=nullptr,*observedInitDevice=nullptr;
std::vector<int> Dx12Contexts;
namespace D3D12Hooks { unsigned calls=0; void HookDevice(Device*) { ++calls; } }
namespace UpscalerInputsDx12 { unsigned calls=0; Device* current=nullptr; void Init(Device* d) { ++calls;current=d; } }
#include "production-device.inc"

int main() try
{
    Device owner,alias,foreign;
    alias.identity=&owner;
    auto& state=State::Instance();
    Check(&owner!=&alias,"fixture uses distinct interface addresses");
    Check(dlssg::depth_contract::SameOwner(&owner,&alias),"canonical COM identity accepts aliases");
    Check(!dlssg::depth_contract::SameOwner(&owner,&foreign),"different COM device is rejected");
    Check(!dlssg::depth_contract::SameOwner(nullptr,&owner),"null identity is rejected");
    alias.queryAllowed=false;
    Check(!dlssg::depth_contract::SameOwner(&owner,&alias),"failed identity query is rejected");
    alias.queryAllowed=true;
    Check(owner.references==1 && alias.references==1 && foreign.references==1,"identity probes balance references");
    Check(!NgxInput::CanAttachNativeDx12Input(nullptr),"null attachment rejected");
    state.currentD3D12Device=&owner;
    Check(NgxInput::CanAttachNativeDx12Input(&alias),"published device alias accepted");
    Check(!NgxInput::CanAttachNativeDx12Input(&foreign),"published foreign device rejected");
    D3D12Device=&owner;
    Check(NgxInput::CanAttachNativeDx12Input(&alias),"private device alias accepted");
    Check(NgxInput::Api::AcceptInitDevice(&alias),"init accepts alias before observed init");
    NgxInput::Api::ObserveInitDevice(&owner);
    Check(owner.references==2,"observed init holds one reference");
    NgxInput::Api::ObserveInitDevice(&alias);
    Check(observedInitDevice==&owner && owner.references==2 && alias.references==1,"repeated alias init preserves one original owner");
    Check(NgxInput::Api::AcceptInitDevice(&alias),"observed init alias accepted");
    Check(!NgxInput::Api::AcceptInitDevice(&foreign),"foreign init rejected");
    state.currentD3D12Device=&foreign;
    Check(!NgxInput::Api::AcceptInitDevice(&alias),"changed published device prevents new attachment");
    Check(NgxInput::Api::AcceptShutdownDevice(&owner) && NgxInput::Api::AcceptShutdownDevice(&alias),
          "observed device and alias may shut down after published device changes");
    Check(!NgxInput::Api::AcceptShutdownDevice(&foreign) && !NgxInput::Api::AcceptShutdownDevice(nullptr),
          "shutdown rejects foreign and null devices");
    state.currentD3D12Device=&owner;
    ID3D12GraphicsCommandList first{&alias},second{&owner},wrong{&foreign},missing{nullptr};
    {
        auto acquired=NgxInput::Api::AcquireCommandDevice(&first);
        Check(acquired.Get()==&alias && first.getCalls==1 && alias.references==2,
              "single device query retains the admitted interface");
    }
    Check(alias.references==1,"acquired command device reference is released");
    Check(NgxInput::Api::AcceptCommandDevice(&first),"command from alias admitted");
    Check(NgxInput::Api::AcceptCommandDevice(&second),"different command same device admitted");
    Check(!NgxInput::Api::AcceptCommandDevice(&wrong),"foreign command device rejected");
    Check(!NgxInput::Api::AcceptCommandDevice(&missing),"missing command device rejected");
    Check(!NgxInput::Api::AcceptCommandDevice(nullptr),"null command rejected");
    first.getAllowed=false;
    Check(!NgxInput::Api::AcceptCommandDevice(&first),"GetDevice failure rejected");
    first.getAllowed=true;
    Check(NgxInput::AttachNativeDx12Input(&alias),"first attachment succeeds");
    Check(alias.references==2 && nativeAttachedDevice==&alias,"attachment owns exactly one device reference");
    Check(IsInitializedOnDevice(&owner) && IsInitializedOnDevice(&alias),"same-owner alias is already initialized");
    Check(!IsInitializedOnDevice(&foreign) && !IsInitializedOnDevice(nullptr),"other or null device is not initialized");
    const auto hooks=D3D12Hooks::calls,inputs=UpscalerInputsDx12::calls;
    for (unsigned i=0;i<20;++i) Check(NgxInput::AttachNativeDx12Input(i%2?&owner:&alias),"repeat alias attachment succeeds");
    Check(alias.references==2 && owner.references==2 && D3D12Hooks::calls==hooks && UpscalerInputsDx12::calls==inputs,"repeat attachment neither leaks nor reinitializes");
    D3D12Device=&owner;state.currentD3D12Device=&owner;UpscalerInputsDx12::current=&owner;
    Check(NgxInput::AttachNativeDx12Input(&owner),"unowned published alias is reattached");
    Check(nativeAttachedDevice==&owner && owner.references==3 && alias.references==1 &&
          D3D12Device==&owner && state.currentD3D12Device==&owner && UpscalerInputsDx12::current==&owner,
          "reattachment pins published and input device before releasing old pin");
    state.nvngxDx12Inited=false;
    Check(NgxInput::AttachNativeDx12Input(&owner),"same-owner reattachment after input reset");
    Check(alias.references==1 && owner.references==3 && nativeAttachedDevice==&owner,"replaced pin is balanced after reattachment");
    Dx12Contexts.push_back(1);
    Check(!NgxInput::DetachNativeDx12Input() && nativeAttachedDevice==&owner,"live selected contexts block detach");
    Dx12Contexts.clear();state.currentD3D12Device=&alias;
    Check(NgxInput::DetachNativeDx12Input(),"empty selected registry may detach");
    Check(!state.currentD3D12Device && !D3D12Device && !nativeAttachedDevice && !state.nvngxDx12Inited && !UpscalerInputsDx12::current,"detach clears aliases belonging to owned device");
    Check(owner.references==2 && alias.references==1,"detach releases exactly its attachment pin");
    observedInitDevice->Release();observedInitDevice=nullptr;
    Check(owner.references==1,"observed pin release balances ownership");
    state.isShuttingDown=true;
    Check(!NgxInput::CanAttachNativeDx12Input(&owner),"process shutdown rejects attachment");
    state.isShuttingDown=false;shutdown=true;
    Check(!NgxInput::CanAttachNativeDx12Input(&owner),"input shutdown rejects attachment");
    shutdown=false;
    Check(NgxInput::AttachNativeDx12Input(&owner),"fresh attachment after detach");
    state.currentD3D12Device=&foreign;
    Check(!NgxInput::CanAttachNativeDx12Input(&owner),"changed published owner prevents admission");
    Check(NgxInput::DetachNativeDx12Input() && state.currentD3D12Device==&foreign,"detach preserves unrelated published device");
    Check(owner.references==1 && alias.references==1 && foreign.references==1,"all COM references balanced");
    std::printf("PASS %u extracted production device admission/ownership checks (COM mocks; no SDK/game/GPU)\n",checks);
}
catch(const std::exception& error) { std::fprintf(stderr,"FAIL native device: %s (check %u)\n",error.what(),checks);return 1; }
