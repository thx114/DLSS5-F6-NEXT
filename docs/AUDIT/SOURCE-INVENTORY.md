# OptiScaler 源码树冗余审计（只读）

> 审计对象：`D:\CODE\DLSS5_NEXT\OptiScaler\`
> 参照工程：`OptiScaler\OptiScaler.vcxproj`（`build.ps1` 唯一构建目标，Release|x64）
> 审计方式：全程只读（`find` / `du` / `grep` / `awk`），未修改、未删除、未移动任何源码或构建文件；本报告是唯一写入产物。
> 审计时间：2026-11-04（构建期间，未触碰 `build\`、`build.ps1`、`external\` 内容）
> 结论口径见文末「口径与限制」，凡无把握处一律标 **待确认**，不给删除建议。

---

## 0. 一页速览

| 指标 | 数值 |
| --- | --- |
| `OptiScaler\` 文件数 / 体积 | **1612 个 / 124,903,963 B（119.1 MiB）** |
| `OptiScaler\` 目录数（含空目录） | 211 个，其中 **空目录 4 个** |
| `external\`（仅统计体积，未审计内部） | 3287 个 / 147,410,233 B（140.6 MiB） |
| `build\`（构建生成物，不属审计范围） | 2,732,232,472 B（约 2.5 GiB） |
| 工程显式引用项 | ClCompile 303 / ClInclude 457 / None 3 / ResourceCompile 1 |
| 工程是否使用通配符 | **否**（0 条 `Include="*\*"`，逐文件显式列出） |
| 工程引用断链（指向不存在的文件） | **0 个** |
| 磁盘源码类文件（.cpp/.h/.inl/.c/.hpp/.asm/.hlsl/.hlsli/.rc） | 1171 个 |
| 未被工程引用的源码类文件 | **411 个 / 12,673,137 B（12.1 MiB）** |
| ⤷ 其中经 `#include` 间接使用（正常，非冗余） | 279 个 / 8,815,002 B |
| ⤷ 其中**无任何引用证据**（候选） | **132 个 / 3,858,135 B（3.68 MiB）** |
| 单文件最大体积项 | `shaders\shader_tools\dxcompiler.dll` 17,951,808 B |
| **体积最大的"可疑但不建议直接删"项** | `library\` 的 `*_x64d.lib`（Debug 库）**34,587,818 B（33.0 MiB）** |

**被点名的怀疑对象结论（详见第 4 节）**：`F5\` 与 `NEXT\` 是**空目录壳**（0 文件）；`wrapped\`、`proxies\`、`genshin\legacy\`、`uid\` **均被工程/代码正常引用，必须保留**；真正的冗余集中在 `genshin\tests\`（测试迭代快照）、`menu\font\Hack.h`（字体中间产物）、`library\` 的 Debug 库、`shaders\shader_tools\`（21 MB 离线工具链）。

---

## 1. 总体规模与体积构成

### 1.1 按扩展名（OptiScaler\ 全树）

| 扩展名 | 文件数 | 字节 | 说明 |
| --- | ---: | ---: | --- |
| `.lib` | 27 | 59,032,026 | `library\` 静态链接库，被 vcxproj 链接器引用 |
| `.h` | 725 | 20,598,440 | 头文件（含预编译着色器字节码头、字体头） |
| `.dll` | 2 | 19,455,112 | 仅 `shaders\shader_tools\`（dxcompiler/dxil） |
| `.cpp` | 289 | 13,264,929 | 翻译单元 |
| `.exe` | 17 | 4,665,280 | 着色器工具 3 个 + genshin 测试夹具 14 个 |
| `.c` | 22 | 1,415,426 | 第三方 C 源码（gimi/pcre2 等） |
| `.dxbc` | 100 | 892,752 | 反汇编夹具，全部在 `genshin\tests\` |
| `.cso` | 53 | 755,604 | 预编译着色器产物（离线构建输入） |
| `.ttf` | 2 | 721,048 | 菜单字体源文件 |
| `.spv` | 24 | 687,624 | Vulkan 预编译着色器 |
| `.asm` | 66 | 672,508 | 反汇编夹具，全部在 `genshin\tests\` |
| `.inl` | 15 | 623,769 | 头内联实现 |
| `.hpp` | 12 | 528,508 | 第三方 C++ 头 |
| `.log` | 130 | 214,908 | **构建/测试日志，全部在 `genshin\tests\`** |
| 其他 | — | 其余 | `.hlsl/.hlsli/.json/.md/.vcxproj/.filters/.def/.ini` 等 |

### 1.2 体积占比（前 5）

`library\` 47.3% ＋ `shaders\` 27.1% ＋ `genshin\` 6.2% ＋ `include\` 4.3% ＋ `menu\` 3.9% ＝ 88.8%。
**换句话说：源码树 89% 的体积不是 C++ 翻译单元，而是链接库、着色器与测试夹具。** 精简应优先此处，而不是去动 `.cpp`。

---

## 2. 顶层目录清单（目录 / 文件数 / 体积 / 是否被工程引用 / 建议）

「工程引用」列的 x/y ＝ 该目录下 `.cpp/.h/.inl/.c/.hpp` 文件总数中，被 `OptiScaler.vcxproj` 显式列出的数量。

| 目录 | 文件数 | 体积 (B) | 最新 mtime | 工程引用 | 建议 |
| --- | ---: | ---: | --- | --- | --- |
| `library\` | 27 | 59,032,026 | 2026-10-11 | n/a（链接库，按 `AdditionalDependencies` 全被引用，仅 `detours.lib` 未被 vcxproj 直接列出，见 4.6） | **保留**；Debug 库 33.0 MiB **待确认** |
| `shaders\` | 335 | 33,900,515 | 2026-11-04 | 152 / 205 | **保留**；`shader_tools\`（21.1 MB）**待确认**，见 4.7 |
| `genshin\` | 467 | 7,696,238 | 2026-11-04 | 46 / 110 | 源码**保留**；`tests\`（5.09 MB）**候选删除/待确认**，见 4.5 |
| `include\` | 88 | 5,421,565 | 2026-10-19 | 28 / 87 | **保留**（多数经 `#include` 使用）；12 个未用 SDK 头 663 KB **待确认** |
| `menu\` | 56 | 4,931,571 | 2026-11-04 | 38 / 53 | **保留**；`font\Hack.h`（1.92 MB）**候选删除** |
| `mods\` | 98 | 3,120,280 | 2026-11-04 | 61 / 82 | **保留**（16 个 `third_party\reference\` 为参考资料，**待确认**） |
| `native\` | 59 | 2,134,063 | 2026-11-04 | 49 / 59 | **保留**；1 个死头 22.7 KB **候选删除** |
| `hooks\` | 39 | 1,502,541 | 2026-11-04 | 36 / 38 | **保留** |
| `integration\` | 32 | 1,096,448 | 2026-11-04 | 19 / 31 | **保留** |
| `dlssnr\` | 64 | 922,684 | 2026-11-04 | 20 / 55 | **保留**；`forwarder\` 为**独立子工程，待确认**，见 4.8 |
| `inputs\` | 53 | 797,146 | 2026-11-04 | 49 / 53 | **保留** |
| `upscalers\` | 91 | 731,359 | 2026-11-04 | 81 / 91 | **保留** |
| `framegen\` | 42 | 707,698 | 2026-11-04 | 36 / 42 | **保留** |
| `resource_tracking\` | 11 | 489,370 | 2026-11-04 | 11 / 11 | **保留** |
| `proxies\` | 12 | 303,071 | 2026-10-20 | **12 / 12** | **保留**（全量被引用，见 4.3） |
| `with_dx12\` | 8 | 313,883 | 2026-11-04 | 6 / 8 | **保留** |
| `exports\` | 9 | 175,419 | 2026-10-11 | 9 / 9 | **保留** |
| `wrapped\` | 4 | 148,388 | 2026-11-04 | **4 / 4** | **保留**（全量被引用，见 4.2） |
| `motion\` | 11 | 150,522 | 2026-10-23 | 10 / 11 | **保留** |
| `low_latency\` | 22 | 127,182 | 2026-11-03 | 21 / 22 | **保留** |
| `misc\` | 11 | 90,154 | 2026-11-01 | 11 / 11 | **保留** |
| `nvapi\` | 14 | 92,793 | 2026-10-11 | 14 / 14 | **保留** |
| `spoofing\` | 6 | 66,127 | 2026-10-23 | 6 / 6 | **保留** |
| `hudfix\` | 2 | 41,595 | 2026-10-11 | 2 / 2 | **保留** |
| `ui_mask\` | 2 | 34,066 | 2026-11-04 | 2 / 2 | **保留** |
| `uid\` | 2 | 30,122 | 2026-11-01 | 0 / 2 → 经 `#include <uid/…>` 使用 | **保留**（未列入工程属正常：纯头） |
| `fsr4\` | 4 | 23,068 | 2026-10-11 | 4 / 4 | **保留** |
| `gpu_time\` | 4 | 9,670 | 2026-10-31 | 4 / 4 | **保留** |
| `scanner\` | 2 | 6,366 | 2026-10-11 | 2 / 2 | **保留** |
| `upscaler_time\` | 2 | 2,433 | 2026-10-11 | 2 / 2 | **保留** |
| **`F5\`** | **0** | **0** | 2026-10-19 | 无 | **候选删除**（空目录，见 4.1） |
| **`NEXT\`** | **0** | **0** | 2026-10-21 | 无 | **候选删除**（3 层空目录壳，见 4.1） |

顶层散文件 34 个，合计约 700 KB，均为工程正常组成（`Config.cpp` 177 KB、`OptiScaler.vcxproj` 95 KB、`dllmain.cpp` 112 KB 等）；`resource_build_date.h` / `resource_build_commit.h` 为构建生成物（每次构建重写，**不要**纳入清理范围）。

---

## 3. 未引用文件清单

### 3.1 判定方法（可复现）

1. 从 vcxproj 抽取全部 `Include="…"`（764 条有效项，`\`→`/`）。
2. 与磁盘上 1171 个源码类文件做差集 → **411 个「磁盘有、工程未显式引用」**。
3. 对这 411 个文件再做一次 `#include` 证据检索：抽取全树 `#include "…"` 与 `#include <…>` 的 basename 集合（921 个），命中即视为「间接使用」。
   - 命中 **279 个**（8,815,002 B）→ 头文件依赖，正常，**不是冗余**。
   - 未命中 **132 个**（3,858,135 B）→ 本文的「候选」清单。

