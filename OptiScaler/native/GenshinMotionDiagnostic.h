#pragma once

// Phase25 diagnostic only. No Config/Logger/game headers: NativeProducer's
// standalone host fixtures must still compile this exact implementation.
#include <d3d12.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <new>

namespace native::GenshinMotionDiagnostic
{
constexpr UINT64 kReadbackLimit = 48ull * 1024 * 1024;
constexpr UINT64 kOutputLimit = 24ull * 1024 * 1024;
constexpr UINT64 kFreeRequired = 64ull * 1024 * 1024;
constexpr UINT64 kMetadataReserve = 16 * 1024;
constexpr UINT kMaxDimension = 4096;
constexpr wchar_t kRoot[] = L"D:\\DATA\\Documents\\ChatGPT\\DLSS F5 NEXT\\audit\\r827-depth-pool-accounting-20261023";

enum class Phase { Dormant, Preparing, Recorded, Submitted, Finished, Failed, Uncertain };
struct Image
{
    ID3D12Resource* source = nullptr;
    ID3D12Resource* readback = nullptr;
    D3D12_RESOURCE_DESC desc {};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint {};
    D3D12_RESOURCE_STATES rest = D3D12_RESOURCE_STATE_COMMON;
    UINT64 rowBytes = 0, bytes = 0, allocationBytes = 0, packedBytes = 0;
    UINT rows = 0;
    bool dumped = false;
};
struct State
{
    std::atomic_flag busy = ATOMIC_FLAG_INIT;
    std::atomic<Phase> phase { Phase::Dormant };
    std::atomic<const void*> activeOwner { nullptr };
    const void* owner = nullptr;
    ID3D12Device* device = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12CommandAllocator* allocator = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    ID3D12Fence* fence = nullptr;
    ID3D12Fence* inputFence = nullptr;
    UINT64 expectedValue = 0, submittedValue = 0, completedValue = 0, inputValue = 0;
    UINT64 producerFrame = 0, inputFrame = 0, birth = 0;
    UINT64 readbackBytes = 0, packedBytes = 0;
    DWORD pid = 0, ioError = 0;
    HRESULT failureHr = S_OK;
    int smoothRadius = 0;
    bool consumerRan = false, commandsRecorded = false, metadataWritten = false;
    std::atomic<bool> ownerRetired { false };
    const char* result = "not-attempted";
    wchar_t directory[512] {};
    Image images[2];
    // Formatting uses bounded static storage under busy, not the game's stack.
    char imageJson[2][2400] {};
    char json[12288] {};
    // Deliberately raw owning COM references. No destructor releases them, even
    // at DLL teardown. A failed Close/Signal, device loss or owner retirement
    // is NEVER a completion witness. This bounded one-shot retains its pins
    // permanently (also after success), with no retry or later GPU work.
};
inline State& Get() noexcept { static State state; return state; }
struct Guard
{
    State& state;
    bool held;
    explicit Guard(State& s) noexcept : state(s), held(!s.busy.test_and_set(std::memory_order_acquire)) {}
    ~Guard() { if (held) state.busy.clear(std::memory_order_release); }
};

inline int Game() noexcept
{
    static const int game = []() noexcept {
        wchar_t path[1024] {}; // truncated/unknown executable paths fail closed
        const DWORD n = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(sizeof(path) / sizeof(path[0])));
        if (!n || n >= sizeof(path) / sizeof(path[0])) return 0;
        const wchar_t* name = std::wcsrchr(path, L'\\');
        name = name ? name + 1 : path;
        if (_wcsicmp(name, L"YuanShen.exe") == 0) return 1;
        if (_wcsicmp(name, L"GenshinImpact.exe") == 0) return 2;
        return 0;
    }();
    return game;
}
inline bool FreeSpace() noexcept
{
    ULARGE_INTEGER available {};
    return GetDiskFreeSpaceExW(kRoot, &available, nullptr, nullptr) && available.QuadPart >= kFreeRequired;
}
inline bool OnDevice(ID3D12DeviceChild* child, ID3D12Device* expected) noexcept
{
    if (!child || !expected) return false;
    ID3D12Device* actual = nullptr;
    IUnknown* a = nullptr;
    IUnknown* b = nullptr;
    const bool same = SUCCEEDED(child->GetDevice(IID_PPV_ARGS(&actual))) && actual &&
                      SUCCEEDED(actual->QueryInterface(IID_PPV_ARGS(&a))) && a &&
                      SUCCEEDED(expected->QueryInterface(IID_PPV_ARGS(&b))) && b && a == b;
    if (b) b->Release();
    if (a) a->Release();
    if (actual) actual->Release();
    return same;
}
inline bool WriteAll(HANDLE file, const void* data, DWORD size) noexcept
{
    const auto* p = static_cast<const unsigned char*>(data);
    while (size)
    {
        DWORD written = 0;
        if (!WriteFile(file, p, size, &written, nullptr) || !written) return false;
        p += written;
        size -= written;
    }
    return true;
}
inline void Metadata(State& s) noexcept
{
    if (!s.directory[0]) return;
    auto& images = s.imageJson;
    for (unsigned i = 0; i < 2; ++i)
    {
        const auto& p = s.images[i];
        const auto& d = p.desc;
        const int n = std::snprintf(images[i], sizeof(images[i]),
            "{\"name\":\"%s\",\"file\":\"%s.xy.f16\",\"source\":\"%p\",\"readback\":\"%p\","
            "\"originaldesc\":{\"Dimension\":%u,\"Alignment\":%llu,\"Width\":%llu,\"Height\":%u,"
            "\"DepthOrArraySize\":%u,\"MipLevels\":%u,\"Format\":%u,\"SampleCount\":%u,\"SampleQuality\":%u,"
            "\"Layout\":%u,\"Flags\":%u},\"width\":%llu,\"height\":%u,"
            "\"format\":\"DXGI_FORMAT_R16G16B16A16_FLOAT\",\"format_enum\":%u,"
            "\"footprint_offset\":%llu,\"pitch\":%u,\"row_bytes\":%llu,\"rows\":%u,"
            "\"readback_bytes\":%llu,\"allocation_bytes\":%llu,\"packed_bytes\":%llu,"
            "\"packed_pitch\":%llu,\"restored_state\":%u,\"dumped\":%s}",
            i == 0 ? "optical_flow_halfres" : "nativeframe_guide_motion_fullres",
            i == 0 ? "optical-flow" : "guide-motion", static_cast<void*>(p.source), static_cast<void*>(p.readback),
            static_cast<unsigned>(d.Dimension), static_cast<unsigned long long>(d.Alignment),
            static_cast<unsigned long long>(d.Width), d.Height, static_cast<unsigned>(d.DepthOrArraySize),
            static_cast<unsigned>(d.MipLevels), static_cast<unsigned>(d.Format), d.SampleDesc.Count, d.SampleDesc.Quality,
            static_cast<unsigned>(d.Layout), static_cast<unsigned>(d.Flags), static_cast<unsigned long long>(d.Width),
            d.Height, static_cast<unsigned>(d.Format), static_cast<unsigned long long>(p.footprint.Offset),
            p.footprint.Footprint.RowPitch, static_cast<unsigned long long>(p.rowBytes), p.rows,
            static_cast<unsigned long long>(p.bytes), static_cast<unsigned long long>(p.allocationBytes),
            static_cast<unsigned long long>(p.packedBytes), static_cast<unsigned long long>(d.Width * 4),
            static_cast<unsigned>(p.rest), p.dumped ? "true" : "false");
        if (n < 0 || static_cast<size_t>(n) >= sizeof(images[i])) return;
    }
    auto& json = s.json;
    const int count = std::snprintf(json, sizeof(json),
        "{\n\"schema\":\"phase25-motion-v1\",\"pid\":%lu,\"process_birth_filetime\":%llu,"
        "\"exe\":\"%s\",\"producer_frame\":%llu,\"input.frame\":%llu,\"minimum_producer_frame\":4,"
        "\"producer\":\"%p\",\"device\":\"%p\",\"queue\":\"%p\",\"list\":\"%p\",\"allocator\":\"%p\","
        "\"input_fence\":\"%p\",\"input_value\":%llu,\"producer_fence\":\"%p\","
        "\"expected_signal_value\":%llu,\"submitted_value\":%llu,\"completed_value\":%llu,"
        "\"flow_tuning\":{\"smoothRadius\":%d,\"effectiveSmoothRadius\":%d},"
        "\"consumer_returned_true\":%s,\"commands_recorded\":%s,\"owner_retired\":%s,"
        "\"phase\":%u,\"result\":\"%s\",\"failure_hr\":%ld,\"io_error\":%lu,"
        "\"gpu_readback_allocation_bytes\":%llu,\"cpu_packed_image_bytes\":%llu,"
        "\"readback_limit\":%llu,\"output_limit_including_metadata\":%llu,"
        "\"packed_format\":\"little-endian IEEE754 binary16 XY interleaved; top-to-bottom rows; no padding; ZW omitted\","
        "\"source_proof\":{\"flow\":\"OpticalFlowDx12::Dispatch -> Flow(), actual same producer frame, half resolution rounded up\","
        "\"guide\":\"TrustMaskDx12::BuildGuides success -> GuideMotion() == NativeFrame.motion passed to consumer\","
        "\"recipe\":\"motion/TrustMask_Hlsl.h GuideMotion: Flow.SampleLevel(Linear,uv,0).xy; values already full-picture pixels\","
        "\"barriers\":\"Flow combined NON_PIXEL|PIXEL and Guide NON_PIXEL -> COPY_SOURCE -> exact original states, subresource 0\","
        "\"submission\":\"same producer list after consumer, before Close; only bound after successful queue Signal\","
        "\"game_fsr_raw_motion_included\":false},"
        "\"limitations\":\"Early one-shot may be static or zero; no colour/screenshot, game FSR raw motion, quality/performance fix or final FG/presentation proof. Pins retained for process lifetime; missing completion never authorizes mapping/release.\","
        "\"images\":[%s,%s]\n}\n",
        s.pid, static_cast<unsigned long long>(s.birth), Game() == 1 ? "YuanShen.exe" : "GenshinImpact.exe",
        static_cast<unsigned long long>(s.producerFrame), static_cast<unsigned long long>(s.inputFrame), s.owner,
        static_cast<void*>(s.device), static_cast<void*>(s.queue), static_cast<void*>(s.list), static_cast<void*>(s.allocator),
        static_cast<void*>(s.inputFence), static_cast<unsigned long long>(s.inputValue), static_cast<void*>(s.fence),
        static_cast<unsigned long long>(s.expectedValue), static_cast<unsigned long long>(s.submittedValue),
        static_cast<unsigned long long>(s.completedValue), s.smoothRadius,
        s.smoothRadius < 0 ? 0 : (s.smoothRadius > 4 ? 4 : s.smoothRadius), s.consumerRan ? "true" : "false",
        s.commandsRecorded ? "true" : "false", s.ownerRetired ? "true" : "false",
        static_cast<unsigned>(s.phase.load()), s.result, static_cast<long>(s.failureHr), s.ioError,
        static_cast<unsigned long long>(s.readbackBytes), static_cast<unsigned long long>(s.packedBytes),
        static_cast<unsigned long long>(kReadbackLimit), static_cast<unsigned long long>(kOutputLimit), images[0], images[1]);
    if (count <= 0 || static_cast<size_t>(count) >= sizeof(json) || static_cast<UINT64>(count) > kMetadataReserve) return;
    wchar_t path[560] {};
    if (swprintf_s(path, L"%s\\motion.json", s.directory) < 0) return;
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) { s.ioError = GetLastError(); return; }
    s.metadataWritten = WriteAll(file, json, static_cast<DWORD>(count));
    if (!s.metadataWritten) s.ioError = GetLastError();
    CloseHandle(file);
}
inline void Fail(State& s, const char* reason, HRESULT hr, bool uncertain) noexcept
{
    s.result = reason;
    s.failureHr = hr;
    s.phase.store(uncertain ? Phase::Uncertain : Phase::Failed);
    Metadata(s);
}

