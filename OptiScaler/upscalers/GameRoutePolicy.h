#pragma once

#include <runtime/GameIdentity.h>
#include <runtime/F6InputPolicy.h>

namespace GameRouting
{


// Retained INI placement opt-in now selects scene output at successful SR.
inline bool UseGenshinSourceOutput(Identity identity, bool requested) noexcept
{
    return F6InputPolicy::DirectDlssInputs &&
        identity == Identity::Genshin && requested;
}
}
