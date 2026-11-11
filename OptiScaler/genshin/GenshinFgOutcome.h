// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

#include <dlssnr/DlssNrFeature_Dx12.h>

namespace GenshinFg
{
enum class Stage : unsigned char
{
    Idle,
    FinishedPictureNoWrite,
    FinishedPictureWritten,
    RefusedMissingProducerJoin,
    Revoked
};

struct Ticket
{
    Stage stage = Stage::Idle;
    uint64_t presentEpoch = 0;
    uint64_t swapchain = 0;
    uint64_t color = 0;
    uint64_t queue = 0;
    uint64_t submissionSerial = 0;
    uint64_t fenceValue = 0;
    bool srAccepted = false;
    bool producerJoin = false;
    bool depthAuthenticated = false;
    bool motionAuthenticated = false;
    bool finalOutputAuthenticated = false;
};

// This is the bounded same-present state machine. It records the real NR
// result, but never upgrades it into DLSSG authority without all producer and
// resource joins being authenticated by a future adapter.
class OutcomeState final
{
    Ticket ticket_{};

public:
    void Reset() noexcept { ticket_ = {}; ticket_.stage = Stage::Revoked; }

    void ObserveFinishedPicture(const DlssNr::FinishedPictureOutcome& outcome,
                                bool srAccepted) noexcept
    {
        ticket_ = {};
        ticket_.presentEpoch = outcome.presentEpoch;
        ticket_.swapchain = outcome.swapchain;
        ticket_.color = outcome.color;
        ticket_.queue = outcome.queue;
        ticket_.submissionSerial = outcome.submissionSerial;
        ticket_.fenceValue = outcome.fenceValue;
        ticket_.srAccepted = srAccepted;
        ticket_.stage = outcome.Written()
            ? Stage::FinishedPictureWritten
            : Stage::FinishedPictureNoWrite;
    }

    // The producer/depth/MV/final-output joins are intentionally explicit.
    // Until a real adapter proves them, DLSSG admission stays refused.
    bool CanAdmitDlssg() noexcept
    {
        if (ticket_.stage != Stage::FinishedPictureWritten || !ticket_.srAccepted ||
            !ticket_.presentEpoch || !ticket_.swapchain || !ticket_.color || !ticket_.queue ||
            !ticket_.submissionSerial || !ticket_.fenceValue || !ticket_.producerJoin ||
            !ticket_.depthAuthenticated || !ticket_.motionAuthenticated ||
            !ticket_.finalOutputAuthenticated)
        {
            ticket_.stage = Stage::RefusedMissingProducerJoin;
            return false;
        }
        return true;
    }

    const Ticket& Snapshot() const noexcept { return ticket_; }
};

inline OutcomeState& Current() noexcept
{
    static OutcomeState state;
    return state;
}
} // namespace GenshinFg
