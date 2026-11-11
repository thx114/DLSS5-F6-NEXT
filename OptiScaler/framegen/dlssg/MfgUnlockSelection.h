#pragma once
#include <string_view>

namespace MfgUnlock
{
// The caller normalizes the plugin basename. One owner per provider image;
// unrelated ASI plugins retain their normal loading path.
inline bool SkipExternalPlugin(std::wstring_view name, bool internalAda, bool internalAmpere)
{
    return (internalAda || internalAmpere) && name == L"mfgunlocker.asi";
}
}
