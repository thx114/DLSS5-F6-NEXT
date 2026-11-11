#include "pch.h"
#include "HoYoShadeDepthSemantics.h"
#include "HoYoShadeDepth.h"
#include "HoYoShadeSelection.h"
#include <array>
#include <cstring>
#include <filesystem>
#include <stdexcept>

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
        if (!runtime || !source.valid || !source.files || source.config.empty() || !counter) return false;
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
Result Project(State& state, reshade::api::effect_runtime* runtime, uint64_t generation,
    Selection::Source& source, const Access* input, ReloadCounter counter) noexcept
{
    Key key;
    key.runtime = reinterpret_cast<uintptr_t>(runtime); key.runtimeGeneration = generation;
    key.selectionSnapshot = source.snapshots; key.selectionGraph = source.graphBuilds;
    try {
        if (input && input->resource && input->reader && input->directionKnown && input->owner &&
            input->generation && input->epoch && Compatible(input->resource->GetDesc(), *input) &&
            input->reader->Validate(*input)) {
            key.valid = true; key.producer = input->owner; key.producerGeneration = input->generation;
            key.epoch = input->epoch; key.reversed = input->reversed; key.yFlipped = input->yFlipped;
        }
        Port port{runtime, source, counter};
        return state.controller.Update(port, key);
    } catch (...) { return {Outcome::ColorOnly, true}; }
}
}
