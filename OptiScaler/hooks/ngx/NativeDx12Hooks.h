#pragma once

struct NvngxModule;
namespace NgxInput
{
// Native core and direct project exports share one process-lifetime route.
// Install only after the proxy has resolved its entire native table.
bool Enabled();
bool Install(NvngxModule& provider);
bool Installed();
bool SelectedInputCall();
bool OwnsModule(void* module);
bool AllowProviderInit();
}
