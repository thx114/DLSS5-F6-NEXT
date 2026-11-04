#pragma once
#include <windows.h>
#include <cstdint>

// A load/session admission gate. Existing graphics hooks are never dismantled
// from a timer or network thread. HTTPS expiry learned after admission applies
// to the NEXT load, via a version-scoped file latch.
namespace MaintenanceGate
{
inline constexpr std::uint64_t CutoffFileTicks = 134379360000000000ULL;
inline constexpr wchar_t LatchSuffix[] = L"\\OptiScaler\\F5-NEXT-20261017-expired-20261101.bin";
inline constexpr char LatchMarker[] = "F5-NEXT-20261017|2026-10-31T16:00:00Z|EXPIRED\n";
inline constexpr wchar_t Notice[] = L"该版本已停止维护，OptiScaler/DLSS 注入已禁用";
constexpr bool IsExpiredAt(std::uint64_t ticks) noexcept { return ticks >= CutoffFileTicks; }
constexpr bool RejectAtLoad(std::uint64_t ticks, bool remembered) noexcept
{
    return IsExpiredAt(ticks) || remembered;
}
// Local time + persisted HTTPS proof only; no network, registry, logger,
// Config or State. The latch is a user-writable maintenance hint, NOT tamper
// protection. Only a fixed local drive path is accepted (UNC/removable rejected).
bool GetLatchPath(wchar_t* path, DWORD capacity) noexcept;
// Local time and strict version-specific latch contents decide this load.
bool RejectThisLoad() noexcept;
// Called in DllMain only to queue a worker. Never waits for it. Windows starts
// its thread routine after DLL_PROCESS_ATTACH notifications have completed.
void QueueWorker(HMODULE module, bool rejected) noexcept;
// Diagnostic/test boundary: synchronous HTTPS Date. Call only from a worker or
// standalone console fixture, never DllMain or any render callback. No writes.
bool QueryHttpsDate(const wchar_t* host, std::uint64_t& ticks) noexcept;
}
