# Licenses

本目录与根 `LICENSE` 从旧根**只读复制**，其余许可正文保持原样。源：`D:/DATA/Documents/ChatGPT/DLSS F5 NEXT/work/full-source-f5-v11-candidate-20261018/`（该目录与 `OptiScaler-f5-nextgen-ui-20261014/` 的许可文件 SHA256 完全一致，属同一上游快照）。

| 文件 | 大小 | 覆盖对象 | 对应本根代码 |
|---|---|---|---|
| `../LICENSE` | 35,149 B | 上游 OptiScaler 主体（GPL-3.0） | 整个 `OptiScaler/` |
| `ReShade_GenericDepth_LICENSE.txt` | 2,092 B | ReShade Generic Depth addon（BSD-3-Clause） | `OptiScaler/resource_tracking/GenericDepth_Select.h`、`native/DepthFinderCore.h` |
| `PeripheralWarp_LICENSE.txt` | 1,076 B | Peripheral Warp（MIT, BeliyG3） | `OptiScaler/shaders/dlssnr/DlssNr_Spatial.h`、`external/peripheral_warp/` |
| `RenoDX_ATTRIBUTION.txt` | 2,910 B | RenoDX（署名） | `shaders/dlssnr/DlssNr_Dx12.cpp` |
| `Inter_OFL.txt` | 4,380 B | Inter 字体（SIL OFL 1.1） | `OptiScaler/menu/font/Inter_Compressed.cpp` 及菜单 UI |
| `DLSS5Feeder_LICENSE.txt` | 1,583 B | DLSS5-Feeder（思路参考，未复制源码） | `OptiScaler/motion/TrustMask_Dx12.*` |

说明：

- `external/` 下各依赖自带许可证（`external/*/LICENSE*`），未在此重复。
- 本目录只放许可与署名文本；不含样例、证据或第三方二进制。
