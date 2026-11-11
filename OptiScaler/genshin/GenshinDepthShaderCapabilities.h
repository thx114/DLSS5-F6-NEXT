// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <native/F5LowShaderBytecode.h>
#include "GenshinDepthMapping_Hlsl.h"
#include <motion/TrustMaskSceneValidity_Hlsl.h>
#include <cstdint>

namespace GenshinDepthShaders {
constexpr uint64_t HashSource(const char* source)noexcept {
    uint64_t hash=14695981039346656037ull;
    for(;*source;++source){hash^=static_cast<unsigned char>(*source);hash*=1099511628211ull;}
    return hash;
}
// Only the new generator emits this API, after compiling every new entry.
// An old header compiles but advertises no depth capability. A source change
// invalidates a previously generated group until Luna regenerates it.
inline const F5LowShaderBytecode::Shader* Find(const char* group,const char* entry)noexcept {
#if defined(F5_DEPTH_SCENE_VALIDITY_ABI) && F5_DEPTH_SCENE_VALIDITY_ABI == 1
    const bool bridge=strcmp(group,"BridgeDepth")==0;
    const char* source=bridge?(strcmp(entry,"ResolveDepth")==0?
        GenshinDepthMappingHlsl::Resolve:GenshinDepthMappingHlsl::Raster):TrustMaskSceneValidityHlsl::kSource;
    const char* key=bridge?(strcmp(entry,"ResolveDepth")==0?"BridgeDepthResolve":"BridgeDepthRaster"):"TrustMaskSceneValidity";
    if(!F5LowShaderBytecode::DepthSourceMatches(key,HashSource(source)))return nullptr;
    return F5LowShaderBytecode::Find(group,entry);
#else
    (void)group;(void)entry;return nullptr;
#endif
}
}
