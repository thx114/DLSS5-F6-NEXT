#pragma once

#include <string_view>

namespace framegen::PrivateFgTestModule
{
enum class Status : unsigned
{
    Disabled,
    Pending,
    Initializing,
    WorkerUnavailable,
    Missing,
    ValidationFailed,
    InventoryFailed,
    AlreadyLoaded,
    LoadFailed,
    EntryMissing,
    InitReturned
};

bool Enabled();
Status CurrentStatus();
bool ProviderPending();
bool OwnsAsiName(std::wstring_view name);
void Prepare();
void Initialize();
void StartupWorkerUnavailable();
bool AwaitGraphicsStartup(unsigned timeoutMilliseconds = 5000);
const char* StatusText();
}
