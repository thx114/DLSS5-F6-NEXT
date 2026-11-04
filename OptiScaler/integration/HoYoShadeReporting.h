#pragma once
#include "HoYoShadeOwnedRuntime.h"
#include <array>
#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>

namespace HoYoShade::Diagnostics
{
struct Report
{
    uint64_t count = 0;
    bool emit = false;
};

// Callback/stage observations are serialized by executionMutex. A changing
// endpoint/reason never resets this clock or creates an address-keyed stream.
// Coalesce rapid changes; retain the LAST EMITTED state so a skipped change is
// reported once stable. Clock/counts are diagnostics only, never GPU authority.
template<class Snapshot>
class ChangeReports
{
  public:
    Report Next(const Snapshot& value, uint64_t now, std::string_view key = {})
    {
        ++observations;
        const bool changed = !initialized || !(value == last) || key != lastKey;
        const auto elapsed = now - lastTime;
        if (initialized && (elapsed < 1000 || (!changed && elapsed < 30000)))
            return {observations, false};
        // Commit the key first: allocation failure cannot partially publish a
        // report. Callers contain all diagnostics exceptions locally.
        lastKey.assign(key);
        last = value; lastTime = now; initialized = true;
        return {observations, true};
    }
  private:
    Snapshot last{};
    std::string lastKey;
    uint64_t observations = 0, lastTime = 0;
    bool initialized = false;
};

// Two process-lifetime scalar streams, never partitioned by object address.
// Logging only: callers must execute GPU work/collection before consulting this.
enum class ExecutionStream : unsigned { OwnedSubmit, NativeSubmit, Count };
class ExecutionReports
{
  public:
    Report Next(ExecutionStream stream) noexcept
    {
        const auto index = static_cast<unsigned>(stream);
        if (index >= counts.size()) return {};
        const auto count = counts[index].fetch_add(1, std::memory_order_relaxed) + 1;
        // Reuse the typed-diagnostic policy: first three, then every 600.
        // Counter overflow is modular; zero is never a report ordinal.
        return { count, count != 0 && OwnedRuntime::ShouldReportDiagnosticCount(count) };
    }

  private:
    static_assert(std::atomic<uint64_t>::is_always_lock_free);
    std::array<std::atomic<uint64_t>, static_cast<unsigned>(ExecutionStream::Count)> counts{};
};
}
