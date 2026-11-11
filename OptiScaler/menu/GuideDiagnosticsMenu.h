// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
class Config;
namespace GuideDiagnosticsMenu {
void Render(Config*);
void RenderPreviewStrip(Config*);
void OpenHistoryWindow() noexcept;
void RenderHistoryTuning(Config*);
void RenderHistoryWindow(Config*);
// Called even while hidden: preview continues, temporary FG-only flag restores.
void Visibility(bool visible) noexcept;
}
