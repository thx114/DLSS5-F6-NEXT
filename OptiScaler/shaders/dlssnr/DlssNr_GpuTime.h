#pragma once

#include "DlssNr_WatchedLists.h"
#include "DlssNr_RecordingLifetime.h"
#include <low_latency/GpuTimeline.h>
#include <algorithm>
#include <array>
#include <optional>
#include <vector>

// NR calls are serialized by g_nrMutex, including submission/reset notifications.
// Observe the existing recording's actual queue/fence. Timing never creates a
// second completion timeline or authorizes a product resource's retirement.
class DlssNrGpuTime
{
  public:
    enum class Point : unsigned { EncodeBegin, EncodeEnd, ModelBegin, ModelEnd, HistoryEnd, ResolveEnd };
    static constexpr unsigned PointCount = 6, IntervalCount = PointCount + 1;
    struct Evidence
    {
        UINT64 beginTicks = 0, endTicks = 0, queueHz = 0, sequence = 0;
        UINT64 appFrame = 0, submissionEpoch = 0, dispatch = 0;
        UINT64 fenceTarget = 0, fenceCompleted = 0;
        uintptr_t list = 0, queue = 0, fence = 0; // identities only; never dereferenced
        double milliseconds = 0;
        UINT64 reflexFrame = 0, timelineDevice = 0;
        double beginQpcUs = 0.0, endQpcUs = 0.0;
        bool intervalsKnown = false;
        std::array<double, IntervalCount> intervalsMs {};
    };
    struct Counters
    {
        UINT64 attempts = 0, dropped = 0, reset = 0, abandoned = 0, completed = 0;
        UINT64 invalid = 0, mapFailed = 0, frequencyFailed = 0, signalFailed = 0;
        UINT64 deviceRemoved = 0, freshOverflow = 0;
        UINT64 missingReceipt = 0;
        unsigned pending = 0, recorded = 0, submitted = 0, quarantined = 0, awaitingReset = 0;
    };

  private:
    using Resource = Microsoft::WRL::ComPtr<ID3D12Resource>;
    struct Sample
    {
        DlssNrGpuLifetime::Use lifetime;
        Microsoft::WRL::ComPtr<ID3D12Fence> fence;
        ID3D12CommandList* commands = nullptr; // identity only
        UINT64 value = 0, frequency = 0, sequence = 0;
        UINT64 appFrame = 0, submissionEpoch = 0, dispatch = 0;
        ID3D12CommandQueue* queue = nullptr; // identity only
        UINT64 reflexFrame = 0, timelineDevice = 0, calibrationGpu = 0, calibrationCpu = 0;
        bool deviceRemoved = false;
        bool occupied = false, ended = false, submitted = false;
        bool collected = false, resetNotified = false;
        bool profile = false, profileResolved = false;
        unsigned points = 0;
    };
    static constexpr unsigned Count = 8;
    const unsigned stride;
    Microsoft::WRL::ComPtr<ID3D12QueryHeap> queries;
    Resource readback;
    std::array<Sample, Count> samples;
    int recording = -1;
    UINT64 sequence = 0, lastSequence = 0, clearedSequence = 0;
    std::optional<double> last;
    std::optional<Evidence> lastEvidence;
    Counters counters;
    // Every completed sample since the last TakeFresh, oldest first: several can complete between two reads, and
    // `last` keeps only the newest (with frames of alternating cost it can lock onto one kind).
    std::vector<Evidence> fresh;

