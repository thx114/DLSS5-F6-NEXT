#define NOMINMAX
#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <array>
#include <cstdio>
#include <filesystem>
#include <future>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include "../../../framegen/dlssg/ZzzFrameScope.h"

using Microsoft::WRL::ComPtr;
unsigned nativeDepth=0, nativeInitDepth=0, nativeStopDepth=0, selectedDepth=0, checks=0;
bool enabled=true;
bool Enabled() { return enabled; }
HMODULE Module(unsigned value) { return reinterpret_cast<HMODULE>(static_cast<uintptr_t>(value)); }
HMODULE dllModule=Module(1);
namespace NgxInput { struct { HMODULE dll=Module(2); } native; }
struct State
{
    HMODULE optiSlInterposer=Module(3), optiSlCommon=Module(4), optiDLSSG=Module(5);
    HMODULE optiSlDLSSG=Module(6), optiSlReflex=Module(7), optiSlPCL=Module(8);
    static State& Instance() { static State value; return value; }
};
struct Config
{
    std::optional<std::wstring> MainDllPath=L"D:/fixture/f6";
    static Config* Instance() { static Config value; return &value; }
};
namespace Util
{
std::filesystem::path ExePath() { return L"D:/fixture/game/ZenlessZoneZero.exe"; }
bool IsSubpath(const std::filesystem::path& path, const std::filesystem::path& base)
{
    const auto rel=path.lexically_relative(base);
    return !rel.empty() && *rel.begin()!=L"..";
}
}
std::unordered_map<HMODULE,std::wstring> modulePaths;
HMODULE ModuleAt(const void* value) { return static_cast<HMODULE>(const_cast<void*>(value)); }
HMODULE TestGetModuleHandleW(const wchar_t*) { return Module(9); }
DWORD TestGetModuleFileNameW(HMODULE module,wchar_t* output,DWORD capacity)
{
    const auto found=modulePaths.find(module);
    if (found==modulePaths.end()) return 0;
    const auto length=found->second.size();
    if (length>=capacity) return capacity;
    std::copy(found->second.begin(),found->second.end(),output);
    output[length]=0;
    return static_cast<DWORD>(length);
}

// Generated from the actual Windows adapter and its canonical COM helper.
// Module inventory and path utilities are injected; origin policy and Scope
// are production code. Native proxy unwrapping is not exercised by this fixture.
#define GetModuleHandleW TestGetModuleHandleW
#define GetModuleFileNameW TestGetModuleFileNameW
#include "production-caller.inc"
#undef GetModuleFileNameW
#undef GetModuleHandleW

void Check(bool value,const char* message)
{
    ++checks;
    if (!value) throw std::runtime_error(message);
}

