// SPDX-License-Identifier: GPL-3.0-or-later
#include <hooks/D3D11On12FactoryBoundary.h>
#include <hooks/RoutedHook.h>
#include <atomic>
#include <cstdio>
#include <stdexcept>
#include <thread>
using namespace D3D11On12FactoryBoundary;
unsigned checks = 0;
void Check(bool ok) { ++checks; if (!ok) throw std::runtime_error("factory boundary check failed"); }
struct __declspec(uuid("1AB8B12B-F1BF-47FB-9FD4-05932A193018")) Requested : IUnknown { virtual unsigned Value() = 0; };
struct Prefix : IUnknown { virtual unsigned Padding() = 0; };
struct Terminal final : Prefix, Requested
{
    ULONG refs=1;
    bool rejectRequested=false;
    HRESULT QueryInterface(REFIID iid,void** output) override {
        *output=nullptr;
        if (iid==__uuidof(IUnknown)) *output=static_cast<Prefix*>(this);
        else if (iid==__uuidof(Requested) && !rejectRequested) *output=static_cast<Requested*>(this);
        else return E_NOINTERFACE;
        AddRef(); return S_OK;
    }
    ULONG AddRef() override { return ++refs; }
    ULONG Release() override { return --refs; }
    unsigned Padding() override { return 7; }
    unsigned Value() override { return 42; }
};
struct Proxy final : IUnknown
{
    ULONG refs=1;
    IUnknown* inner;
    GUID contract;
    HRESULT unwrapResult=S_OK;
    Proxy(IUnknown* object,GUID id) : inner(object),contract(id) {}
    HRESULT QueryInterface(REFIID iid,void** output) override {
        *output=nullptr;
        if (iid==__uuidof(IUnknown)) { *output=this;AddRef();return S_OK; }
        if (iid==contract) {
            if (unwrapResult!=S_OK) return unwrapResult;
            *output=inner;if(inner)inner->AddRef();return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG AddRef() override { return ++refs; }
    ULONG Release() override { return --refs; }
};
namespace NestedCreation
{
struct DeviceDomain { static void Returned(HRESULT,int) { ++returned; } inline static unsigned returned=0; };
using On12Domain=D3D11On12FactoryBoundary::Domain<DeviceDomain>;
unsigned policies=0;
HRESULT On12Policy(int);
HRESULT DevicePolicy(int);
HookOwnership::RoutedHook<&On12Policy,SIZE_MAX,On12Domain> on12;
HookOwnership::RoutedHook<&DevicePolicy,SIZE_MAX,DeviceDomain> device;
__declspec(noinline) HRESULT NativeOn12(int value) { volatile int marker=value;return Internal(0x80000000u)?marker+42:E_FAIL; }
__declspec(noinline) HRESULT ProxyOn12(int value) { volatile int marker=value;return NativeOn12(marker); }
__declspec(noinline) HRESULT NativeDevice(int value) { volatile int marker=value;return ProxyOn12(marker); }
HRESULT On12Policy(int value) { ++policies;Scope scope;return on12(value); }
HRESULT DevicePolicy(int value) { return device(value); }
void Test()
{
    HookOwnership::Own::Batch batch;
    Check(on12.Add(batch,reinterpret_cast<PVOID>(NativeOn12))==NO_ERROR);
    Check(on12.Add(batch,reinterpret_cast<PVOID>(ProxyOn12))==NO_ERROR);
    Check(device.Add(batch,reinterpret_cast<PVOID>(NativeDevice))==NO_ERROR);
    Check(batch.Commit()==NO_ERROR);
    Check(NativeDevice(3)==45 && policies==1 && DeviceDomain::returned==1 && depth==0);
    Check(ProxyOn12(5)==47 && policies==2 && DeviceDomain::returned==2 && depth==0);
    std::vector<HookOwnership::Own::Slot*> slots;on12.AppendSlots(slots);device.AppendSlots(slots);
    Check(HookOwnership::Own::DetachAll(std::span<HookOwnership::Own::Slot* const>(slots))==NO_ERROR);
    Check(NativeDevice(3)==E_FAIL && depth==0);
}
}
int main()
{
    Terminal terminal;
    Proxy inner(static_cast<Prefix*>(&terminal),NativeObjectContracts::Streamline);
    Proxy outer(&inner,NativeObjectContracts::ReShade);
    Check(static_cast<void*>(static_cast<Prefix*>(&terminal)) != static_cast<void*>(static_cast<Requested*>(&terminal)));
    void* output=&outer;
    Check(Returned(S_OK,0x80000000u,__uuidof(Requested),&output)==S_OK && output==&outer);
    {
        Scope scope;
        Check(Internal(0x80000000u) && !Internal(0));
        Check(Returned(S_OK,0,__uuidof(Requested),&output)==S_OK && output==&outer);
        Check(Returned(E_FAIL,0x80000000u,__uuidof(Requested),&output)==E_FAIL && output==&outer);
        void* empty=nullptr;
        Check(Returned(S_OK,0x80000000u,__uuidof(Requested),&empty)==E_UNEXPECTED && !empty);
        Check(Returned(S_OK,0x80000000u,__uuidof(Requested),nullptr)==E_UNEXPECTED);
        for(unsigned pass=0;pass<1000;++pass) {
            outer.AddRef();output=&outer;
            Check(Returned(S_OK,0x80000000u,__uuidof(Requested),&output)==S_OK);
            Check(output==static_cast<Requested*>(&terminal) && static_cast<Requested*>(output)->Value()==42);
            static_cast<Requested*>(output)->Release();
            Check(terminal.refs==1 && inner.refs==1 && outer.refs==1);
        }
        terminal.rejectRequested=true;outer.AddRef();output=&outer;
        Check(Returned(S_OK,0x80000000u,__uuidof(Requested),&output)==E_NOINTERFACE && !output);
        Check(terminal.refs==1 && inner.refs==1 && outer.refs==1);terminal.rejectRequested=false;
        inner.unwrapResult=E_FAIL;outer.AddRef();output=&outer;
        Check(Returned(S_OK,0x80000000u,__uuidof(Requested),&output)==E_FAIL && !output);
        Check(terminal.refs==1 && inner.refs==1 && outer.refs==1);inner.unwrapResult=S_OK;
        inner.inner=&outer;outer.AddRef();output=&outer;
        Check(Returned(S_OK,0x80000000u,__uuidof(Requested),&output)==E_NOINTERFACE && !output);
        Check(inner.refs==1 && outer.refs==1);inner.inner=static_cast<Prefix*>(&terminal);
        { Scope nested; Check(depth==2); }
        Check(depth==1);
        std::atomic<bool> publicThreadUnaffected=false;
        std::thread other([&] { publicThreadUnaffected=!Internal(0x80000000u); {Scope nested;publicThreadUnaffected=publicThreadUnaffected&&Internal(0x80000000u);} publicThreadUnaffected=publicThreadUnaffected&&depth==0; });
        other.join();Check(publicThreadUnaffected && depth==1);
    }
    Check(depth==0 && !Internal(0x80000000u));
    try {Scope scope;throw 1;} catch(int) {} Check(depth==0);
    NestedCreation::Test();
    std::printf("PASS %u production internal factory ownership, adjusted IID, failures, cycles, nesting and thread isolation checks\n",checks);
}
