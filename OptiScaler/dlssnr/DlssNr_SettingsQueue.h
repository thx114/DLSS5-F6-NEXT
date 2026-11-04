#pragma once

#include <cstdint>
#include <limits>
#include <mutex>

// The menu publishes the newest enum value; DX12 applies it at an evaluation
// boundary under its own state mutex. No provider work runs on the menu thread.
namespace DlssNrSettingsQueue
{
constexpr uint32_t kColourEncodingCount = 5;
struct ColourEncodingUpdate
{
    uint64_t generation = 0;
    uint32_t value = 0;
};
inline std::mutex& Mutex()
{
    static std::mutex mutex;
    return mutex;
}
inline ColourEncodingUpdate& Pending()
{
    static ColourEncodingUpdate pending {};
    return pending;
}
inline uint64_t& NextGeneration()
{
    static uint64_t generation = 0;
    return generation;
}
inline bool IsValidColourEncoding(uint32_t value)
{
    return value < kColourEncodingCount;
}
inline bool SubmitColourEncoding(uint32_t value)
{
    if (!IsValidColourEncoding(value))
        return false;
    std::lock_guard lock(Mutex());
    uint64_t& generation = NextGeneration();
    if (generation == (std::numeric_limits<uint64_t>::max)())
        return false;
    Pending() = { ++generation, value };
    return true;
}
inline bool ConsumeColourEncoding(uint32_t& value, uint64_t& generation)
{
    std::lock_guard lock(Mutex());
    const ColourEncodingUpdate update = Pending();
    if (update.generation == 0)
        return false;
    Pending() = {};
    value = update.value;
    generation = update.generation;
    return IsValidColourEncoding(value);
}
} // namespace DlssNrSettingsQueue