inline void Record(const void* owner, ID3D12Device* device, ID3D12GraphicsCommandList* list,
                   ID3D12CommandAllocator* allocator, ID3D12CommandQueue* queue, ID3D12Fence* fence,
                   UINT64 nextValue, ID3D12Resource* flow, ID3D12Resource* guide, UINT64 frame, UINT64 inputFrame,
                   UINT width, UINT height, int smoothRadius, bool consumerRan,
                   ID3D12Fence* inputFence, UINT64 inputValue) noexcept
{
    if (!Game() || frame < 4) return;
    auto& s = Get();
    if (s.phase.load() != Phase::Dormant) return;
    Guard lock(s);
    if (!lock.held || s.phase.load() != Phase::Dormant) return;
    s.phase.store(Phase::Preparing); // consumes the sole attempt, including every refusal/failure
    s.owner = owner; s.activeOwner.store(owner);
    s.pid = GetCurrentProcessId(); s.producerFrame = frame; s.inputFrame = inputFrame;
    s.smoothRadius = smoothRadius; s.consumerRan = consumerRan;
    FILETIME born {}, ended {}, kernel {}, user {};
    if (GetProcessTimes(GetCurrentProcess(), &born, &ended, &kernel, &user))
        s.birth = (static_cast<UINT64>(born.dwHighDateTime) << 32) | born.dwLowDateTime;
    // No retries, directory overwrite, config, shader, preview or image conversion.
    // Check available space BEFORE creating anything or allocating GPU readbacks.
    if (!FreeSpace()) { Fail(s, "preflight-free-space-below-64MiB", E_FAIL, false); return; }
    wchar_t directory[512] {};
    if (swprintf_s(directory, L"%s\\phase25-motion-PID%lu", kRoot, s.pid) < 0 ||
        !CreateDirectoryW(directory, nullptr))
    { Fail(s, "exclusive-output-directory-refused", E_FAIL, false); return; }
    std::wmemcpy(s.directory, directory, std::wcslen(directory) + 1);
    if (!owner || !device || !list || !allocator || !queue || !fence || !flow || !guide || flow == guide ||
        !nextValue || nextValue == UINT64_MAX || !width || !height || width > kMaxDimension || height > kMaxDimension ||
        queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT ||
        FAILED(device->GetDeviceRemovedReason()) || !OnDevice(list, device) || !OnDevice(allocator, device) ||
        !OnDevice(queue, device) || !OnDevice(fence, device) || !OnDevice(flow, device) || !OnDevice(guide, device))
    { Fail(s, "owner-device-resource-or-dimension-refused", E_FAIL, false); return; }
    auto& a = s.images[0]; auto& b = s.images[1];
    a.desc = flow->GetDesc(); b.desc = guide->GetDesc();
    a.rest = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    b.rest = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    if (a.desc.Width != (width + 1) / 2 || a.desc.Height != (height + 1) / 2 ||
        b.desc.Width != width || b.desc.Height != height)
    { Fail(s, "same-frame-flow-guide-size-refused", E_FAIL, false); return; }
    D3D12_RESOURCE_DESC buffers[2] {};
    for (unsigned i = 0; i < 2; ++i)
    {
        auto& p = s.images[i]; const auto& d = p.desc;
        if (d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || d.Format != DXGI_FORMAT_R16G16B16A16_FLOAT ||
            d.DepthOrArraySize != 1 || d.MipLevels != 1 || d.SampleDesc.Count != 1 || d.SampleDesc.Quality != 0)
        { Fail(s, "texture-description-refused", E_FAIL, false); return; }
        device->GetCopyableFootprints(&d, 0, 1, 0, &p.footprint, &p.rows, &p.rowBytes, &p.bytes);
        if (!p.bytes || p.bytes == UINT64_MAX || p.bytes > kReadbackLimit || p.footprint.Offset != 0 ||
            p.rows != d.Height || p.rowBytes != d.Width * 8 || p.footprint.Footprint.RowPitch < p.rowBytes ||
            p.footprint.Footprint.Width != d.Width || p.footprint.Footprint.Height != d.Height ||
            p.footprint.Footprint.Depth != 1 || p.footprint.Footprint.Format != d.Format ||
            p.footprint.Footprint.RowPitch % D3D12_TEXTURE_DATA_PITCH_ALIGNMENT != 0 ||
            static_cast<UINT64>(p.footprint.Footprint.RowPitch) * (d.Height - 1) + p.rowBytes > p.bytes)
        { Fail(s, "copy-footprint-refused", E_FAIL, false); return; }
        auto& buffer = buffers[i];
        buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; buffer.Width = p.bytes; buffer.Height = 1;
        buffer.DepthOrArraySize = 1; buffer.MipLevels = 1; buffer.SampleDesc.Count = 1;
        buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        const auto allocation = device->GetResourceAllocationInfo(0, 1, &buffer);
        p.allocationBytes = allocation.SizeInBytes;
        p.packedBytes = d.Width * d.Height * 4;
        if (!p.allocationBytes || p.allocationBytes == UINT64_MAX || p.allocationBytes > kReadbackLimit - s.readbackBytes ||
            p.packedBytes > kOutputLimit - kMetadataReserve - s.packedBytes)
        { Fail(s, "readback-48MiB-or-output-24MiB-limit", E_FAIL, false); return; }
        s.readbackBytes += p.allocationBytes; s.packedBytes += p.packedBytes;
    }
    // Retain every object directly referenced by the diagnostic commands, plus
    // submission/owner objects, BEFORE recording anything. Never borrow lifetimes.
    s.device = device; device->AddRef(); s.list = list; list->AddRef();
    s.allocator = allocator; allocator->AddRef(); s.queue = queue; queue->AddRef();
    s.fence = fence; fence->AddRef(); s.expectedValue = nextValue;
    s.inputFence = inputFence; s.inputValue = inputValue;
    if (inputFence) inputFence->AddRef();
    a.source = flow; flow->AddRef(); b.source = guide; guide->AddRef();
    D3D12_HEAP_PROPERTIES heap {}; heap.Type = D3D12_HEAP_TYPE_READBACK;
    for (unsigned i = 0; i < 2; ++i)
    {
        const HRESULT hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffers[i],
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&s.images[i].readback));
        if (FAILED(hr)) { Fail(s, "readback-allocation-failed", hr, false); return; }
    }
    s.result = "prepared-not-submitted";
    Metadata(s);
    if (!s.metadataWritten) { Fail(s, "metadata-preflight-write-failed", E_FAIL, false); return; }
    if (s.ownerRetired.load()) { Fail(s, "owner-retired-before-recording", E_FAIL, true); return; }
    for (auto& p : s.images)
    {
        D3D12_RESOURCE_BARRIER barrier {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = p.source; barrier.Transition.Subresource = 0;
        barrier.Transition.StateBefore = p.rest; barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        list->ResourceBarrier(1, &barrier);
        D3D12_TEXTURE_COPY_LOCATION src {}, dst {};
        src.pResource = p.source; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; src.SubresourceIndex = 0;
        dst.pResource = p.readback; dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint = p.footprint;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE; barrier.Transition.StateAfter = p.rest;
        list->ResourceBarrier(1, &barrier);
    }
    s.commandsRecorded = true;
    s.result = "recorded-awaiting-close-and-signal";
    s.phase.store(Phase::Recorded);
    Metadata(s);
}

