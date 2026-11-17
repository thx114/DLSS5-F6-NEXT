#include "pch.h"
#include "HoYoShadeDepthSemantics.h"
#include "HoYoShadeDepth.h"
#include "HoYoShadeSelection.h"
#include <array>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#ifdef HOYOSHADE_STAGE_TEST
#include <cstdio>
#endif

namespace HoYoShade::Depth::Semantics
{
namespace
{
// This adapter has no file/core/loader ownership and never calls save/reset or
// a natural runtime setter. API mutations can update only the existing private
// config/preset copies. All user definitions except these TWO names survive.
struct Port {
    reshade::api::effect_runtime* runtime;
    Selection::Source& source;
    ReloadCounter counter;
    uint64_t ReloadSerial() const noexcept { return counter ? counter(runtime) : 0; }
    bool PrivateIdle() const {
        // Caller supplies exclusive idle authority; this checks target identity.
        // Initial Create/Acquire need not expose the private preset yet. False
        // is a nonsticky Pending guard: no setters, keep advancing real core.
        if (!runtime || !source.valid || !source.files || source.config.empty() || !counter || !ReloadSerial()) return false;
        // A readable preset path precedes its first load. ReShade's macro
        // setters serialize the current in-memory table; calling them before
        // the first real reload would overwrite the copied user's definitions.
        const auto expected = source.files->directory / L"preset.ini";
        const auto config = source.files->directory / L"ReShade.ini";
        if (std::filesystem::u8path(source.config).lexically_normal() != config.lexically_normal()) return false;
        std::array<char, 32768> path{}; size_t size = path.size();
        runtime->get_current_preset_path(path.data(), &size);
        return path[0] && size < path.size() &&
            std::filesystem::u8path(path.data()).lexically_normal() == expected.lexically_normal();
    }
    bool Definition(const char* effect, const char* name, const char* expected) const {
        // Read in one bounded buffer. A too-large/truncated/expression value
        // never counts as the canonical 0/1 projected direction.
        char value[32]{}; size_t size = sizeof(value);
        const bool found = effect ? runtime->get_preprocessor_definition_for_effect(effect, name, value, &size) :
            runtime->get_preprocessor_definition(name, value, &size);
        if (!found) return false;
        return size < sizeof(value) && std::memchr(value, 0, sizeof(value)) && !std::strcmp(value, expected);
    }
    template<class F> bool Effects(F&& action) const {
        // The natural graph contains the entire library; the private compile
        // scope contains only its actually selected effects. Project both real
        // direction definitions for ALL selected effects, including overrides,
        // without creating preset entries/reload work for absent disabled fx.
        if (source.effects.empty() || source.effects.size() > Selection::MaxItems) return false;
        bool any = false;
        for (const auto& effect : source.effects) {
            if (!effect.enabled) continue;
            if (effect.key.empty()) return false;
            any = true;
            if (!action(effect.key.c_str())) return false;
        }
        return any;
    }
    bool Matches(const Key& key) const {
        if (!PrivateIdle()) return false;
        const char* reversed = key.reversed ? "1" : "0";
        const char* flipped = key.yFlipped ? "1" : "0";
        return Definition(nullptr, Reversed, reversed) && Definition(nullptr, UpsideDown, flipped) &&
            Effects([&](const char* effect) {
                // Project explicit effect-scoped values too: a copied user's
                // per-effect override must not silently defeat real direction.
                return Definition(effect, Reversed, reversed) && Definition(effect, UpsideDown, flipped);
            });
    }
    void Project(const Key& key) const {
        if (!PrivateIdle()) throw std::logic_error("depth semantics target is not private");
        const char* reversed = key.reversed ? "1" : "0";
        const char* flipped = key.yFlipped ? "1" : "0";
        if (!Definition(nullptr, Reversed, reversed)) runtime->set_preprocessor_definition(Reversed, reversed);
        if (!Definition(nullptr, UpsideDown, flipped)) runtime->set_preprocessor_definition(UpsideDown, flipped);
        if (!Effects([&](const char* effect) {
            if (!Definition(effect, Reversed, reversed)) runtime->set_preprocessor_definition_for_effect(effect, Reversed, reversed);
            if (!Definition(effect, UpsideDown, flipped)) runtime->set_preprocessor_definition_for_effect(effect, UpsideDown, flipped);
            return true;
        })) throw std::logic_error("depth semantics effect selection incomplete");
    }
    void QueueReload() const {
        if (!PrivateIdle()) throw std::logic_error("depth semantics target left private scope");
        // Explicit single request, in the idle projection phase. Setters may
        // already queue changes; no reload call occurs in Begin/render work.
        runtime->reload_effect_next_frame(nullptr);
    }
};
}
void ObserveCompiled(CompiledDefinitions& compiled, reshade::api::effect_runtime* runtime, uint64_t reload) noexcept
{
    compiled = {};
    try {
        if (!runtime || !reload) return;
        const Selection::CallBoundary call;
        CompiledDefinitions next; next.runtime = runtime; next.reload = reload;
        std::vector<reshade::api::effect_technique> handles;
        Selection::CaptureReason reason; size_t count = 0;
        if (!Selection::EnumerateTechniques(runtime, handles, count, reason, call)) return;
        const auto read = [&](const char* effect, const char* name, bool& value) {
            char text[32]{}; size_t size = sizeof(text); bool found = false;
            if (!call.Invoke([&] { found = runtime->get_preprocessor_definition_for_effect(effect, name, text, &size); })) return false;
            if (!found) {
                size = sizeof(text);
                if (!call.Invoke([&] { found = runtime->get_preprocessor_definition(name, text, &size); })) return false;
            }
            if (!found || size >= sizeof(text) || text[1] || (text[0] != '0' && text[0] != '1')) return false;
            value = text[0] == '1'; return true;
        };
        for (auto technique : handles) {
            char name[32768]{}; size_t size = sizeof(name);
            if (!call.Invoke([&] { runtime->get_technique_effect_name(technique, name, &size); }) ||
                !name[0] || size >= sizeof(name)) return;
            if (std::any_of(next.effects.begin(), next.effects.end(), [&](const auto& effect) { return effect.name == name; })) continue;
            auto& effect = next.effects.emplace_back(); effect.name = name;
            effect.known = read(name, Reversed, effect.reversed) && read(name, UpsideDown, effect.flipped);
        }
        if (call.Current()) compiled = std::move(next);
    } catch (...) { compiled = {}; }
}
Result InspectDirections(const CompiledDefinitions& compiled,reshade::api::effect_runtime* runtime,
    const Selection::Source& source,bool reversed,bool flipped,ReloadCounter counter) noexcept
{
    if(!runtime || !source.valid || !counter || compiled.runtime!=runtime || !compiled.reload ||
        counter(runtime)!=compiled.reload)return {};
    for(const auto& effect:source.effects)if(effect.enabled) {
        const auto found=std::find_if(compiled.effects.begin(),compiled.effects.end(),[&](const auto& e){return e.name==effect.key;});
        if(found==compiled.effects.end() || !found->known || found->reversed!=reversed || found->flipped!=flipped)return {};
    }
    return {Outcome::Ready};
}
Result InspectCurrent(const CompiledDefinitions& compiled, reshade::api::effect_runtime* runtime, const Selection::Source& source,
                      const Access* input, ReloadCounter counter) noexcept
{
    try {
        if (!runtime || !source.valid || !input || !input->reader || !input->resource ||
            !input->directionKnown || !Compatible(input->resource->GetDesc(), *input) ||
            !input->reader->Validate(*input) || !counter || compiled.runtime != runtime || !compiled.reload ||
            counter(runtime) != compiled.reload) return {};
        for (const auto& effect : source.effects) if (effect.enabled) {
            const auto found = std::find_if(compiled.effects.begin(), compiled.effects.end(),
                [&](const auto& value) { return value.name == effect.key; });
            if (found == compiled.effects.end() || !found->known || found->reversed != input->reversed ||
                found->flipped != input->yFlipped) return {};
        }
        return {Outcome::Ready};
    } catch (...) { return {}; }
}
Result Project(State& state, reshade::api::effect_runtime* runtime, uint64_t generation,
    Selection::Source& source, const Access* input, ReloadCounter counter) noexcept
{
    Key key;
    key.runtime = reinterpret_cast<uintptr_t>(runtime); key.runtimeGeneration = generation;
    // The immutable snapshot already identifies selected effect files and
    // definitions. Reflection rebuild counters may change every uncached
    // owner callback without changing any compile input.
    key.selectionSnapshot = source.snapshots;
    try {
        if (input && input->resource && input->reader && input->directionKnown && input->owner &&
            input->generation && input->epoch && Compatible(input->resource->GetDesc(), *input) &&
            input->reader->Validate(*input)) {
            key.valid = true; key.producer = input->owner; key.producerGeneration = input->generation;
            key.epoch = input->epoch; key.reversed = input->reversed; key.yFlipped = input->yFlipped;
        }
        Port port{runtime, source, counter};
        const auto result = state.controller.Update(port, key);
#ifdef HOYOSHADE_STAGE_TEST
        static unsigned reports = 0;
        if (++reports <= 12) std::printf("depth semantics runtime=%p gen=%llu producer=%llu gen=%llu epoch=%llu snapshot=%llu valid=%d serial=%llu outcome=%u invalidate=%d\n",
            runtime, generation, key.producer, key.producerGeneration, key.epoch, key.selectionSnapshot,
            key.valid, port.ReloadSerial(), static_cast<unsigned>(result.outcome), result.invalidateSelection);
#endif
        return result;
    } catch (...) { return {Outcome::ColorOnly, true}; }
}
}
