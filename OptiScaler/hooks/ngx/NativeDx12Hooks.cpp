#include "pch.h"
#include "NativeDx12Hooks.h"
#include "NativeDx12Router.h"

#include <proxies/NVNGX_Proxy.h>
#include <inputs/NVNGX_DLSS.h>
#include <upscalers/GameRoutePolicy.h>
#include <TlHelp32.h>
#include <intrin.h>
#include <array>
#include <vector>
#include <unordered_set>

namespace NgxInput
{
namespace
{
// Detours edits this table to the native trampolines, never the proxy's
// published table. The latter is rebound to scoped backend functions only
// after the whole transaction commits.
NvngxModule native;
NvngxModule selected;
std::atomic_bool installed { false };
std::mutex installMutex;
thread_local unsigned nativeDepth = 0;
thread_local unsigned selectedDepth = 0;
thread_local std::optional<NVSDK_NGX_Result> initReplay;
thread_local std::optional<NVSDK_NGX_Result> shutdownReplay;
ID3D12Device* observedInitDevice = nullptr;
NVSDK_NGX_Parameter* pendingProbe = nullptr;
std::mutex parameterMutex;
std::unordered_set<NVSDK_NGX_Parameter*> persistentParameters;

struct Depth
{
    unsigned& value;
    explicit Depth(unsigned& counter) : value(counter) { ++value; }
    ~Depth() { --value; }
};
struct Replay
{
    std::optional<NVSDK_NGX_Result>& slot;
    std::optional<NVSDK_NGX_Result> previous;
    Replay(std::optional<NVSDK_NGX_Result>& target, NVSDK_NGX_Result result) : slot(target), previous(target)
    { slot = result; }
    ~Replay() { slot = previous; }
};

HMODULE ModuleAt(const void* address)
{
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                      reinterpret_cast<LPCWSTR>(address), &module);
    return module;
}

bool External(const void* caller)
{
    if (nativeDepth || selectedDepth || !Enabled())
        return false;
    const auto module = ModuleAt(caller);
    if (!module || module == dllModule || module == native.dll)
        return false;
    const auto& state = State::Instance();
    if (module == state.optiSlInterposer || module == state.optiSlCommon || module == state.optiDLSSG ||
        module == state.optiSlDLSSG || module == state.optiSlReflex || module == state.optiSlPCL)
        return false;
    if (module == GetModuleHandleW(nullptr))
        return true;

    wchar_t buffer[32768];
    const auto length = GetModuleFileNameW(module, buffer, static_cast<DWORD>(std::size(buffer)));
    if (!length || length == std::size(buffer))
        return false;
    const std::filesystem::path path(buffer);
    const auto localSl = std::filesystem::path(Config::Instance()->MainDllPath.value()) / L"streamline";
    if (Util::IsSubpath(path.lexically_normal(), localSl.lexically_normal()))
        return false;
    const auto name = path.filename().wstring();
    // Unknown DLLs (including snippets and independent NR forwarders) are
    // native backend callers. Only actual game NGX consumers may create input.
    return _wcsicmp(name.c_str(), L"sl.common.dll") == 0 || _wcsicmp(name.c_str(), L"sl.dlss.dll") == 0 ||
           (_wcsicmp(name.c_str(), L"UnityPlayer.dll") == 0 &&
            Util::IsSubpath(path.lexically_normal(), Util::ExePath().parent_path().lexically_normal()));
}

// Receipts are bounded per entry/owner; there are no frame-index activity
// claims. Unknown handles have an unknown feature and id, even if their Id
// happens to fall in an OptiScaler numeric range.
void Receipt(const char* entry, Owner owner, const NVSDK_NGX_Handle* handle,
             std::optional<NVSDK_NGX_Feature> feature, std::optional<unsigned> id)
{
    static std::array<std::atomic<unsigned>, 16> counts {};
    const unsigned kind = strcmp(entry, "Create") == 0 ? 0 : strcmp(entry, "Evaluate") == 0 ? 1 : 2;
    if (counts[kind * 4 + static_cast<unsigned>(owner)].fetch_add(1) >= 8)
        return;
    const char* ownerName = owner == Owner::Selected ? "selected" : owner == Owner::Native ? "native" :
                            owner == Owner::Backend ? "backend" : "unknown/native-forward";
    LOG_INFO("ZZZ NGX DX12 entry={} owner={} feature={} handle={:p} id={}", entry, ownerName,
             feature ? std::to_string(static_cast<int>(*feature)) : "unknown", static_cast<const void*>(handle),
             id ? std::to_string(*id) : "unknown");
}

template <class F, class... Args> NVSDK_NGX_Result NativeCall(F function, Args... args)
{
    Depth scope(nativeDepth);
    return function ? function(args...) : NVSDK_NGX_Result_FAIL_FeatureNotSupported;
}
template <class F, class... Args> NVSDK_NGX_Result SelectedCall(F function, Args... args)
{
    Depth scope(selectedDepth);
    return function ? function(args...) : NVSDK_NGX_Result_FAIL_FeatureNotSupported;
}

struct Api
{
    using Handle = NVSDK_NGX_Handle;
    using Feature = NVSDK_NGX_Feature;
    using Result = NVSDK_NGX_Result;
    static bool Success(Result result) { return result == NVSDK_NGX_Result_Success; }
    static Result Ok() { return NVSDK_NGX_Result_Success; }
    static Result Fail() { return NVSDK_NGX_Result_Fail; }
    static unsigned Id(const Handle* handle) { return handle->Id; }
    static bool IsSelectedFeature(Feature feature) { return feature == NVSDK_NGX_Feature_SuperSampling; }
    static Result FinishInputInit(Result result, ID3D12Device* device)
    { return Success(result) && AttachNativeDx12Input(device) ? result : Fail(); }
    static void ProviderReady(bool ready) { NVNGXProxy::SetDx12Inited(ready); }
    static bool AcceptInitDevice(ID3D12Device* device)
    { return device && (!observedInitDevice || observedInitDevice == device) && CanAttachNativeDx12Input(device); }
    static void ObserveInitDevice(ID3D12Device* device)
    {
        if (observedInitDevice == device) return;
        if (observedInitDevice) observedInitDevice->Release();
        observedInitDevice = device;
        device->AddRef();
    }
    static void ForgetInitDevice()
    {
        DetachNativeDx12Input();
        // Only reached after actual successful native Shutdown. The native
        // runtime then owns teardown of a probe whose Destroy had failed.
        pendingProbe = nullptr;
        { std::lock_guard lock(parameterMutex); persistentParameters.clear(); }
        if (observedInitDevice) observedInitDevice->Release();
        observedInitDevice = nullptr;
    }
    static bool AcceptCommandDevice(ID3D12GraphicsCommandList* command)
    {
        if (!command) return false;
        ID3D12Device* device = nullptr;
        if (FAILED(command->GetDevice(IID_PPV_ARGS(&device))) || !device) return false;
        const bool accepted = (!observedInitDevice || observedInitDevice == device) && CanAttachNativeDx12Input(device);
        device->Release();
        return accepted;
    }
    static void Receipt(const char* entry, Owner owner, const Handle* handle,
                        std::optional<Feature> feature, std::optional<unsigned> id)
    { NgxInput::Receipt(entry, owner, handle, feature, id); }

