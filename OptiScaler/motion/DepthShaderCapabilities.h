// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <native/F5LowShaderBytecode.h>
#include <motion/TrustMaskSceneValidity_Hlsl.h>
#include <cstdint>
#include <cstring>

namespace DepthShaderCapabilities
{
constexpr uint64_t HashSource(const char* source) noexcept
{
    uint64_t hash = 14695981039346656037ull;
    for (; *source; ++source)
    {
        hash ^= static_cast<unsigned char>(*source);
        hash *= 1099511628211ull;
    }
    return hash;
}

inline const F5LowShaderBytecode::Shader* Find(const char* group, const char* entry) noexcept
{
#if defined(F5_DEPTH_SCENE_VALIDITY_ABI) && F5_DEPTH_SCENE_VALIDITY_ABI == 1
    if (!group || !entry || std::strcmp(group, "TrustMaskSceneValidity") != 0)
        return nullptr;
    if (!F5LowShaderBytecode::DepthSourceMatches("TrustMaskSceneValidity",
                                                 HashSource(TrustMaskSceneValidityHlsl::kSource)))
        return nullptr;
    return F5LowShaderBytecode::Find(group, entry);
#else
    (void) group;
    (void) entry;
    return nullptr;
#endif
}
} // namespace DepthShaderCapabilities
