#pragma once

#include <algorithm>
#include <cwctype>
#include <string>
#include <string_view>

namespace framegen
{
inline bool IsDlssgProviderPath(std::wstring_view path)
{
    std::wstring normalized;
    normalized.reserve(path.size());
    for (wchar_t c : path)
    {
        c = c == L'/' ? L'\\' : static_cast<wchar_t>(std::towlower(c));
        if (c == L'\\' && !normalized.empty() && normalized.back() == L'\\') continue;
        normalized.push_back(c);
    }
    const std::wstring_view view(normalized);
    const auto slash = view.find_last_of(L'\\');
    const auto name = slash == view.npos ? view : view.substr(slash + 1);
    return name == L"nvngx_dlssg.dll" || view.find(L"\\models\\dlssg\\") != view.npos;
}
}