    static bool AttachInput(ID3D12GraphicsCommandList* command)
    {
        if (!AcceptCommandDevice(command)) return false;
        // SDK contract: AllocateParameters may only succeed after NGX Init.
        // This is a CPU-side probe; unlike Create it records no GPU work.
        if (!pendingProbe)
        {
            const auto allocation = NativeCall(native.D3D12_AllocateParameters, &pendingProbe);
            if (!Success(allocation) || !pendingProbe) return false;
        }
        const auto destroyed = NativeCall(native.D3D12_DestroyParameters, pendingProbe);
        if (!Success(destroyed))
        {
            // Vendor ownership is retained; never delete a native table with
            // the C++ allocator, even if the vendor cannot destroy it.
            return false;
        }
        pendingProbe = nullptr;
        ID3D12Device* device = nullptr;
        if (FAILED(command->GetDevice(IID_PPV_ARGS(&device))) || !device) return false;
        const bool attached = AttachNativeDx12Input(device);
        device->Release();
        if (attached)
        {
            ProviderReady(true);
            LOG_INFO("ZZZ NGX DX12 late input attached by native allocation evidence; Init metadata unknown, native init device unknown");
        }
        return attached;
    }
    static Result NativeCreate(ID3D12GraphicsCommandList* command, Feature feature, NVSDK_NGX_Parameter* params,
                               Handle** out)
    { return NativeCall(native.D3D12_CreateFeature, command, feature, params, out); }
    static Result SelectedCreate(ID3D12GraphicsCommandList* command, Feature feature, NVSDK_NGX_Parameter* params,
                                 Handle** out)
    { return SelectedCall(selected.D3D12_CreateFeature, command, feature, params, out); }
    static Result NativeEvaluate(ID3D12GraphicsCommandList* command, const Handle* handle,
                                 const NVSDK_NGX_Parameter* params, PFN_NVSDK_NGX_ProgressCallback callback)
    { return NativeCall(native.D3D12_EvaluateFeature, command, handle, params, callback); }
    static Result SelectedEvaluate(ID3D12GraphicsCommandList* command, const Handle* handle,
                                   const NVSDK_NGX_Parameter* params, PFN_NVSDK_NGX_ProgressCallback callback)
    { return SelectedCall(selected.D3D12_EvaluateFeature, command, handle, params, callback); }
    static Result NativeRelease(Handle* handle) { return NativeCall(native.D3D12_ReleaseFeature, handle); }
    static Result SelectedRelease(Handle* handle)
    {
        if (State::Instance().isShuttingDown) return Fail();
        return SelectedCall(selected.D3D12_ReleaseFeature, handle);
    }
    static void DeleteSelectedHandle(Handle* handle) { delete handle; }
};
Api api;
Dx12Router<Api> router(api);

bool InternalProjectCall(bool project, const void* caller)
{
    return project && selectedDepth && !nativeDepth && ModuleAt(caller) == dllModule;
}

NVSDK_NGX_Result Init(bool project, const void* caller, unsigned long long app, const wchar_t* path,
                      ID3D12Device* device, const NVSDK_NGX_FeatureCommonInfo* info, NVSDK_NGX_Version version)
{
    if (InternalProjectCall(project, caller))
        return SelectedCall(selected.D3D12_Init, app, path, device, info, version);
    if (initReplay)
        return *initReplay;
    if (nativeDepth) return NativeCall(native.D3D12_Init, app, path, device, info, version);
    return router.Init(External(caller) && path && device, device,
        [&] { return NativeCall(native.D3D12_Init, app, path, device, info, version); },
        [&](auto result) {
            Replay replay(initReplay, result);
            return Api::FinishInputInit(SelectedCall(selected.D3D12_Init, app, path, device, info, version), device);
        });
}
NVSDK_NGX_Result InitExt(bool project, const void* caller, unsigned long long app, const wchar_t* path,
                         ID3D12Device* device, NVSDK_NGX_Version version, const NVSDK_NGX_FeatureCommonInfo* info)
{
    if (InternalProjectCall(project, caller))
        return SelectedCall(selected.D3D12_Init_Ext, app, path, device, version, info);
    if (initReplay)
        return *initReplay;
    if (nativeDepth) return NativeCall(native.D3D12_Init_Ext, app, path, device, version, info);
    return router.Init(External(caller) && path && device, device,
        [&] { return NativeCall(native.D3D12_Init_Ext, app, path, device, version, info); },
        [&](auto result) {
            Replay replay(initReplay, result);
            return Api::FinishInputInit(SelectedCall(selected.D3D12_Init_Ext, app, path, device, version, info), device);
        });
}
NVSDK_NGX_Result InitProject(bool project, const void* caller, const char* id, NVSDK_NGX_EngineType engine,
                             const char* engineVersion, const wchar_t* path, ID3D12Device* device,
                             NVSDK_NGX_Version version, const NVSDK_NGX_FeatureCommonInfo* info)
{
    if (InternalProjectCall(project, caller))
        return SelectedCall(selected.D3D12_Init_ProjectID, id, engine, engineVersion, path, device, version, info);
    if (initReplay)
        return *initReplay;
    if (nativeDepth) return NativeCall(native.D3D12_Init_ProjectID, id, engine, engineVersion, path, device, version, info);
    return router.Init(External(caller) && id && engineVersion && path && device, device,
        [&] { return NativeCall(native.D3D12_Init_ProjectID, id, engine, engineVersion, path, device, version, info); },
        [&](auto result) {
            Replay replay(initReplay, result);
            return Api::FinishInputInit(SelectedCall(selected.D3D12_Init_ProjectID, id, engine, engineVersion, path, device, version, info), device);
        });
}
NVSDK_NGX_Result Stop(bool project, const void* caller)
{
    if (shutdownReplay)
        return *shutdownReplay;
    if (nativeDepth) return NativeCall(native.D3D12_Shutdown);
    return router.Shutdown(External(caller), [&] { return NativeCall(native.D3D12_Shutdown); },
        [&](auto result) {
            Replay replay(shutdownReplay, result);
            return SelectedCall(selected.D3D12_Shutdown);
        });
}
NVSDK_NGX_Result StopDevice(bool project, const void* caller, ID3D12Device* device)
{
    if (shutdownReplay)
        return *shutdownReplay;
    if (nativeDepth) return NativeCall(native.D3D12_Shutdown1, device);
    return router.Shutdown(External(caller), [&] { return NativeCall(native.D3D12_Shutdown1, device); },
        [&](auto result) {
            Replay replay(shutdownReplay, result);
            return SelectedCall(selected.D3D12_Shutdown1, device);
        });
}

// Separate entry stubs retain the entry surface as well as the immediate
// caller. In particular a project Init internally calls its own Init_Ext.
#define INIT_STUBS(prefix, project) \
NVSDK_NGX_Result prefix##Init(unsigned long long a, const wchar_t* p, ID3D12Device* d, \
    const NVSDK_NGX_FeatureCommonInfo* i, NVSDK_NGX_Version v) { return Init(project, _ReturnAddress(), a,p,d,i,v); } \
