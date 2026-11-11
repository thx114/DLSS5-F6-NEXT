#pragma once

#include "DlssNrCurrentOutputImage.h"
#include <shaders/dlssnr/DlssNr_SubmissionLifetime.h>
#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <chrono>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <sstream>
#include <iomanip>

namespace DlssNrCurrentOutput
{
using Microsoft::WRL::ComPtr;
inline constexpr uint64_t MaxReadbackBytes = 128ull * 1024 * 1024;
inline constexpr uint64_t MaxTightBytes = 128ull * 1024 * 1024;
inline constexpr unsigned MaxExtent = 16384;
inline thread_local unsigned deferEndpoint = 0;
struct DeferEndpoint
{
    DeferEndpoint() { ++deferEndpoint; }
    ~DeferEndpoint() { --deferEndpoint; }
    DeferEndpoint(const DeferEndpoint&) = delete;
    DeferEndpoint& operator=(const DeferEndpoint&) = delete;
};

struct Metadata
{
    uint64_t epoch = 0, dispatch = 0;
    unsigned encoding = 0, debugView = 0, guidePreview = 0;
    bool hdr = false, reused = false, modelApplied = false;
    bool modelApplicationKnown = true, selectionKnown = true, colorSpaceKnown = false;
    unsigned colorSpace = 0;
    const char* stage = "NR-final-target-after-FinishColor-before-Opt-UI";
};
struct Status
{
    uint64_t revision = 0;
    bool busy = false, failed = false;
    std::string message = "Capture idle (F10)";
    std::string directory;
};

inline bool FormatInfo(DXGI_FORMAT format, ImageInfo& image)
{
    switch (format)
    {
    case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        image.pixels = Pixels::Rgba8; image.bytesPerPixel = 4; return true;
    case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        image.pixels = Pixels::Bgra8; image.bytesPerPixel = 4; return true;
    case DXGI_FORMAT_R10G10B10A2_UNORM:
        image.pixels = Pixels::Rgb10A2; image.bytesPerPixel = 4; return true;
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
        image.pixels = Pixels::Half4; image.bytesPerPixel = 8; return true;
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
        image.pixels = Pixels::Float4; image.bytesPerPixel = 16; return true;
    default: return false; // typeless/multiplane semantics are not guessed
    }
}

// One request, one immutable readback, one worker, no request queue. Source/list/
// allocator/queue references survive through physical completion and the end of
// their recording generation. Never call Map on a discarded/replayed recording.
class Capture
{
    struct Job
    {
        std::filesystem::path directory;
        std::chrono::steady_clock::time_point requested = std::chrono::steady_clock::now();
        ComPtr<ID3D12Resource> source, readback;
        ComPtr<ID3D12CommandList> commands;
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12Device> device;
        ComPtr<ID3D12CommandQueue> queue;
        ComPtr<ID3D12Fence> fence;
        DlssNrGpuLifetime::Use use; // borrowed generation owner only; worker never reads token fields
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint {};
        D3D12_RESOURCE_DESC sourceDesc {};
        ImageInfo image;
        Metadata metadata;
        uint64_t readbackBytes = 0, target = 0;
        unsigned x = 0, y = 0;
        D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
        bool recorded = false, submitted = false, closed = false, invalid = false;
        bool unknown = false, cancelled = false, terminalReported = false;
        bool encoding = false, owned = false, gpuRetired = false;
        // Diagnostic identities only; never dereferenced after owner retirement.
        uintptr_t sourceIdentity = 0, queueIdentity = 0, commandListIdentity = 0;
    };
    std::mutex mutex_;
    std::condition_variable changed_;
    std::shared_ptr<Job> job_;
    std::atomic<ID3D12CommandList*> watched_ { nullptr };
    Status status_;
    uint64_t requestSequence_ = 0;
    bool shuttingDown_ = false;
    // Zero = idle, one = Present capture acquiring/using a backbuffer, two = resize.
    // Present admission is a single try-CAS and never waits for a resize/worker.
    std::atomic<unsigned> admission_ { 0 };
#ifdef DLSSNR_CURRENT_OUTPUT_CAPTURE_TEST
    inline static std::atomic<bool> testPauseWrite_ { false }, testWritePaused_ { false };
#endif

