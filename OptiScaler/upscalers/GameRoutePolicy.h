#pragma once

#include <cwchar>
#include <runtime/F6InputPolicy.h>

namespace GameRouting
{
enum class Identity
{
    Other,
    Genshin,
    StarRail,
    ZenlessZoneZero
};

inline Identity IdentifyExecutable(const wchar_t* executable) noexcept
{
    if (!executable)
        return Identity::Other;
    if (_wcsicmp(executable, L"YuanShen.exe") == 0 || _wcsicmp(executable, L"GenshinImpact.exe") == 0)
        return Identity::Genshin;
    if (_wcsicmp(executable, L"StarRail.exe") == 0)
        return Identity::StarRail;
    if (_wcsicmp(executable, L"ZenlessZoneZero.exe") == 0)
        return Identity::ZenlessZoneZero;
    return Identity::Other;
}

inline bool UseGenshinBridgeTransport(Identity identity, bool producerScopePresent) noexcept
{
    return F6InputPolicy::FsrBridgeEnabled && identity == Identity::Genshin && producerScopePresent;
}

inline bool UseGenshinPresentPipeline(Identity identity, bool requested) noexcept
{
    return F6InputPolicy::DirectDlssInputs && !F6InputPolicy::FsrBridgeEnabled &&
        identity == Identity::Genshin && requested;
}
}