NVSDK_NGX_Result prefix##InitExt(unsigned long long a, const wchar_t* p, ID3D12Device* d, \
    NVSDK_NGX_Version v, const NVSDK_NGX_FeatureCommonInfo* i) { return InitExt(project, _ReturnAddress(), a,p,d,v,i); } \
NVSDK_NGX_Result prefix##InitProject(const char* id, NVSDK_NGX_EngineType e, const char* ev, const wchar_t* p, \
    ID3D12Device* d, NVSDK_NGX_Version v, const NVSDK_NGX_FeatureCommonInfo* i) \
    { return InitProject(project, _ReturnAddress(), id,e,ev,p,d,v,i); } \
NVSDK_NGX_Result prefix##Stop() { return Stop(project, _ReturnAddress()); } \
NVSDK_NGX_Result prefix##StopDevice(ID3D12Device* d) { return StopDevice(project, _ReturnAddress(), d); }
INIT_STUBS(N, false)
INIT_STUBS(P, true)
#undef INIT_STUBS

NVSDK_NGX_Result Create(ID3D12GraphicsCommandList* command, NVSDK_NGX_Feature feature,
                        NVSDK_NGX_Parameter* params, NVSDK_NGX_Handle** out)
{
    // A native core's export-to-export delegation is part of the same Create,
    // not a second backend owner for the external Create's returned pointer.
    if (nativeDepth) return NativeCall(native.D3D12_CreateFeature, command, feature, params, out);
    return router.Create(External(_ReturnAddress()), command, feature, params, out);
}
NVSDK_NGX_Result Evaluate(ID3D12GraphicsCommandList* command, const NVSDK_NGX_Handle* handle,
                          const NVSDK_NGX_Parameter* params, PFN_NVSDK_NGX_ProgressCallback callback)
{ return router.Evaluate(External(_ReturnAddress()), command, handle, params, callback); }
NVSDK_NGX_Result Release(NVSDK_NGX_Handle* handle)
{ return router.Release(External(_ReturnAddress()), handle); }

