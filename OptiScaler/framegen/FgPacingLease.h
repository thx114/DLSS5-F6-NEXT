#pragma once
#include <sl.h>
#include <cstdint>
#include <memory>

// Independent CPU/SDK lifetime, NEVER GPU retirement authority. The concrete
// provider must keep the ORIGINAL SDK token and loaded APIs valid until the
// lease dies, exclude token recycling and close admission during teardown.
// None of these methods may access an unretained provider, acquire its FG or
// continuity/execution locks, or reacquire a token from a saved frame number.
class FGPacingLease
{
public:
    virtual ~FGPacingLease() = default;
    virtual bool Valid() const noexcept = 0;
    virtual uint64_t Frame() const noexcept = 0;
    virtual sl::Result Sleep() noexcept = 0;
};
using FGPacingLeasePtr = std::shared_ptr<FGPacingLease>;
