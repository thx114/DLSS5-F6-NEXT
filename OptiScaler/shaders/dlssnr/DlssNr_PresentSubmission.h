#pragma once
#include <cstdint>

namespace DlssNrPresentSubmission
{
// A model-create token is advanced only after the endpoint has executed its list
// and successfully enqueued its fence signal. App presents and SR evaluates may
// stop/advance independently; neither is evidence that this list was submitted.
// The high bit separates this owner from the existing SR/native epoch domain.
class Clock
{
  public:
    static constexpr uint64_t Domain = uint64_t{1} << 63;
    uint64_t Token() const { return _quarantined ? 0 : Domain | _sequence; }
    bool Commit(uint64_t token)
    {
        if (!token || token != Token()) return false;
        if (_sequence == Domain - 1) { Quarantine(); return false; }
        ++_sequence;
        return true;
    }
    void Quarantine() { _quarantined = true; }
    bool Quarantined() const { return _quarantined; }

  private:
    uint64_t _sequence = 1;
    bool _quarantined = false;
};
}