NVSDK_NGX_Result Parameters(NVSDK_NGX_Parameter** out)
{
    const auto result = NativeCall(native.D3D12_GetParameters, out);
    if (Api::Success(result) && out && *out)
    {
        std::lock_guard lock(parameterMutex);
        persistentParameters.insert(*out);
    }
    return result;
}
NVSDK_NGX_Result Allocate(NVSDK_NGX_Parameter** out)
{ return NativeCall(native.D3D12_AllocateParameters, out); }
NVSDK_NGX_Result Capabilities(NVSDK_NGX_Parameter** out)
{ return NativeCall(native.D3D12_GetCapabilityParameters, out); }
NVSDK_NGX_Result Destroy(NVSDK_NGX_Parameter* params)
{
    if (!nativeDepth)
    {
        std::lock_guard lock(parameterMutex);
        if (persistentParameters.contains(params)) return NVSDK_NGX_Result_FAIL_InvalidParameter;
    }
    return NativeCall(native.D3D12_DestroyParameters, params);
}
NVSDK_NGX_Result Scratch(NVSDK_NGX_Feature feature, const NVSDK_NGX_Parameter* params, size_t* size)
{ return NativeCall(native.D3D12_GetScratchBufferSize, feature, params, size); }

// These are proxy backend exits, never game input. A thread-local scope covers
// native export-to-export calls and callbacks that reenter another hooked API.
NVSDK_NGX_Result BInit(unsigned long long a, const wchar_t* p, ID3D12Device* d,
                       const NVSDK_NGX_FeatureCommonInfo* i, NVSDK_NGX_Version v)
{
    if (initReplay) return *initReplay;
    if (nativeDepth) return NativeCall(native.D3D12_Init, a,p,d,i,v);
    return router.Init(false, d, [&] { return NativeCall(native.D3D12_Init, a,p,d,i,v); }, [](auto r) { return r; });
}
NVSDK_NGX_Result BInitExt(unsigned long long a, const wchar_t* p, ID3D12Device* d,
                          NVSDK_NGX_Version v, const NVSDK_NGX_FeatureCommonInfo* i)
{
    if (initReplay) return *initReplay;
    if (nativeDepth) return NativeCall(native.D3D12_Init_Ext, a,p,d,v,i);
    return router.Init(false, d, [&] { return NativeCall(native.D3D12_Init_Ext, a,p,d,v,i); }, [](auto r) { return r; });
}
NVSDK_NGX_Result BInitProject(const char* id, NVSDK_NGX_EngineType e, const char* ev, const wchar_t* p,
                              ID3D12Device* d, NVSDK_NGX_Version v, const NVSDK_NGX_FeatureCommonInfo* i)
{
    if (initReplay) return *initReplay;
    if (nativeDepth) return NativeCall(native.D3D12_Init_ProjectID, id,e,ev,p,d,v,i);
    return router.Init(false, d, [&] { return NativeCall(native.D3D12_Init_ProjectID, id,e,ev,p,d,v,i); }, [](auto r) { return r; });
}
NVSDK_NGX_Result BStop()
{
    if (shutdownReplay) return *shutdownReplay;
    if (nativeDepth) return NativeCall(native.D3D12_Shutdown);
    return router.Shutdown(false, [&] { return NativeCall(native.D3D12_Shutdown); }, [](auto r) {
        Replay replay(shutdownReplay, r);
        return SelectedCall(selected.D3D12_Shutdown);
    });
}
NVSDK_NGX_Result BStopDevice(ID3D12Device* device)
{
    if (shutdownReplay) return *shutdownReplay;
    if (nativeDepth) return NativeCall(native.D3D12_Shutdown1, device);
    return router.Shutdown(false, [&] { return NativeCall(native.D3D12_Shutdown1, device); }, [&](auto r) {
        Replay replay(shutdownReplay, r);
        return SelectedCall(selected.D3D12_Shutdown1, device);
    });
}
NVSDK_NGX_Result BCreate(ID3D12GraphicsCommandList* command, NVSDK_NGX_Feature feature,
                         NVSDK_NGX_Parameter* params, NVSDK_NGX_Handle** out)
{ return router.Create(false, command, feature, params, out); }
NVSDK_NGX_Result BEvaluate(ID3D12GraphicsCommandList* command, const NVSDK_NGX_Handle* handle,
                           const NVSDK_NGX_Parameter* params, PFN_NVSDK_NGX_ProgressCallback callback)
{ return router.Evaluate(false, command, handle, params, callback); }
NVSDK_NGX_Result BRelease(NVSDK_NGX_Handle* handle) { return router.Release(false, handle); }

