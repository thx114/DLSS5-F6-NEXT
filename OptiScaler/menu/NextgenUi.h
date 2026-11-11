#pragma once

#include "MenuPages.h"
#include <imgui/imgui.h>

// DLSS5 NEXT presentation layer for the unmodified F5 feature pages.
// This owns appearance and navigation only; it does not own feature state.
namespace NextgenUi
{
class StyleScope final
{
public:
    explicit StyleScope(bool lightTheme);
    ~StyleScope();

    StyleScope(const StyleScope&) = delete;
    StyleScope& operator=(const StyleScope&) = delete;
};

// SR / NR / FG / Settings tabs with group-specific page tabs. Requests are consumed
// by the existing page owner on its next draw; rendering never changes feature state.
void RenderHeader(MenuPages::Page currentPage);
// Footer choices edit the existing in-memory language/theme. Returns true for
// a theme change; the owner rebuilds after temporary style scopes are gone.
bool RenderFooterLanguage();
// Keep semantic status colours readable on the active light menu surfaces.
ImVec4 ReadableTextColor(const ImVec4& colour);
// Width of the brand, language and theme buttons in the active font/style.
float FooterLanguageWidth();
// Continue the footer row only when the next control fits inside its content area.
void FooterSameLineIfFits(float nextWidth, float spacing = -1.0f);
bool English();
void SetEnglish(bool english);
} // namespace NextgenUi
