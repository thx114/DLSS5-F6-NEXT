#include "../NativeDx12Router.h"
#include <cstdio>
#include <functional>
#include <stdexcept>
#include <unordered_set>

// The adapter is extracted at test time. Module lookup, SDK calls and device
// ownership are simulated; no DLL loading, SDK, GPU or exported stub is tested.
using NVSDK_NGX_Result = int;
using NVSDK_NGX_Version = int;
using NVSDK_NGX_EngineType = int;
struct NVSDK_NGX_FeatureCommonInfo {};
struct ID3D12Device {};
struct NVSDK_NGX_Handle { unsigned Id; };
struct NVSDK_NGX_Parameter {};
constexpr int NVSDK_NGX_Result_Success = 1;
constexpr int NVSDK_NGX_Result_Fail = -1;
constexpr int NVSDK_NGX_Result_FAIL_FeatureNotSupported = -2;
constexpr int NVSDK_NGX_Result_FAIL_InvalidParameter = -3;

namespace NgxInput
{
thread_local unsigned nativeDepth = 0, nativeInitDepth = 0, nativeStopDepth = 0, selectedDepth = 0;
thread_local std::optional<NVSDK_NGX_Result> initReplay, shutdownReplay;
const void* const vendorModule = reinterpret_cast<const void*>(1);
const void* const appModule = reinterpret_cast<const void*>(2);
const void* dllModule = reinterpret_cast<const void*>(3);
const void* currentCaller = appModule;
const void* ModuleAt(const void* caller) { return caller; }
bool External(const void* caller) { return caller == appModule && !nativeDepth && !selectedDepth; }

using InitFn = int (*)(unsigned long long, const wchar_t*, ID3D12Device*, const NVSDK_NGX_FeatureCommonInfo*, int);
using InitExtFn = int (*)(unsigned long long, const wchar_t*, ID3D12Device*, int, const NVSDK_NGX_FeatureCommonInfo*);
using InitProjectFn = int (*)(const char*, int, const char*, const wchar_t*, ID3D12Device*, int, const NVSDK_NGX_FeatureCommonInfo*);
struct Module
{
    const void* dll = vendorModule;
    InitFn D3D12_Init = nullptr;
    InitExtFn D3D12_Init_Ext = nullptr;
    InitProjectFn D3D12_Init_ProjectID = nullptr;
    int (*D3D12_Shutdown)() = nullptr;
    int (*D3D12_Shutdown1)(ID3D12Device*) = nullptr;
    int (*D3D12_DestroyParameters)(NVSDK_NGX_Parameter*) = nullptr;
};
Module native, selected;
std::function<int()> onInit, onInitExt, onStop, onStopDevice, onEvaluate;
unsigned initCalls = 0, initExtCalls = 0, stopCalls = 0, stopDeviceCalls = 0, evaluateCalls = 0;
unsigned destroyCalls = 0;
std::mutex parameterMutex;
std::unordered_set<NVSDK_NGX_Parameter*> persistentParameters;
ID3D12Device* observed = nullptr;
bool providerReady = false;

int VendorInit(unsigned long long, const wchar_t*, ID3D12Device*, const NVSDK_NGX_FeatureCommonInfo*, int)
{ ++initCalls; return onInit ? onInit() : NVSDK_NGX_Result_Success; }
int VendorInitExt(unsigned long long, const wchar_t*, ID3D12Device*, int, const NVSDK_NGX_FeatureCommonInfo*)
{ ++initExtCalls; return onInitExt ? onInitExt() : NVSDK_NGX_Result_Success; }
int VendorInitProject(const char*, int, const char*, const wchar_t*, ID3D12Device*, int, const NVSDK_NGX_FeatureCommonInfo*)
{ return NVSDK_NGX_Result_Success; }
int VendorStop() { ++stopCalls; return onStop ? onStop() : NVSDK_NGX_Result_Success; }
int VendorStopDevice(ID3D12Device*) { ++stopDeviceCalls; return onStopDevice ? onStopDevice() : NVSDK_NGX_Result_Success; }
int VendorDestroy(NVSDK_NGX_Parameter*) { ++destroyCalls; return NVSDK_NGX_Result_Success; }

struct Api
{
    using Handle = NVSDK_NGX_Handle;
    using Feature = int;
    using Result = int;
    static bool Success(int result) { return result == NVSDK_NGX_Result_Success; }
    static int Fail() { return NVSDK_NGX_Result_Fail; }
    static int FinishInputInit(int result, ID3D12Device*) { return result; }
    static bool AcceptInitDevice(ID3D12Device* device) { return device && (!observed || observed == device); }
    static bool AcceptShutdownDevice(ID3D12Device* device) { return device && (!observed || observed == device); }
    static void ObserveInitDevice(ID3D12Device* device) { observed = device; }
    static void ForgetInitDevice() { observed = nullptr; }
    static void ProviderReady(bool ready) { providerReady = ready; }
    static unsigned Id(const Handle* handle) { return handle->Id; }
    static bool AcceptCommandDevice(int) { return true; }
    static void Receipt(const char*, Owner, const Handle*, std::optional<int>, std::optional<unsigned>) {}
    static int NativeEvaluate(int, const Handle*, int, int);
    static int SelectedEvaluate(int, const Handle*, int, int) { return Fail(); }
};
Api api;
Dx12Router<Api> router(api);

#define _ReturnAddress() currentCaller
#include "production-lifecycle.inc"
#undef _ReturnAddress

int Api::NativeEvaluate(int, const Handle*, int, int)
{
    return NativeCall(+[]() -> int { ++evaluateCalls; return onEvaluate ? onEvaluate() : NVSDK_NGX_Result_Success; });
}
}

