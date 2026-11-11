// SPDX-License-Identifier: GPL-3.0-or-later
// CPU-only ownership tests. No driver, NGX feature or GPU is executed.
#include <nvsdk_ngx_params.h>
#include "../../inputs/NVNGX_DLSS.h"
#include "../FeatureTransition.h"
#include <cassert>
#include <climits>
#include <cstdio>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

struct Parameters final : NVSDK_NGX_Parameter
{
    std::vector<int>& destroyed;
    int id;
    std::map<std::string, unsigned> values;
    Parameters(std::vector<int>& log, int value) : destroyed(log), id(value) {}
    ~Parameters() { destroyed.push_back(id); }
    void Set(const char* key, unsigned value) override { values[key] = value; }
    void Set(const char* key, int value) override { values[key] = static_cast<unsigned>(value); }
#define OTHER_TYPE(T) \
    void Set(const char*, T) override {} \
    NVSDK_NGX_Result Get(const char*, T*) const override { return NVSDK_NGX_Result_Fail; }
    OTHER_TYPE(unsigned long long)
    OTHER_TYPE(float)
    OTHER_TYPE(double)
    OTHER_TYPE(ID3D11Resource*)
    OTHER_TYPE(ID3D12Resource*)
    OTHER_TYPE(void*)
#undef OTHER_TYPE
    NVSDK_NGX_Result Get(const char*, unsigned*) const override { return NVSDK_NGX_Result_Fail; }
    NVSDK_NGX_Result Get(const char*, int*) const override { return NVSDK_NGX_Result_Fail; }
    void Reset() override { values.clear(); }
    static void Destroy(NVSDK_NGX_Parameter* p) { delete static_cast<Parameters*>(p); }
};
struct Feature
{
    std::vector<int>& destroyed;
    Parameters* parameters;
    int id;
    ~Feature()
    {
        // The old parameter table must still be live during feature release.
        assert(parameters && parameters->id > 0);
        destroyed.push_back(id);
    }
    unsigned GetFeatureFlags() const { return 41; }
    unsigned RenderWidth() const { return 1280; }
    unsigned RenderHeight() const { return 720; }
    unsigned DisplayWidth() const { return 2560; }
    unsigned DisplayHeight() const { return 1440; }
    int PerfQualityValue() const { return 2; }
};
using Context = ContextData<Feature>;

Context::ParameterOwner Allocate(std::vector<int>& log, int id)
{
    return {new Parameters(log, id), Parameters::Destroy};
}
void Seed(Context& context, std::vector<int>& log)
{
    context.OwnCreateParams(Allocate(log, 2));
    context.feature = std::make_unique<Feature>(Feature{log, static_cast<Parameters*>(context.createParams), 1});
    log.clear(); // Ignore construction temporary, retaining the real feature.
}
int main()
{
    using FeatureTransition::Phase;
    int counter = 0;
    assert(FeatureTransition::Advance(counter) == Phase::Retire && counter == 1);
    assert(FeatureTransition::Advance(counter) == Phase::Construct && counter == 2);
    assert(FeatureTransition::Advance(counter) == Phase::Initialize && counter == 3);
    for (int invalid : {-1, 3, 4, INT_MAX})
    {
        counter = invalid;
        assert(FeatureTransition::Advance(counter) == Phase::Invalid && counter == invalid);
    }
    unsigned groups = 1;
    const auto recipe = [](auto& table, auto& feature) { FeatureTransition::CopyRecipe(table, feature); };
    {
        std::vector<int> log;
        Context context;
        Seed(context, log);
        auto* oldTable = context.createParams;
        auto retired = FeatureTransition::Prepare(context, static_cast<NVSDK_NGX_Parameter*>(nullptr), false,
            [&] { return Allocate(log, 4); }, recipe);
        assert(retired && retired->feature && !context.feature);
        assert(retired->createParams == oldTable && retired->createParamsOwner.get() == oldTable);
        assert(context.createParams == context.createParamsOwner.get() && context.createParams != oldTable);
        const auto& values = static_cast<Parameters*>(context.createParams)->values;
        assert(values.size() == 6);
        assert(values.at(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags) == 41);
        assert(values.at(NVSDK_NGX_Parameter_Width) == 1280 && values.at(NVSDK_NGX_Parameter_Height) == 720);
        assert(values.at(NVSDK_NGX_Parameter_OutWidth) == 2560 && values.at(NVSDK_NGX_Parameter_OutHeight) == 1440);
        assert(values.at(NVSDK_NGX_Parameter_PerfQualityValue) == 2);
        retired.reset();
        assert((log == std::vector<int>{1, 2}));
        context.ClearCreateParams();
        assert((log == std::vector<int>{1, 2, 4}));
        ++groups;
    }
    {
        std::vector<int> log;
        Parameters caller(log, 9);
        caller.values["OptiScaler"] = 1; // Allocation tags do not transfer ownership.
        caller.values["caller-preset"] = 17;
        Context context;
        Seed(context, log);
        bool allocated = false;
        auto retired = FeatureTransition::Prepare(context, static_cast<NVSDK_NGX_Parameter*>(&caller), true,
            [&] { allocated = true; return Allocate(log, 4); }, recipe);
        assert(retired && !allocated && !context.createParamsOwner && context.createParams == &caller);
        assert(caller.values.at("caller-preset") == 17);
        context.ClearCreateParams();
        assert(log.empty());
        retired.reset();
        assert((log == std::vector<int>{1, 2}) && caller.id == 9);
        ++groups;
    }
    {
        std::vector<int> log;
        Context context;
        Seed(context, log);
        auto* feature = context.feature.get();
        auto* table = context.createParams;
        auto retired = FeatureTransition::Prepare(context, static_cast<NVSDK_NGX_Parameter*>(nullptr), false,
            []() -> Context::ParameterOwner { return {nullptr, nullptr}; }, recipe);
        assert(!retired && context.feature.get() == feature && context.createParams == table && log.empty());
        ++groups;
    }
    {
        std::vector<int> log;
        Context context;
        Seed(context, log);
        auto* feature = context.feature.get();
        auto* table = context.createParams;
        bool threw = false;
        try
        {
            FeatureTransition::Prepare(context, static_cast<NVSDK_NGX_Parameter*>(nullptr), false,
                [&] { return Allocate(log, 4); }, [](auto&, auto&) { throw std::runtime_error("populate"); });
        }
        catch (const std::runtime_error&) { threw = true; }
        assert(threw && context.feature.get() == feature && context.createParams == table);
        assert((log == std::vector<int>{4}));
        ++groups;
    }
    {
        std::vector<int> log;
        Context context;
        Seed(context, log);
        bool allocated = false;
        auto allocate = [&] { allocated = true; return Allocate(log, 4); };
        assert(!FeatureTransition::Prepare(context, context.createParams, true, allocate, recipe));
        assert(!FeatureTransition::Prepare(context, static_cast<NVSDK_NGX_Parameter*>(nullptr), true, allocate, recipe));
        assert(!allocated && context.feature && context.createParamsOwner && log.empty());
        Context empty;
        assert(!FeatureTransition::Prepare(empty, context.createParams, false, allocate, recipe));
        assert(!allocated);
        ++groups;
    }
    std::printf("FeatureTransition: %u CPU groups passed\n", groups);
}
