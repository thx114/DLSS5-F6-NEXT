#pragma once
#include <cstdint>

struct ID3D12CommandQueue;
struct ID3D12Fence;

// A pending idle marker is a submission receipt, not completion proof. Only
// reuse it while the caller has stopped producers for these owned resources.
struct InteropQueueDrainReceipt
{
    ID3D12CommandQueue* queue = nullptr;
    ID3D12Fence* fence = nullptr;
    std::uint64_t target = 0;

    template<class Submit>
    bool ReuseOrSubmit(ID3D12CommandQueue* owner, ID3D12Fence* timeline,
                       const std::uint64_t& value, Submit submit)
    {
        if (target != 0 && queue == owner && fence == timeline && target == value)
            return true;
        *this = {};
        if (!submit()) return false;
        *this = {owner, timeline, value};
        return true;
    }
};