    void Say(const std::string& text, bool failed, bool busy, const std::string& path = {})
    {
        ++status_.revision; status_.message = text; status_.failed = failed;
        status_.busy = busy; status_.directory = path;
    }
    static std::string PathText(const std::filesystem::path& path)
    {
        const auto utf8 = path.u8string();
        return std::string(reinterpret_cast<const char*>(utf8.data()), utf8.size());
    }
    static DWORD WINAPI WorkerEntry(void* parameter)
    {
        auto* self = static_cast<Capture*>(parameter);
        HMODULE module = nullptr;
        // The requesting thread acquired this reference before starting us. A
        // second reference is not needed; pass it through a bounded member slot.
        { std::lock_guard lock(self->mutex_); module = self->workerModule_; }
        self->RunGuarded();
        FreeLibraryAndExitThread(module, 0);
    }
    HMODULE workerModule_ = nullptr;


    // Called with mutex_, both by the worker and by resize admission. Closure
    // and exact last-submit completion are separate requirements. Cancellation,
    // elapsed time, a token's first fence, or queue mismatch cannot retire work.
    bool RetireGpuLocked(Job& job)
    {
        if (job.gpuRetired) return true;
        if (!job.recorded) return false;
        const bool removed = job.device && FAILED(job.device->GetDeviceRemovedReason());
        if (!removed)
        {
            if (!job.closed) return false;
            if (job.submitted)
            {
                if (job.unknown || !job.fence || !job.target) return false;
                const auto done = job.fence->GetCompletedValue();
                if (done == UINT64_MAX || done < job.target) return false;
            }
            else job.invalid = true; // closed, never executed: safe discard only
        }
        else job.invalid = job.cancelled = true;
        // Keep the readback for CPU Map, but drop every source/list/allocator
        // owner before encoding or IO. Numeric metadata was captured earlier.
        job.commands.Reset(); job.allocator.Reset(); job.source.Reset();
        job.queue.Reset(); job.use.reset();
        job.gpuRetired = true;
        watched_.store(nullptr);
        return true;
    }

    bool ReserveResize()
    {
        unsigned idle = 0;
        return admission_.compare_exchange_strong(idle, 2);
    }
    bool RetireResizeCapture()
    {
        if (admission_.load() != 2) return false;
        // Reservation must precede real owned NR closure. Its successful Reset
        // notifies a borrowed capture; only then can its exact fence retire it.
        // Fail without waiting if recording/worker has this short metadata lock.
        std::unique_lock lock(mutex_, std::try_to_lock);
        return lock.owns_lock() && (!job_ || !job_->recorded || RetireGpuLocked(*job_));
    }
    void EndResize() { admission_.store(0); }

    void RunGuarded() noexcept
    {
        try { Run(); }
        catch (const std::exception& error)
        {
            std::lock_guard lock(mutex_);
            // Exception must never drop a still executable readback/list. Keep
            // the bounded job parked; Shutdown cannot destroy this singleton.
            if (job_) job_->invalid = true;
            Say(std::string("Capture failed: ") + error.what() + "; resources retained if necessary", true, bool(job_));
        }
        catch (...)
        {
            std::lock_guard lock(mutex_);
            if (job_) job_->invalid = true;
            Say("Capture failed; bounded resources retained for safety", true, bool(job_));
        }
    }

