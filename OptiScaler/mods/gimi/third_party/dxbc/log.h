#pragma once
// R1006 private adapter: never import the upstream global FILE logger.
// Errors cross the bridge as structured parse errors/HRESULT only.
namespace gimi_dxbc_vendor { constexpr bool gLogDebug = false; }
#define LogInfo(...) ((void)0)
#define LogDebug(...) ((void)0)
#define LogInfoNoNL(...) ((void)0)
