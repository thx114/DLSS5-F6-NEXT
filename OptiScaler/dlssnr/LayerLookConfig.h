#pragma once
#include "LayerLookProfile.h"
#include <string>

namespace DlssNr::LayerLook
{
template<class Configuration, class ReadBool, class ReadUInt, class ReadFloat>
void Load(Configuration& config, ReadBool readBool, ReadUInt readUInt, ReadFloat readFloat)
{
    config.DlssNrLayerLookMode.set_from_config(readBool("LayerLookMode"));
    for (unsigned layer = 0; layer < MaxPassCount; ++layer)
    {
        auto& storage = config.DlssNrLayerLooks[layer];
        const std::string prefix = "Pass" + std::to_string(layer + 1) + "Look";
        storage.DlssNrLookMode.set_from_config(readBool(prefix + "Mode"));
        auto inherit = readUInt(prefix + "Inherit");
        if (inherit) *inherit = std::min(*inherit, 2u);
        storage.inherit.set_from_config(inherit);
        auto overrides = readUInt(prefix + "Overrides");
        if (overrides) *overrides &= ParameterMask;
        storage.overrides.set_from_config(overrides);
        auto uiMask = readUInt(prefix + "UiMask");
        if (uiMask) *uiMask = BoundUiMask(*uiMask);
        storage.uiMask.set_from_config(uiMask);
        Visit(storage, [&](unsigned i, auto& option) {
            auto value = readFloat(prefix + Parameters[i].key);
            if (value) *value = Bound(*value, Parameters[i]);
            option.set_from_config(value);
        });
    }
}
template<class Configuration, class WriteBool, class WriteUInt, class WriteFloat>
void Save(Configuration& config, WriteBool writeBool, WriteUInt writeUInt, WriteFloat writeFloat)
{
    writeBool("LayerLookMode", config.DlssNrLayerLookMode.value_for_config());
    for (unsigned layer = 0; layer < MaxPassCount; ++layer)
    {
        auto& storage = config.DlssNrLayerLooks[layer];
        const std::string prefix = "Pass" + std::to_string(layer + 1) + "Look";
        writeBool(prefix + "Mode", storage.DlssNrLookMode.value_for_config());
        writeUInt(prefix + "Inherit", storage.inherit.value_for_config());
        writeUInt(prefix + "Overrides", storage.overrides.value_for_config());
        writeUInt(prefix + "UiMask", storage.uiMask.value_for_config());
        Visit(storage, [&](unsigned i, auto& option) { writeFloat(prefix + Parameters[i].key, option.value_for_config()); });
    }
}
}
