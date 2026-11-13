#include <integration/HoYoShadeEffectRequirements.h>
#include <vector>
#include <string>
#include <cassert>
#include <cstdio>
struct Technique { std::string effect, name; bool enabled = true; };
struct Uniform {
    std::string effect, name, annotationSource;
    size_t count = 1;
    std::vector<int> ints;
    std::vector<float> floats;
};
struct Source {
    std::vector<Technique> techniques;
    std::vector<Uniform> uniforms;
    int mxaoLaunchpadNormals = 1;
};
int main() {
    using namespace HoYoShade::EffectRequirements;
    Source source;
    assert(!Inspect(source).depth);
    source.techniques = {{"MartysMods_LAUNCHPAD.fx", "MartysMods_Launchpad"}, {"MartysMods_MXAO.fx", "MartysMods_MXAO"}};
    source.uniforms = {{"MartysMods_LAUNCHPAD.fx", "NORMALS_MODE", "", 1, {1}, {}}};
    auto plan = Inspect(source); assert(plan.depth && plan.launchpadNormals && plan.status == Status::Ready);
    source.uniforms[0].ints[0] = 0; assert(Inspect(source).status == Status::Ready); // still produces base normals
    std::swap(source.techniques[0], source.techniques[1]); assert(Inspect(source).status == Status::LaunchpadOrder);
    source.techniques[1].enabled = false; assert(Inspect(source).status == Status::LaunchpadMissing);
    source.mxaoLaunchpadNormals = 0; assert(Inspect(source).status == Status::Ready && Inspect(source).depth);
    source.mxaoLaunchpadNormals = -1; assert(Inspect(source).status == Status::DefinitionUnknown);
    source.mxaoLaunchpadNormals = 1;
    source.uniforms.push_back({"MartysMods_MXAO.fx", "HOYO_NORMAL_BLEND", "", 1, {}, {0.0f}});
    assert(Inspect(source).status == Status::Ready && !Inspect(source).launchpadNormals);
    source.uniforms.back().floats[0] = 1.0f; assert(Inspect(source).status == Status::LaunchpadMissing);
    source.techniques[1].enabled = true; std::swap(source.techniques[0], source.techniques[1]);
    source.uniforms.erase(source.uniforms.begin()); assert(Inspect(source).status == Status::NormalsUnknown);
    std::puts("PASS installed LP/MXAO depth, live blend, macro and ordered normals contracts");
}
