// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "../runtime/InputObservation.h"

namespace GenshinLegacy {
// Scalar/atomic demand only; no nativeMutex, owner acquisition or GPU poll.
FramePipeline::InputObservationDemand ReadNewInputObservationDemand() noexcept;
}
