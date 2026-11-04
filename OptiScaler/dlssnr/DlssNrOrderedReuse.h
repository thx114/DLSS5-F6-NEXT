#pragma once
#include <cstddef>

// Admission only, never completion or resource ownership. The DX12 integration supplies
// these facts from the local Genshin producer scope and real post-Execute callback.
namespace DlssNrOrderedReuse
{
inline constexpr std::size_t RetirementCapacity = 8;
struct Facts
{
    bool localProducer = false, submitted = false, signalled = false;
    bool sameQueue = false, sameDeviceAndType = false, healthy = false;
    bool sameContract = false, adjacent = false, freeRetirement = false, recordingBoundary = false;
};
inline bool Allows(const Facts& f)
{
    return f.localProducer && f.submitted && f.signalled && f.sameQueue &&
        f.sameDeviceAndType && f.healthy && f.sameContract && f.adjacent && f.freeRetirement && f.recordingBoundary;
}
}
