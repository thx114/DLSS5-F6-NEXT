// SPDX-License-Identifier: GPL-3.0-or-later
// Real production query recording, WARP execution and actual queue fences.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#define DLSSNR_LIFETIME_TEST_SEAM
#include <windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <d3dx/d3dx12.h>
#include <algorithm>
#include <array>
#include <optional>
#include <memory>
#include <vector>
#include <cstdio>
#include <stdexcept>
namespace Util {
inline bool CheckForRealObject(const char*, IUnknown*, IUnknown**) { return false; }
}
#include "../../DlssNr_GpuTime.h"
#include "../../DlssNr_OrderedLifetime.h"

using Microsoft::WRL::ComPtr;
namespace {
unsigned checks = 0;
void Check(bool value, const char* why) {
    ++checks;
    if (!value) throw std::runtime_error(why);
}
void Hr(HRESULT value, const char* why) { Check(SUCCEEDED(value), why); }
struct Recording {
    DlssNrGpuLifetime::Use use;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    explicit Recording(ID3D12Device* device) {
        Hr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "allocator");
        Hr(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
            IID_PPV_ARGS(&list)), "recording");
    }
    void Reset(DlssNrGpuTime& timer, DlssNrGpuLifetime::Registry& registry) {
        Hr(allocator->Reset(), "completed/discarded allocator reset");
        Hr(list->Reset(allocator.Get(), nullptr), "real successful list reset");
        registry.ResetRecording(list.Get());
        timer.ResetRecording(list.Get());
    }
};
struct Gpu {
    DlssNrGpuLifetime::TestFaults faults;
    DlssNrGpuLifetime::Registry registry {&faults};
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12Fence> drain;
    ComPtr<ID3D12InfoQueue> messages;
    UINT64 drained = 0;
    HANDLE event = nullptr;
    Gpu() {
        ComPtr<ID3D12Debug> debug;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) debug->EnableDebugLayer();
        ComPtr<IDXGIFactory4> factory;
        ComPtr<IDXGIAdapter> warp;
        Hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "factory");
        Hr(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)), "WARP");
        Hr(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "device");
        D3D12_COMMAND_QUEUE_DESC desc {};
        desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        Hr(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue)), "queue");
        Hr(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&drain)), "test drain fence");
        event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        Check(event != nullptr, "test event");
        device.As(&messages);
    }
    ~Gpu() { if (event) CloseHandle(event); }
    void Wait() {
        // Test harness only. Production timer has no CPU wait/event.
        Hr(queue->Signal(drain.Get(), ++drained), "test drain signal");
        Hr(drain->SetEventOnCompletion(drained, event), "test drain event");
        Check(WaitForSingleObject(event, 10000) == WAIT_OBJECT_0, "test GPU completion");
    }
    void Submit(DlssNrGpuTime& timer, Recording& r) {
        Hr(r.list->Close(), "close query recording");
        ID3D12CommandList* lists[] {r.list.Get()};
        queue->ExecuteCommandLists(1, lists);
        registry.Submitted(queue.Get(), 1, lists);
        timer.Submitted(queue.Get(), 1, lists);
    }
    void Start(DlssNrGpuTime& timer, Recording& r, UINT64 dispatch) {
        r.use = registry.Recording(device.Get(), r.list.Get());
        Check(bool(r.use), "actual production recording token");
        timer.Start(r.list.Get(), r.use, 1000 + dispatch, 2000 + dispatch, dispatch);
    }
    void CheckMessages() {
        if (!messages) { std::puts("D3D12 debug layer unavailable; WARP execution still checked"); return; }
        const auto count = messages->GetNumStoredMessagesAllowedByRetrievalFilter();
        for (UINT64 i = 0; i < count; ++i) {
            SIZE_T bytes = 0;
            Hr(messages->GetMessage(i, nullptr, &bytes), "debug message size");
            std::vector<unsigned char> data(bytes);
            auto* message = reinterpret_cast<D3D12_MESSAGE*>(data.data());
            Hr(messages->GetMessage(i, message, &bytes), "debug message");
            if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) std::fprintf(stderr, "%s\n", message->pDescription);
            Check(message->Severity > D3D12_MESSAGE_SEVERITY_ERROR, "no D3D12 validation errors");
        }
    }
};
void Marks(DlssNrGpuTime& timer, ID3D12GraphicsCommandList* list, unsigned count = DlssNrGpuTime::PointCount) {
    for (unsigned i = 0; i < count; ++i) timer.Mark(list, static_cast<DlssNrGpuTime::Point>(i));
}
void CheckEvidence(const DlssNrGpuTime::Evidence& e, Gpu& gpu, Recording& r, UINT64 dispatch, bool intervals) {
    Check(e.dispatch == dispatch && e.appFrame == 1000 + dispatch && e.submissionEpoch == 2000 + dispatch,
        "same actual source/dispatch identity");
    Check(e.queue == reinterpret_cast<uintptr_t>(gpu.queue.Get()) && e.list == reinterpret_cast<uintptr_t>(r.list.Get()),
        "actual submitting queue/list identity");
    Check(e.fence && e.fenceTarget && e.fenceCompleted >= e.fenceTarget && e.fenceCompleted != UINT64_MAX,
        "real timer completion witness");
    Check(r.use && e.fence == reinterpret_cast<uintptr_t>(r.use->executions[0].fence.Get()) &&
        e.fenceTarget == r.use->executions[0].target, "timer reuses exact existing recording fence without another Signal");
    Check(e.beginTicks > 0 && e.endTicks >= e.beginTicks && e.milliseconds >= 0, "valid total timestamp span");
    Check(e.intervalsKnown == intervals, "only complete sampled recordings publish partitions");
    if (intervals) {
        double sum = 0;
        for (const auto ms : e.intervalsMs) { Check(ms >= 0 && std::isfinite(ms), "nonnegative disjoint interval"); sum += ms; }
        Check(std::abs(sum - e.milliseconds) < 1e-8, "same-recording intervals exactly partition total");
    }
}
void Cases(Gpu& gpu) {
    for (const bool profile : {false, true}) {
        DlssNrGpuTime timer(gpu.device.Get(), "test", profile);
        Recording r(gpu.device.Get()), unrelated(gpu.device.Get());
        for (const UINT64 dispatch : {1ull, 2ull, 3ull, 4ull, 64ull, 65ull, 128ull}) {
            gpu.Start(timer, r, dispatch);
            timer.Mark(unrelated.list.Get(), DlssNrGpuTime::Point::EncodeBegin); // foreign list is ignored
            timer.Mark(r.list.Get(), DlssNrGpuTime::Point::ModelBegin); // out of sequence is ignored
            Marks(timer, r.list.Get());
            timer.Mark(r.list.Get(), DlssNrGpuTime::Point::ResolveEnd); // duplicate is ignored
            timer.End(r.list.Get());
            gpu.Submit(timer, r);
            gpu.Wait();
            timer.ReadGpuTime(gpu.queue.Get());
            const auto fresh = timer.TakeFreshEvidence();
            Check(fresh.size() == 1, "one completed sample per recording");
            CheckEvidence(fresh.front(), gpu, r, dispatch, profile && (dispatch <= 3 || dispatch % 64 == 0));
            Check(timer.TakeFreshEvidence().empty(), "fresh evidence consumed once");
            r.Reset(timer, gpu.registry);
        }
        Hr(r.list->Close(), "close idle recording");
        Hr(unrelated.list->Close(), "close unused recording");
    }
    for (unsigned points = 0; points < DlssNrGpuTime::PointCount; ++points) {
        DlssNrGpuTime timer(gpu.device.Get(), "partial", true);
        Recording r(gpu.device.Get());
        gpu.Start(timer, r, 1);
        Marks(timer, r.list.Get(), points);
        timer.End(r.list.Get());
        gpu.Submit(timer, r);
        gpu.Wait();
        timer.ReadGpuTime(gpu.queue.Get());
        const auto fresh = timer.TakeFreshEvidence();
        Check(fresh.size() == 1, "early return still records a valid total");
        CheckEvidence(fresh.front(), gpu, r, 1, false);
    }
    {
        DlssNrGpuTime timer(gpu.device.Get(), "wrong end", true);
        Recording r(gpu.device.Get()), unrelated(gpu.device.Get());
        gpu.Start(timer, r, 1);
        Marks(timer, r.list.Get());
        timer.End(unrelated.list.Get()); // must not resolve another recording's queries
        timer.End(r.list.Get());
        gpu.Submit(timer, r);
        gpu.Wait();
        timer.ReadGpuTime(gpu.queue.Get());
        const auto fresh = timer.TakeFreshEvidence();
        Check(fresh.size() == 1, "foreign End does not steal the sample");
        CheckEvidence(fresh.front(), gpu, r, 1, true);
        Hr(unrelated.list->Close(), "close unused foreign recording");
    }
    {
        DlssNrGpuTime timer(gpu.device.Get(), "discard", true);
        Recording r(gpu.device.Get());
        gpu.Start(timer, r, 1);
        Marks(timer, r.list.Get(), 2);
        Hr(r.list->Close(), "close unsubmitted recording");
        r.Reset(timer, gpu.registry);
        Check(timer.Status().reset == 1 && timer.Status().pending == 0, "real discarded recording Reset releases sample");
        Check(!timer.ReadGpuTime(gpu.queue.Get()) && timer.TakeFreshEvidence().empty(), "discard is not GPU completion");
        Hr(r.list->Close(), "close reset idle recording");
    }
    {
        DlssNrGpuTime timer(gpu.device.Get(), "delayed queue", true);
        ComPtr<ID3D12Fence> gate;
        Hr(gpu.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)), "delay gate");
        std::vector<std::unique_ptr<Recording>> recordings;
        for (unsigned i = 0; i < 9; ++i) recordings.push_back(std::make_unique<Recording>(gpu.device.Get()));
        Hr(gpu.queue->Wait(gate.Get(), 1), "actual GPU queue delay");
        for (UINT64 dispatch = 1; dispatch <= 8; ++dispatch) {
            auto& r = *recordings[dispatch - 1];
            gpu.Start(timer, r, dispatch);
            Marks(timer, r.list.Get()); timer.End(r.list.Get()); gpu.Submit(timer, r);
        }
        const bool uncompletedHidden = !timer.ReadGpuTime(gpu.queue.Get()) && timer.TakeFreshEvidence().empty();
        gpu.Start(timer, *recordings[8], 9);
        Marks(timer, recordings[8]->list.Get()); timer.End(recordings[8]->list.Get());
        const auto busy = timer.Status();
        // Always unblock before any assertion can throw and destroy live owners.
        Hr(gate->Signal(1), "release actual delay"); gpu.Wait();
        Check(uncompletedHidden, "recorded/submitted is not completed");
        Check(busy.pending == 8 && busy.submitted == 8 && busy.dropped == 1, "bounded slots skip busy sample without growth or wait");
        timer.ReadGpuTime(gpu.queue.Get());
        const auto fresh = timer.TakeFreshEvidence();
        Check(fresh.size() == 8 && timer.Status().pending == 8, "completed executable recordings still own query slots until Reset");
        for (size_t i = 0; i < fresh.size(); ++i) CheckEvidence(fresh[i], gpu, *recordings[i], i + 1, i < 3);
        for (unsigned i = 0; i < 8; ++i) {
            recordings[i]->Reset(timer, gpu.registry);
            Hr(recordings[i]->list->Close(), "close reset idle recording");
        }
        Check(timer.Status().pending == 0, "actual Reset and GPU completion jointly release all eight slots");
        Hr(recordings[8]->list->Close(), "close dropped recording");
    }
    {
        DlssNrGpuTime timer(gpu.device.Get(), "clear", true);
        Recording r(gpu.device.Get());
        gpu.Start(timer, r, 1); Marks(timer, r.list.Get()); timer.End(r.list.Get());
        gpu.Submit(timer, r); timer.ClearLast(); gpu.Wait();
        Check(!timer.ReadGpuTime(gpu.queue.Get()) && timer.TakeFreshEvidence().empty(), "clear cannot republish an old in-flight sample");
    }
}
void SharedReceiptCases() {
    using History = DlssNrGpuLifetime::OrderedHistory<unsigned>;
    for (unsigned fault = 0; fault < 4; ++fault) {
        Gpu gpu;
        DlssNrGpuTime timer(gpu.device.Get(), "shared actual receipt", true);
        History history;
        UINT64 published = 0;
        bool discarded = false;
        auto publish = [&](History::Record& slot, bool ordered) {
            DlssNrGpuLifetime::PublishHistoryRecord(slot, ordered, published,
                [](History::Record&, bool) {});
        };
        auto discard = [&] { discarded = true; };
        auto observed = [](History::Record&) {};
        Recording first(gpu.device.Get()), second(gpu.device.Get());
        auto prepare = [&](Recording& r, UINT64 ordinal) {
            gpu.Start(timer, r, ordinal);
            Marks(timer, r.list.Get()); timer.End(r.list.Get());
            auto& slot = history.current;
            Check(!slot.fence && !slot.value && !slot.retirementQueue,
                "new history generation starts without an older receipt");
            slot.commands = r.list.Get(); slot.commandOwner = r.list;
            slot.queue = gpu.queue.Get(); slot.resourceUse = r.use;
            slot.contract = 1; slot.ordinal = slot.generation = ordinal;
            slot.pending = slot.publish = slot.ordered = true;
        };
        prepare(first, 1);
        Hr(first.list->Close(), "close shared consumer recording");
        ID3D12CommandList* lists[] {first.list.Get()};
        gpu.queue->ExecuteCommandLists(1, lists);
        gpu.faults.failSignal = fault == 2;
        if (fault != 1) gpu.registry.Submitted(gpu.queue.Get(), 1, lists);
        gpu.faults.failSignal = false;
        timer.Submitted(gpu.queue.Get(), 1, lists);
        history.Submitted(gpu.queue.Get(), 1, lists, publish, discard, observed);
        if (fault == 3) {
            // D3D12 permits replay only after the prior execution has completed.
            // Leave the timer unread so isolation is checked before publication.
            gpu.Wait();
            gpu.queue->ExecuteCommandLists(1, lists);
            gpu.registry.Submitted(gpu.queue.Get(), 1, lists);
            timer.Submitted(gpu.queue.Get(), 1, lists);
            history.Submitted(gpu.queue.Get(), 1, lists, publish, discard, observed);
        }
        if (fault) {
            gpu.Wait(); timer.ReadGpuTime(gpu.queue.Get());
            Check(discarded && first.use->quarantined && timer.TakeFreshEvidence().empty(),
                "missing registry, failed Signal or replay cannot serve either receipt consumer");
            first.Reset(timer, gpu.registry);
            history.Reset(first.list.Get(), publish, discard);
            Check(!first.use->Complete() && !first.use->owners.empty(),
                "CPU drain and Reset cannot cure an unproven shared execution");
        } else {
            auto firstUse = first.use;
            const auto oldFence = history.current.fence;
            Check(published == 1 && oldFence == firstUse->executions[0].fence &&
                history.current.value == firstUse->executions[0].target,
                "history consumes the exact registry receipt after timer consumed it");
            Check(history.Advance(true, second.list.Get(), gpu.queue.Get(), 1, 2),
                "adjacent history advance preserves the previous receipt and owners");
            prepare(second, 2); gpu.Submit(timer, second);
            lists[0] = second.list.Get();
            history.Submitted(gpu.queue.Get(), 1, lists, publish, discard, observed);
            Check(history.current.fence != oldFence && history.Pending() == 2 && !discarded,
                "new execution has its own actual receipt while prior generation stays retained");
            gpu.Wait(); timer.ReadGpuTime(gpu.queue.Get());
            const auto evidence = timer.TakeFreshEvidence();
            Check(evidence.size() == 2, "both shared-consumer generations publish completed query evidence");
            CheckEvidence(evidence[0], gpu, first, 1, true);
            CheckEvidence(evidence[1], gpu, second, 2, true);
            second.Reset(timer, gpu.registry);
            history.Reset(second.list.Get(), publish, discard);
            Check(history.Pending() == 1 && !firstUse->Complete() && !firstUse->owners.empty(),
                "out-of-order Reset cannot close the previous executable generation");
            first.Reset(timer, gpu.registry);
            history.Reset(first.list.Get(), publish, discard);
            Check(!history.Pending() && firstUse->Complete() && firstUse->owners.empty(),
                "both real Reset gates and actual completion retire shared owners");
        }
        Hr(first.list->Close(), "close shared consumer reset recording");
        Hr(second.list->Close(), "close shared consumer second recording");
        gpu.CheckMessages();
    }
}

