#pragma once

#include "DlssNr_WatchedLists.h"
#include <low_latency/GpuTimeline.h>

// NR calls are serialized by g_nrMutex, including submission/reset notifications.
// Associate every query pair with its actual submitting queue and GPU completion.
class DlssNrGpuTime
{
  public:
    struct Evidence
    {
        UINT64 beginTicks = 0, endTicks = 0, queueHz = 0, sequence = 0;
        UINT64 appFrame = 0, submissionEpoch = 0, dispatch = 0;
        UINT64 fenceTarget = 0, fenceCompleted = 0;
        uintptr_t list = 0, queue = 0, fence = 0; // identities only; never dereferenced
        double milliseconds = 0;
        UINT64 reflexFrame = 0, timelineDevice = 0;
        double beginQpcUs = 0.0, endQpcUs = 0.0;
    };
    struct Counters
    {
        UINT64 attempts = 0, dropped = 0, reset = 0, abandoned = 0, completed = 0;
        UINT64 invalid = 0, mapFailed = 0, frequencyFailed = 0, signalFailed = 0;
        UINT64 deviceRemoved = 0, freshOverflow = 0;
        unsigned pending = 0, recorded = 0, submitted = 0, quarantined = 0;
    };

  private:
    using Resource = Microsoft::WRL::ComPtr<ID3D12Resource>;
    struct Sample
    {
        Microsoft::WRL::ComPtr<ID3D12Fence> fence;
        ID3D12CommandList* commands = nullptr; // identity only
        UINT64 value = 0, frequency = 0, sequence = 0;
        UINT64 appFrame = 0, submissionEpoch = 0, dispatch = 0;
        ID3D12CommandQueue* queue = nullptr; // identity only
        UINT64 reflexFrame = 0, timelineDevice = 0, calibrationGpu = 0, calibrationCpu = 0;
        bool signalFailed = false, deviceRemoved = false;
        bool occupied = false, ended = false, submitted = false;
    };
    static constexpr unsigned Count = 8;
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
            if (!s.occupied || !s.submitted || s.signalFailed || s.deviceRemoved) continue;
            const auto completed = s.fence->GetCompletedValue();
            if (completed == UINT64_MAX)
            {
                s.deviceRemoved = true;
                ++counters.deviceRemoved;
                continue; // unknown completion: quarantine, never recycle
            }
            if (completed < s.value) continue;
            UINT64* data = nullptr;
            D3D12_RANGE range { i * 2 * sizeof(UINT64), (i * 2 + 2) * sizeof(UINT64) };
            if (SUCCEEDED(readback->Map(0, &range, (void**) &data)))
            {
                const auto begin = data[i * 2], end = data[i * 2 + 1];
                if (s.frequency && end >= begin)
                {
                    const double ms = double(end - begin) * 1000.0 / double(s.frequency);
                    Evidence evidence { begin, end, s.frequency, s.sequence, s.appFrame,
                        s.submissionEpoch, s.dispatch, s.value, completed,
                        reinterpret_cast<uintptr_t>(s.commands), reinterpret_cast<uintptr_t>(s.queue),
                        reinterpret_cast<uintptr_t>(s.fence.Get()), ms };
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
            s.occupied = false; // real completion proves this slot safe even on failed readback
        }
    }

  public:
    DlssNrGpuTime(ID3D12Device* device, [[maybe_unused]] const char* name)
    {
        D3D12_QUERY_HEAP_DESC queryDesc { D3D12_QUERY_HEAP_TYPE_TIMESTAMP, Count * 2, 0 };
        if (FAILED(device->CreateQueryHeap(&queryDesc, IID_PPV_ARGS(&queries)))) return;
        const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK);
        const auto desc = CD3DX12_RESOURCE_DESC::Buffer(Count * 2 * sizeof(UINT64));
        if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)))) return;
        for (auto& s : samples)
            if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&s.fence))))
            { readback.Reset(); return; }
    }

    void Start(ID3D12GraphicsCommandList* cmd, UINT64 appFrame = 0, UINT64 submissionEpoch = 0, UINT64 dispatch = 0, UINT64 reflexFrame = 0)
    {
        ++counters.attempts;
        if (recording >= 0) ++counters.abandoned; // held until the list reset; do not recycle GPU queries
        recording = -1;
        if (!readback) { ++counters.dropped; return; }
        Collect();
        for (unsigned i = 0; i < Count; ++i)
        {
            auto& s = samples[i];
            if (s.occupied) continue;
            s.commands = cmd;
            ID3D12GraphicsCommandList* real = nullptr;
            if (Util::CheckForRealObject(__FUNCTION__, cmd, (IUnknown**) &real)) s.commands = real;
            DlssNrWatchedLists::Add(s.commands); // the submit/reset hooks must hear about this list
            s.occupied = true;
            s.ended = s.submitted = false;
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
            s.signalFailed = s.deviceRemoved = false;
            ++s.value;
            recording = (int) i;
            cmd->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, i * 2);
            return;
        }
        ++counters.dropped; // all eight slots are still owned
    }

    void End(ID3D12GraphicsCommandList* cmd)
    {
        if (recording < 0) return;
        const auto i = (unsigned) recording;
        cmd->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, i * 2 + 1);
        cmd->ResolveQueryData(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, i * 2, 2,
                             readback.Get(), i * 2 * sizeof(UINT64));
        samples[i].ended = true;
        recording = -1;
    }

    void Submitted(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists)
    {
        for (auto& s : samples)
        {
            if (!s.occupied || !s.ended || s.submitted) continue;
            for (UINT i = 0; i < count; ++i)
                if (lists[i] == s.commands)
                {
                    s.queue = queue;
                    if (FAILED(queue->GetTimestampFrequency(&s.frequency)) || !s.frequency)
                    { s.frequency = 0; ++counters.frequencyFailed; }
                    if (s.reflexFrame && GpuTimeline::Enabled() &&
                        FAILED(queue->GetClockCalibration(&s.calibrationGpu, &s.calibrationCpu)))
                        s.calibrationGpu = s.calibrationCpu = 0;
                    // Executed after the real ExecuteCommandLists: protects query readback AND reuse.
                    // A failed signal must quarantine an executed slot, not make it look discarded.
                    s.submitted = true;
                    if (FAILED(queue->Signal(s.fence.Get(), s.value)))
                    { s.frequency = 0; s.signalFailed = true; ++counters.signalFailed; }
                    break;
                }
        }
    }

    void ResetRecording(ID3D12CommandList* cmd)
    {
        for (auto& s : samples)
            if (s.occupied && !s.submitted && s.commands == cmd)
            {
                if (recording >= 0 && &s == &samples[recording]) recording = -1;
                s.occupied = false;
                ++counters.reset;
            }
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
                if (s.signalFailed || s.deviceRemoved) ++out.quarantined;
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
