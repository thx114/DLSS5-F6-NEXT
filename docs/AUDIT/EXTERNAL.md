# external/ 第三方依赖审计与瘦身评估

- 审计对象：`D:\CODE\DLSS5_NEXT\external\`
- 审计方式：**只读**（未修改/删除 external、OptiScaler、build.ps1 任何内容）
- 审计时间：2026-11-04
- 构建事实基线：`build.ps1` → Release x64，`build\out\OptiScaler.dll` 34,795,008 bytes，exit 0
- 关键构建事实：`build.ps1` 传入 `/p:PostBuildEventUseInBuild=false`，即 vcxproj 的 **PostBuildEvent（分发复制）在干净构建中不执行**

---

## 0. 结论摘要（先看这里）

| 指标 | 数值 |
|---|---|
| external 现状 | **140.6 MiB**（147,410,233 bytes） / **3287 文件** / 19 个子目录 |
| 保守可回收（A 级，构建+分发均无引用） | **≈ 110.0 MiB（78.3%）** |
| 激进可回收（A+B 级，含未被任何头文件引用的 Vulkan-Hpp 头） | **≈ 113.8 MiB（80.9%）** |
| 瘦身后保留 | **≈ 30.5 MiB（A 级）/ 26.8 MiB（B 级）** |
| 最占空间 3 个库 | **FidelityFX-SDK 60.6 MiB**、**vulkan 30.5 MiB**、**FidelityFX-SDK-v2 29.2 MiB**（合计 120.3 MiB，占 85.6%） |
| 可整库移除 | **RTX40MFG-Unlock**（0 文件，空目录） |
| 构建完全不使用、仅剩分发意图的库 | **FidelityFX-SDK-v2**（除 `Kits\FidelityFX\signedbin` + `docs\license.md` 外全部可删，29.1 MiB） |
| 其余 17 个库 | 均有**编译或链接证据**，不能整库删除，只能内容级裁剪 |

**一句话**：体积几乎全部集中在 3 个大库的「文档 + 样例 + 预编译多平台库 + SDK 主体源码」上，这些目录在编译/链接上不可达；把它们裁掉可省 110 MiB 而完全不影响当前 Release x64 构建。

---

## 1. 判定方法（两路交叉证据）

判定「某库是否被使用」必须两路同时为空才算【可移除】，避免误删：

**路径 1 — 工程引用（编译/链接可达性）**
解析 `OptiScaler\OptiScaler.vcxproj` 中所有含 `external` 的配置：

| 位置 | 内容 |
|---|---|
| IncludePath（行 103/107/111，Debug/Release 各平台） | `external\simpleini`、`external\nvngx_dlss_sdk`、`external\unordered_dense\include`、`external\vulkan\include`、`external\xess\inc\xess` |
| IncludePath（行 115/121/125，x64 Release/Debug 全量） | 上述 + `xess\inc\xell`、`xess\inc\xess_fg`、`FidelityFX-SDK\ffx-api\include\ffx_api`、`spdlog\include`、`freetype`、`streamline`、`streamline1`、`nvapi`、`nlohmann`、`fakenvapi`、`magic_enum\include\magic_enum`、`AntiLag2-SDK`、`latencyflex` |
| LibraryPath（行 104/108/112） | `external\xess\lib` |
| LibraryPath（行 116/122/126） | `external\xess\lib`、`external\freetype` |
| PostBuildEvent 复制（行 252–275，**干净构建未启用**） | `xess\LICENSE.txt`、`xess\bin\*.dll`、`FidelityFX-SDK\docs\license.md`、`FidelityFX-SDK\PrebuiltSignedDLL\amd_fidelityfx_vk.dll`、`FidelityFX-SDK-v2\Kits\FidelityFX\signedbin\amd_fidelityfx_{loader,upscaler,framegeneration}_dx12.dll`、`FidelityFX-SDK-v2\docs\license.md`、`directx_agility_sdk\lib\*.dll`、`directx_agility_sdk\LICENSE.txt` |
| ClInclude（行 621） | `..\external\nvapi\nvapi.h` |

> MSVC 只能读到 `IncludePath/LibraryPath` + 源码相对路径里的文件。**不在以上清单里的目录在编译期不可达**——这是裁剪的核心依据。

**路径 2 — 源码 `#include` / 链接证据**
在 `OptiScaler\` 全量 grep 头文件名（含相对路径引用，见 `peripheral_warp`）。

**唯一一条相对路径引用**（不经 IncludePath，必须单独看）：
```
OptiScaler\shaders\dlssnr\DlssNr_Spatial.h:16:
#include "../../../external/peripheral_warp/SpatialWarp.h"
```
除此之外，源码中对 external 的引用 **100% 走 IncludePath**，没有第二条旁路。

---

## 2. 逐库总表

体积按 `du -sb`（apparent bytes）换算 MiB；引用方式列同时给路径与源码证据。

| # | 子目录 | 体积 | 文件 | vcxproj 引用 | 源码 #include / 链接证据 | 判定 |
|---|---|---:|---:|---|---|---|
| 1 | FidelityFX-SDK | 60.62 MiB | 1565 | IncludePath 仅 `ffx-api\include\ffx_api`；分发 `docs\license.md`、`PrebuiltSignedDLL\amd_fidelityfx_vk.dll` | 5 个文件 `#include "ffx_api.h"`（`inputs\FfxApi_Dx12.h`、`FfxApi_Vk.h`、`FfxApiExe_Dx12.cpp`、`FG\FfxApi_Dx12_FG.h`、`proxies\FfxApi_Proxy.h`） | **仅需 ffx-api/include（0.10 MiB）+ license + PrebuiltSignedDLL**，其余 60.49 MiB 可删 |
| 2 | vulkan | 30.54 MiB | 79 | IncludePath `vulkan\include` | 31 文件：`vulkan/vulkan.h`(20)、`vulkan/vulkan.hpp`(9)、`vulkan_win32.h`(5)、`vulkan_core.h`(2) | **仅需 include（13.2 MiB 的 registry 可删）** |
| 3 | FidelityFX-SDK-v2 | 29.16 MiB | 743 | **无 IncludePath/LibraryPath**；仅 PostBuild 复制 `Kits\FidelityFX\signedbin\*.dll` + `docs\license.md` | **0 处 #include、0 处链接** | **构建完全不使用**；保留 signedbin + license.md，其余 29.10 MiB 可删 |
| 4 | freetype | 5.21 MiB | 56 | IncludePath `freetype` + LibraryPath `freetype` + `freetype.lib` 在 AdditionalDependencies | 3 文件：`imgui_freetype.cpp`、`imgui_draw.cpp`、`imconfig.h` | **保留**（硬依赖：头 + lib） |
| 5 | xess | 4.31 MiB | 393 | IncludePath `xess\inc\{xess,xell,xess_fg}`；LibraryPath `xess\lib`；分发 `xess\bin\*.dll` + LICENSE | 15 文件引用 xess/xell 头；源码出现 `libxess`/`libxess_dx11`/`libxess_fg`/`libxell` | **保留 inc/lib/bin/LICENSE**；doc(2.70)+samples(1.41) 可删 |
| 6 | nvapi | 3.22 MiB | 30 | IncludePath `nvapi`；ClInclude `..\external\nvapi\nvapi.h` | 17 文件 `#include <nvapi.h>`；2 文件 `NvApiDriverSettings.h`；`nvapi_lite_*` 被 nvapi.h 拉入 | **保留头文件**；amd64/x86 预编译 lib（1.36 MiB，未链接）+ Sample_Code 可删 |
| 7 | directx_agility_sdk | 3.21 MiB | 2 | 仅 PostBuild 复制 `lib\*.dll`(D3D12Core.dll 3.20 MiB) + LICENSE.txt | 无 #include（`d3d12.h` 来自 Windows SDK，非此目录） | **保留**（分发硬依赖：D3D12Core.dll） |
| 8 | spdlog | 1.15 MiB | 169 | IncludePath `spdlog\include` | 5 文件：`spdlog/spdlog.h`、`sinks/base_sink.h`、`sinks/basic_file_sink.h`、`async.h/async_logger.h`、`details/*` | **保留 include**；tests/bench/example/src/cmake(0.22) 可删 |
| 9 | magic_enum | 1.03 MiB | 54 | IncludePath `magic_enum\include\magic_enum` | 27 文件 `magic_enum.hpp` | **保留 include**；test/example/doc/cmake(0.87) 可删 |
| 10 | nlohmann | 0.91 MiB | 1 | IncludePath `nlohmann` | 3 文件 `#include <json.hpp>` | **保留**（单文件 json.hpp） |
| 11 | unordered_dense | 0.53 MiB | 119 | IncludePath `unordered_dense\include` | 13 文件 `#include <ankerl/unordered_dense.h>` | **保留 include**；test/scripts/example(0.44) 可删 |
| 12 | simpleini | 0.35 MiB | 43 | IncludePath `simpleini` | 1 文件 `Config.cpp` 用 `SimpleIni.h`（`ConvertUTF.*`/`SI_CONVERT*` 全项目 0 引用） | **保留 SimpleIni.h**；tests/Doxyfile/vcproj/other(0.18) 可删 |
| 13 | streamline | 0.16 MiB | 18 | IncludePath `streamline` | 4 文件：`sl_core_api.h`/`sl_reflex.h`/`sl_dlss_g.h` 等 | **保留**（纯头，整体即所需） |
| 14 | nvngx_dlss_sdk | 0.12 MiB | 7 | IncludePath `nvngx_dlss_sdk` | 29 文件 `nvsdk_ngx*.h` | **保留**（regs/*.reg 为工具用，2 KiB，可留） |
| 15 | streamline1 | 0.03 MiB | 3 | IncludePath `streamline1` | 4 文件 `sl1.h`/`sl1_consts.h`/`sl1_reflex.h` | **保留** |
| 16 | AntiLag2-SDK | 0.017 MiB | 2 | IncludePath `AntiLag2-SDK` | 3 文件 `ffx_antilag2_dx1{1,2}.h` | **保留** |
| 17 | latencyflex | 0.014 MiB | 1 | IncludePath `latencyflex` | 5 文件 `latencyflex.h` | **保留** |
| 18 | peripheral_warp | 0.007 MiB | 2 | **不在 IncludePath** | 1 文件相对路径 `#include "../../../external/peripheral_warp/SpatialWarp.h"` | **保留**（易被误删，特此标注） |
| 19 | RTX40MFG-Unlock | **0 bytes** | **0** | 无 | 无（`MfgUnlock` 逻辑在 `Config.cpp` 里，与本目录无关；该目录为空且**不在 git 中**） | **【可移除】整库（空目录）** |
| | **合计** | **140.58 MiB** | **3287** | | | |

---

## 3. 体积大户分解

### 3.1 FidelityFX-SDK 60.62 MiB（最大户，98.9% 可裁）

| 子项 | 体积 | 用途 | 编译可达？ |
|---|---:|---|---|
| `sdk/` | 18.99 MiB | FSR 技术源码 + 预编译库（含 ARM64/PIX/`arm64` 二进制） | 否 |
| `docs/` | 16.95 MiB | 文档 + 大量 SVG/PNG（单个 `upscaler-debug-overlay.svg` 6.93 MiB） | 否（仅 `docs/license.md` 1,142 B 需分发） |
| `framework/` | 14.62 MiB | Cauldron 示例框架 + ACS/AGS/ARM64 .lib | 否 |
| `samples/` | 9.74 MiB | 样例程序与媒体 | 否 |
| `ffx-api/` | 0.13 MiB | `include/` 102,935 B（**必需**）+ `src/` 137,983 B（未编译） | **include 是** |
| `PrebuiltSignedDLL/` | 5,486 B | 仅 2 个 .lib（`amd_fidelityfx_dx12.lib`/`_vk.lib`），**无 vcxproj 想要的 amd_fidelityfx_vk.dll** | 分发占位 |

- 工程实际链接的 FSR 库来自 **`OptiScaler\library\fsr2|fsr2_212|fsr31`**（`ffx_fsr2_api_x64.lib`、`ffx_fsr3_x64.lib`、`ffx_frameinterpolation_x64.lib`、`ffx_opticalflow_x64.lib` 等），**不是** external 里的 `sdk/`。
- 工程用到的 FSR 技术头文件来自 **`OptiScaler\include\fsr2|fsr3|fsr31`**（本地副本），仅 `ffx_api.h` 一族来自 `external\FidelityFX-SDK\ffx-api\include\ffx_api`。

### 3.2 vulkan 30.54 MiB

| 子项 | 体积 | 说明 |
|---|---:|---|
| `include/vulkan/*.h(hpp)` | 17.35 MiB | 头文件，**必需**（`vulkan\include` 在 IncludePath） |
| `registry/` | 13.16 MiB | `validusage.json` 10.02 MiB + `vk.xml` 2.68 MiB，注册表生成器数据，**编译不可达** |
| 根目录 cmake/README/LICENSE | 0.02 MiB | 无用 |
| `tests/` | 3.7 KiB | 无用 |
| 其中 `vulkan_raii.hpp` | 1.67 MiB | **无任何文件 include**（`vulkan.hpp` 只 include enums/to_string/handles/structs/funcs；全项目 0 处 `vulkan_raii`） |
| 其中 `vulkan_hash.hpp` + `vulkan_static_assertions.hpp` + `vulkan_video.hpp` + `vulkan_extension_inspection.hpp` + `vulkan_shared.hpp` + `include/vk_video/` | 2.05 MiB | 均无 include 者（全项目 0 引用） |

### 3.3 FidelityFX-SDK-v2 29.16 MiB

| 子项 | 体积 | 说明 |
|---|---:|---|
| `Kits/FidelityFX/`（除 signedbin） | 12.0 MiB | API/framegeneration/upscalers/backend 源码 + `docs/` 8.2 MiB |
| `Kits/OpenSource/` | 7.9 MiB | `amd/acs`、`amd/ags` 预编译库（含 ARM64） |
| `Kits/Cauldron2/` | 3.4 MiB | 示例框架 |
| `docs/` | 7.11 MiB | 文档（其中 4 处重复的 `upscaler-debug-overlay.svg` 各 6.93 MiB，v1/v2 各 2 份） |
| `Samples/` | 0.74 MiB | 样例 |
| `Kits/FidelityFX/signedbin/` | **2,880 B** | 仅 `amd_fidelityfx_loader_dx12.lib`（**无 DLL**）——按任务要求**保留** |
| `docs/license.md` | 61,784 B | 分发用——**保留** |

### 3.4 xess 4.31 MiB

| 子项 | 体积 | 说明 |
|---|---:|---|
| `inc/` | 0.10 MiB | **必需**（xess/xell/xess_fg 头） |
| `lib/` | 0.06 MiB | **必需**（libxess/libxess_dx11/libxess_fg/libxell，4 个 .lib 与源码引用一一对应） |
| `bin/` | 9,163 B | **保持不动**：PostBuild 复制 `bin\*.dll`；**当前目录内无任何 DLL，只有 4 个样例 HLSL shader** |
| `doc/` | 2.70 MiB | 可删 |
| `samples/` | 1.41 MiB | 可删（`bin/basic_sample_shaders` 亦属样例，仅 9 KiB，可留可删） |
| `LICENSE.txt` / `third-party-programs.txt` | 0.04 MiB | 保留（分发许可） |

### 3.5 nvapi 3.22 MiB

| 子项 | 体积 | 说明 |
|---|---:|---|
| 头文件（`nvapi.h` 1.33 MiB + `nvapi_lite_*`/`nvHLSLExtns*`/`NvApiDriverSettings.*`） | 1.78 MiB | **必需**（`#include <nvapi.h>`、`<NvApiDriverSettings.h>`） |
| `amd64/nvapi64.lib` | 0.94 MiB | **未链接**（LibraryPath 无 `external\nvapi`，AdditionalDependencies 无 `nvapi64.lib`）→ 可删 |
| `x86/nvapi.lib` | 0.41 MiB | 同上 → 可删 |
| `Sample_Code/` | 0.08 MiB | 样例 → 可删 |

---

## 4. 可回收空间估算（分级）

### A 级 —— 构建与分发均无引用，可安全删除（保守）

| # | 路径 | 回收 | 依据 |
|---|---|---:|---|
| 1 | `FidelityFX-SDK-v2\Kits\` 除 `FidelityFX\signedbin\` | 21.23 MiB | v2 无 IncludePath/LibraryPath；0 处 #include；signedbin 保留 |
| 2 | `FidelityFX-SDK\sdk\` | 18.99 MiB | 不在 IncludePath；链接库取自 OptiScaler\library\fsr* |
| 3 | `FidelityFX-SDK\docs\` 除 `license.md` | 16.95 MiB | 仅 license.md 被复制；其余为文档/媒体 |
| 4 | `FidelityFX-SDK\framework\` | 14.62 MiB | Cauldron 示例框架，不可达 |
| 5 | `vulkan\registry\` | 13.16 MiB | 注册表生成数据，编译不可达 |
| 6 | `FidelityFX-SDK\samples\` | 9.74 MiB | 样例，不可达 |
| 7 | `FidelityFX-SDK-v2\docs\` 除 `license.md` | 7.11 MiB | 仅 license.md 被复制 |
| 8 | `xess\doc\` | 2.70 MiB | 文档，不可达 |
| 9 | `xess\samples\` | 1.41 MiB | 样例，不可达 |
| 10 | `nvapi\amd64\` + `nvapi\x86\` | 1.36 MiB | 未出现在 LibraryPath / AdditionalDependencies |
| 11 | `FidelityFX-SDK-v2\Samples\` | 0.74 MiB | 样例，不可达 |
| 12 | `magic_enum\test\` + `doc\` + `example\` + `cmake\` | 0.86 MiB | 仅需 `include\magic_enum` |
| 13 | `unordered_dense\test\` + `scripts\` + `example\` + 根 cmake | 0.44 MiB | 仅需 `include\ankerl` |
| 14 | `spdlog\tests\` + `bench\` + `example\` + `src\` + `cmake\` | 0.22 MiB | 纯 header-only 使用（无 `SPDLOG_COMPILED_LIB`），仅需 `include\spdlog` |
| 15 | `simpleini\tests\` + `Doxyfile` + `vcproj\` + `other\` + README | 0.18 MiB | 仅需 `SimpleIni.h`（`ConvertUTF.*` 0 引用，可一并删 25 KiB） |
| 16 | `FidelityFX-SDK\ffx-api\src\` | 0.13 MiB | 未编译（无 ClCompile 引用），仅 include 需要 |
| 17 | `nvapi\Sample_Code\` | 0.08 MiB | 样例 |
| 18 | `vulkan\tests\` + 根 cmake/README | 0.03 MiB | 不可达 |
| 19 | `RTX40MFG-Unlock\`（空目录） | **0 B** | 0 文件，不在 git，无任何引用 |
| | **A 级合计** | **≈ 110.0 MiB（78.3%）** | 剩余 ≈ 30.5 MiB |

### B 级 —— 需确认后再删（Vulkan-Hpp 未引用头，3.72 MiB）

| 路径 | 回收 | 说明 |
|---|---:|---|
| `vulkan\include\vulkan\vulkan_raii.hpp` | 1.67 MiB | `vulkan.hpp` 不 include 它，全项目 0 引用；但属官方头集合，若将来用 `vulkan_raii` 需恢复 |
| `vulkan_hash.hpp`、`vulkan_static_assertions.hpp`、`vulkan_video.hpp`、`vulkan_extension_inspection.hpp`、`vulkan_shared.hpp`、`include\vk_video\` | 2.05 MiB | 同上，当前 0 引用（`grep -c vulkan_hash` 在 handles/structs/funcs/enums 中均为 0） |

> A + B ≈ **113.8 MiB（80.9%）**，剩余 ≈ 26.8 MiB。
> 注意：删除仅缩小工作树；`external` 已被 git 跟踪（3283 文件），`.git` 目录当前 **81 MiB** 历史仍保留旧 blob，除非另行改写历史。

---

## 5. 安全移除 Top 10（按回收量排序，逐项验证依据）

| 排名 | 目标 | 回收 | 验证依据（可复核） |
|---|---|---:|---|
| 1 | `FidelityFX-SDK-v2\Kits\`（保留 `FidelityFX\signedbin\`） | 21.23 MiB | ① vcxproj 全文 grep `FidelityFX-SDK-v2` 仅 4 处，全部是 PostBuild `copy`（行 264–267），无 IncludePath/LibraryPath；② `grep -rn "FidelityFX-SDK-v2" OptiScaler` 在源码 0 命中；③ 源码 0 处 include v2 API 头（v1 的 `ffx_api.h` 才是被用的那套）；④ 干净构建 `/p:PostBuildEventUseInBuild=false`，连 copy 都不执行 |
| 2 | `FidelityFX-SDK\sdk\` | 18.99 MiB | IncludePath 只暴露 `ffx-api\include\ffx_api`，`sdk\` 在编译期不可达；链接的 `ffx_*.lib` 来自 `OptiScaler\library\fsr2|fsr2_212|fsr31` |
| 3 | `FidelityFX-SDK\docs\`（保留 `license.md`） | 16.95 MiB | 仅 `license.md`（1,142 B）被 copy 到 `Licenses\FidelityFX_v1_LICENSE.md`；其余为文档/6.93 MiB SVG 等，不可达 |
| 4 | `FidelityFX-SDK\framework\` | 14.62 MiB | Cauldron + ACS/AGS/PIX 预编译库，仅示例程序使用；不在任何 IncludePath/LibraryPath |
| 5 | `vulkan\registry\` | 13.16 MiB | `validusage.json`/`vk.xml` 为代码生成输入；编译只需 `vulkan\include`；`.json/.xml` 不参与编译 |
| 6 | `FidelityFX-SDK\samples\` | 9.74 MiB | 样例程序与媒体，不可达 |
| 7 | `FidelityFX-SDK-v2\docs\`（保留 `license.md`） | 7.11 MiB | 仅 `license.md`（61,784 B）被 copy 为 `FidelityFX_v2_LICENSE.md` |
| 8 | `xess\doc\` | 2.70 MiB | 仅 `inc\`、`lib\`、`bin\`、`LICENSE.txt`、`third-party-programs.txt` 被使用 |
| 9 | `xess\samples\` | 1.41 MiB | 同上；样例工程不参与构建 |
| 10 | `nvapi\amd64\` + `nvapi\x86\` | 1.36 MiB | LibraryPath 无 `external\nvapi`；AdditionalDependencies 无 `nvapi64.lib`/`nvapi.lib`；运行时 fakenvapi 走 `LoadLibrary` 而非静态链接 |
| — | 附：`RTX40MFG-Unlock\`（空目录） | 0 B | `find external/RTX40MFG-Unlock -type f` = 0；`git ls-files` 中不存在（git 不跟踪空目录）；`MfgUnlock` 功能实现在 `Config.cpp` |

---

## 6. 严禁删除（硬依赖 / 分发必需）

| 路径 | 原因 |
|---|---|
| `xess\lib\libxess.lib`、`libxess_dx11.lib`、`libxess_fg.lib`、`libxell.lib` | LibraryPath + 链接硬依赖（源码引用名与 4 个 .lib 完全对应） |
| `xess\inc\{xess,xell,xess_fg}\` | IncludePath；15 个源文件引用 |
| `xess\bin\` | PostBuild `copy xess\bin\*.dll` 的目标目录（当前为空，**不要在此放无关文件，也不要删目录**） |
| `xess\LICENSE.txt`、`xess\third-party-programs.txt` | 分发许可 |
| `FidelityFX-SDK-v2\Kits\FidelityFX\signedbin\` | PostBuild 复制 FFX v2 签名 DLL 的源目录 |
| `FidelityFX-SDK\PrebuiltSignedDLL\` | PostBuild 复制 `amd_fidelityfx_vk.dll` 的源目录 |
| `FidelityFX-SDK\ffx-api\include\ffx_api\` | IncludePath；`ffx_api.h` 被 5 个文件引用 |
| `FidelityFX-SDK\docs\license.md`、`FidelityFX-SDK-v2\docs\license.md` | 分发许可（1,142 B / 61,784 B） |
| `directx_agility_sdk\lib\D3D12Core.dll` + `LICENSE.txt` | 分发硬依赖（D3D12 Agility 运行时） |
| `freetype\freetype.lib`、`freetype\ft2build.h`、`freetype\freetype\` | 链接 + 头 |
| `peripheral_warp\` | 通过相对路径 `../../../external/peripheral_warp/SpatialWarp.h` 引用，**IncludePath 里看不到，极易误删** |
| 其余被 IncludePath 直接点名的目录 | 见 §1 路径 1 |

---

## 7. 审计发现的问题（与本任务相关的旁证，仅报告，未修改）

1. **分发 DLL 全部缺失（重要）**
   `external` 全库只有 **1 个 DLL**：`directx_agility_sdk\lib\D3D12Core.dll`。而 vcxproj 行 261–266 指望复制的
   `xess\bin\*.dll`（libxess.dll 等）、`FidelityFX-SDK\PrebuiltSignedDLL\amd_fidelityfx_vk.dll`、
   `FidelityFX-SDK-v2\Kits\FidelityFX\signedbin\amd_fidelityfx_{loader,upscaler,framegeneration}_dx12.dll`
   **在树内均不存在**（`signedbin` 只有 1 个 .lib，`PrebuiltSignedDLL` 只有 2 个 .lib）。
   → 当前「构建成功」不受影响，因为 `build.ps1` 传了 `PostBuildEventUseInBuild=false`；但一旦执行分发打包，这些 copy 会为空操作或报错。
   **【需确认】**：XeSS/FFX v2 运行时 DLL 是否本应由其它流程注入，或需从官方包补入。
2. **死路径 IncludePath**：`$(SolutionDir)external\fakenvapi` 指向不存在的目录（`external\fakenvapi` 不存在；真实实现位于 `OptiScaler\nvapi\fakenvapi\`，通过 `#include "nvapi/fakenvapi.h"` 由 `$(ProjectDir)` 解析）。属于无害冗余，**未改动**。
3. **重复内容**：`upscaler-debug-overlay.svg` 6.93 MiB 在 v1/v2 各出现 2 次（合计 27.7 MiB），随 §4 的 docs 清理一并消失。
4. **无 .git / 无压缩包**：`find external -name .git` 与 `-iname '*.zip|*.7z|*.tar*|*.rar|*.gz'` 均为 0 命中 → 不存在嵌套仓库或压缩包混入。
5. **多平台垃圾集中在已判死目录**：ARM64 库（`amd_acs_ARM64*`、`amd_ags_ARM64.lib`）、`arm64\WinPixEventRuntime.lib`、非 Windows 头（`vulkan_metal.h` 等）全部位于 `FidelityFX-SDK/{sdk,framework,samples}`、`FidelityFX-SDK-v2/Kits/OpenSource`，随 A 级清理一并移除，无需单独处理。
6. `external` 已被 git 跟踪（3283 文件）；删除后**请用 `git rm -r --cached` + 提交**，否则仅工作树变小、`.git`（81 MiB）历史不变。

---

## 8. 建议的验证流程（删除后）

1. 先做**可逆快照**：`git status` 确认工作树干净；对要删的目录打 tag 或 `git stash`/分支提交前状态可回退。
2. 分批删除（建议顺序：v2/Kits → v2/docs → FidelityFX sdk/framework/samples/docs → vulkan/registry → xess/doc+samples → nvapi libs/样例 → 小库 test/doc），每批后执行：
   ```
   pwsh -NoProfile -File .\build.ps1 -Clean
   ```
   验收：`exit 0` 且 `build\out\OptiScaler.dll` **size 仍为 34,795,008 bytes**（尺寸不变 = 编译/链接输入未变）。
3. 若尺寸变化：立即回退该批，并检查是否误删 IncludePath 命中的目录（尤其 `peripheral_warp`、`ffx-api/include`）。
4. 分发路径单独验证（需先补齐缺失 DLL）：`MSBuild /p:PostBuildEventUseInBuild=true`，确认 `x64\Release\a\OptiScaler\` 下 DLL 与 Licenses 齐全。
5. 删除后回收确认：`du -sb external | awk '{printf "%.2f MiB\n",$1/1048576}'` → 预期 **≈30.5 MiB**（A 级）。

---

## 9. 复核命令（本次审计实际使用，只读）

```bash
# 体积/文件数
cd external && for d in */; do printf "%-22s %9d %5d\n" "${d%/}" "$(du -sb "$d"|cut -f1)" "$(find "$d" -type f|wc -l)"; done | sort -k2 -rn