int main()
{
    using namespace NgxInput;
    try
    {
        ComPtr<IDXGIFactory4> factory;
        ComPtr<IDXGIAdapter> warp;
        ComPtr<ID3D12Device> device;
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> command, other;
        Check(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))),"create factory");
        Check(SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))),"get WARP");
        Check(SUCCEEDED(D3D12CreateDevice(warp.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device))),"create device");
        Check(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator))),"create allocator");
        Check(SUCCEEDED(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&command))),"create command");
        Check(SUCCEEDED(command->Close()),"close command");
        Check(SUCCEEDED(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&other))),"create other command");
        Check(SUCCEEDED(other->Close()),"close other command");
        ComPtr<IUnknown> identity;
        Check(SUCCEEDED(command.As(&identity)),"canonical identity");
        constexpr uintptr_t api=0x900;
        const auto list=reinterpret_cast<uintptr_t>(identity.Get());
        using namespace dlssg::zzz;
        const Origin origin{api,list,0,2,4679,true,true};
        gameApi.store(api);
        // Resolve owned modules successfully outside the local SDK directory:
        // rejection must come from the module identity, not a missing path.
        for (unsigned caller=1;caller<=8;++caller)
            modulePaths[Module(caller)]=L"D:/fixture/other/owned-" + std::to_wstring(caller) + L".dll";
        modulePaths[Module(10)]=L"D:/fixture/game/sl.common.dll";
        modulePaths[Module(11)]=L"D:/fixture/game/sl.dlss.dll";
        modulePaths[Module(12)]=L"D:/fixture/game/UnityPlayer.dll";
        modulePaths[Module(13)]=L"D:/fixture/f6/streamline/sl.common.dll";
        modulePaths[Module(14)]=L"D:/fixture/game/intermediary.dll";
        modulePaths[Module(15)]=L"D:/unrelated/UnityPlayer.dll";
        modulePaths[Module(16)]=L"D:/fixture/f6/streamline/intermediary.dll";
        Check(!NativeDelegation(Module(2)),"vendor origin requires an active native call");
        nativeDepth=1; nativeInitDepth=1;
        Check(NativeInitDelegation(Module(2)),"vendor init delegation is retained");
        Check(!NativeInitDelegation(Module(9)),"application init callback is not vendor delegation");
        Check(!NativeStopDelegation(Module(2)),"vendor init cannot bypass shutdown admission");
        nativeInitDepth=0; nativeStopDepth=1;
        Check(NativeStopDelegation(Module(2)),"vendor shutdown delegation is retained");
        Check(!NativeStopDelegation(Module(9)),"application shutdown callback is not vendor delegation");
        Check(!NativeInitDelegation(Module(2)),"vendor shutdown cannot bypass init admission");
        nativeDepth=0; nativeStopDepth=0;
        Check(ResolveInputOrigin(Module(15),command.Get())==InputOrigin::Backend,"unrelated UnityPlayer without game scope");
        for (unsigned caller: {9u,10u,11u,12u})
            Check(ResolveInputOrigin(Module(caller))==InputOrigin::GameModule,"known game caller");
        Check(ResolveInputOrigin(Module(14),command.Get())==InputOrigin::Backend,"unknown module without game scope");
        {
            Scope game(origin,true);
            Check(ResolveInputOrigin(Module(14),command.Get())==InputOrigin::SynchronousGame,"synchronous game through intermediary must be input");
            Check(ResolveInputOrigin(Module(14),other.Get())==InputOrigin::Backend,"wrong native list");
            Check(ResolveInputOrigin(Module(14))==InputOrigin::Backend,"missing command");
            Check(!External(Module(14)),"non-Create APIs cannot borrow game scope");
            for (unsigned caller=0;caller<=8;++caller)
                Check(ResolveInputOrigin(Module(caller),command.Get())==InputOrigin::Backend,"owned or missing module must stay backend");
            for (unsigned caller: {13u,16u,17u})
                Check(ResolveInputOrigin(Module(caller),command.Get())==InputOrigin::Backend,"local SDK or unresolved module must stay backend");
            nativeDepth=1;
            Check(ResolveInputOrigin(Module(14),command.Get())==InputOrigin::Backend,"native recursion");
            nativeDepth=0; selectedDepth=1;
            Check(ResolveInputOrigin(Module(14),command.Get())==InputOrigin::Backend,"selected backend recursion");
            selectedDepth=0; enabled=false;
            Check(ResolveInputOrigin(Module(14),command.Get())==InputOrigin::Backend,"disabled input");
            enabled=true; gameApi.store(api+1);
            Check(ResolveInputOrigin(Module(14),command.Get())==InputOrigin::Backend,"unregistered API");
            gameApi.store(api);
            Check(std::async(std::launch::async,[&] {
                return ResolveInputOrigin(Module(14),command.Get())==InputOrigin::Backend;
            }).get(),"cross-thread call");
            {
                Scope local(origin,false);
                Check(ResolveInputOrigin(Module(14),command.Get())==InputOrigin::Backend,"nested local call shadows game");
            }
            Check(ResolveInputOrigin(Module(14),command.Get())==InputOrigin::SynchronousGame,"restore actual game scope");
            {
                Selection selected(api,0,0,list,0x300,0x500);
                Check(ResolveInputOrigin(Module(14),command.Get())==InputOrigin::Backend,"claimed source cannot authorize another input");
            }
        }
        for (bool success: {false,true})
        {
            Scope finished(origin,true);
            finished.Finish(success);
            Check(ResolveInputOrigin(Module(14),command.Get())==InputOrigin::Backend,"completed scope is not current");
        }
        Check(ResolveInputOrigin(Module(14),command.Get())==InputOrigin::Backend,"scope lifetime ended");
        std::printf("PASS %u actual NGX caller adapter checks; real WARP COM identity, no GPU execution or game DLL\n",checks);
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr,"FAIL actual NGX caller adapter: %s (check %u)\n",error.what(),checks);
        return 1;
    }
}
