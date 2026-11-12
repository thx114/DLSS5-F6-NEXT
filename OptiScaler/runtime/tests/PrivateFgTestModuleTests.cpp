#include "../../../tests/private-fg-test/Harness.h"
#include <framegen/dlssg/DlssgOptionsPolicy.h>
#include <framegen/dlssg/DlssgProviderIdentity.h>
#include <framegen/dlssg/DlssgTelemetry.h>
#include <cstdio>
#include <exception>
#include <limits>
#include <thread>

// Compile the actual default branch with the product switch absent. The other
// translation unit compiles the private branch; neither copies product logic.
#ifdef F6_PRIVATE_FG_TEST_MODULE
#error Compile this suite without a global F6_PRIVATE_FG_TEST_MODULE definition
#endif
#define framegen PrivateFgDefaultBuild
#include "../../../tests/private-fg-test/ProductionSeams.h"
#include "../../framegen/dlssg/PrivateFgTestModule.cpp"
#include "../../../tests/private-fg-test/EndProductionSeams.h"
#undef framegen
#undef F6_PRIVATE_FG_TEST_MODULE

namespace
{
using PrivateFgFixture::Check;

void DefaultBuild()
{
    namespace Module = PrivateFgDefaultBuild::PrivateFgTestModule;
    Check(!Module::Enabled(), "default build is disabled");
    Check(Module::CurrentStatus() == Module::Status::Disabled, "default build starts disabled");
    Check(!Module::ProviderPending(), "default build never defers provider");
    Check(!Module::OwnsAsiName(L"MFGUnlocker.asi"), "default build does not claim ASI names");
    Module::Prepare();
    Module::StartupWorkerUnavailable();
    Check(Module::AwaitGraphicsStartup() && Module::AwaitGraphicsStartup(0),
          "default graphics startup succeeds with default or zero timeout");
    std::atomic_bool ready { true };
    std::vector<std::jthread> callers;
    for (unsigned i = 0; i < 16; ++i)
        callers.emplace_back([&] {
            for (unsigned j = 0; j < 16; ++j)
            {
                Module::Prepare();
                Module::Initialize();
                Module::StartupWorkerUnavailable();
                if (!Module::AwaitGraphicsStartup(0)) ready = false;
            }
        });
    for (auto& caller : callers) caller.join();
    Check(ready, "default Prepare/Initialize/worker-failure/Await remain no-ops under concurrent calls");
    Check(Module::CurrentStatus() == Module::Status::Disabled, "default Initialize remains disabled");
    Check(!Module::ProviderPending(), "default Initialize never leaves provider pending");
    const auto& s = PrivateFgFixture::state;
    Check(s.fileNameCalls == 0 && s.fileOpenCalls == 0 && s.fileReadCalls == 0 &&
          s.snapshotCalls == 0 && s.firstModuleCalls == 0 &&
          s.nextModuleCalls == 0 && s.finishHashCalls == 0 &&
          s.loadCalls == 0 && s.resolveCalls == 0 && s.initCalls == 0 &&
          s.ntdllModuleCalls == 0 && s.rtlLookups == 0 && s.rtlCalls == 0 &&
          s.memoryReadCalls == 0 && s.memoryQueryCalls == 0 && s.processFlagsReadCalls == 0 && s.pebReadCalls == 0 &&
          s.lockReadCalls == 0 && s.debugReadCalls == 0 && !s.badMemoryArguments,
          "default startup and worker-failure APIs perform no IO/load/RTL/memory-proof work");
}

void ProviderPaths()
{
    for (const auto* path : {
             L"nvngx_dlssg.dll", L"NVNGX_DLSSG.DLL", L"D:\\Streamline\\NvNgX_DlSsG.DlL",
             L"d:/cache/models/dlssg/versions/7/provider.bin",
             L"D:\\Cache\\MoDeLs\\DlSsG\\Versions\\7\\provider.bin",
             L"D:\\Cache\\\\Models\\\\Dlssg\\\\Versions\\7\\provider.bin",
             L"D://Cache///Models//Dlssg///Versions//7/provider.bin",
             L"D:/Cache\\\\Models//DLSSG\\\\Versions/7/provider.bin" })
        Check(framegen::IsDlssgProviderPath(path), "provider basename or normalized OTA path accepted");
    for (const auto* path : {
             L"", L"nvngx_dlss.dll", L"nvngx_dlssd.dll", L"sl.dlss_g.dll", L"nvngx_dlssg.dll.bak",
             L"not_nvngx_dlssg.dll", L"D:/models/dlss/versions/provider.bin",
             L"D:/models/dlssg2/versions/provider.bin", L"D:/other_models/dlssg/provider.bin",
             L"D:/models/dlssg", L"D:/models//dlss//provider.bin" })
        Check(!framegen::IsDlssgProviderPath(path), "unrelated provider paths rejected");
}

void TelemetryIntervals()
{
    namespace Telemetry = framegen::DlssgTelemetry;
    const auto& counters = Telemetry::Get();
    Check(!counters.optionsSeen.load() && !counters.stateSeen.load(), "telemetry starts unseen");
    const dlssg::options::ProviderOwner owner {
        1, 1, 0x100, 0x200, 0x300, 0x400, 0x500, 0x600, 0x700, 0, 1920, 1080 };
    dlssg::options::GenerationState capability;
    sl::DLSSGState sdk {};
    sdk.numFramesToGenerateMax = 2;
    sdk.bIsDynamicMFGSupported = sl::eFalse;
    Telemetry::RecordSetOptions(5, 2, true, 0);
    std::vector<std::jthread> writers;
    for (const unsigned count : { 0u, 3u, 5000u, 1u })
        writers.emplace_back([count] { Telemetry::RecordState(count); });
    for (auto& writer : writers) writer.join();
    Check(counters.maxPresented.load() == 5000, "concurrent interval writers preserve the observed maximum");
    Check(capability.Maximum(owner) == 0, "telemetry cannot create capability before SDK observation");
    unsigned maximum = 5000;
    for (const unsigned interval : { 0u, 1u, 2u, 7u, 1000u, (std::numeric_limits<unsigned>::max)(), 0u })
    {
        sdk.numFramesActuallyPresented = interval;
        capability.Observe(owner, sdk, sl::Result::eOk, true);
        Telemetry::RecordState(interval);
        maximum = (std::max)(maximum, interval);
        Check(counters.stateSeen.load(std::memory_order_acquire), "interval publication is visible");
        Check(counters.presented.load() == interval && counters.maxPresented.load() == maximum,
              "interval count is retained without clamping or multiplier inference");
        Check(capability.Maximum(owner) == 2 && capability.Select(owner, 5, true).attemptedGenerated == 2 &&
              !capability.Select(owner, 5, true).dynamic && !capability.acceptedKnown && capability.FixedTotal() == 0,
              "interval count cannot raise SDK capability or certify applied options");
        Check(counters.requested.load() == 5 && counters.sent.load() == 2 && counters.active.load() &&
              counters.result.load() == 0, "state observation cannot rewrite set-options telemetry");
    }
    capability.Observe(owner, sdk, sl::Result::eErrorInvalidState, true);
    Telemetry::RecordState(2000);
    Check(capability.Maximum(owner) == 0 && !capability.Select(owner, 5, false),
          "telemetry cannot turn failed SDK capability into support");
    Telemetry::RecordSetOptions(4, 1, false, 17);
    Check(counters.optionsSeen.load(std::memory_order_acquire) && counters.requested.load() == 4 &&
          counters.sent.load() == 1 && !counters.active.load() && counters.result.load() == 17,
          "options telemetry preserves actual request/submission/result");
    Check(counters.presented.load() == 2000 && counters.maxPresented.load() == maximum,
          "options telemetry does not reset interval observations");
}
}

int wmain(int argc, wchar_t** argv)
{
    try
    {
        DefaultBuild();
        // Child processes run one private scenario; parent runs shared contracts
        // once before orchestrating all scenarios through the same executable.
        if (argc == 1)
        {
            ProviderPaths();
            TelemetryIntervals();
        }
        return PrivateFgFixture::Run(argc, argv);
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "PrivateFgTestModule FAIL: %s\n", error.what());
        return 1;
    }
}