#define DX12_ENTRIES(X) \
    X(Init, NInit, PInit, BInit) \
    X(Init_Ext, NInitExt, PInitExt, BInitExt) \
    X(Init_ProjectID, NInitProject, PInitProject, BInitProject) \
    X(Shutdown, NStop, PStop, BStop) \
    X(Shutdown1, NStopDevice, PStopDevice, BStopDevice) \
    X(GetParameters, Parameters, Parameters, Parameters) \
    X(AllocateParameters, Allocate, Allocate, Allocate) \
    X(GetCapabilityParameters, Capabilities, Capabilities, Capabilities) \
    X(DestroyParameters, Destroy, Destroy, Destroy) \
    X(GetScratchBufferSize, Scratch, Scratch, Scratch) \
    X(CreateFeature, Create, Create, BCreate) \
    X(EvaluateFeature, Evaluate, Evaluate, BEvaluate) \
    X(ReleaseFeature, Release, Release, BRelease)

struct Threads
{
    std::vector<HANDLE> handles;
    bool Update()
    {
        if (DetourUpdateThread(GetCurrentThread()) != NO_ERROR) return false;
        const auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snapshot == INVALID_HANDLE_VALUE) return false;
        THREADENTRY32 entry { sizeof(THREADENTRY32) };
        bool ok = Thread32First(snapshot, &entry) != FALSE;
        while (ok)
        {
            if (entry.th32OwnerProcessID == GetCurrentProcessId() && entry.th32ThreadID != GetCurrentThreadId())
            {
                const auto thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT |
                                              THREAD_QUERY_INFORMATION, FALSE, entry.th32ThreadID);
                if (!thread)
                {
                    // A thread that disappeared during enumeration has no IP
                    // to update. Other access failures invalidate the transaction.
                    if (GetLastError() != ERROR_INVALID_PARAMETER) { ok = false; break; }
                }
                else
                {
                    handles.push_back(thread);
                    if (DetourUpdateThread(thread) != NO_ERROR) { ok = false; break; }
                }
            }
            if (!Thread32Next(snapshot, &entry))
            {
                ok = GetLastError() == ERROR_NO_MORE_FILES;
                break;
            }
        }
        CloseHandle(snapshot);
        return ok;
    }
    ~Threads() { for (auto thread : handles) CloseHandle(thread); }
};
}