    void Write(const Job& job, const std::vector<uint8_t>& tight)
    {
        // Nothing in this function runs on Present/submit/reset/menu threads.
        std::filesystem::create_directories(job.directory.parent_path());
        if (!std::filesystem::create_directory(job.directory))
            throw std::runtime_error("Capture timestamp directory already exists");
        {
            std::ofstream raw(job.directory / "output.raw", std::ios::binary);
            raw.exceptions(std::ios::badbit | std::ios::failbit);
            raw.write(reinterpret_cast<const char*>(tight.data()), static_cast<std::streamsize>(tight.size()));
            raw.close();
        }
        // Raw first: an encoding/disk error cannot silently pass as a good PNG.
        WritePng(job.directory / "output.png", job.image, tight);
        std::ofstream meta(job.directory / "metadata.json", std::ios::binary);
        meta.exceptions(std::ios::badbit | std::ios::failbit);
        const bool derivative = job.image.encoding == 0 || job.image.encoding == 1 || job.image.encoding == 4 ||
                                job.image.pixels == Pixels::Half4 || job.image.pixels == Pixels::Float4;
        meta << "{\n  \"schema\": 1,\n  \"stage\": \"" << job.metadata.stage << "\",\n"
             << "  \"includesOptUI\": false,\n  \"epoch\": " << job.metadata.epoch
             << ",\n  \"dispatch\": " << job.metadata.dispatch
             << ",\n  \"sourceIdentity\": " << job.sourceIdentity
             << ",\n  \"queueIdentity\": " << job.queueIdentity
             << ",\n  \"commandListIdentity\": " << job.commandListIdentity
             << ",\n  \"dxgiFormat\": " << unsigned(job.sourceDesc.Format)
             << ",\n  \"allocationWidth\": " << job.sourceDesc.Width
             << ",\n  \"allocationHeight\": " << job.sourceDesc.Height
             << ",\n  \"rawBytesPerPixel\": " << job.image.bytesPerPixel
             << ",\n  \"rawRowBytes\": " << uint64_t(job.image.width) * job.image.bytesPerPixel
             << ",\n  \"subrect\": [" << job.x << ',' << job.y << ',' << job.image.width << ',' << job.image.height << ']'
             << ",\n  \"restoredState\": " << unsigned(job.state)
             << ",\n  \"encoding\": " << job.image.encoding
             << ",\n  \"swapchainColorSpaceKnown\": " << (job.metadata.colorSpaceKnown ? "true" : "false")
             << ",\n  \"swapchainColorSpace\": " << job.metadata.colorSpace
             << ",\n  \"primaries\": \"" << (job.image.encoding == 2 ? "sRGB by selected colour contract" : "UNKNOWN; not inferred from transfer") << "\""
             << ",\n  \"debugView\": " << job.metadata.debugView
             << ",\n  \"guidePreview\": " << job.metadata.guidePreview
             << ",\n  \"selectionObserved\": " << (job.metadata.selectionKnown ? "true" : "false")
             << ",\n  \"reused\": " << (job.metadata.reused ? "true" : "false")
             << ",\n  \"modelApplied\": " << (job.metadata.modelApplied ? "true" : "false")
             << ",\n  \"modelApplicationObserved\": " << (job.metadata.modelApplicationKnown ? "true" : "false")
             << ",\n  \"fenceTarget\": " << job.target
             << ",\n  \"raw\": \"output.raw: original little-endian typed pixels, tight rows, top-to-bottom, no quantization\""
             << ",\n  \"png\": \"output.png: RGBA16; integer rescaling is quantized; float values are clamped"
             << (derivative ? "; DERIVED PREVIEW, not HDR reference; no inferred sRGB tag" : "") << "\""
             << ",\n  \"previewMapping\": \"linear: per-component Reinhard then sRGB-like transfer; PQ: ST2084 EOTF/203nits then same; unknown primaries retained, no gamut conversion; SDR: code values\""
             << ",\n  \"completion\": \"actual Execute callback, successful same-queue Signal, physical fence, sealed recording generation\"\n}\n";
        meta.close();
    }

