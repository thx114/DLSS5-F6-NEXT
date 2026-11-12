#!/usr/bin/env python3
"""Compile CPU policy tests and the extracted production monitor method only."""
import argparse
from pathlib import Path
import subprocess


PRELUDE = r'''
#include <Windows.h>
#include <unknwn.h>
#include <nvapi.h>
#include <array>
#include <atomic>
#include <mutex>
#include <optional>
#include <string>
#include <cstdio>
#include <cstdlib>
#include <utility>
#include <spdlog/spdlog.h>
#include <runtime/ReflexReportPolicy.h>
#include <runtime/ReflexQueueTiming.h>
static ULONGLONG tick = 1000;
static ULONGLONG TestTick() { return tick; }
#define GetTickCount64 TestTick
static std::string raw;
template<class... Args>
void Log(fmt::format_string<Args...> format, Args&&... args)
{ raw = fmt::format(format, std::forward<Args>(args)...); }
#define LOG_INFO(...) Log(__VA_ARGS__)
#define LOG_WARN(...) Log(__VA_ARGS__)
namespace magic_enum { template<class T> const char* enum_name(T) { return "mock_status"; } }
struct TimingEntry { double position, length; };
struct State {
    uint64_t reflexFrameId = 0;
    static State& Instance() { static State state; return state; }
};
static bool estimated = false;
static bool XefgOutput() { return false; }
namespace fakenvapi { static bool isUsingAsMainNvapi() { return estimated; } }
namespace GpuTimeline {
    enum class Stream : unsigned { Bridge, Nr };
    struct Reading { uint64_t frame = 0, device = 0; double beginUs = 0, endUs = 0; };
    static Reading anchor;
    static std::array<Reading, 2> spans;
    static unsigned reads = 0;
    inline bool Enabled() { return true; }
    inline Reading Anchor(uint64_t frame, IUnknown* device) {
        ++reads;
        return anchor.frame == frame && anchor.device == reinterpret_cast<uintptr_t>(device) ? anchor : Reading{};
    }
    inline Reading Read(Stream stream, uint64_t frame) {
        ++reads;
        const auto& r = spans[unsigned(stream)]; return r.frame == frame ? r : Reading{};
    }
}
static NV_LATENCY_RESULT_PARAMS d3d {};
static NV_VULKAN_LATENCY_RESULT_PARAMS vk {};
static NvAPI_Status fixtureApiStatus = NVAPI_OK;
static std::atomic<uint64_t> _lastFrameId[20] {};
static std::atomic<IUnknown*> _lastDev[20] {};
'''

CLASS_BODY = r'''
    inline static bool _inited = true;
    inline static IUnknown* _lastSleepDev = reinterpret_cast<IUnknown*>(7);
    inline static IUnknown* _ownSleepDev = nullptr;
    inline static HANDLE _lastVkSleepDev = nullptr;
    inline static bool o_NvAPI_D3D_GetLatency = true, o_NvAPI_Vulkan_GetLatency = true;
    inline static std::atomic<bool> _timingEstimated = false;
    inline static std::recursive_mutex _sleepModeMutex;
    static bool bridgedReflexForwarding() { return false; }
    static NvAPI_Status hkNvAPI_D3D_GetLatency(IUnknown*, NV_LATENCY_RESULT_PARAMS* out) {
        *out = d3d; return fixtureApiStatus;
    }
    static NvAPI_Status hkNvAPI_Vulkan_GetLatency(HANDLE, NV_VULKAN_LATENCY_RESULT_PARAMS* out) {
        *out = vk; return fixtureApiStatus;
    }
    static bool updateTimingData(TimingReportSnapshot& snapshot);
};
'''

