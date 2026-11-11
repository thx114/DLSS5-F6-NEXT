#pragma once
#include "integration/hoyoshade/sdk/reshade/reshade_events.hpp"

namespace HoYoShade
{
// Signatures and version from the pinned public SDK. No header auto-discovery,
// static import library, private offsets or unversioned vtable casts.
struct CoreApi
{
    static constexpr uint32_t Version = 20;
    using RegisterAddon = bool (*)(void*, uint32_t);
    using UnregisterAddon = void (*)(void*);
    using RegisterEvent = void (*)(reshade::addon_event, void*);
    using UnregisterEvent = void (*)(reshade::addon_event, void*);
    using CreateEffectRuntime = bool (*)(reshade::api::device_api, void*, void*, void*, const char*, reshade::api::effect_runtime**);
    using DestroyEffectRuntime = void (*)(reshade::api::effect_runtime*);
    using GetConfigValue = bool (*)(void*, reshade::api::effect_runtime*, const char*, const char*, char*, size_t*);
    using GetBasePath = void (*)(char*, size_t*);
    RegisterAddon registerAddon = nullptr;
    UnregisterAddon unregisterAddon = nullptr;
    RegisterEvent registerEvent = nullptr;
    UnregisterEvent unregisterEvent = nullptr;
    CreateEffectRuntime createEffectRuntime = nullptr;
    DestroyEffectRuntime destroyEffectRuntime = nullptr;
    GetConfigValue getConfigValue = nullptr;
    GetBasePath getBasePath = nullptr;
    bool accepted = false;
    bool Handshake(void* self) noexcept
    {
        if (accepted) return true;
        if (!self || !registerAddon || !unregisterAddon || !registerEvent || !unregisterEvent) return false;
        accepted = registerAddon(self, Version);
        return accepted;
    }
    template<reshade::addon_event Event>
    bool Subscribe(typename reshade::addon_event_traits<Event>::decl callback) noexcept
    {
        if (!accepted || !callback) return false;
        registerEvent(Event, reinterpret_cast<void*>(callback));
        // The SDK returns void: this is a request, callback delivery is separate evidence.
        return true;
    }
};

}