### 3.2 132 个候选的分类（**先分类，再谈删**）

| 类别 | 文件数 | 字节 | 性质 | 处置建议 |
| --- | ---: | ---: | --- | --- |
| 自有代码（非测试） | 10 | 2,109,632 | `.cpp/.h`，全树无 include、无工程引用 | 7 个**候选删除**，3 个**待确认**（见 3.4） |
| 第三方 SDK 头 / 示例 | 12 | 663,827 | `include\imgui\imgui_demo.cpp`、`include\d3dx\D3DX11*.h`、`include\fsr*\*shaders*.h`、`include\detours\detver.h` | 多为上游原样文件，**待确认**（删了不影响构建，但会破坏 SDK 完整性） |
| 着色器源码（离线构建输入） | 41 | 329,913 | `shaders\**\*.hlsl/.hlsli` | **不是僵尸**：由 `shaders\shader_tools\*.bat` 离线编译成 `.cso`/头文件，**保留** |
| 测试夹具（genshin\tests） | 69 | 754,763 | 66 `.asm` 反汇编快照 + 2 `.cpp` + 1 `.h` | 随 `genshin\tests\` 整体处置，见 4.5 |

### 3.3 未引用文件 Top 50（按体积降序，已剔除 `genshin\tests\` 的重复夹具，另行列汇总）

| # | 字节 | 文件 | 类别 |
| ---: | ---: | --- | --- |
| 1 | 1,921,697 | `menu/font/Hack.h` | 自有·字体中间产物 |
| 2 | 583,267 | `include/imgui/imgui_demo.cpp` | 三方·官方 demo |
| 3 | 112,944 | `shaders/dlssnr/precompile/dlssnr.hlsl` | 着色器源 |
| 4 | 61,828 | `shaders/dlssnr/precompile/dlssnr_detail_reuse.hlsl` | 着色器源 |
| 5 | 49,044 | `dlssnr/forwarder/dlssnr_forwarder.cpp` | 独立子工程源 |
| 6 | 46,601 | `shaders/output_scaling/fsr1/FSR_RCAS_Shader.h` | 着色器预编译头 |
| 7 | 26,070 | `include/d3dx/D3DX11tex.h` | 三方 SDK 头 |
| 8 | 25,669 | `shaders/rcas/precompile/da_das_sharpen.hlsl` | 着色器源 |
| 9 | 24,869 | `mods/gimi/third_party/reference/ShaderRegex.cpp` | 参考资料 |
| 10 | 22,661 | `native/GenshinMotionDiagnostic.h` | 自有·死头 |
| 11 | 19,823 | `include/fsr31/shaders/ffx_fsr3upscaler_common.h` | 三方 SDK 头 |
| 12 | 15,616 | `genshin/GenshinPassEvidence.h` | 自有·死头 |
| 13 | 15,517 | `genshin/GenshinCameraObserver.cpp` | 自有·死源 |
| 14 | 14,023 | `shaders/rcas/precompile/da_rcas_sharpen.hlsl` | 着色器源 |
| 15 | 11,682 | `shaders/sgsr1/precompile/sgsr1.hlsl` | 着色器源 |
| 16 | 11,574 | `shaders/dlssnr/precompile/dlssnr_detail_stats.hlsl` | 着色器源 |
| 17 | 11,029 | `include/d3dx/D3DX11async.h` | 三方 SDK 头 |
| 18 | 8,570 | `genshin/GenshinDepthContract.h` | 自有·死头 |
| 19 | 8,006 | `genshin/shaders/GenshinDepthTransform.hlsl` | 着色器源 |
| 20 | 6,664 | `shaders/dlssnr/precompile/dlssnr_lut.hlsl` | 着色器源 |
| 21 | 6,243 | `shaders/dlssnr/precompile/dlssnr_spatial.hlsl` | 着色器源 |
| 22 | 6,091 | `shaders/dlssnr/precompile/dlssnr_replace_curve.hlsli` | 着色器源 |
| 23 | 6,022 | `shaders/output_scaling/precompile/bcds_magc.hlsl` | 着色器源 |
| 24 | 5,168 | `shaders/output_scaling/precompile/bcus.hlsl` | 着色器源 |
| 25 | 4,897 | `shaders/rcas/precompile/rcas.hlsl` | 着色器源 |
| 26 | 4,321 | `shaders/output_scaling/precompile/bcds_kaiser2.hlsl` | 着色器源 |
| 27 | 4,118 | `shaders/output_scaling/precompile/bcds_kaiser3.hlsl` | 着色器源 |
| 28 | 4,107 | `menu/MenuPhysicalTiming.h` | 自有·死头 |
| 29 | 3,981 | `shaders/output_scaling/precompile/bcds_bicubic.hlsl` | 着色器源 |
| 30 | 3,746 | `include/d3dx/D3DX11core.h` | 三方 SDK 头 |
| 31 | 3,722 | `shaders/dlssnr/precompile/dlssnr_spatial_warp.hlsli` | 着色器源 |
| 32 | 3,595 | `include/fsr2/dx11/shaders/ffx_fsr2_shaders_dx11.h` | 三方 SDK 头 |
| 33 | 3,554 | `include/fsr2_212/vk/shaders/ffx_fsr2_shaders_vk.h` | 三方 SDK 头 |
| 34 | 3,497 | `include/fsr2_212/dx12/shaders/ffx_fsr2_shaders_dx12.h` | 三方 SDK 头 |
| 35 | 3,472 | `include/fsr2/vk/shaders/ffx_fsr2_shaders_vk.h` | 三方 SDK 头 |
| 36 | 3,420 | `include/fsr2/dx12/shaders/ffx_fsr2_shaders_dx12.h` | 三方 SDK 头 |
| 37 | 3,210 | `shaders/output_scaling/precompile/bcds_lanczos3.hlsl` | 着色器源 |
| 38 | 3,157 | `shaders/dlssnr/precompile/dlssnr_finished_color.hlsl` | 着色器源 |
| 39 | 3,081 | `shaders/output_scaling/precompile/bcds_lanczos2.hlsl` | 着色器源 |
| 40 | 3,025 | `shaders/output_scaling/precompile/bcds_catmull.hlsl` | 着色器源 |
| 41 | 2,539 | `shaders/magnifier/precompile/Magnifier.hlsl` | 着色器源 |
| 42 | 2,510 | `shaders/dlssnr/precompile/dlssnr_guide_preview.hlsl` | 着色器源 |
| 43 | 2,262 | `shaders/dlssnr/precompile/dlssnr_exposure_adapt.hlsl` | 着色器源 |
| 44 | 2,231 | `shaders/output_scaling/fsr1/fsr_easu.hlsl` | 着色器源 |
| 45 | 1,740 | `shaders/output_scaling/fsr1/fsr_rcas.hlsl` | 着色器源 |
| 46 | 1,671 | `include/d3dx/D3DX11.h` | 三方 SDK 头 |
| 47 | 1,270 | `shaders/dlssnr/precompile/dlssnr_pq.hlsli` | 着色器源 |
| 48 | 977 | `shaders/hudless_compare/precompile/hudless_compare.hlsl` | 着色器源 |
| 49 | 950 | `dlssnr/LookUiSnapshot.h` | 自有·死头 |
| 50 | 833 | `shaders/resource_flip/precompiled/RF.hlsl` | 着色器源 |

> 说明：上表第 51–63 名（合计约 6 KB）均为 `shaders\*\precompile\*.hlsl` 与零星 SDK 头，不逐一列出。
> **`genshin\tests\` 另有 69 个未被工程引用的文件（754,763 B）**，其重复度极高（例如 10 个完全同名的 `reflection.asm`，每个 10,308 B，分属 10 个迭代快照目录），不作为 Top 50 逐条列出，按目录整体处置。

### 3.4 自有代码「零引用」明细（10 个，2,109,632 B）——逐条证据

全树检索（`.cpp/.h/.vcxproj/.filters`）中 **无任何出现**，且不在工程 ClCompile/ClInclude 中：

| 文件 | 字节 | 判定 | 说明 |
| --- | ---: | --- | --- |
| `menu/font/Hack.h` | 1,921,697 | **候选删除** | 未压缩字体字节头（旧方案）；现行编译单元是 `menu/font/Hack_Compressed.cpp`，`Hack_Compressed.cpp` 的注释说明它由 `Hack-Regular.ttf` 生成 |
| `native/GenshinMotionDiagnostic.h` | 22,661 | **候选删除** | 全树零引用；名称带诊断语义，疑似早期诊断方案残留 |
| `genshin/GenshinPassEvidence.h` | 15,616 | **候选删除** | 全树零引用 |
| `genshin/GenshinCameraObserver.cpp` | 15,517 | **候选删除（需一次人工确认）** | 未被编译、无人 include；`.cpp` 不进编译即等于不存在，但删除前建议确认没有计划恢复 |
| `genshin/GenshinDepthContract.h` | 8,570 | **候选删除** | 全树零引用 |
| `menu/MenuPhysicalTiming.h` | 4,107 | **候选删除** | 全树零引用 |
| `dlssnr/LookUiSnapshot.h` | 950 | **候选删除** | 全树零引用 |
| `mods/gimi/third_party/reference/ShaderRegex.cpp` | 24,869 | **待确认** | 位于 `third_party/reference/`，目录名表明是「参考实现留档」，不是构建单元 |
| `dlssnr/forwarder/dlssnr_forwarder.cpp` | 49,044 | **待确认** | 它属于**独立子工程** `dlssnr/forwarder/dlssnr_forwarder.vcxproj` + `CMakeLists.txt`，不被 `build.ps1` 构建 |
| `shaders/output_scaling/fsr1/FSR_RCAS_Shader.h` | 46,601 | **待确认** | 着色器预编译头，可能只服务于离线着色器再生成流程，需对照 `shader_tools` 脚本确认 |

---

## 4. 重点怀疑目录：逐个结论（含引用证据）

### 4.1 `F5\` 与 `NEXT\` —— 确认是空目录壳，**零风险**
- `F5\`：0 文件、0 字节（mtime 2026-10-19）。
- `NEXT\`：其下只有 `NEXT\work\` → `full-source-f5-v11-candidate-20261018\`、`r294-r266-build\`，**全部 0 文件**；目录名带旧日期（20261018）与旧里程碑（r294-r266），是迁移时按目录结构复制的残留壳。
- 引用证据：全树 `grep` 未发现任何 `#include`、路径字符串或 vcxproj 条目指向这两个目录（唯一命中是 `menu/font/Inter_Compressed.cpp` 的压缩字面量里偶然出现的 "F5/" 字节串，无关）。
- 结论：**候选删除（删除空目录，0 字节，仅减少结构噪音与误导）**。

