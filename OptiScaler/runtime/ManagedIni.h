// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <SimpleIni.h>
#include <string>
#include <utility>

namespace FramePipeline
{
// Merge only keys owned by the caller. Preserve other runtime/user sections,
// values and comments rather than truncating the companion configuration.
// Pure CPU serialization: the caller owns all file IO and error handling.
inline bool MergeManagedIni(const std::string& existing, const std::string& managed, std::string& output)
{
    const auto supported = [](const std::string& value) {
        if (value.find('\0') != std::string::npos) return false;
        if (value.size() < 2) return true;
        const auto a = static_cast<unsigned char>(value[0]);
        const auto b = static_cast<unsigned char>(value[1]);
        return !((a == 0xff && b == 0xfe) || (a == 0xfe && b == 0xff));
    };
    // The byte-oriented parser may stop at NUL and still report success.
    // Refuse unsupported encodings before parsing or replacing the output.
    if (!supported(existing) || !supported(managed)) return false;
    CSimpleIniA result, updates;
    result.SetUnicode(); updates.SetUnicode();
    if (result.LoadData(existing) < 0 || updates.LoadData(managed) < 0) return false;
    CSimpleIniA::TNamesDepend sections, keys;
    updates.GetAllSections(sections);
    for (const auto& section : sections)
    {
        keys.clear();
        if (!updates.GetAllKeys(section.pItem, keys)) return false;
        for (const auto& key : keys)
        {
            const auto* value = updates.GetValue(section.pItem, key.pItem);
            if (!value || result.SetValue(section.pItem, key.pItem, value) < 0) return false;
        }
    }
    std::string merged;
    if (result.Save(merged) < 0) return false;
    output = std::move(merged);
    return true;
}
}
