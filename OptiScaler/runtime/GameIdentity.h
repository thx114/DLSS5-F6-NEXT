// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cwchar>
#include <string_view>

namespace GameRouting
{
enum class Identity { Other, Genshin, StarRail, ZenlessZoneZero };

inline Identity IdentifyExecutable(std::wstring_view path) noexcept
{
    const auto slash = path.find_last_of(L"/\\");
    const auto name = slash == std::wstring_view::npos ? path : path.substr(slash + 1);
    const auto equals = [name](std::wstring_view expected) {
        return name.size() == expected.size() && _wcsnicmp(name.data(), expected.data(), name.size()) == 0;
    };
    if (equals(L"YuanShen.exe") || equals(L"GenshinImpact.exe")) return Identity::Genshin;
    if (equals(L"StarRail.exe")) return Identity::StarRail;
    if (equals(L"ZenlessZoneZero.exe")) return Identity::ZenlessZoneZero;
    return Identity::Other;
}

inline Identity IdentifyExecutable(const wchar_t* path) noexcept
{
    return path ? IdentifyExecutable(std::wstring_view(path)) : Identity::Other;
}

inline bool IsGenshinExecutable(std::wstring_view path) noexcept
{
    return IdentifyExecutable(path) == Identity::Genshin;
}
}