CASES = r'''
static unsigned checks = 0;
static void Check(bool ok, const char* message) {
    ++checks;
    if (!ok) { std::fprintf(stderr, "FAIL %u: %s\n", checks, message); std::exit(1); }
}
template<class Report> static void Fill(Report& report, uint64_t frame, uint64_t begin) {
    report = {}; report.frameID = frame;
    report.simStartTime = begin; report.simEndTime = begin + 100;
    report.renderSubmitStartTime = begin + 100; report.renderSubmitEndTime = begin + 200;
    report.osRenderQueueStartTime = begin + 300; report.osRenderQueueEndTime = begin + 1000;
    report.gpuRenderStartTime = begin + 600; report.gpuRenderEndTime = begin + 1000;
}
int main() {
    ReflexHooks::TimingReportSnapshot snapshot;
    Fill(d3d.frameReport[62], 10, 10000); Fill(d3d.frameReport[63], 11, 12000);
    GpuTimeline::anchor = {10, 7, 5000, 5002};
    GpuTimeline::spans = {{{10, 7, 5101, 5301}, {10, 7, 5301, 5401}}};
    Check(ReflexHooks::updateTimingData(snapshot), "latest report accepted");
    Check(snapshot.frameId == 11, "older paired report cannot win actual hook selection");
    Check(!snapshot.nrGpuMs && !snapshot.bridgeGpuMs, "old enrichment absent on latest snapshot");
    Check(snapshot.timingData[TimeRange]->length == 1000, "raw baseline range selected");
    Check(raw.find("report_index=63 newest_index=63") != std::string::npos, "selection metadata logged");
    Check(raw.find("physical_queue=UNKNOWN display_owner=UNKNOWN") != std::string::npos,
          "raw metadata does not infer physical queue or display owner");

    tick = 1600;
    GpuTimeline::anchor = {11, 7, 5000, 5002};
    GpuTimeline::spans = {{{11, 7, 5101, 5301}, {11, 7, 5301, 7301}}};
    Check(ReflexHooks::updateTimingData(snapshot), "report remains available with unproven matching metadata");
    Check(snapshot.timingData[TimeRange]->length == 1000, "enrichment cannot extend actual Reflex baseline");
    Check(!snapshot.bridgeGpu && !snapshot.bridgeGpuMs && !snapshot.nrGpuMs && !snapshot.nrGpu,
          "same ID/device without report-instance receipt stays unknown");
    Check(raw.find("range_ms=1.0000") != std::string::npos &&
          raw.find("report_begin_us=12000 report_end_us=13000") != std::string::npos,
          "raw range remains driver-only after enrichment");
    Check(raw.find("report_unchanged_ms=600") != std::string::npos,
          "enrichment arrival did not refresh report age");

    d3d.frameReport[62] = d3d.frameReport[63];
    Fill(d3d.frameReport[63], 11, 14000); tick = 2200;
    Check(ReflexHooks::updateTimingData(snapshot) && snapshot.frameId == 11,
          "same ID with different submit/endpoints selects newest valid completion");
    Check(!snapshot.bridgeGpuMs && !snapshot.nrGpuMs && !snapshot.bridgeGpu && !snapshot.nrGpu,
          "same device/ID new report cannot reuse old anchor/span within buffer or across polls");
    Check(raw.find("report_begin_us=14000 report_end_us=15000") != std::string::npos &&
          raw.find("enrichment=unknown_report_instance") != std::string::npos,
          "new report endpoints and absent instance evidence logged");
    d3d.frameReport[63].renderSubmitStartTime += 10;
    d3d.frameReport[63].renderSubmitEndTime += 10; tick = 2600;
    Check(ReflexHooks::updateTimingData(snapshot) && !snapshot.nrGpuMs && !snapshot.bridgeGpuMs,
          "same ID and same outer range with changed submit cannot revive old GPU cache");
    Check(GpuTimeline::reads == 0, "unproven GPU caches are never read for report enrichment");

    tick = 4201;
    Check(!ReflexHooks::updateTimingData(snapshot), "unchanged report expires in actual hook");
    Check(snapshot.frameId == 0 && !snapshot.timingData[TimeRange] && !snapshot.nrGpuMs,
          "stale result clears entire published snapshot without retrying older report");
    d3d.frameReport[63] = {}; tick = 4300;
    Check(ReflexHooks::updateTimingData(snapshot) && snapshot.frameId == 11,
          "invalid newest slot allows next latest valid completion");
    Check(raw.find("skipped_invalid_slots=1") != std::string::npos, "invalid-slot fallback explained");

    d3d.frameReport[63] = {}; d3d.frameReport[63].frameID = 12;
    d3d.frameReport[63].simStartTime = 20000; d3d.frameReport[63].simEndTime = 20100;
    Check(ReflexHooks::updateTimingData(snapshot) && snapshot.frameId == 12, "marker-only latest report kept");
    Check(!snapshot.beforeGpuQueueMs && !snapshot.timingData[OsRenderQueue] && !snapshot.timingData[GpuRender],
          "missing endpoints stay unknown in published snapshot");
    Fill(d3d.frameReport[63], 13, 22000);
    d3d.frameReport[63].gpuRenderStartTime = d3d.frameReport[63].osRenderQueueStartTime - 1;
    Check(ReflexHooks::updateTimingData(snapshot), "non-nested report retains independent intervals");
    Check(!snapshot.beforeGpuQueueMs && snapshot.timingData[OsRenderQueue] && snapshot.timingData[GpuRender],
          "non-nested queue wait remains unknown, not clamped to zero");
    Fill(d3d.frameReport[63], 14, 24000);
    d3d.frameReport[63].osRenderQueueEndTime = UINT64_MAX;
    d3d.frameReport[63].gpuRenderEndTime = UINT64_MAX;
    Check(ReflexHooks::updateTimingData(snapshot), "valid marker pairs survive invalid GPU endpoints");
    Check(!snapshot.beforeGpuQueueMs && !snapshot.timingData[OsRenderQueue] && !snapshot.timingData[GpuRender],
          "sentinel endpoints cannot leak into queue split or timing bars");
    fixtureApiStatus = NVAPI_ERROR;
    Check(!ReflexHooks::updateTimingData(snapshot) && snapshot.frameId == 0, "API failure clears prior snapshot");
    fixtureApiStatus = NVAPI_OK; d3d = {};
    Check(!ReflexHooks::updateTimingData(snapshot) && !snapshot.timingData[TimeRange], "empty API response stays unknown");

    ReflexHooks::_lastSleepDev = nullptr; ReflexHooks::_lastVkSleepDev = reinterpret_cast<HANDLE>(8);
    Fill(vk.frameReport[62], 1, 30000); Fill(vk.frameReport[63], 2, 32000);
    Check(ReflexHooks::updateTimingData(snapshot) && snapshot.frameId == 2, "Vulkan uses same actual selection policy");
    estimated = true;
    Check(ReflexHooks::updateTimingData(snapshot) && snapshot.estimated && !snapshot.beforeGpuQueueMs &&
          !snapshot.nrGpuMs && !snapshot.gpuActiveRenderTimeUs, "estimated route cannot publish measured queue/enrichment");
    ReflexHooks::_inited = false;
    Check(!ReflexHooks::updateTimingData(snapshot) && snapshot.frameId == 0, "uninitialized hook clears snapshot");
    std::printf("Reflex production monitor: %u checks passed; extracted method, mocked APIs/clock/GPU metadata, CPU only\n", checks);
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, required=True, help='Isolated test output directory')
    parser.add_argument('--devcmd', type=Path, default=Path('D:/VS2022BuildTools/Common7/Tools/VsDevCmd.bat'))
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[3]
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    source = (root / 'OptiScaler/hooks/Reflex_Hooks.cpp').read_text(encoding='utf-8-sig')
    header = (root / 'OptiScaler/hooks/Reflex_Hooks.h').read_text(encoding='utf-8-sig')
    enum_start = header.index('enum TimingType')
    enum_end = header.index('\n};', enum_start) + len('\n};')
    snapshot_start = header.index('    struct TimingReportSnapshot')
    snapshot_end = header.index('\n    };', snapshot_start) + len('\n    };')
    method_start = source.index('#define UPDATE_TIMING_ENTRY')
    method_end = source.index('// For updating information about Reflex hooks', method_start)
    fixture = out / 'ReflexReportHookExtracted.cpp'
    fixture.write_text(PRELUDE + header[enum_start:enum_end] + '\nclass ReflexHooks { public:\n' +
                       header[snapshot_start:snapshot_end] + CLASS_BODY +
                       source[method_start:method_end] + CASES, encoding='utf-8')
    suites = [root / 'OptiScaler/runtime/tests/ReflexReportPolicyTests.cpp',
              root / 'OptiScaler/runtime/tests/ReflexQueueTimingTests.cpp', fixture]
    for test in suites:
        exe = out / (test.stem + '.exe')
        includes = [root / 'OptiScaler', root / 'external/nvapi', root / 'external/spdlog/include']
        command = f'call "{args.devcmd}" -no_logo -arch=x64 -host_arch=x64 && cl /nologo /utf-8 /std:c++20 /EHsc /W4 /WX /Od '
        command += ' '.join(f'/I"{path}"' for path in includes)
        command += f' /Fe:"{exe}" /Fo:"{out / (test.stem + ".obj")}" "{test}"'
        # cmd.exe consumes its own quote grammar; list2cmdline would insert
        # backslash-escaped quotes into the batch invocation on Windows.
        for name, invocation in [('compile', f'cmd.exe /d /s /c "{command}"'), ('result', [str(exe)])]:
            completed = subprocess.run(invocation, cwd=out, capture_output=True, text=True, encoding='oem', errors='replace')
            log = completed.stdout + completed.stderr
            (out / f'{test.stem}-{name}.log').write_text(log, encoding='utf-8')
            print(log, end='', flush=True)
            if completed.returncode:
                raise SystemExit(completed.returncode)
    print(f'Isolated CPU evidence: {out}')


if __name__ == '__main__':
    main()
