// SPDX-License-Identifier: GPL-3.0-or-later
#include <Windows.h>
#include <nvapi.h>
#include <runtime/ReflexReportPolicy.h>
#include <runtime/ReflexQueueTiming.h>
#include <cstdio>
#include <cstdlib>

namespace
{
unsigned checks = 0;
void Check(bool condition, const char* message)
{
    ++checks;
    if (!condition) { std::fprintf(stderr, "FAIL %u: %s\n", checks, message); std::exit(1); }
}
template<class Report>
void Fill(Report& report, uint64_t frame, uint64_t begin)
{
    report = {};
    report.frameID = frame;
    report.simStartTime = begin; report.simEndTime = begin + 100;
    report.renderSubmitStartTime = begin + 100; report.renderSubmitEndTime = begin + 200;
    report.presentStartTime = begin + 200; report.presentEndTime = begin + 250;
    report.driverStartTime = begin + 100; report.driverEndTime = begin + 300;
    report.osRenderQueueStartTime = begin + 300; report.osRenderQueueEndTime = begin + 1000;
    report.gpuRenderStartTime = begin + 600; report.gpuRenderEndTime = begin + 1000;
}
template<class Results>
void SelectionCases()
{
    using namespace ReflexReportPolicy;
    Results results {};
    auto& rows = results.frameReport;
    Check(!LatestCompleted(rows), "empty NVAPI array is unknown");
    Fill(rows[62], 10, 10000);
    Fill(rows[63], 11, 12000);
    auto selected = LatestCompleted(rows);
    Check(selected && selected->index == 63 && rows[selected->index].frameID == 11,
          "newest valid completion selected");

    // Optional GPU data is not an input to the production selection policy.
    // The extracted hook fixture also supplies real old anchor/span metadata.
    for (unsigned poll = 0; poll < 8; ++poll)
    {
        selected = LatestCompleted(rows);
        Check(selected && selected->index == 63, "repeated polls cannot prefer old paired frame");
    }
    Check(selected->range.begin == 12000 && selected->range.end == 13000,
          "latest baseline endpoints remain its own");

    rows[63].frameID = 0;
    selected = LatestCompleted(rows);
    Check(selected && selected->index == 62 && selected->newestIndex == 63 && selected->newestFrame == 0,
          "zero frame ID skipped and selection metadata retained");
    rows[63] = {}; rows[63].frameID = 11;
    Check(LatestCompleted(rows)->index == 62, "missing endpoints skipped");
    rows[63].simStartTime = 20; rows[63].simEndTime = 10;
    Check(LatestCompleted(rows)->index == 62, "reversed-only report skipped");
    rows[63].simStartTime = 20; rows[63].simEndTime = UINT64_MAX;
    Check(LatestCompleted(rows)->index == 62, "sentinel-only report skipped");
    rows[63].simEndTime = 20;
    Check(LatestCompleted(rows)->index == 62, "zero total range skipped");

    // Marker-only reports remain usable. Missing GPU/queue endpoints must not
    // force fallback to a richer older report or fabricate a zero queue wait.
    rows[63].simEndTime = 30;
    selected = LatestCompleted(rows);
    Check(selected && selected->index == 63 && selected->range.begin == 20 && selected->range.end == 30,
          "partial marker-only newest report retained");
    Check(!ReflexQueueTiming::Split(11, false, rows[63].osRenderQueueStartTime,
        rows[63].osRenderQueueEndTime, rows[63].gpuRenderStartTime, rows[63].gpuRenderEndTime),
        "missing queue/GPU split stays unknown");

    Fill(rows[63], 11, 12000);
    rows[63].simEndTime = 11999;
    Check(LatestCompleted(rows)->range.begin == 12100, "invalid pair cannot widen otherwise valid range");
    Fill(rows[63], 11, 12000);
    rows[63].gpuRenderStartTime = rows[63].osRenderQueueStartTime - 1;
    Check(LatestCompleted(rows)->index == 63, "non-nested report keeps valid independent intervals");
    Check(!ReflexQueueTiming::Split(11, false, rows[63].osRenderQueueStartTime,
        rows[63].osRenderQueueEndTime, rows[63].gpuRenderStartTime, rows[63].gpuRenderEndTime),
        "non-nested queue split stays unknown, not zero");

    Fill(rows[62], UINT64_MAX - 1, 10000); Fill(rows[63], 1, 12000);
    Check(LatestCompleted(rows)->index == 63, "completion order wins over numeric marker ID");
    Fill(rows[62], 1, 10000);
    Check(LatestCompleted(rows)->index == 63, "duplicate marker IDs prefer newest array entry");
    rows[62] = {}; rows[63] = {};
    Check(!LatestCompleted(rows), "empty next poll cannot reuse previous selection");
}

void FreshnessCases()
{
    using namespace ReflexReportPolicy;
    Freshness freshness;
    Check(freshness.Observe(11, {100, 200}, 7, false, 1000), "first report fresh");
    Check(freshness.Observe(11, {100, 200}, 7, false, 3000), "duplicate valid at two-second boundary");
    Check(freshness.UnchangedMs(3000) == 2000, "duplicates do not reset unchanged time");
    Check(!freshness.Observe(11, {100, 200}, 7, false, 3001), "unchanged report expires");
    Check(!freshness.Observe(11, {100, 200}, 7, false, 9000), "expired duplicate cannot revive itself");
    Check(freshness.Observe(12, {200, 300}, 7, false, 9001), "new completed report refreshes");
    Check(freshness.Observe(12, {200, 301}, 7, false, 11002), "changed endpoints refresh same frame ID");
    Check(freshness.Observe(12, {200, 301}, 8, false, 14003), "observed device change resets freshness");
    Check(freshness.Observe(12, {200, 301}, 8, true, 17004), "real/estimated route change resets freshness");
    Check(freshness.Observe(12, {200, 301}, 8, true, 1), "clock reversal restarts monitor interval");
}

}

int main()
{
    // Instantiate the production policy against BOTH actual vendored NVAPI
    // report layouts; no driver call, device, GPU work or game is involved.
    SelectionCases<NV_LATENCY_RESULT_PARAMS>();
    SelectionCases<NV_VULKAN_LATENCY_RESULT_PARAMS>();
    FreshnessCases();
    std::printf("Reflex report policy: %u checks passed; actual D3D/Vulkan layouts, CPU only\n", checks);
}
