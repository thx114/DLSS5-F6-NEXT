#pragma once

// Copy/submission diagnostics and export-resolution logging only. Never replace a provider export:
// NGX providers validate their immediate caller, so even argument-preserving wrappers can break FG.
// Unobserved resolution routes and provider-internal resource consumption remain UNKNOWN.
#include <windows.h>
#include <d3d12.h>
#include <nvsdk_ngx.h>
#include <wrl/client.h>
#include <framegen/dlssg/MfgUnlockProvider.h>
#include <array>
#include <atomic>
#include <mutex>
#include <cstring>
#include <climits>

namespace DlssNrFgTrace
{
inline constexpr const char* Build = "F5-STARRAIL-STABLE-GUIDE-CONTRACT-20261016-l";
using Evaluate = decltype(&NVSDK_NGX_D3D12_EvaluateFeature);

inline uintptr_t Identity(IUnknown* object)
{
    if (!object) return 0;
    Microsoft::WRL::ComPtr<IUnknown> identity;
    return SUCCEEDED(object->QueryInterface(IID_PPV_ARGS(&identity)))
               ? reinterpret_cast<uintptr_t>(identity.Get()) : 0;
}

struct Copy
{
    uint64_t epoch = 0, sequence = 0, value = 0;
    uintptr_t source = 0, color = 0, swapchain = 0, queue = 0, fence = 0;
    unsigned index = UINT_MAX;
    uint64_t armSequence = 0;
};
struct Output
{
    Copy copy;
    uint64_t epoch = 0, sequence = 0, done = 0;
    uintptr_t color = 0, commandList = 0, queue = 0;
    bool ran = false;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence;
};
inline std::mutex snapshotMutex;
inline Copy lastCopy;
inline Output lastOutput;
inline std::atomic_bool armed { false };
inline uint64_t copies = 0, outputs = 0, armedCopies = 0;

struct Snapshot
{
    Copy copy;
    Output output;
    bool nrMatchesCurrentCopy = false;
};
inline Snapshot ReadSnapshot()
{
    std::lock_guard lock(snapshotMutex);
    return { lastCopy, lastOutput,
             lastCopy.sequence != 0 && lastOutput.copy.sequence == lastCopy.sequence &&
             lastOutput.copy.epoch == lastCopy.epoch && lastOutput.epoch == lastCopy.epoch && lastOutput.ran };
}

inline bool Sample(uint64_t count) { return count <= 8 || count % 300 == 0; }

inline void BridgeCopy(uint64_t epoch, IUnknown* source, IUnknown* color, IUnknown* swapchain, unsigned index,
                       IUnknown* queue, IUnknown* fence, uint64_t value)
{
    Copy copy { epoch, 0, value, Identity(source), Identity(color), Identity(swapchain), Identity(queue),
                Identity(fence), index };
    {
        std::lock_guard lock(snapshotMutex);
        copy.sequence = ++copies;
        copy.armSequence = ++armedCopies;
        lastCopy = copy;
    }
    if (!armed.exchange(true))
        LOG_INFO("F5 NR trace armed: build={} pid={}; identity-only, no pixel/readback proof", Build, GetCurrentProcessId());
    if (Sample(copy.armSequence))
        LOG_INFO("F5 NR trace copy: seq={} epoch={} sc={:X} index={} source={:X} color={:X} queue={:X} fence={:X} value={}",
                 copy.sequence, epoch, copy.swapchain, index, copy.source, copy.color, copy.queue, copy.fence, value);
}

inline void NrSubmitted(uint64_t epoch, IUnknown* color, IUnknown* commandList, IUnknown* queue,
                        ID3D12Fence* fence, uint64_t done, bool ran)
{
    Output output;
    output.epoch = epoch; output.color = Identity(color); output.commandList = Identity(commandList);
    output.queue = Identity(queue); output.fence = fence; output.done = done; output.ran = ran;
    {
        std::lock_guard lock(snapshotMutex);
        output.copy = lastCopy;
        output.sequence = ++outputs;
        lastOutput = output;
    }
    if (Sample(output.sequence))
        LOG_INFO("F5 NR trace submit: seq={} epoch={} color={:X} list={:X} queue={:X} fence={:X} done={} completed={} ran={} copySeq={} copyEpoch={} copyColor={:X} sameColor={}; submitted is not displayed",
                 output.sequence, epoch, output.color, output.commandList, output.queue, Identity(fence), done,
                 fence ? fence->GetCompletedValue() : 0, ran, output.copy.sequence, output.copy.epoch,
                 output.copy.color, output.color != 0 && output.color == output.copy.color);
}

inline void EndBridge()
{
    armed.store(false);
    std::lock_guard lock(snapshotMutex);
    lastCopy = {}; lastOutput = {}; // no buffers, queues, command lists or provider modules are retained
    armedCopies = 0;
}

struct Resolution { uintptr_t module = 0, function = 0; };
inline std::array<Resolution, 8> resolutions;
inline std::mutex resolveMutex;

// Logging may inspect an export's origin, but every path returns exactly the original pointer.
// Do not call the resolved function here, keep an Evaluate trampoline, or alter its caller address.
inline FARPROC RecordResolved(HMODULE module, LPCSTR name, FARPROC resolved, HMODULE self)
{
    if (!resolved || !module || module == self || reinterpret_cast<uintptr_t>(name) < 0x10000 ||
        std::strcmp(name, "NVSDK_NGX_D3D12_EvaluateFeature") != 0)
        return resolved;
    wchar_t path[2048] {};
    const auto length = GetModuleFileNameW(module, path, _countof(path));
    if (!length || length >= _countof(path) || !MfgUnlock::Provider::IsProviderPath(std::wstring_view(path, length)))
        return resolved;
    char utf8[8192] {};
    WideCharToMultiByte(CP_UTF8, 0, path, -1, utf8, _countof(utf8), nullptr, nullptr);
    std::lock_guard lock(resolveMutex);
    for (size_t i = 0; i < resolutions.size(); ++i)
    {
        const auto moduleId = reinterpret_cast<uintptr_t>(module);
        const auto functionId = reinterpret_cast<uintptr_t>(resolved);
        if (resolutions[i].module == moduleId && resolutions[i].function == functionId) return resolved;
        if (resolutions[i].function) continue;
        resolutions[i] = { moduleId, functionId };
        LOG_INFO("F5 NR trace resolved: build={} pid={} slot={} module={:X} function={:X} path={}; original pointer preserved, no Evaluate wrapper",
                 Build, GetCurrentProcessId(), i, reinterpret_cast<uintptr_t>(module),
                 reinterpret_cast<uintptr_t>(resolved), utf8);
        return resolved;
    }
    // Bounded diagnostics cannot prevent the real function from being returned.
    return resolved;
}
} // namespace DlssNrFgTrace
