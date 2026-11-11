// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <runtime/GpuWorkDomain.h>

// Capture adapter compatibility. Submission ownership itself is shared by
// every input API; game-specific semantics remain at the capture boundary.
namespace GenshinTransport {
using FramePipeline::GpuWitness;
using FramePipeline::GpuWorkDomain;
}