bool Enabled()
{
    static const bool zzz = GameRouting::IdentifyExecutable(Util::ExePath().filename().c_str()) ==
                            GameRouting::Identity::ZenlessZoneZero;
    return zzz && Config::Instance()->EnableDlssInputs.value_or_default();
}
bool Installed() { return installed.load(std::memory_order_acquire); }
bool OwnsModule(void* module) { return Installed() && (module == native.dll || module == dllModule); }
bool AllowProviderInit() { return !Installed(); }

bool Install(NvngxModule& provider)
{
    if (!Enabled()) return false;
    std::lock_guard lock(installMutex);
    if (Installed()) return provider.dll == native.dll;
    if (!provider.dll || provider.dll == dllModule) return false;
    native = provider;
    selected = {};
    selected.dll = dllModule;
#define RESOLVE(name, n, p, b) \
    selected.D3D12_##name = reinterpret_cast<decltype(selected.D3D12_##name)>( \
        KernelBaseProxy::GetProcAddress_()(dllModule, "NVSDK_NGX_D3D12_" #name));
    DX12_ENTRIES(RESOLVE)
#undef RESOLVE
    if (!native.D3D12_CreateFeature || !native.D3D12_EvaluateFeature || !native.D3D12_ReleaseFeature ||
        !native.D3D12_AllocateParameters || !native.D3D12_GetCapabilityParameters || !native.D3D12_DestroyParameters ||
        (!native.D3D12_Shutdown && !native.D3D12_Shutdown1) ||
        (!native.D3D12_Init && !native.D3D12_Init_Ext && !native.D3D12_Init_ProjectID) ||
        !selected.D3D12_CreateFeature || !selected.D3D12_EvaluateFeature || !selected.D3D12_ReleaseFeature)
    {
        LOG_WARN("ZZZ NGX DX12 hook installation rejected: incomplete native/project table");
        native = {}; selected = {};
        return false;
    }
    const auto nativeBefore = native;
    const auto selectedBefore = selected;
    // Pin image bases, never Detours' private trampoline allocations. A failed
    // pin is an installation failure, not process-lifetime ownership evidence.
    HMODULE pinned = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                           reinterpret_cast<LPCWSTR>(native.dll), &pinned) ||
        !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                           reinterpret_cast<LPCWSTR>(dllModule), &pinned))
    {
        LOG_WARN("ZZZ NGX DX12 hook installation rejected: module pin failed");
        return false;
    }
    if (DetourTransactionBegin() != NO_ERROR) return false;
    Threads threads;
    bool ok = threads.Update();