void LifetimeCases() {
    {
        Gpu gpu;
        DlssNrGpuTime timer(gpu.device.Get(), "early Reset", true);
        Recording r(gpu.device.Get());
        ComPtr<ID3D12Fence> gate;
        ComPtr<ID3D12CommandAllocator> replacement;
        Hr(gpu.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)), "early Reset gate");
        Hr(gpu.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&replacement)), "spare allocator");
        Hr(gpu.queue->Wait(gate.Get(), 1), "delay original execution");
        gpu.Start(timer, r, 1); Marks(timer, r.list.Get()); timer.End(r.list.Get()); gpu.Submit(timer, r);
        // A list may Reset with a fresh allocator while its previous execution
        // is pending. The old allocator is kept alive and is never reset here.
        const auto reset = r.list->Reset(replacement.Get(), nullptr);
        if (SUCCEEDED(reset)) {
            gpu.registry.ResetRecording(r.list.Get()); timer.ResetRecording(r.list.Get());
        }
        const bool held = timer.Status().pending == 1 && !timer.ReadGpuTime(gpu.queue.Get());
        Hr(gate->Signal(1), "unblock old execution"); gpu.Wait();
        Hr(reset, "actual early Reset with fresh allocator");
        Check(held, "Reset alone cannot recycle or publish in-flight queries");
        timer.ReadGpuTime(gpu.queue.Get());
        const auto fresh = timer.TakeFreshEvidence();
        Check(fresh.size() == 1 && timer.Status().pending == 0, "delayed completion plus early Reset releases sample");
        CheckEvidence(fresh.front(), gpu, r, 1, true);
        Hr(r.list->Close(), "close replacement recording"); gpu.CheckMessages();
    }
    {
        Gpu gpu;
        DlssNrGpuTime timer(gpu.device.Get(), "replay", true);
        Recording original(gpu.device.Get()), current(gpu.device.Get());
        gpu.Start(timer, original, 1); Marks(timer, original.list.Get()); timer.End(original.list.Get());
        gpu.Submit(timer, original); gpu.Wait(); timer.ReadGpuTime(gpu.queue.Get());
        Check(timer.TakeFreshEvidence().size() == 1 && timer.Status().pending == 1,
            "first completion leaves executable query slot owned");
        // Record a new source before the old executable recording is replayed.
        gpu.Start(timer, current, 2); Marks(timer, current.list.Get()); timer.End(current.list.Get());
        ID3D12CommandList* replay[] {original.list.Get()};
        gpu.queue->ExecuteCommandLists(1, replay);
        gpu.registry.Submitted(gpu.queue.Get(), 1, replay); timer.Submitted(gpu.queue.Get(), 1, replay);
        gpu.Submit(timer, current); gpu.Wait(); timer.ReadGpuTime(gpu.queue.Get());
        const auto fresh = timer.TakeFreshEvidence();
        Check(fresh.size() == 1, "replayed old source cannot become new evidence");
        CheckEvidence(fresh.front(), gpu, current, 2, true);
        Check(gpu.registry.Halted() && timer.Status().quarantined == 1,
            "timer preserves production replay isolation instead of releasing its queries");
        current.Reset(timer, gpu.registry); original.Reset(timer, gpu.registry);
        Check(timer.Status().pending == 1 && timer.Status().quarantined == 1,
            "Reset cannot release a quarantined replay generation");
        Hr(original.list->Close(), "close isolated recording"); Hr(current.list->Close(), "close current recording");
        gpu.CheckMessages();
    }
    {
        Gpu gpu;
        auto timer = std::make_unique<DlssNrGpuTime>(gpu.device.Get(), "retained storage", true);
        Recording r(gpu.device.Get()); ComPtr<ID3D12Fence> gate;
        Hr(gpu.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)), "storage gate");
        Hr(gpu.queue->Wait(gate.Get(), 1), "delay storage use");
        gpu.Start(*timer, r, 1); Marks(*timer, r.list.Get()); timer->End(r.list.Get()); gpu.Submit(*timer, r);
        timer.reset(); // pending query heap/readback must outlive the timer object
        const auto heldOwners = r.use->owners.size();
        Hr(gate->Signal(1), "unblock retained storage"); gpu.Wait();
        Check(heldOwners == 2 && r.use->GpuComplete() && !r.use->Complete(), "real owner retains query heap/readback beyond timer destruction");
        Hr(r.allocator->Reset(), "completed storage allocator reset");
        Hr(r.list->Reset(r.allocator.Get(), nullptr), "completed storage list reset");
        gpu.registry.ResetRecording(r.list.Get());
        Check(r.use->Complete() && r.use->owners.empty(), "actual Reset retires retained timer storage");
        Hr(r.list->Close(), "close storage recording"); gpu.CheckMessages();
    }
    {
        Gpu gpu;
        DlssNrGpuTime timer(gpu.device.Get(), "failed Signal", true);
        Recording r(gpu.device.Get());
        gpu.Start(timer, r, 1); Marks(timer, r.list.Get()); timer.End(r.list.Get());
        gpu.faults.failSignal = true; gpu.Submit(timer, r); gpu.faults.failSignal = false;
        gpu.Wait();
        Check(!timer.ReadGpuTime(gpu.queue.Get()) && timer.TakeFreshEvidence().empty(), "an unrelated completed drain is not a failed recording's completion");
        r.Reset(timer, gpu.registry);
        Check(timer.Status().pending == 1 && timer.Status().quarantined == 1 && r.use->quarantined,
            "failed actual Signal stays isolated even after CPU wait and Reset");
        Hr(r.list->Close(), "close failed Signal recording"); gpu.CheckMessages();
    }
    {
        Gpu gpu;
        DlssNrGpuTime timer(gpu.device.Get(), "foreign token", true);
        Recording r(gpu.device.Get()), other(gpu.device.Get());
        auto wrong = gpu.registry.Recording(gpu.device.Get(), other.list.Get());
        timer.Start(r.list.Get(), wrong, 1001, 2001, 1);
        Check(timer.Status().dropped == 1 && timer.Status().pending == 0, "another recording's token grants no query rights");
        auto full = gpu.registry.Recording(gpu.device.Get(), r.list.Get());
        full->owners.resize(DlssNrGpuLifetime::Token::OwnerCapacity);
        timer.Start(r.list.Get(), full, 1001, 2001, 1);
        Check(timer.Status().dropped == 2 && !full->quarantined && !gpu.registry.Halted(), "optional query storage never consumes the last product-owner capacity");
        Hr(r.list->Close(), "close rejected query recording"); Hr(other.list->Close(), "close foreign recording");
        gpu.CheckMessages();
    }
}
}
int main() {
    try {
        Gpu gpu; Cases(gpu); gpu.CheckMessages(); LifetimeCases(); SharedReceiptCases();
        std::printf("NR GPU timer: %u WARP checks, no SDK/game performance proof\n", checks);
        return 0;
    } catch (const std::exception& e) { std::fprintf(stderr, "FAIL NR GPU timer: %s (%u checks)\n", e.what(), checks); return 1; }
}
