// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "GuideCalibrationImages.h"
#include "GuideCapture.h"
#include <array>
#include <string>

namespace GuideCalibrationRuntime {
struct Status {
    bool active=false,busy=false;uint64_t generation=0,source=0,epoch=0,feature=0,configuration=0,tick=0;
    GuideCalibration::Stage stage=GuideCalibration::Stage::Still;
    GuideCalibration::Recommendation recommendation;
    GuideCalibration::PairResult pair;
    GuideCalibration::Images images;
    std::string reason="Start guided calibration";
};
void Start() noexcept;
void Stop() noexcept;
void SetStage(GuideCalibration::Stage) noexcept;
void SetExclusions(const std::vector<GuideCalibration::Rect>&) noexcept;
void SelectCandidate(unsigned) noexcept;
// May run on hidden-menu frame callbacks. Never waits for scoring work.
void Tick() noexcept;
bool Read(Status&) noexcept;
bool Active() noexcept;
}