    void Collect()
    {
        for (unsigned i = 0; i < Count; ++i)
        {
            auto& s = samples[i];
            if (!s.occupied) continue;
            // An executable generation can be replayed after its first fence
            // completes. Only the real owner can close it; retain query storage
            // through the same Complete gate as all other recording owners.
            if (s.lifetime->quarantined || s.deviceRemoved) continue;
            if (s.collected && !s.lifetime->closed) continue; // Reset is an event, not another fence poll
            const bool reusable = s.lifetime->Complete();
            if (!s.submitted || !s.ended || s.collected || !s.fence)
            {
                if (reusable) { s.occupied = false; s.lifetime.reset(); }
                continue;
            }
            const auto completed = s.fence->GetCompletedValue();
            if (completed == UINT64_MAX)
            {
                s.deviceRemoved = true;
                ++counters.deviceRemoved;
                continue; // unknown completion: quarantine, never recycle
            }
            if (completed < s.value) continue;
            if (!reusable && !s.lifetime->GpuComplete()) continue;
            UINT64* data = nullptr;
            const unsigned base = i * stride;
            D3D12_RANGE range { base * sizeof(UINT64), (base + stride) * sizeof(UINT64) };
            if (SUCCEEDED(readback->Map(0, &range, (void**) &data)))
            {
                const auto begin = data[base], end = data[base + 1];
                if (s.frequency && end >= begin)
                {
                    const double ms = double(end - begin) * 1000.0 / double(s.frequency);
                    Evidence evidence { begin, end, s.frequency, s.sequence, s.appFrame,
                        s.submissionEpoch, s.dispatch, s.value, completed,
                        reinterpret_cast<uintptr_t>(s.commands), reinterpret_cast<uintptr_t>(s.queue),
                        reinterpret_cast<uintptr_t>(s.fence.Get()), ms };
                    if (s.profileResolved)
                    {
                        UINT64 previous = begin;
                        evidence.intervalsKnown = true;
                        for (unsigned point = 0; point < IntervalCount; ++point)
                        {
                            const auto tick = point < PointCount ? data[base + 2 + point] : end;
                            if (tick < previous || tick > end) evidence.intervalsKnown = false;
                            evidence.intervalsMs[point] = tick >= previous ?
                                double(tick - previous) * 1000.0 / double(s.frequency) : 0.0;
                            previous = tick;
                        }
                    }
                    evidence.reflexFrame = s.reflexFrame;
                    evidence.timelineDevice = s.timelineDevice;
                    if (s.calibrationCpu && s.frequency)
                    {
                        evidence.beginQpcUs = GpuTimeline::GpuTicksToQpcUs(begin, s.calibrationGpu, s.calibrationCpu, s.frequency);
                        evidence.endQpcUs = GpuTimeline::GpuTicksToQpcUs(end, s.calibrationGpu, s.calibrationCpu, s.frequency);
                    }
                    ++counters.completed;
                    if (s.sequence > lastSequence)
                    {
                        last = ms;
                        lastEvidence = evidence;
                        lastSequence = s.sequence;
                    }
                    if (s.sequence > clearedSequence)
                    {
                        if (fresh.size() >= 2 * Count) // bounded evidence, counted loss
                        {
                            auto oldest = std::min_element(fresh.begin(), fresh.end(),
                                [](const Evidence& a, const Evidence& b) { return a.sequence < b.sequence; });
                            if (oldest->sequence < evidence.sequence) *oldest = evidence;
                            ++counters.freshOverflow;
                        }
                        else fresh.push_back(evidence);
                    }
                }
                else ++counters.invalid;
                D3D12_RANGE written { 0, 0 };
                readback->Unmap(0, &written);
            }
            else ++counters.mapFailed;
            s.collected = true;
            if (reusable) { s.occupied = false; s.lifetime.reset(); }
        }
    }

