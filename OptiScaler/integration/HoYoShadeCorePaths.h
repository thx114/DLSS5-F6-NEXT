// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <filesystem>
#include <vector>
#include <cwchar>

namespace HoYoShade::CorePaths
{
inline bool Named(const std::filesystem::path& path, const wchar_t* name)
{ return !_wcsicmp(path.filename().c_str(), name); }

// Supported installation layouts only. No disk walk or preset/shader-path
// inference. The main INI always belongs to the current executable directory.
inline std::vector<std::filesystem::path> Candidates(const std::filesystem::path& directory)
{
    namespace fs = std::filesystem;
    std::vector<fs::path> result {directory / L"HoYoShade/ReShade64.dll"};
    const auto parent = directory.parent_path();
    if ((Named(parent, L"local") || Named(parent, L"official")) && Named(parent.parent_path(), L"OptiScaler"))
        result.push_back(parent.parent_path().parent_path() / L"HoYoShade/ReShade64.dll");
    // cache/games/<game>/OptiRuntime/<identity>/OptiScaler
    if (Named(directory, L"OptiScaler") && Named(parent.parent_path(), L"OptiRuntime")) {
        const auto games = parent.parent_path().parent_path().parent_path();
        if (Named(games, L"games") && Named(games.parent_path(), L"cache"))
            result.push_back(games.parent_path().parent_path() / L"HoYoShade/ReShade64.dll");
    }
    return result;
}
inline std::filesystem::path Unique(const std::vector<std::filesystem::path>& directories)
{
    namespace fs = std::filesystem;
    fs::path found;
    for (const auto& directory : directories) {
        if (directory.empty()) continue;
        for (const auto& candidate : Candidates(directory)) {
            std::error_code error;
            if (!fs::is_regular_file(candidate, error)) continue;
            const auto path = fs::canonical(candidate, error);
            if (error) return {};
            if (!found.empty() && _wcsicmp(found.c_str(), path.c_str())) return {};
            found = path;
        }
    }
    return found;
}
}
