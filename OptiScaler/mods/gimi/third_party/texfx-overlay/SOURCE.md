# Fixed TexFx DebugShaderOutput

TexFx by Unicornshell and SinsOfSeven; CC BY-SA 4.0, reproduced in LICENSE.
The entire original DebugShaderOutput.hlsl was read from the user's installed
D:/APPS/XXMI/GIMI/Core/GIMI/Libraries/TexFx/hic_sunt_dracones on 2026-11-04.
The fixed local GIMI reference was also read. No upstream-latest claim.
The header embeds that source with CRLF normalized to LF and no other edits.
Preparation requires exact equality after that same newline normalization;
comments, declarations, VS positions/UVs, FLIP, all four samples, depth,
rt6 alpha/g tests, discard and PASS branching are retained. The compiler
uses the original VERTEX_SHADER/PIXEL_SHADER macros and main/vs_5_0/ps_5_0.
No file name, section label, digest or reflection alone authorizes arbitrary
shader content. No include handler permits extra sources.
