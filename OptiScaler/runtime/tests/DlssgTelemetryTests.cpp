#include <framegen/dlssg/DlssgOptionsPolicy.h>
#include <framegen/dlssg/DlssgProviderIdentity.h>
#include <framegen/dlssg/DlssgTelemetry.h>
#include <algorithm>
#include <cstdio>
#include <exception>
#include <limits>
#include <stdexcept>
#include <thread>
#include <vector>

namespace
{
unsigned checks = 0;
void Check(bool result, const char* message)
{
    ++checks;
    if (!result) throw std::runtime_error(message);
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

int main()
{
    try
    {
        ProviderPaths();
        TelemetryIntervals();
        std::printf("DlssgTelemetry PASS: %u checks\n", checks);
        return 0;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "DlssgTelemetry FAIL: %s\n", error.what());
        return 1;
    }
}
