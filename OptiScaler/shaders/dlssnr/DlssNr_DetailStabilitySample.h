// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

struct ID3D12Resource;

// Both surfaces are written together, on the same recording and source ordinal.
// The long-lived birth fingerprint is not part of the short filter's sample.
struct DlssNrDetailStabilitySample
{
    ID3D12Resource* residual = nullptr; // filtered edit RGB + mean lag, RGBA16F
    ID3D12Resource* raw = nullptr;      // current unfiltered model input RGB

    explicit operator bool() const { return residual && raw; }
};
