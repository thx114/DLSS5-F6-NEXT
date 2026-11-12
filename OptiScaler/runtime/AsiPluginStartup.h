// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <filesystem>

namespace AsiPluginStartup
{
// Configure during attach, before publishing the existing startup worker. No
// plugin code is loaded or called here. Relative paths bind to the startup CWD.
void Configure(const std::filesystem::path& directory, int lateDelaySeconds,
               void (*onPatchSucceeded)() = nullptr);
void Initialize();
void WorkerUnavailable();
// Wait for the ordinary initialization queue to terminate, including failures.
// Explicit -loadlate plugins retain their delayed semantics and are not part of
// this barrier. Neither completion nor an entry return certifies SDK capability.
bool AwaitGraphicsStartup(unsigned timeoutMilliseconds = 5000);
}