### 4.2 `wrapped\`（4 文件 / 148 KB）—— **必须保留**
引用证据（多个已编译 TU 直接 include）：
- `hooks/D3D11_Hooks.cpp`、`hooks/DxgiFactory_Hooks.cpp`、`hooks/DxgiFactory_WrappedCalls.cpp`、`genshin/GenshinLegacyCapture.cpp` → `#include <wrapped/wrapped_swapchain.h>`
- `hooks/DxgiFactory_WrappedCalls.h` → `#include <wrapped/wrapped_factory.h>`
- 4 文件 **4/4 全部** 在 vcxproj 中；`wrapped_swapchain.cpp` 122 KB 且 mtime 为 2026-11-04 14:39（近期活跃开发）。
- 结论：**保留**。名字像"包装层冗余"，实际是 DXGI 交换链包装的核心实现。

### 4.3 `proxies\`（12 文件 / 303 KB）—— **必须保留**
- 12/12 全部列入 vcxproj `ClInclude`；纯头文件代理（`FfxApi_Proxy.h`、`NVNGX_Proxy.h`、`XeSS_Proxy.h`、`Streamline_Proxy.h` 等），是 DLL 转发导出机制的数据源，与 `Source.def`（37 KB 导出表）配套。
- 结论：**保留**。注意：修改/删除会直接改变 DLL 导出符号，属最高风险类别。

