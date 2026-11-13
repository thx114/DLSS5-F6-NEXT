#pragma once

#include <runtime/GameIdentity.h>
#include <runtime/F6InputPolicy.h>

namespace GameRouting
{


inline bool UseGenshinPresentPipeline(Identity identity, bool requested) noexcept
{
    return F6InputPolicy::DirectDlssInputs &&
        identity == Identity::Genshin && requested;
}
}