#define ATTACH(name, n, p, b) \
    if (ok && native.D3D12_##name) ok = DetourAttach(reinterpret_cast<PVOID*>(&native.D3D12_##name), \
                                                    reinterpret_cast<PVOID>(n)) == NO_ERROR; \
    if (ok && selected.D3D12_##name) ok = DetourAttach(reinterpret_cast<PVOID*>(&selected.D3D12_##name), \
                                                      reinterpret_cast<PVOID>(p)) == NO_ERROR;
    DX12_ENTRIES(ATTACH)
#undef ATTACH
    if (!ok)
    {
        DetourTransactionAbort();
        native = nativeBefore; selected = selectedBefore;
        LOG_WARN("ZZZ NGX DX12 hook transaction aborted");
        return false;
    }
    // Threads enlisted above remain suspended through Commit. Publish backend
    // exits before they resume; these slots only call the committed trampolines.
#define BIND(name, n, p, b) \
    if (native.D3D12_##name) provider.D3D12_##name = b;
    DX12_ENTRIES(BIND)
#undef BIND
    const auto result = DetourTransactionCommit();
    if (result != NO_ERROR)
    {
        provider = nativeBefore;
        native = nativeBefore; selected = selectedBefore;
        LOG_WARN("ZZZ NGX DX12 hook commit failed: {}", result);
        return false;
    }
    // Both images were pinned before the transaction. Routing intentionally
    // outlives UnhookApis and every individual external NGX session.
    installed.store(true, std::memory_order_release);
    LOG_INFO("ZZZ NGX DX12 cached native exports and direct proxy inputs attached; pre-hook metadata unknown");
    return true;
}
#undef DX12_ENTRIES
}
