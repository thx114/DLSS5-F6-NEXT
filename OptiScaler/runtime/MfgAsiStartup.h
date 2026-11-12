// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <filesystem>
#include <string_view>

namespace MfgAsiStartup
{
// The caller lowercases the basename. Keep aliases out of the generic/late loader.
bool OwnsName(std::wstring_view normalizedName) noexcept;
void Select(const std::filesystem::path& path);
void Initialize();
void WorkerUnavailable();
bool AwaitGraphicsStartup(unsigned timeoutMilliseconds = 5000);
}
