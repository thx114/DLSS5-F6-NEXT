// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Call-local provider result. A successful rebuild request has no current
// picture. OutputRecorded means commands were recorded on the supplied context
// or list; final passes, submission and GPU retirement remain the caller's job.
enum class FeatureEvaluation
{
    Failed,
    NoOutput,
    OutputRecorded,
};
