#pragma once
#include <string_view>
#include <cstddef>
#include <cstdint>

namespace HoYoShade::EffectRequirements {
// Explicit contracts for the two installed effects. Other effects retain
// their optional depth contract; this is not a shader-language parser.
inline bool Effect(std::string_view path, std::string_view name) noexcept {
    return path.substr(path.find_last_of("/\\") + 1) == name;
}
enum class Status { Ready, DefinitionUnknown, LaunchpadMissing, LaunchpadOrder, NormalsUnknown };
struct Plan {
    Status status = Status::Ready;
    bool depth = false, launchpadNormals = false;
    int normalsMode = -1;
};
template<class Source> Plan Inspect(const Source& source) noexcept {
    Plan plan;
    size_t launchpad = SIZE_MAX, mxao = SIZE_MAX;
    for (size_t i = 0; i < source.techniques.size(); ++i) {
        const auto& technique = source.techniques[i];
        if (!technique.enabled) continue;
        if (Effect(technique.effect, "MartysMods_LAUNCHPAD.fx")) {
            plan.depth = true;
            if (technique.name == "MartysMods_Launchpad") launchpad = i;
        }
        if (Effect(technique.effect, "MartysMods_MXAO.fx")) { plan.depth = true; mxao = i; }
    }
    float blend = 1.0f;
    for (const auto& uniform : source.uniforms) {
        if (Effect(uniform.effect, "MartysMods_LAUNCHPAD.fx") && uniform.name == "NORMALS_MODE" &&
            uniform.annotationSource.empty() && uniform.count == 1 && uniform.ints.size() == 1)
            plan.normalsMode = uniform.ints.front();
        if (Effect(uniform.effect, "MartysMods_MXAO.fx") && uniform.name == "HOYO_NORMAL_BLEND" &&
            uniform.annotationSource.empty() && uniform.count == 1 && uniform.floats.size() == 1)
            blend = uniform.floats.front();
    }
    if (mxao == SIZE_MAX) return plan;
    if (source.mxaoLaunchpadNormals < 0) { plan.status = Status::DefinitionUnknown; return plan; }
    plan.launchpadNormals = source.mxaoLaunchpadNormals != 0 && blend > 0.0001f;
    if (!plan.launchpadNormals) return plan;
    if (launchpad == SIZE_MAX) plan.status = Status::LaunchpadMissing;
    else if (launchpad > mxao) plan.status = Status::LaunchpadOrder;
    else if (plan.normalsMode < 0) plan.status = Status::NormalsUnknown;
    return plan;
}
}