    void Run()
    {
        std::shared_ptr<Job> job;
        { std::lock_guard lock(mutex_); job = job_; }
        if (!job) return;
        HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        bool eventRegistered = false;
        for (;;)
        {
            std::unique_lock lock(mutex_);
            const auto age = std::chrono::steady_clock::now() - job->requested;
            if (!job->recorded && (job->cancelled || age > std::chrono::seconds(2)))
            {
                if (!job->terminalReported) Say("Capture not available: no current eligible output within 2 seconds", true, false);
                else { ++status_.revision; status_.busy = false; }
                watched_.store(nullptr); job_.reset(); lock.unlock(); if (event) CloseHandle(event); return;
            }
            if (job->recorded && !job->gpuRetired && age > std::chrono::seconds(10) && !job->terminalReported)
            {
                job->invalid = true; job->terminalReported = true;
                Say("Capture timed out; readback retained until actual retirement (busy)", true, true);
            }
            if (job->recorded && RetireGpuLocked(*job)) break;
            if (job->submitted && job->closed && !job->unknown)
            {
                const uint64_t target = job->target;
                auto fence = job->fence;
                lock.unlock();
                if (event && !eventRegistered)
                    eventRegistered = SUCCEEDED(fence->SetEventOnCompletion(target, event));
                if (eventRegistered) WaitForSingleObject(event, 100); // worker only
                else
                {
                    std::unique_lock waitLock(mutex_);
                    changed_.wait_for(waitLock, std::chrono::milliseconds(100));
                }
            }
            else changed_.wait_for(lock, std::chrono::milliseconds(100));
        }
        if (event) CloseHandle(event);
        bool write = false;
        {
            std::lock_guard lock(mutex_);
            write = !job->invalid && !job->cancelled && !shuttingDown_;
            job->encoding = true;
            if (write) Say("Capture encoding on background worker", false, true, PathText(job->directory));
        }
        std::string failure;
        if (write)
        {
            try
            {
                const size_t rowBytes = size_t(job->image.width) * job->image.bytesPerPixel;
                std::vector<uint8_t> tight(rowBytes * job->image.height);
                void* mapped = nullptr;
                D3D12_RANGE range { 0, static_cast<SIZE_T>(job->readbackBytes) };
                if (FAILED(job->readback->Map(0, &range, &mapped)) || !mapped)
                    throw std::runtime_error("Readback Map failed");
                for (unsigned y = 0; y < job->image.height; ++y)
                    std::memcpy(tight.data() + size_t(y) * rowBytes,
                        static_cast<const uint8_t*>(mapped) + job->footprint.Offset + size_t(y) * job->footprint.Footprint.RowPitch,
                        rowBytes);
                D3D12_RANGE written { 0, 0 }; job->readback->Unmap(0, &written);
                job->readback.Reset(); // CPU pixels now independent of GPU owners
#ifdef DLSSNR_CURRENT_OUTPUT_CAPTURE_TEST
                // Test executable only: pause AFTER owner retirement and Map,
                // without holding the service mutex or any source reference.
                while (testPauseWrite_.load())
                { testWritePaused_.store(true); Sleep(1); }
                testWritePaused_.store(false);
#endif
                Write(*job, tight);
            }
            catch (const std::exception& error) { failure = error.what(); }
            catch (...) { failure = "Unknown encoder/IO failure"; }
        }
        {
            std::lock_guard lock(mutex_);
            if (write && failure.empty()) Say("Capture saved", false, false, PathText(job->directory));
            else if (!failure.empty()) Say("Capture failed: " + failure, true, false, PathText(job->directory));
            else Say("Capture cancelled/rejected; no image saved", true, false);
            watched_.store(nullptr); job_.reset();
        }
    }

public:

