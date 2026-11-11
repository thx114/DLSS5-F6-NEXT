// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <memory>
// DlssNrFrameInfo only stores this opaque observation. Avoid importing NGX,
// Config or the diagnostics implementation into the lifecycle fixture.
namespace GuideSourceMetadata {
struct Descriptor;
using Ptr = std::shared_ptr<const Descriptor>;
}