inline void Failed(const void* owner, ID3D12Device* device, const char* reason, HRESULT hr) noexcept
{
    auto& s = Get(); Guard lock(s);
    if (!lock.held || s.owner != owner || s.device != device || s.phase.load() != Phase::Recorded) return;
    Fail(s, reason, hr, true); // no retry, release, map or fabricated completion
}
inline void Submitted(const void* owner, ID3D12Device* device, ID3D12Fence* fence, UINT64 value) noexcept
{
    auto& s = Get(); Guard lock(s);
    if (!lock.held || s.phase.load() != Phase::Recorded) return;
    if (s.owner != owner) return; // another producer cannot bind our submission
    if (s.ownerRetired || s.device != device || s.fence != fence || value != s.expectedValue || !value || value == UINT64_MAX)
    { Fail(s, "foreign-or-invalid-submission", E_FAIL, true); return; }
    s.submittedValue = value;
    s.result = "close-and-queue-signal-succeeded-awaiting-completion";
    s.phase.store(Phase::Submitted);
    Metadata(s);
}
inline void Poll(const void* owner, ID3D12Device* device) noexcept
{
    auto& s = Get();
    const auto phase = s.phase.load();
    if (phase != Phase::Recorded && phase != Phase::Submitted) return;
    Guard lock(s);
    if (!lock.held || s.owner != owner) return; // nonblocking, same producer only
    if (s.ownerRetired || device != s.device)
    { Fail(s, "owner-or-device-changed", E_FAIL, true); return; }
    if (s.phase.load() == Phase::Recorded)
    { Fail(s, "later-run-without-successful-signal", E_FAIL, true); return; }
    if (s.phase.load() != Phase::Submitted) return;
    if (!s.fence || !s.submittedValue || FAILED(device->GetDeviceRemovedReason()))
    { Fail(s, "missing-fence-or-device-removed", E_FAIL, true); return; }
    const UINT64 complete = s.fence->GetCompletedValue();
    s.completedValue = complete;
    if (complete == UINT64_MAX) { Fail(s, "fence-device-removed-sentinel", E_FAIL, true); return; }
    if (!complete || complete < s.submittedValue) return; // no wait/event/time admission
    if (!FreeSpace()) { Fail(s, "output-free-space-below-64MiB", E_FAIL, false); return; }
    auto* row = new (std::nothrow) unsigned char[kMaxDimension * 4];
    if (!row) { Fail(s, "cpu-row-allocation-failed", E_OUTOFMEMORY, false); return; }
    bool ok = true;
    for (unsigned i = 0; i < 2 && ok; ++i)
    {
        auto& p = s.images[i];
        void* mapped = nullptr;
        const D3D12_RANGE range { 0, static_cast<SIZE_T>(p.bytes) };
        const HRESULT hr = p.readback->Map(0, &range, &mapped);
        if (FAILED(hr) || !mapped)
        {
            if (SUCCEEDED(hr)) { const D3D12_RANGE none { 0, 0 }; p.readback->Unmap(0, &none); }
            s.failureHr = FAILED(hr) ? hr : E_POINTER; s.result = "readback-map-failed"; ok = false; break;
        }
        wchar_t path[560] {};
        const int n = swprintf_s(path, L"%s\\%s.xy.f16", s.directory, i == 0 ? L"optical-flow" : L"guide-motion");
        HANDLE file = n < 0 ? INVALID_HANDLE_VALUE : CreateFileW(path, GENERIC_WRITE, 0, nullptr,
            CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) { s.ioError = GetLastError(); s.result = "image-create-failed"; ok = false; }
        else
        {
            for (UINT y = 0; y < p.desc.Height && ok; ++y)
            {
                const auto* src = static_cast<const unsigned char*>(mapped) + p.footprint.Offset +
                                  static_cast<SIZE_T>(y) * p.footprint.Footprint.RowPitch;
                for (UINT x = 0; x < p.desc.Width; ++x) std::memcpy(row + x * 4, src + x * 8, 4);
                ok = WriteAll(file, row, static_cast<DWORD>(p.desc.Width * 4));
                if (!ok) { s.ioError = GetLastError(); s.result = "image-write-failed-partial"; }
            }
            if (!CloseHandle(file)) { s.ioError = GetLastError(); s.result = "image-close-failed"; ok = false; }
            p.dumped = ok;
        }
        const D3D12_RANGE none { 0, 0 }; p.readback->Unmap(0, &none);
    }
    delete[] row;
    if (ok) { s.result = "two-actual-motion-maps-written"; s.phase.store(Phase::Finished); Metadata(s); }
    else Fail(s, s.result, s.failureHr, false);
}
inline void Retired(const void* owner, ID3D12Device* device) noexcept
{
    auto& s = Get();
    if (s.activeOwner.load() != owner) return;
    // Publish even when the nonblocking diagnostic lock is occupied. A new
    // producer at the same address cannot impersonate the original owner.
    s.ownerRetired.store(true);
    Guard lock(s);
    if (!lock.held || s.owner != owner || s.device != device) return;
    if (s.phase.load() == Phase::Recorded || s.phase.load() == Phase::Submitted)
        Fail(s, "owner-retired-before-poll-completion", E_FAIL, true);
}
} // namespace native::GenshinMotionDiagnostic