  public:
    DlssNrGpuTime(ID3D12Device* device, [[maybe_unused]] const char* name, bool profile = false)
        : stride(profile ? 2 + PointCount : 2)
    {
        D3D12_QUERY_HEAP_DESC queryDesc { D3D12_QUERY_HEAP_TYPE_TIMESTAMP, Count * stride, 0 };
        if (FAILED(device->CreateQueryHeap(&queryDesc, IID_PPV_ARGS(&queries)))) return;
        const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK);
        const auto desc = CD3DX12_RESOURCE_DESC::Buffer(Count * stride * sizeof(UINT64));
        if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)))) return;
    }

    void Start(ID3D12GraphicsCommandList* cmd, const DlssNrGpuLifetime::Use& lifetime,
               UINT64 appFrame = 0, UINT64 submissionEpoch = 0, UINT64 dispatch = 0, UINT64 reflexFrame = 0)
    {
        ++counters.attempts;
        if (recording >= 0) ++counters.abandoned; // held until the list reset; do not recycle GPU queries
        recording = -1;
        if (!readback || !lifetime || lifetime->closed || lifetime->submitted || lifetime->quarantined)
        { ++counters.dropped; return; }
        ID3D12GraphicsCommandList* real = nullptr;
        auto* identity = Util::CheckForRealObject(__FUNCTION__, cmd, (IUnknown**)&real) ? real : cmd;
        if (lifetime->listOwner.Get() != identity) { ++counters.dropped; return; }
        Collect();
        for (unsigned i = 0; i < Count; ++i)
        {
            auto& s = samples[i];
            if (s.occupied) continue;
            // Optional diagnostics must not exhaust/quarantine a product token.
            if (lifetime->owners.size() + 2 > DlssNrGpuLifetime::Token::OwnerCapacity)
            { ++counters.dropped; return; }
            if (!lifetime->Retain(queries.Get()) || !lifetime->Retain(readback.Get()))
            { ++counters.dropped; return; }
            s.commands = identity;
            s.lifetime = lifetime;
            DlssNrWatchedLists::Add(s.commands); // the submit/reset hooks must hear about this list
            s.occupied = true;
            s.ended = s.submitted = false;
            s.collected = s.resetNotified = false;
            s.fence.Reset(); s.value = 0;
            s.sequence = ++sequence;
            s.appFrame = appFrame;
            s.submissionEpoch = submissionEpoch;
            s.dispatch = dispatch;
            const auto marker = GpuTimeline::Capture(reflexFrame);
            s.reflexFrame = marker.frame;
            s.timelineDevice = marker.device;
            s.calibrationGpu = s.calibrationCpu = 0;
            s.queue = nullptr;
            s.frequency = 0;
            s.deviceRemoved = false;
            // Extra stamps share the existing completion signal/readback. Only
            // the first three and every 64th real NR dispatch are profiled.
            s.profile = stride > 2 && dispatch && (dispatch <= 3 || dispatch % 64 == 0);
            s.points = 0; s.profileResolved = false;
            recording = (int) i;
            cmd->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, i * stride);
            return;
        }
        ++counters.dropped; // all eight slots are still owned
    }

    void Mark(ID3D12GraphicsCommandList* cmd, Point point)
    {
        if (recording < 0) return;
        auto& s = samples[recording];
        if (!s.profile) return;
        const auto index = static_cast<unsigned>(point);
        if (index >= PointCount || s.points != (1u << index) - 1u) return;
        ID3D12GraphicsCommandList* real = nullptr;
        auto* identity = Util::CheckForRealObject(__FUNCTION__, cmd, (IUnknown**)&real) ? real : cmd;
        if (identity != s.commands) return;
        cmd->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, unsigned(recording) * stride + 2 + index);
        s.points |= 1u << index;
    }

    void End(ID3D12GraphicsCommandList* cmd)
    {
        if (recording < 0) return;
        const auto i = (unsigned) recording;
        auto& s = samples[i];
        ID3D12GraphicsCommandList* real = nullptr;
        auto* identity = Util::CheckForRealObject(__FUNCTION__, cmd, (IUnknown**)&real) ? real : cmd;
        if (identity != s.commands) return;
        const auto base = i * stride;
        cmd->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, base + 1);
        cmd->ResolveQueryData(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, base, 2,
                             readback.Get(), base * sizeof(UINT64));
        s.profileResolved = s.profile && s.points == (1u << PointCount) - 1u;
        // Missing/failed/early-return stages never resolve unwritten queries.
        if (s.profileResolved)
            cmd->ResolveQueryData(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, base + 2, PointCount,
                                 readback.Get(), (base + 2) * sizeof(UINT64));
        samples[i].ended = true;
        recording = -1;
    }

    void Submitted(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists)
    {
        for (auto& s : samples)
        {
            if (!s.occupied || !s.ended || s.submitted || s.resetNotified) continue;
            for (UINT i = 0; i < count; ++i)
                if (lists[i] == s.commands)
                {
                    s.queue = queue;
                    s.submitted = true;
                    // Registry::Submitted ran first, after this actual Execute.
                    // Reuse its exact queue/fence/value instead of issuing one
                    // Signal per total/model/layer timer on every source frame.
                    if (const auto* execution = DlssNrGpuLifetime::ActualSubmission(s.lifetime, s.commands, queue))
                    { s.fence = execution->fence; s.value = execution->target; }
                    if (!s.fence) { ++counters.missingReceipt; break; }
                    if (FAILED(queue->GetTimestampFrequency(&s.frequency)) || !s.frequency)
                    { s.frequency = 0; ++counters.frequencyFailed; }
                    if (s.reflexFrame && GpuTimeline::Enabled() &&
                        FAILED(queue->GetClockCalibration(&s.calibrationGpu, &s.calibrationCpu)))
                        s.calibrationGpu = s.calibrationCpu = 0;
                    break;
                }
        }
    }

    void ResetRecording(ID3D12CommandList* cmd)
    {
        for (auto& s : samples)
            if (s.occupied && !s.resetNotified && s.commands == cmd && s.lifetime->closed)
            {
                if (recording >= 0 && &s == &samples[recording]) recording = -1;
                s.resetNotified = true;
                ++counters.reset;
            }
        Collect();
    }

    void ClearLast()
    {
        last.reset();
        lastEvidence.reset();
        lastSequence = sequence; // older, in-flight samples must not repopulate the display
        clearedSequence = sequence;
        fresh.clear();
    }

    std::optional<double> ReadGpuTime([[maybe_unused]] ID3D12CommandQueue* queue)
    {
        Collect();
        return last;
    }

    const std::optional<Evidence>& LatestEvidence() const { return lastEvidence; }

    Counters Status() const
    {
        auto out = counters;
        for (const auto& s : samples)
            if (s.occupied)
            {
                ++out.pending;
                if (s.submitted) ++out.submitted; else ++out.recorded;
                if (s.lifetime->quarantined || s.deviceRemoved) ++out.quarantined;
                else if (s.collected && !s.lifetime->closed) ++out.awaitingReset;
            }
        return out;
    }

    std::vector<Evidence> TakeFreshEvidence()
    {
        std::sort(fresh.begin(), fresh.end(), [](const Evidence& a, const Evidence& b) { return a.sequence < b.sequence; });
        std::vector<Evidence> out;
        out.swap(fresh);
        return out;
    }

    // The samples completed since the last call, oldest first. Call after ReadGpuTime.
    std::vector<double> TakeFresh()
    {
        std::sort(fresh.begin(), fresh.end(), [](const Evidence& a, const Evidence& b) { return a.sequence < b.sequence; });
        std::vector<double> out;
        out.reserve(fresh.size());
        for (const auto& sample : fresh)
            out.push_back(sample.milliseconds);
        fresh.clear();
        return out;
    }
};