    // The caller must declare this before GetBuffer and its local COM owners:
    // reverse destruction ends admission only AFTER those locals are released.
    class PresentScope
    {
        Capture* owner_ = nullptr;
    public:
        PresentScope()
        {
            auto& owner = Capture::Instance(); unsigned idle = 0;
            if (owner.admission_.compare_exchange_strong(idle, 1)) owner_ = &owner;
        }
        ~PresentScope() { if (owner_) owner_->admission_.store(0); }
        explicit operator bool() const { return owner_ != nullptr; }
        PresentScope(const PresentScope&) = delete;
        PresentScope& operator=(const PresentScope&) = delete;
    };
    // One bounded global capture lane. Concurrent/unretired resize is refused;
    // an unrelated swapchain can conservatively receive WAS_STILL_DRAWING too.
    class ResizeScope
    {
        Capture* owner_ = nullptr;
    public:
        enum class Mode { RetireCapture, ReserveOnly };
        explicit ResizeScope(Mode mode = Mode::RetireCapture)
        {
            auto& owner = Capture::Instance();
            if (!owner.ReserveResize()) return;
            owner_ = &owner;
            // Preserve standalone admission's original retirement gate. Wrappers
            // reserve first, close real owned NR, then explicitly check readiness.
            if (mode == Mode::RetireCapture && !ReadyForResize())
            { owner_->EndResize(); owner_ = nullptr; }
        }
        ~ResizeScope() { if (owner_) owner_->EndResize(); }
        bool ReadyForResize() { return owner_ && owner_->RetireResizeCapture(); }
        explicit operator bool() const { return owner_ != nullptr; }
        ResizeScope(const ResizeScope&) = delete;
        ResizeScope& operator=(const ResizeScope&) = delete;
    };
#ifdef DLSSNR_CURRENT_OUTPUT_CAPTURE_TEST
    static void PauseBeforeWriteForTest(bool pause) { testPauseWrite_.store(pause); }
    static bool IsWritePausedForTest() { return testWritePaused_.load(); }
    bool HasGpuOwnersForTest()
    {
        std::lock_guard lock(mutex_);
        return job_ && (job_->source || job_->commands || job_->allocator || job_->queue);
    }
#endif
    // Deliberately process-lifetime: C++ static destruction cannot release unknown
    // GPU work or join a worker while DllMain holds the loader lock.
    static Capture& Instance() { static Capture* instance = new Capture; return *instance; }
    Status GetStatus() { std::lock_guard lock(mutex_); return status_; }
    bool Requested()
    {
        std::lock_guard lock(mutex_);
        return job_ && !job_->recorded && !job_->cancelled && !shuttingDown_;
    }
    void Unavailable(const char* reason)
    {
        std::lock_guard lock(mutex_);
        if (job_) { Say("Capture busy: previous request still owns its resources", false, true); return; }
        Say(std::string("Capture not available: ") + reason, true, false);
    }
    void RejectPending(const char* reason)
    {
        std::lock_guard lock(mutex_);
        if (job_ && !job_->recorded)
        {
            job_->cancelled = job_->terminalReported = true;
            Say(std::string("Capture failed: ") + reason, true, true);
            changed_.notify_all();
        }
    }
    void Request(const std::filesystem::path& root)
    {
        std::lock_guard lock(mutex_);
        if (job_) { Say("Capture busy: previous request still owns its resources", false, true); return; }
        if (shuttingDown_) { Say("Capture not available during teardown", true, false); return; }
        auto job = std::make_shared<Job>();
        SYSTEMTIME utc {}; GetSystemTime(&utc);
        std::wostringstream name;
        name << std::setfill(L'0') << std::setw(4) << utc.wYear << std::setw(2) << utc.wMonth
             << std::setw(2) << utc.wDay << L'-' << std::setw(2) << utc.wHour << std::setw(2) << utc.wMinute
             << std::setw(2) << utc.wSecond << L'-' << std::setw(3) << utc.wMilliseconds
             << L"Z-pid" << GetCurrentProcessId() << L'-' << ++requestSequence_;
        job->directory = root / name.str();
        HMODULE module = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                reinterpret_cast<LPCWSTR>(&WorkerEntry), &module))
        { Say("Capture failed: could not retain encoder module", true, false); return; }
        workerModule_ = module; job_ = job;
        HANDLE worker = CreateThread(nullptr, 0, &WorkerEntry, this, 0, nullptr);
        if (!worker)
        { job_.reset(); FreeLibrary(module); Say("Capture failed: worker could not start", true, false); return; }
        CloseHandle(worker);
        Say("Capture requested: next current output", false, true, PathText(job->directory));
    }

    // Called under the product NR mutex. All preflight/allocation succeeds before
    // inserting any copy. Source state and the exact active box are restored.
    bool Record(ID3D12GraphicsCommandList* commands, ID3D12Device* device, ID3D12Resource* source,
                D3D12_RESOURCE_STATES state, unsigned x, unsigned y, unsigned width, unsigned height,
                const Metadata& metadata, const DlssNrGpuLifetime::Use& use,
                ID3D12CommandQueue* expectedQueue = nullptr, ID3D12CommandAllocator* allocator = nullptr,
                bool owned = false, ID3D12CommandList* identity = nullptr)
    {
        std::lock_guard lock(mutex_);
        auto job = job_;
        if (!job || job->recorded || job->cancelled || shuttingDown_ || admission_.load() == 2 || deferEndpoint) return false;
        const auto reject = [&](const char* why) {
            job->cancelled = true; job->terminalReported = true;
            Say(std::string("Capture not available: ") + why, true, true); changed_.notify_all(); return false;
        };
        if (!commands || !device || !source || commands->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT)
            return reject("missing exact DIRECT recording identity");
        if ((!owned && !use) || (owned && (use || !expectedQueue || !allocator || !identity || identity != commands)))
            return reject("borrowed recording needs NR use; private capture requires empty use and exact queue/allocator/list");
        const auto desc = source->GetDesc();
        ImageInfo image {}; image.width = width; image.height = height; image.encoding = metadata.encoding;
        if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.DepthOrArraySize != 1 || desc.MipLevels != 1 ||
            desc.SampleDesc.Count != 1 || !width || !height || width > MaxExtent || height > MaxExtent ||
            uint64_t(x) + width > desc.Width || uint64_t(y) + height > desc.Height || !FormatInfo(desc.Format, image))
            return reject("unsupported format or invalid active subrect");
        if (uint64_t(width) * height * image.bytesPerPixel > MaxTightBytes) return reject("CPU image exceeds 128 MiB budget");
        ComPtr<ID3D12Device> sourceDevice, listDevice;
        if (FAILED(source->GetDevice(IID_PPV_ARGS(&sourceDevice))) ||
            FAILED(commands->GetDevice(IID_PPV_ARGS(&listDevice))) || sourceDevice.Get() != device || listDevice.Get() != device)
            return reject("source/list device mismatch");
        ComPtr<ID3D12Device> queueDevice, allocatorDevice;
        if ((expectedQueue && (expectedQueue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT ||
                FAILED(expectedQueue->GetDevice(IID_PPV_ARGS(&queueDevice))) || queueDevice.Get() != device)) ||
            (allocator && (FAILED(allocator->GetDevice(IID_PPV_ARGS(&allocatorDevice))) || allocatorDevice.Get() != device)))
            return reject("queue/allocator device mismatch");
        D3D12_RESOURCE_DESC compact = desc; compact.Width = width; compact.Height = height;
        UINT rows = 0; UINT64 rowSize = 0, bytes = 0;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint {};
        device->GetCopyableFootprints(&compact, 0, 1, 0, &footprint, &rows, &rowSize, &bytes);
        if (!bytes || bytes > MaxReadbackBytes || rows != height || rowSize != uint64_t(width) * image.bytesPerPixel)
            return reject("readback exceeds 128 MiB budget or invalid footprint");
        ComPtr<ID3D12Resource> readback; ComPtr<ID3D12Fence> fence;
        D3D12_RESOURCE_DESC buffer {}; buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width = bytes; buffer.Height = 1; buffer.DepthOrArraySize = 1; buffer.MipLevels = 1;
        buffer.SampleDesc.Count = 1; buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        D3D12_HEAP_PROPERTIES heap {}; heap.Type = D3D12_HEAP_TYPE_READBACK; heap.CreationNodeMask = heap.VisibleNodeMask = 1;
        if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback))) ||
            FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
            return reject("readback/fence allocation failed");
        job->device = device; job->source = source; job->commands = identity ? identity : commands; job->queue = expectedQueue;
        job->allocator = allocator; job->readback = readback; job->fence = fence; job->use = use;
        job->sourceIdentity = reinterpret_cast<uintptr_t>(source);
        job->commandListIdentity = reinterpret_cast<uintptr_t>(job->commands.Get());
        job->queueIdentity = reinterpret_cast<uintptr_t>(expectedQueue);
        job->sourceDesc = desc; job->footprint = footprint; job->readbackBytes = bytes;
        job->image = image; job->metadata = metadata; job->x = x; job->y = y; job->state = state; job->owned = owned;
        D3D12_RESOURCE_BARRIER barrier {}; barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = source; barrier.Transition.Subresource = 0;
        barrier.Transition.StateBefore = state; barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        if (state != D3D12_RESOURCE_STATE_COPY_SOURCE) commands->ResourceBarrier(1, &barrier);
        D3D12_TEXTURE_COPY_LOCATION src {}; src.pResource = source; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION dst {}; dst.pResource = readback.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = footprint;
        const D3D12_BOX box { x, y, 0, x + width, y + height, 1 };
        commands->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
        if (state != D3D12_RESOURCE_STATE_COPY_SOURCE)
        { std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter); commands->ResourceBarrier(1, &barrier); }
        job->recorded = true;
        watched_.store(job->commands.Get());
        Say("Capture recorded: waiting for actual submit and recording closure", false, true, PathText(job->directory));
        changed_.notify_all(); return true;
    }

    bool Watches(ID3D12CommandList* list)
    {
        return list && watched_.load() == list;
    }
    void Submitted(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists)
    {
        std::lock_guard lock(mutex_);
        auto job = job_;
        if (!job || !job->recorded || job->closed || !queue || !lists) return;
        for (UINT i = 0; i < count; ++i)
        {
            if (lists[i] != job->commands.Get()) continue;
            if (job->submitted) { job->invalid = true; Say("Capture rejected: repeated command-list execution", true, true); }
            job->submitted = true;
            ComPtr<ID3D12Device> queueDevice;
            if ((job->queue && job->queue.Get() != queue) || queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT ||
                FAILED(queue->GetDevice(IID_PPV_ARGS(&queueDevice))) || queueDevice.Get() != job->device.Get())
            {
                job->invalid = job->unknown = true;
                Say("Capture rejected: queue/device mismatch; bounded readback quarantined", true, true);
                continue;
            }
            job->queue = queue; job->queueIdentity = reinterpret_cast<uintptr_t>(queue);
            if (FAILED(queue->Signal(job->fence.Get(), ++job->target)))
            {
                job->invalid = job->unknown = true;
                Say("Capture failed: queue Signal failed; bounded readback quarantined", true, true);
            }
        }
        changed_.notify_all();
    }
    void Reset(ID3D12CommandList* list)
    {
        std::lock_guard lock(mutex_);
        if (!job_ || !job_->recorded || job_->commands.Get() != list || job_->closed) return;
        job_->closed = true; if (!job_->submitted) job_->invalid = true;
        watched_.store(nullptr);
        changed_.notify_all();
    }
    // A private, never-exposed list can be sealed by its creator immediately
    // after its sole Execute callback. The service retains the allocator/list.
    void SealOwned(ID3D12CommandList* list)
    {
        std::lock_guard lock(mutex_);
        if (job_ && job_->commands.Get() == list && job_->owned && job_->submitted && !job_->closed)
        { job_->closed = true; watched_.store(nullptr); changed_.notify_all(); }
    }
    void CancelOwnedRecording(ID3D12CommandList* list)
    {
        std::lock_guard lock(mutex_);
        if (job_ && job_->commands.Get() == list && job_->owned && !job_->submitted)
        { job_->closed = job_->invalid = job_->cancelled = true; watched_.store(nullptr); changed_.notify_all(); }
    }
    void Shutdown()
    {
        std::lock_guard lock(mutex_); shuttingDown_ = true;
        if (job_)
        {
            job_->cancelled = true;
            Say("Capture cancelled during teardown; physical ownership retained", true, true);
        }
        changed_.notify_all(); // no join/wait, no premature GPU release
    }
};
} // namespace DlssNrCurrentOutput