### 4.4 `include\`（88 文件 / 5.42 MB）—— 保留，内含 12 个未用 SDK 头（663 KB，待确认）
- 87 个源文件中仅 28 个显式列入工程，其余靠 `#include` 拉取，属正常第三方头布局。
- 唯一大件是 `include\imgui\`（25 文件 / 4.39 MB），包含官方 `imgui_demo.cpp`（583 KB）与 `ImGuiNotify.hpp`（24.6 KB），均未被编译/引用。
- `include\d3dx\` 4 个 `D3DX11*.h`（42.5 KB）未被任何文件 include，但工程链接了 `d3dx11.lib`，**删除前需确认这些头是否已由 Windows SDK 覆盖** → **待确认**。
- 结论：**保留**；仅 `imgui_demo.cpp` 与 4 个 D3DX11 头列为可精简候选。

### 4.5 `genshin\tests\`（360 文件 / 5,090,217 B）—— **本次最大的一块"过程产物"**
- 结构：`fg-depth-camera\`（208 文件 / 3.72 MB，含 `r851-source`、`r851-source-v2`、`pre-fix`、`pre-fix-diag`、`final`、`final-r852`、`final-r852b/c/d/e`、`perspective-v1/v2/v3`、`release-fp-fast` 共 **13 个迭代快照目录**）＋ `fg-animated-character\`（152 文件 / 1.37 MB，含 `candidate-v1/v2/v3`、`final`、`final-embedded` 共 5 个快照）。
- 内容：100 `.dxbc`、66 `.asm`（反汇编快照）、130 `.log`（stdout/stderr 各 65）、41 `.json`、14 `.exe`（同 source 多次重编译产物）、2 `.py`（`run.py` 测试驱动）、2 `.cpp`（`fixture.cpp`）、`GenshinFgGameMotion.before.h`（42.7 KB，"改前"快照）、`product.patch`（2.9 KB）。
- 引用证据：**不被 `build.ps1` 构建**（`build.ps1` 只传 `OptiScaler\OptiScaler.vcxproj`）；`OptiScaler` 树内没有任何 `.ps1/.bat/.py/.vcxproj` 引用 `genshin/tests`；仅若干头文件注释提到 `tests/*_smoke.cpp`（指的是旧根下的主机端 smoke 测试，不是本目录）。
- 结论：**候选删除 / 待确认**。建议保留 `final*` 与 `run.py`、`fixture.cpp` 作为回归样本（约 1.6 MB），删除 `candidate-v*`、`pre-fix*`、`r851-*`、`final-r852b/c/d/e`、`perspective-v*` 等中间快照（约 **3.4 MB**）。**是否保留历史快照属产品决策，本报告不下结论。**

### 4.6 `library\`（27 文件 / 59.03 MB）—— 保留，但 Debug 库 33.0 MiB 值得决策
- 27 个 `.lib` 中 26 个在 vcxproj `AdditionalDependencies` 中逐名列出（`ffx_*`、`vulkan-1.lib`、`d3dx11.lib`、`freetype.lib`）；仅 `library\detours\detours.lib`（841,500 B）未被 vcxproj 直接列出，但 `#include <detours/detours.h>` 被 6+ 个已编译 TU 使用 → **该库的具体链接方式待确认**（可能由 props 或 `/DEFAULTLIB` 间接生效），**不要动**。
- 构成：`fsr31\` 10 个 / 23.47 MB，`fsr2\` 8 个 / 21.77 MB，`fsr2_212\` 6 个 / 12.88 MB，`detours` 0.84 MB，`vulkan` 61 KB，`d3dx` 14 KB。
- **Debug 库（`*_x64d.lib`）12 个 / 34,587,818 B（33.0 MiB）**：只被 vcxproj 的 Debug/ReleaseDebug 配置使用，而 `build.ps1` 固定 `Configuration=Release`。若确定只维护 Release 构建，这是**单笔体积最大的可精简项**；反之（需要 Debug 构建或崩溃分析）**必须保留** → **待确认**。
- `fsr2\` 与 `fsr2_212\` 是两代 FSR2 并存（`include\` 下同样并存两套头），属**有意的多版本支持**，不是误迁重复 → 保留。

### 4.7 `shaders\shader_tools\`（10 文件 / 21.06 MB）—— 是**离线工具链**，不是误入的二进制
- 内容：`dxcompiler.dll` 17.95 MB、`dxil.dll` 1.50 MB、`dxc.exe` 1.13 MB、`dxv.exe` 295 KB、`fxc.exe` 183 KB + 4 个 `.bat` + `create_header.py`。
- 引用证据：`shaders\shader_tools\build_precompiled_shader*.bat`、`shaders\dlssnr\precompile\build_spatial.bat`、`build_detail_reuse.ps1`、`build_guide_preview.ps1` 都按相对路径调用 `..\..\shader_tools\dxc.exe` —— **只在"重新生成预编译着色器"时才需要，普通 DLL 构建完全不用**。
- 结论：**保留，但建议移出源码树**（如 `tools\shader_tools\`）。这使得 `OptiScaler\` 立刻减重 21.1 MB（占全树 17%），且对 `build.ps1` 零影响（构建链不引用它）。是否迁移属结构决策 → **待确认（迁移而非删除）**。

### 4.8 `dlssnr\forwarder\`（6 文件 / 约 50 KB + 空目录）—— 独立子工程，**待确认**
- 含 `dlssnr_forwarder.vcxproj`（3.4 KB）、`CMakeLists.txt`、`README.md`、`dlssnr_forwarder.cpp`（49 KB）。
- 不被 `build.ps1` 构建（`build.ps1` 只构建 `OptiScaler.vcxproj`），主工程也不引用它；`inputs\NVNGX_DLSS_Vk.cpp` 仅在一行注释中提到它。
- 目录内还有一个空目录 `dlssnr\forwarder\OptiScaler\`（迁移残留）。
- 结论：**待确认**（若 forwarder DLL 仍是发布物的一部分，构建链需要补；若不是，可整体移入 `legacy\` 归档）。

### 4.9 其他"名字可疑但实际正常"的项（避免误删）
| 项 | 字节 | 结论 |
| --- | ---: | --- |
| `genshin\legacy\`（8 文件 / 218 KB） | 218,404 | **保留**：`Il2CppCallSiteHook.cpp/h`、`Fsr2FamilyTakeover.h`、`MotionFlipPolicy.h`、`ResourceIdentification.h` 均列入 vcxproj；`CameraProducer*.inl`、`TranslationFrameTimePolicy.h` 经 `#include` 使用 |
| `shaders\hud_copy\`、`shaders\resource_copy\` | 36,628 | **保留**：目录名含 "copy" 是"拷贝通道着色器"之意，非备份 |
| `menu\font\*.ttf`（721 KB） | 721,048 | **待确认**：`Hack_Compressed.cpp`/`Inter_Compressed.cpp` 由这两个 TTF 生成；若接受"以后不再重新生成字体头"，可删；否则保留 |
| `upscalers\`（91 文件 / 731 KB） | 731,359 | **保留**：81/91 在工程中，是各放大器的实现主体 |
| `with_dx12\`、`ui_mask\` 类小目录 | < 320 KB | **保留**：全部或绝大多数被引用 |

---

## 5. 二进制 / 备份 / 临时文件清单

### 5.1 二进制（`external\` 之外，OptiScaler\ 内）

| 文件 | 字节 | 性质 | 建议 |
| --- | ---: | --- | --- |
| `shaders\shader_tools\dxcompiler.dll` | 17,951,808 | 离线着色器编译器 | 保留（建议移出源码树） |
| `shaders\shader_tools\dxil.dll` | 1,503,304 | dxc 依赖 | 同上 |
| `shaders\shader_tools\dxc.exe` | 1,126,984 | dxc 主程序 | 同上 |
| `genshin\tests\fg-depth-camera\` 9 个 `dynamic-fixture.exe`（同源多版本） | 合计 2,691,968 | 测试夹具多次重编译 | 随 tests 快照处置 |
| `shaders\shader_tools\dxv.exe` | 295,456 | DXBC 反汇编器 | 同 dxc |
| `genshin\tests\fg-animated-character\` 5 个 `animated-fixture.exe` | 合计 354,352 | 测试夹具 | 随 tests 快照处置 |
| `shaders\shader_tools\fxc.exe` | 182,616 | 旧版着色器编译器 | 保留（`build_precompiled_shader_fxc.bat` 在用） |
| `menu\font\Inter-Regular.ttf` / `Hack-Regular.ttf` | 411,640 / 309,408 | 字体源 | **待确认** |

- 未见 `.pdb`、`.zip`、`.7z`、`.png`、`.jpg`、`.ico` 误入（`build\` 生成物除外）。
- 预编译着色器产物（**离线构建输入，非冗余**）：100 个 `.dxbc`（892,752 B）、53 个 `.cso`（755,604 B）、24 个 `.spv`（687,624 B）。

### 5.2 备份 / 临时文件

| 模式 | 命中 |
| --- | --- |
| `*.bak` / `*~` / `*.orig` / `*.rej` / `*.old` / `*.tmp` / `*.swp` / `copy of*` / `*.backup` / `*.prev` | **0 个** |
| `*.before*`（"改前"快照） | 1 个：`genshin\tests\fg-depth-camera\GenshinFgGameMotion.before.h`（42,700 B） |
| `*.patch` / `*.diff` | 1 个：`genshin\tests\fg-animated-character\product.patch`（2,869 B） |
| 含日期名的目录 | 1 个：`NEXT\work\full-source-f5-v11-candidate-20261018\`（空） |
| 迭代快照目录（r851/r852/candidate-v*/pre-fix/perspective-v*） | 18 个（见 4.5） |
| 构建/测试日志 `.log` | **130 个 / 214,908 B，全部位于 `genshin\tests\`（stdout/stderr）** |

> 结论：**没有传统意义的 `.bak` 类垃圾**。所谓"备份痕迹"是**语义化的迭代快照**（`r852b/c/d/e`、`candidate-v1/v2/v3`、`pre-fix`、`.before.h`），这正是旧项目"技术债"的形态——用复制目录代替版本控制。

### 5.3 空目录（4 个）

| 路径 | 说明 |
| --- | --- |
| `OptiScaler\F5\` | 迁移残留 |
| `OptiScaler\NEXT\work\full-source-f5-v11-candidate-20261018\` | 迁移残留（旧日期） |
| `OptiScaler\NEXT\work\r294-r266-build\` | 迁移残留（旧里程碑） |
| `OptiScaler\dlssnr\forwarder\OptiScaler\` | 迁移残留 |

---

## 6. 低风险精简 Top 10

排序原则：**先删「没有任何代码能用到」的东西，再谈「体积大但可能还要用」的东西。** 每项均给出一句话安全理由。

| # | 项目 | 可回收 | 为什么安全 |
| ---: | --- | ---: | --- |
| 1 | 4 个空目录（`F5\`、`NEXT\work\**`、`dlssnr\forwarder\OptiScaler\`） | 0 B（4 目录） | 0 文件、0 字节，任何构建/`#include`/脚本都不可能读空目录，删除只消除结构噪音与"这里曾经有东西"的误导。 |
| 2 | `menu\font\Hack.h` | 1.92 MB | 未压缩字体字节头，全树零 include、不在 vcxproj；现行方案是 `Hack_Compressed.cpp`（工程内），两者互斥，留的是编译单元而非旧头。 |
| 3 | 7 个零引用自有死文件（`native\GenshinMotionDiagnostic.h`、`genshin\GenshinPassEvidence.h`、`genshin\GenshinCameraObserver.cpp`、`genshin\GenshinDepthContract.h`、`menu\MenuPhysicalTiming.h`、`dlssnr\LookUiSnapshot.h` 等） | 约 67 KB | 全树 `.cpp/.h/.vcxproj/.filters` 检索零出现：既不被编译，也不被包含，删除对产物无任何影响（`.cpp` 不进 `ClCompile` 本就等于不存在）。 |
| 4 | `genshin\tests\` 的中间迭代快照（`candidate-v*`、`pre-fix*`、`r851-source*`、`final-r852b/c/d/e`、`perspective-v*`） | **约 3.4 MB** | 每个快照是同一夹具的重复构建产物与日志，保留 `final\` + `final-embedded\` + `run.py` + `fixture.cpp` 即保留可复现的回归入口；该目录不被 `build.ps1` 与任何脚本引用。 |
| 5 | `genshin\tests\` 内的 130 个 `.log`（stdout/stderr） | 215 KB | 是一次性运行的输出留档，测试可用 `run.py` 重跑再生，不参与任何构建或断言逻辑。 |
| 6 | `GenshinFgGameMotion.before.h` + `product.patch` | 45.6 KB | 前者是"改前"文件快照、后者是补丁草稿，均为过程留档；真正的实现在已编译的 `genshin\*` 中，删除不影响行为。 |
| 7 | `include\imgui\imgui_demo.cpp` | 583 KB | ImGui 官方 demo，从未进入 `ClCompile`、也无任何 `#include` 指向它；删掉不改变编译结果（仅降低 SDK 完整性，若想保上游原样可只移入归档）。 |
| 8 | `include\d3dx\` 4 个未用 `D3DX11*.h` | 42.5 KB | 全树零 include；**但**工程链接了 `d3dx11.lib`，删除前请确认无隐式依赖 →（标 **待确认**，仅作为低风险候选）。 |
| 9 | 着色器源码与工具链**结构归位**（不删）：41 个未被工程引用的 `.hlsl/.hlsli` 归入 `shaders\src\`，`shaders\shader_tools\`（21.06 MB）迁到 `tools\` | 迁移 21.06 MB | `.hlsl` 由离线 `.bat/.ps1` 编译成 `.cso`/头文件，`shader_tools` 只被这些脚本按相对路径调用；`build.ps1` 完全不引用它们，迁移对 Release 构建零影响。 |
| 10 | `dlssnr\forwarder\` 判定归档 | 约 50 KB | 独立 `vcxproj`+`CMakeLists`，`build.ps1` 不构建、主工程不引用；**先确认它不是发布物**（若不是，整体移入 `legacy\`；若是，则构建链需补齐——这正是风险点，故列"待确认"）。 |

**可回收合计（不含第 9、10 项的结构迁移）**：约 **5.9 MB / 约 40 个文件 + 4 个目录**（2.0 MB 死代码与字体头 + 3.4 MB 测试快照 + 0.26 MB 日志与补丁 + 0.6 MB demo）。
**若再加上"仅 Release 构建"决策（第 4.6 节 Debug 库 33.0 MiB）**：可回收体积可达 **约 39 MB（全树 32%）**，但那不再是"零风险"，而是产品策略选择。

---

## 7. 需要 Lead / 用户拍板的三件事（不属于"低风险"，请不要顺手做）

| 项 | 体积 | 决策点 |
| --- | ---: | --- |
| `library\` 的 12 个 `*_x64d.lib` | **34.6 MB（33.0 MiB）** | 是否还需要 Debug 配置构建 / 崩溃分析？只要 Release 就删；否则原样保留。 |
| `shaders\shader_tools\` 工具链 | **21.1 MB** | 是否还要重新生成预编译着色器？留在源码树 vs 迁到 `tools\`（建议后者）。 |
| `genshin\tests\` 历史快照 | 3.4 MB | 是否需要保留历史回归样本用于对照？ |

---

## 8. 口径与限制（务必与结论一起看）

1. **「未被工程引用」≠「死代码」**。本报告已把 411 个未引用文件拆成 4 类：`#include` 依赖（279）、离线着色器/汇编构建输入（41）、测试夹具（69）、零引用自有代码（10）。只有最后一类与部分三方 SDK 头属真正的删除候选。
2. **未做语义级死代码分析**：不做 C++ 符号级可达性、不分析 `#ifdef` 条件编译分支、不分析运行期反射或"按名字符串解析 DLL 导出"（`Source.def` + `proxies\` 属此类，故一律保留）。因此"零引用"仅指**文本可见的引用**。
3. **`external\` 只统计体积**（3287 文件 / 140.6 MiB / 15 个顶层依赖），未审计内部冗余；本报告不对 `external\` 提出任何建议。
4. **未审计 `build\`**（2.5 GiB 生成物，`build.ps1 -Clean` 可清）。
5. 目录级"工程引用 x/y"中，y 只统计 `.cpp/.h/.inl/.c/.hpp`，不含 `.asm`/`.hlsl`/`.cso`/`.spv`（这些按设计不进 vcxproj）。
6. 体积均为字节精确值（`du -sb` / `stat`），MiB 为换算参考（1 MiB = 1,048,576 B）。
7. `build.ps1` 固定 `Configuration=Release`、`Platform=x64`、单一目标 `OptiScaler.vcxproj`；本文所有"是否参与构建"的结论均以此为准。
8. **工程引用完整性**：vcxproj 全部 764 条 `Include` 均无通配符，且逐条核对后**无一断链**（`shaders\magnifier\precompile\Magnifier_Shader_Vk.spv`、`integration\hoyoshade\sdk\LICENSE.md`、`..\external\nvapi\nvapi.h` 均存在于磁盘）。迁移本身没有丢文件。
9. 全程只读：本审计未修改/删除/移动 `OptiScaler\`、`external\`、`build.ps1` 下任何内容；除本文件外未写入任何路径。

---

## 9. 执行约定（本报告只是清单，不含任何删除动作）

按本项目既有清理约定（**无用文件只移入 `cleanup/<任务>/<原相对路径>` 并登记，不永久删除**），本报告里的"候选删除"应理解为：

1. **执行方式**：移动到 `D:\CODE\DLSS5_NEXT\cleanup\<任务名>\<原相对路径>`，并在登记表写清根目录、用途、字节数、是否重解析点/硬链接、是否被占用；**不做永久删除**。
2. **不覆盖同名**：目标已存在同名项时改前缀，不合并覆盖。
3. **未知或锁定不动**：占用中的文件（如构建正在读的 `OptiScaler\` 内容）一律跳过，不强杀进程。
4. **同盘移动不称"释放空间"**：`cleanup\` 与源码树同盘时体积不会真正下降，只解决结构噪音；需要真实释放才另行讨论。
5. **前置条件**：`build.ps1` 正在编译，任何移动都应在构建结束、且 `git status` 干净（或已 commit）之后进行；`resource_build_date.h`、`resource_build_commit.h`、`build\` 属生成物，永不进入清理范围。
6. **顺序建议**：先做第 6 节 Top 10 的 1→7 项（零引用 + 过程产物），第 8/10 项确认后再做；第 9 项（21 MB 工具链）与第 7 节的 Debug 库属结构/策略决策，需 Lead 明确授权。