unsigned checks = 0;
void Check(bool condition, const char* message)
{
    ++checks;
    if (!condition) throw std::runtime_error(message);
}

int main() try
{
    using namespace NgxInput;
    ID3D12Device device;
    native.D3D12_Init = VendorInit;
    native.D3D12_Init_Ext = VendorInitExt;
    native.D3D12_Init_ProjectID = VendorInitProject;
    native.D3D12_Shutdown = VendorStop;
    native.D3D12_Shutdown1 = VendorStopDevice;
    native.D3D12_DestroyParameters = VendorDestroy;
    auto init = [&](const void* caller) { return Init(false, caller, 1, L"path", &device, nullptr, 0); };
    auto ext = [&](const void* caller) { return InitExt(false, caller, 1, L"path", &device, 0, nullptr); };
    NVSDK_NGX_Parameter persistent;
    persistentParameters.insert(&persistent);

    onEvaluate = [&] {
        const auto before = stopCalls;
        const auto beforeInit = initCalls;
        const auto beforeExt = initExtCalls;
        Check(init(appModule) == NVSDK_NGX_Result_Fail, "app Init rejected during Evaluate");
        Check(ext(appModule) == NVSDK_NGX_Result_Fail, "app InitExt rejected during Evaluate");
        Check(init(vendorModule) == NVSDK_NGX_Result_Fail, "vendor caller without Init depth rejected during Evaluate");
        Check(initCalls == beforeInit && initExtCalls == beforeExt, "Evaluate callback did not enter vendor Init");
        Check(Stop(false, appModule) == NVSDK_NGX_Result_Fail, "app Stop rejected during Evaluate");
        Check(stopCalls == before, "Evaluate callback did not enter vendor Shutdown");
        Check(StopDevice(false, appModule, &device) == NVSDK_NGX_Result_Fail, "app StopDevice rejected during Evaluate");
        Check(stopDeviceCalls == 0, "Evaluate callback did not enter vendor Shutdown1");
        currentCaller = appModule;
        Check(Destroy(&persistent) == NVSDK_NGX_Result_FAIL_InvalidParameter,
              "app callback cannot destroy persistent native parameters");
        Check(destroyCalls == 0 && persistentParameters.contains(&persistent), "rejected destroy retains parameter");
        currentCaller = vendorModule;
        Check(Destroy(&persistent) == NVSDK_NGX_Result_Success,
              "vendor parameter destroy delegates while native call is active");
        Check(destroyCalls == 1 && !persistentParameters.contains(&persistent), "successful vendor destroy removes record");
        currentCaller = appModule;
        return NVSDK_NGX_Result_Success;
    };
    Check(router.Evaluate(false, 0, static_cast<const NVSDK_NGX_Handle*>(nullptr), 0, 0) == NVSDK_NGX_Result_Success,
          "native Evaluate completed");
    onEvaluate = {};
    Check(evaluateCalls == 1, "one native Evaluate");

    onStop = [&] {
        const auto before = initCalls;
        Check(init(appModule) == NVSDK_NGX_Result_Fail, "app Init rejected during Shutdown");
        Check(initCalls == before, "Shutdown callback did not enter vendor Init");
        Check(ext(appModule) == NVSDK_NGX_Result_Fail, "app InitExt rejected during Shutdown");
        Check(initExtCalls == 0, "Shutdown callback did not enter vendor InitExt");
        return NVSDK_NGX_Result_Success;
    };
    Check(Stop(false, appModule) == NVSDK_NGX_Result_Success, "Shutdown completed");
    onStop = {};

    onStop = [&] {
        const auto before = stopDeviceCalls;
        Check(StopDevice(false, appModule, &device) == NVSDK_NGX_Result_Fail, "app Shutdown1 rejected during vendor Shutdown");
        Check(StopDevice(false, nullptr, &device) == NVSDK_NGX_Result_Fail, "unknown Shutdown1 caller rejected during vendor Shutdown");
        Check(stopDeviceCalls == before, "unproven Shutdown1 callers did not enter vendor");
        return StopDevice(false, vendorModule, &device);
    };
    Check(Stop(false, appModule) == NVSDK_NGX_Result_Success && stopDeviceCalls == 1,
          "vendor Shutdown delegates to Shutdown1");
    onStop = {};
    onStopDevice = [&] { return Stop(false, vendorModule); };
    const auto previousStops = stopCalls;
    Check(StopDevice(false, appModule, &device) == NVSDK_NGX_Result_Success && stopCalls == previousStops + 1,
          "vendor Shutdown1 delegates to Shutdown");
    onStopDevice = {};

    onInit = [&] {
        const auto before = initExtCalls;
        Check(ext(appModule) == NVSDK_NGX_Result_Fail, "app InitExt rejected during vendor Init");
        Check(ext(nullptr) == NVSDK_NGX_Result_Fail, "unknown InitExt caller rejected during vendor Init");
        Check(initExtCalls == before, "unproven InitExt callers did not enter vendor");
        return ext(vendorModule);
    };
    Check(init(appModule) == NVSDK_NGX_Result_Success && initExtCalls == 1,
          "vendor Init delegates to InitExt");
    onInit = {};
    onInitExt = [&] { return init(vendorModule); };
    const auto previousInits = initCalls;
    Check(ext(appModule) == NVSDK_NGX_Result_Success && initCalls == previousInits + 1,
          "vendor InitExt delegates to Init");
    onInitExt = {};

    onStop = [] { return -7; };
    Check(Stop(false, appModule) == -7, "vendor shutdown failure propagated");
    onStop = {};
    Check(init(appModule) == NVSDK_NGX_Result_Success, "Init admission restored after failed shutdown");
    Check(Stop(false, appModule) == NVSDK_NGX_Result_Success, "Shutdown admission restored after failure");
    onInit = [] { return -8; };
    Check(init(appModule) == -8, "vendor init failure propagated");
    onInit = {};
    Check(init(appModule) == NVSDK_NGX_Result_Success, "Init admission restored after failed init");
    Check(Stop(false, appModule) == NVSDK_NGX_Result_Success, "Shutdown admitted after failed init");
    {
        selectedDepth = 1;
        Replay replay(initReplay, NVSDK_NGX_Result_Success);
        Check(init(dllModule) == NVSDK_NGX_Result_Success, "project Init replay accepted");
        Check(init(appModule) == NVSDK_NGX_Result_Fail, "app Init cannot borrow replay");
        selectedDepth = 0;
    }
    {
        selectedDepth = 1;
        Replay replay(shutdownReplay, NVSDK_NGX_Result_Success);
        Check(Stop(false, dllModule) == NVSDK_NGX_Result_Success, "project Shutdown replay accepted");
        Check(StopDevice(false, appModule, &device) == NVSDK_NGX_Result_Fail, "app Shutdown1 cannot borrow replay");
        selectedDepth = 0;
    }
    Check(!nativeDepth && !nativeInitDepth && !nativeStopDepth && !selectedDepth, "all depth counters restored");
    std::printf("PASS %u lifecycle checks (mock SDK and module identity)\n", checks);
    return 0;
}
catch (const std::exception& error)
{
    std::fprintf(stderr, "FAIL lifecycle check %u: %s\n", checks, error.what());
    return 1;
}
