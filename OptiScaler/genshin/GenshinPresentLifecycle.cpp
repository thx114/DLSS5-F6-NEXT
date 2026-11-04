// SPDX-License-Identifier: GPL-3.0-or-later
#include "GenshinPresentLifecycle.h"
namespace GenshinLegacy::Presentation
{
Ticket Ledger::Begin(const Owner& owner,std::uint64_t swapchain,const Frame& frame,UINT flags,bool present1) noexcept
{
    if(!owner.Known() || !swapchain || (flags & DXGI_PRESENT_TEST))return {};
    Entry* entry=nullptr;
    for(auto& candidate:entries_)if(candidate.swapchain==swapchain){entry=&candidate;break;}
    if(!entry)for(auto& candidate:entries_)if(!candidate.swapchain){entry=&candidate;
        entry->swapchain=swapchain;entry->epoch=nextEpoch_++;break;}
    if(!entry || entry->pending.Known())return {}; // Nested/concurrent same-target call is not guessed.
    entry->pending={owner,frame,swapchain,entry->epoch,nextAttempt_++,flags,present1};
    return entry->pending;
}
Receipt Ledger::Complete(const Ticket& ticket,HRESULT result,const Owner& current) noexcept
{
    if(!ticket.Known() || ticket.owner!=current)return {};
    for(auto& entry:entries_)if(entry.swapchain==ticket.swapchain && entry.epoch==ticket.swapchainEpoch){
        if(entry.pending!=ticket)return {};
        Receipt receipt;receipt.ticket=ticket;receipt.rawResult=result;receipt.recorded=true;
        // Status results (including OCCLUDED/S_FALSE) do not prove this frame was presented.
        receipt.successfulNonTest=result==S_OK && !(ticket.flags & DXGI_PRESENT_TEST);
        receipt.sourceFrameKnown=ticket.frame.Known();
        if(receipt.successfulNonTest)receipt.completionSerial=nextCompletion_++;
        entry.pending={};entry.completed=receipt;return receipt;}
    return {};
}
void Ledger::Forget(std::uint64_t swapchain) noexcept
{
    for(auto& entry:entries_)if(entry.swapchain==swapchain){entry={};return;}
}
void Ledger::Reset() noexcept{for(auto& entry:entries_)entry={};}
bool Ledger::IsCurrent(const Receipt& receipt)const noexcept
{
    if(!receipt.recorded || !receipt.successfulNonTest || !receipt.sourceFrameKnown || !receipt.completionSerial)return false;
    for(const auto& entry:entries_)if(entry.swapchain==receipt.ticket.swapchain && entry.epoch==receipt.ticket.swapchainEpoch)
        return entry.completed.recorded && entry.completed.ticket==receipt.ticket &&
            entry.completed.completionSerial==receipt.completionSerial && entry.completed.rawResult==S_OK;
    return false;
}
}