# 路径 1：vcxproj 里的 external 引用
grep -o 'external[^;<"]\{0,40\}' OptiScaler/OptiScaler.vcxproj | sort | uniq -c | sort -rn

# 路径 2：源码 #include 证据（示例）
grep -rl 'SimpleIni\.h' OptiScaler            # simpleini
grep -rl 'spdlog/' OptiScaler                 # spdlog
grep -rl 'json\.hpp' OptiScaler               # nlohmann
grep -rl 'magic_enum\.hpp' OptiScaler         # magic_enum
grep -rl 'ankerl/unordered_dense\.h' OptiScaler
grep -rl 'vulkan/vulkan' OptiScaler           # vulkan
grep -rl '<nvapi\.h>' OptiScaler              # nvapi
grep -rl 'nvsdk_ngx' OptiScaler               # nvngx_dlss_sdk
grep -rl 'ffx_api\.h' OptiScaler              # FidelityFX-SDK (ffx-api)
grep -rl 'ffx_antilag2' OptiScaler            # AntiLag2-SDK
grep -rl 'latencyflex\.h' OptiScaler          # latencyflex
grep -rl 'SpatialWarp' OptiScaler             # peripheral_warp (相对路径)
grep -rlE 'sl_[a-z_]+\.h' OptiScaler          # streamline
grep -rlE 'sl1\.h|sl1_' OptiScaler            # streamline1
grep -rlE '(xess|xell)[^>"]*\.h' OptiScaler   # xess

# 旁路引用（相对路径）
grep -rnE '#include[[:space:]]*"[^"]*external[^"]*"' OptiScaler

# 多平台/二进制/压缩包排查
find external -type f \( -iname '*.so' -o -iname '*.dylib' -o -iname '*.a' -o -iname '*.dll' -o -iname '*.lib' -o -iname '*.exe' \) -printf '%10s %p\n' | sort -rn
find external -type f \( -iname '*.zip' -o -iname '*.7z' -o -iname '*.tar*' \) ; find external -name .git
```

---

*本报告仅做只读分析，未修改 `external\`、`OptiScaler\`、`build.ps1` 任何内容。删除动作需由人工按 §8 流程分批执行并逐批构建验证。*
