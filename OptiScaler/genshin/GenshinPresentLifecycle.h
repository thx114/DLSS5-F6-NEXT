// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstdint>
#include <dxgi.h>

namespace GenshinLegacy::Presentation
{
struct Owner
{
    std::uint64_t device=0,deviceEpoch=0,context=0,ownerEpoch=0,thread=0;
    bool operator==(const Owner&)const noexcept=default;
    bool Known()const noexcept{return device && deviceEpoch && context && ownerEpoch && thread;}
};
struct Frame
{
    std::uint64_t instance=0,generation=0,sourceFrame=0,producerEpoch=0;
    bool operator==(const Frame&)const noexcept=default;
    bool Known()const noexcept{return instance && generation && sourceFrame && producerEpoch;}
};
struct Ticket
{
    Owner owner{};Frame frame{};
    std::uint64_t swapchain=0,swapchainEpoch=0,attempt=0;
    UINT flags=0;bool present1=false;
    bool operator==(const Ticket&)const noexcept=default;
    bool Known()const noexcept{return owner.Known() && swapchain && swapchainEpoch && attempt;}
};
struct Receipt
{
    Ticket ticket{};
    HRESULT rawResult=E_PENDING;
    std::uint64_t completionSerial=0;
    bool recorded=false,successfulNonTest=false,sourceFrameKnown=false;
};
// Caller owns the render thread/native mutex; no callbacks, heap or polling.
// This records native-call completion, never physical display/FG frame count.
class Ledger final
{
    struct Entry {std::uint64_t swapchain=0,epoch=0;Ticket pending{};Receipt completed{};};
    std::array<Entry,8> entries_{};
    std::uint64_t nextEpoch_=1,nextAttempt_=1,nextCompletion_=1;
public:
    Ticket Begin(const Owner&,std::uint64_t swapchain,const Frame&,UINT flags,bool present1) noexcept;
    Receipt Complete(const Ticket&,HRESULT result,const Owner& current) noexcept;
    void Forget(std::uint64_t swapchain) noexcept;
    void Reset() noexcept;
    bool IsCurrent(const Receipt&)const noexcept;
};
}
