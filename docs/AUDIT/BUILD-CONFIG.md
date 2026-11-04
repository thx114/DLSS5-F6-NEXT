# 工程配置体检 — BUILD-CONFIG

**对象**：`OptiScaler/OptiScaler.vcxproj`（1406 行，95 KB）+ 唯一构建入口 `build.ps1`（102 行）
**根**：`D:\CODE\DLSS5_NEXT`（新根，干净迁移）
**旧根**：`D:\DATA\Documents\ChatGPT\DLSS F5 NEXT`（保留中，未删除；真正的完整源码树在其 `work\full-source-f5-v11-candidate-20261018\`）
**性质**：只读审计。本报告是唯一产出，未修改 `OptiScaler\`、`external\`、`build.ps1`、`README.md`。

**实测基线**：`build\out\OptiScaler.dll` = 34,795,008 bytes（Release x64，exit 0）；同目录另有 `OptiScaler.exp` 147,796 B、`OptiScaler.lib` 248,056 B。

## 结论摘要（关键数字）

| 指标 | 数值 |
|---|---|
| IncludePath / LibraryPath 死路径（目标目录不存在） | **1 条唯一路径**（`external\fakenvapi`），在 3 个 x64 配置各出现 1 次 = **3 处引用**；LibraryPath 死路径 **0** |
| ClCompile/ClInclude/None/ResourceCompile 坏条目（磁盘不存在） | **0 / 764** |
| PostBuildEvent 需要的外部输入 | **21 条**（另 1 条 `copy NUL` 无源文件）；**缺 16 条**、可用 5 条 |
| 其中"旧根也找不到"的输入 | **6 条**（`READ ME - DLSS Neural Rendering.txt` + xess/FidelityFX 共 5 个 DLL 通配） |
| 可从旧 worktree 直接取回的输入 | **10 条 / 27 个文件**（6 个许可证 + 20 个 LUT + ini + 2 个 setup 脚本） |
| 完全空转/不可达的构建配置 | **3 个**（Debug|Win32、Release|Win32、ReleaseDebug|Win32）+ ReleaseDebug|x64 不被 `build.ps1` 使用 |
| 逐字节重复的配置行 | 3 组各 3 行完全相同（IncludePath/LibraryPath/Win32 IncludePath）＝可合并掉 2 份 |
| 真实产物缺件 | **1 个**：`OptiScaler\D3D12_OptiScaler\D3D12Core.dll`（源在 `external\directx_agility_sdk\lib\D3D12Core.dll`，确实存在） |

一句话：**工程"能编译"是干净的（0 坏条目、0 死库路径），脏的是"发行侧"** —— PostBuild 那套 29 条命令全是为旧根 `x64\Release\a\` 发行目录服务的，而新根既没有那些素材，真实流水线（旧 `BUILD-DEPLOY.ps1` 与新 `build.ps1`）也都用 `/p:PostBuildEventUseInBuild=false` 关掉了它。

---

## 1. IncludePath / LibraryPath

### 1.1 逐条清单（以 `Release|x64`（第 121/122 行）为准，`Debug|x64`、`ReleaseDebug|x64` 三行逐字节相同）

`$(SolutionDir)` = `D:\CODE\DLSS5_NEXT\`（由 `build.ps1` 传入，保留尾分隔符）；`$(ProjectDir)` = `OptiScaler\`。

| # | 条目（原文） | 展开后存在？ | 判定 |
|---|---|---|---|
| 1 | `$(ProjectDir)` | ✅ | 【保留】工程头文件根 |
| 2 | `$(ProjectDir)include\` | ✅ | 【保留】第三方头（detours/imgui/spdlog sink） |
| 3 | `$(SolutionDir)external\vulkan\include` | ✅ | 【保留】 |
| 4 | `$(SolutionDir)external\nvngx_dlss_sdk` | ✅ | 【保留】 |
| 5 | `$(SolutionDir)external\xess\inc\xess` | ✅ | 【保留】 |
| 6 | `$(SolutionDir)external\xess\inc\xell` | ✅ | 【保留】 |
| 7 | `$(SolutionDir)external\xess\inc\xess_fg` | ✅ | 【保留】 |
| 8 | `$(SolutionDir)external\FidelityFX-SDK\ffx-api\include\ffx_api` | ✅ | 【保留】 |
| 9 | `$(SolutionDir)external\simpleini` | ✅ | 【保留】 |
| 10 | `$(SolutionDir)external\unordered_dense\include` | ✅ | 【保留】 |
| 11 | `$(SolutionDir)external\spdlog\include` | ✅ | 【保留】 |
| 12 | `$(SolutionDir)external\freetype` | ✅ | 【保留】 |
| 13 | `$(SolutionDir)external\streamline` | ✅ | 【保留】代码 `#include <sl.h>`、`<sl_reflex.h>` 需要 |
| 14 | `$(SolutionDir)external\streamline1` | ✅ | 【保留】代码 `#include <sl1_reflex.h>` 需要——**不要**当成 streamline 的重复项删掉 |
| 15 | `$(SolutionDir)external\nvapi` | ✅ | 【保留】 |
| 16 | `$(SolutionDir)external\nlohmann` | ✅ | 【保留】 |
| 17 | **`$(SolutionDir)external\fakenvapi`** | ❌ **不存在** | **【可直接修】删除**（见 1.2） |
| 18 | `$(SolutionDir)external\magic_enum\include\magic_enum` | ✅ | 【保留】 |
| 19 | `$(SolutionDir)external\AntiLag2-SDK` | ✅ | 【保留】 |
| 20 | `$(SolutionDir)external\latencyflex` | ✅ | 【保留】 |

`LibraryPath`（第 122 行，8 条全部存在，0 死路径）：
`$(ProjectDir)library\fsr2`、`library\fsr2_212`、`library\fsr31`、`library\vulkan`、`library\d3dx`、`library\detours`、`$(SolutionDir)external\xess\lib`、`$(SolutionDir)external\freetype` —— 全部 ✅ 【保留】。

补充核验：`Release|x64` 的 22 个 `AdditionalDependencies`（第 242 行）中，凡是应由工程目录提供的都**真实存在**——`vulkan-1.lib`（library\vulkan）、`freetype.lib`、`lib ffx_fsr2_api_*`×4（library\fsr2）、`ffx_fsr2_212_*`×3（library\fsr2_212）、`ffx_fsr3/upscaler/opticalflow/frameinterpolation/backend_dx11`×5（library\fsr31）。其余 7 个（`winhttp/dxgi/d3d11/d3d12/dxguid/d3dcompiler/WindowsApp/Dbghelp`）来自 Windows SDK，不在 `LibraryPath` 里找，属正常。

### 1.2 死路径详情：`external\fakenvapi`

- 出现位置：第 **115、121、125** 行（Debug|x64 / Release|x64 / ReleaseDebug|x64 的 IncludePath），共 3 处。
- 新根 `external\` 只有 19 个目录（`AntiLag2-SDK, FidelityFX-SDK, FidelityFX-SDK-v2, RTX40MFG-Unlock, directx_agility_sdk, freetype, latencyflex, magic_enum, nlohmann, nvapi, nvngx_dlss_sdk, peripheral_warp, simpleini, spdlog, streamline, streamline1, unordered_dense, vulkan, xess`），**没有 fakenvapi**。
- 该头文件已被内联进产品源码：`OptiScaler\nvapi\fakenvapi.h`、`nvapi\fakenvapi\{log,low_latency,nvapi_calls}.h` 都在 vcxproj 的 ClInclude 里且磁盘存在，代码用 `#include "nvapi/fakenvapi.h"` 这类相对路径，不需要外部 include 目录。
- **判定：【可直接修】**——删掉这 3 处 `$(SolutionDir)external\fakenvapi;`。当前它只贡献"编译器每 TU 多探一个不存在的目录"的开销，删掉零风险（本次构建成功即证明无依赖）。

### 1.3 写法不一致（不致命，但值得顺手规范化）

同一个"external 下某目录"在文件里有 **3 种拼法**：

| 拼法 | 例子 | 出现处 |
|---|---|---|
| 单反斜杠 | `$(SolutionDir)external\simpleini`（第 115/121/125 行） | x64 全部（13 条） |
| 前导反斜杠 | `$(SolutionDir)\external\nvngx_dlss_sdk` | Win32 全部（第 103/107/111 行） |
| 双反斜杠 | `$(SolutionDir)external\\simpleini` | 仅第 103/107/111 行的 simpleini |

`SolutionDir` 已带尾分隔符，前导 `\` 会生成 `...NEXT\\external\...`。Windows/MSBuild 会折叠，本次构建无告警，故**不是错误**；但它是迁移遗留的噪音。【可直接修（低优先）】统一为 `$(SolutionDir)external\X`。

### 1.4 可合并项（结构性重复）

| 组 | 行号 | 事实 | 判定 |
|---|---|---|---|
| x64 IncludePath ×3 | 115 / 121 / 125 | md5 完全相同 | 【可直接修】提成一个无条件 `<PropertyGroup>`（或 `Directory.Build.props`），从 60 条降到 20 条 |
| x64 LibraryPath ×3 | 116 / 122 / 126 | md5 完全相同 | 同上，24 条 → 8 条 |
| Win32 IncludePath ×3 | 103 / 107 / 111 | md5 完全相同 | 【需确认】Win32 配置本身已不可达（见 3.3），建议整组删除而不是合并 |
| Win32 LibraryPath ×3 | 104 / 108 / 112 | 完全相同 | 同上 |

### 1.5 一个真实的可移植性缺陷：工程没有 `SolutionDir` 兜底

`vcxproj` 里 41 处 `$(SolutionDir)` 全部是**使用方**，`<PropertyGroup Label="Globals">`（第 29-35 行）里**没有**默认值。新根也没有 `.sln`（`find . -maxdepth 2 -name '*.sln'` 为空），所以只有 `build.ps1` 能构建它；直接在 VS 里打开 vcxproj 或 `msbuild OptiScaler.vcxproj` 都会让 IncludePath 变成 `\external\simpleini` 之类的根路径而失败。

- **【可直接修】**：在 Globals 组加一行兜底，`/p:SolutionDir` 传入时仍优先——
  ```xml
  <SolutionDir Condition="'$(SolutionDir)'==''">$(MSBuildThisFileDirectory)..\</SolutionDir>
  ```
  收益：工程不再"只能被 build.ps1 驱动"，IDE 内也能直接打开。

---

## 2. PostBuildEvent（第 247-277 行）

### 2.1 它到底做了什么

`Release|x64` 的 PostBuildEvent 是 29 条 cmd 命令：**5 条 `md`**（建目录）、**1 条 `del`**、**1 条 `move`**、**22 条 `copy`**。目标目录是旧根的 `$(SolutionDir)x64\Release\a\`（旧式发行目录），源文件全部是**解决方案根目录下**的素材，而不是构建产物：

```
md   x64\Release\a\                      # 1
move x64\Release\$(TargetFileName) → a\  # 2  把 DLL 从输出目录搬进发行目录
md   a\Licenses ; del a\Licenses\*.*     # 3-4
md   a\OptiScaler ; md a\OptiScaler\LUTs ; md a\OptiScaler\D3D12_OptiScaler\   # 其余 md
copy ... 21 个源
copy NUL a\!! EXTRACT ALL FILES TO GAME FOLDER !!   # 空标记文件，不依赖任何源
```

### 2.2 21 条 copy 源逐个核验（新根 vs 旧根）

| # | 行 | 源（展开） | 新根 | 旧根 | 判定 |
|---|---|---|---|---|---|
| 1 | 252 | `external\xess\LICENSE.txt` | ✅ | ✅ | 【保留】 |
| 2 | **253** | `READ ME - DLSS Neural Rendering.txt` | ❌ | ❌ **全盘不存在**（`find` 旧根无 `READ ME*`） | **【可直接修】删掉這一整行**——素材已永久丢失，且 `README.md` 已取代它 |
| 3 | 254 | `Licenses\RenoDX_ATTRIBUTION.txt` | ❌ | ✅（worktree） | 【需确认】取回还是弃用 |
| 4 | 255 | `Licenses\MFGUnlock_LICENSE.txt` | ❌ | ✅ | 同上 |
| 5 | 256 | `Licenses\ReShade_GenericDepth_LICENSE.txt` | ❌ | ✅ | 同上 |
| 6 | 257 | `Licenses\DLSS5Feeder_LICENSE.txt` | ❌ | ✅ | 同上 |
| 7 | 258 | `Licenses\Inter_OFL.txt` | ❌ | ✅ | 同上 |
| 8 | 259 | `Licenses\PeripheralWarp_LICENSE.txt` | ❌ | ✅ | 同上 |
| 9 | **261** | `external\xess\bin\*.dll` | ⚠️ 目录存在但**0 个 DLL**（只有 `basic_sample_shaders\`） | ⚠️ 旧根同样 0 个 | **【可直接修】删掉**——源从来不存在，此命令一直只会让 PostBuild 报错 |
| 10 | 262 | `external\FidelityFX-SDK\docs\license.md` | ✅ | ✅ | 【保留】 |
| 11 | **263** | `external\FidelityFX-SDK\PrebuiltSignedDLL\amd_fidelityfx_vk.dll` | ❌ 该目录仅有 `amd_fidelityfx_dx12.lib`、`amd_fidelityfx_vk.lib` | ❌ 同样只有 `.lib` | **【可直接修】删掉**（源只有 .lib，从来没有 .dll） |
| 12 | **264** | `external\FidelityFX-SDK-v2\Kits\FidelityFX\signedbin\amd_fidelityfx_loader_dx12.dll` | ❌ 目录仅有 `amd_fidelityfx_loader_dx12.lib` | ❌ 同 | **【可直接修】删掉** |
| 13 | **265** | 同上 `amd_fidelityfx_upscaler_dx12.dll` | ❌ | ❌ | **【可直接修】删掉** |
| 14 | **266** | 同上 `amd_fidelityfx_framegeneration_dx12.dll` | ❌ | ❌ | **【可直接修】删掉** |
| 15 | 267 | `external\FidelityFX-SDK-v2\docs\license.md` | ✅ | ✅ | 【保留】 |
| 16 | **269** | `LUTs\*.*`（20 个 `.cube`） | ❌ 新根 0 个 `.cube` | ✅ 20 个（worktree `LUTs\`） | 【需确认】发行素材，见 2.4 |
| 17 | 270 | `OptiScaler.ini` | ❌ | ✅（worktree） | 【需确认】运行时自动生成，见 2.4 |
| 18 | 271 | `setup_windows.bat` | ❌ | ✅（worktree） | 【需确认】 |
| 19 | 272 | `setup_linux.sh` | ❌ | ✅（worktree） | 【需确认】 |
| 20 | 274 | `external\directx_agility_sdk\lib\*.dll` | ✅（`D3D12Core.dll`，1 个） | ✅ | **【可直接修】把它从"发行目录"改到"构建输出"**（见 2.5，这是唯一真正的缺件） |
| 21 | 275 | `external\directx_agility_sdk\LICENSE.txt` | ✅ | ✅ | 【保留】 |

**统计：21 条输入 → 可用 5 条、缺失 16 条。** 16 条里：6 条（`READ ME` + 5 个 DLL 通配/具体 DLL）在旧根也不存在 → 这套 PostBuild 在旧根**本来就是坏的**；10 条（6 许可证 + LUTs + ini + 2 个 setup）在旧 worktree 里齐备 → 属于"没迁移过来"，而非"丢失"。

### 2.3 现在跳过它（`/p:PostBuildEventUseInBuild=false`）意味着什么

- 这是**有意为之，不是事故**：旧根的 `BUILD-DEPLOY.ps1` 第 151 行同样是 `/p:PostBuildEventUseInBuild=false`，新 `build.ps1` 第 74 行原样继承。
- 直接后果：`build\out\` 只有 `OptiScaler.dll / .exp / .lib`，不再产生 `x64\Release\a\` 那套发行目录。
- 旧流水线的真实部署目标只有一个文件：`BUILD-DEPLOY.ps1` 把 `out/OptiScaler.dll` 覆盖到 `...\HoYoShadeHub_Portable_1.4.5.0_x64_Full\OptiScaler\official\F6\OptiScaler.dll`。**分发 Licenses/LUTs/ini 从来不是它的事**。
- 只有上游的 `package_release.ps1`（未迁移，留在旧 worktree）依赖 PostBuild 产物：它写死 `$src = "$root\x64\Release\a"`，再从那里取 `OptiScaler.dll`、`Licenses\`、`OptiScaler\` 两个目录。也就是说：**要恢复"完整发行包"，必须先恢复 PostBuild 或改写 package_release.ps1 的取件路径** —— 【需确认】，这是产品决策，不是配置 bug。

### 2.4 逐个回答"是否需要随产物分发"

| 内容 | 运行时是否需要 | 证据 | 判定 |
|---|---|---|---|
| **`D3D12Core.dll`** | **需要（有条件）** | `dllmain.cpp:948` 在 `FsrAgilitySDKUpgrade` 开启时从 `optiDllPath\D3D12_OptiScaler\D3D12Core.dll` 加载；取不到再退到游戏自带 `D3D12\` 目录和裸 `LoadLibrary` | **【可直接修】** 唯一真缺件，见 2.5 |
| `libxess*.dll` / `libxell.dll` | **不随我们分发** | `Config.cpp:2703` 只探测**游戏目录**下 `libxess.dll`，`DllNames.h:94-99` 同样按游戏目录解析；代码对缺失只 `LOG_WARN` 降级 | 【保留】现状合理，删掉第 261 行即可 |
| `amd_fidelityfx_*.dll` | **不随我们分发** | 同上，`DllNames.h:108-111` 按游戏目录/`OptiDllPath` 解析；`dllmain.cpp:1100-1141` 全是"已在内存则 hook"的探测逻辑 | 【保留】删掉 263-266 行 |
| `LUTs\*.cube` | **可选增强** | `dlssnr\DlssNr_Menu.cpp:134-147`：`LutFolder()` 不存在时直接返回空列表，不崩、不报错，UI 里 LUT 下拉为空 | 【需确认】要发就补素材，不发则功能静默缺席 |
| `OptiScaler.ini` | **不需要预置** | `Config.cpp:117` 路径 = `OptiScaler.dll` 同目录 `OptiScaler.ini`；`Config.cpp:2628-2629` 保存时 `ini.SaveFile()` 自动落盘 | 【保留】预置只是给用户一份调好的默认值（UX 选择） |
| `setup_windows.bat` / `setup_linux.sh` | 部署辅助脚本 | 上游 `package_release.ps1` 把它们列进 `$sourceFiles` 必发清单 | 【需确认】属打包范畴 |
| `Licenses\*` / `READ ME` | 合规/说明文本 | 上游清单同样要求 | 【需确认】`READ ME` 已彻底丢失 |

### 2.5 【可直接修】唯一真缺件的修法（改 build.ps1，不在本次写入范围）

构建成功后补一步，代价 ~1 行，且不依赖任何"旧根素材"：

```powershell
# after the artifact check in build.ps1
$agility = Join-Path $root 'external\directx_agility_sdk\lib\D3D12Core.dll'
if (Test-Path -LiteralPath $agility) {
    $dst = Join-Path $outDir 'OptiScaler\D3D12_OptiScaler'
    $null = [IO.Directory]::CreateDirectory($dst)
    Copy-Item -LiteralPath $agility -Destination $dst -Force
}
```

同时把 vcxproj 的 PostBuildEvent 整段改成注释或删掉（29 条命令里 16 条不可满足，留着只会让"哪天有人打开 PostBuild"变成必然失败）。

### 2.6 顺带发现：`ReleaseDebug|x64` 的 PostBuild 依赖外部 7z.exe

第 308 行：`& '7z.exe' a -t7z ...`。`7z.exe` 既不在仓库也不在 PATH（未在任何构建脚本里配置），该配置一旦启用 PostBuild 必然失败。当前被同一开关关掉。【需确认】是否需要保留 `ReleaseDebug|x64`。

---

## 3. 工程条目（ClCompile / ClInclude / None / ResourceCompile）

### 3.1 坏条目数：**0 / 764**

| 类型 | 条目数 | 磁盘不存在 |
|---|---|---|
| `ClCompile` | 303 | **0** |
| `ClInclude` | 457 | **0** |
| `None` | 3（`Magnifier_Shader_Vk.spv`、`Source.def`、`integration\hoyoshade\sdk\LICENSE.md`） | **0** |
| `ResourceCompile` | 1（`OptiScaler.rc`） | **0** |
| 合计 | **764** | **0** |

判定：**【保留】**。迁移脚本 `tools\migrate-copy.ps1` 只拷了 `OptiScaler\` 与 `external\`，恰好覆盖了工程引用的全部相对路径，所以条目层是干净的——这也是本次"能一次编过"的结构性原因。

### 3.2 跨目录条目

`ClInclude Include="..\external\nvapi\nvapi.h"`（第 621 行）是唯一引用 `external\` 的条目，文件存在。【保留】（也可改成 `$(SolutionDir)external\...` 风格，纯风格问题）。

### 3.3 不可达的构建配置（真实的技术债）

`ProjectConfigurations` 声明了 6 个配置，但：

| 配置 | 谁在用 | 状况 | 判定 |
|---|---|---|---|
| `Release\|x64` | `build.ps1`（唯一入口） | 主力 | 【保留】 |
| `Debug\|x64` | 无 | 有 `IntDir=.\x64\Debug\`（会把中间件写回源码目录）、空的 `StackReserveSize/StackCommitSize`、空 `PreBuildEvent`、多余的 `<TargetExt>.dll</TargetExt>` | 【需确认】保留但清理噪音 |
| `ReleaseDebug\|x64` | 无 | PostBuild 依赖不存在的 `7z.exe` | 【需确认】 |
| `Debug\|Win32`、`Release\|Win32`、`ReleaseDebug\|Win32` | **无（新根连 .sln 都没有）** | 三组 IncludePath/LibraryPath 是 x64 的**过期子集**（只有 5 条 include，缺 FidelityFX/streamline/nvapi/spdlog/freetype…），本就是坏的 | **【可直接修】整组删除**，共 3 组配置 + 3 组重复路径 + 3 个 ItemDefinitionGroup |

Win32 三组删掉的收益：约 120 行配置、9 组重复路径；风险：零（`build.ps1` 只传 `Configuration=Release;Platform=x64`）。

---

## 4. PreprocessorDefinitions

全文件只有 6 处定义 + 1 处 ResourceCompile，逐条如下：

| 位置 | 定义 | 分析 | 判定 |
|---|---|---|---|
| 第 191 行 `Debug\|x64` | `_DEBUG;_DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR;_WINDOWS;_USRDLL;` | 无重复 | 【保留】 |
| 第 226 行 `Release\|x64` | `NDEBUG;_DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR;_WINDOWS;_USRDLL;` | 无重复；`_DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR` 是 MSVC STL 已知的 `std::mutex` constexpr 构造规避宏，**有效，别删** | 【保留】 |
| 第 286 行 `ReleaseDebug\|x64` | `NODEBUG;_DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR;...` | **`NODEBUG` 是 `NDEBUG` 的错拼**。全仓搜索 `\bNODEBUG\b` → **0 个文件使用**，纯无效宏 | **【可直接修】改成 `NDEBUG`**（或删掉） |
| 第 132/151/172 行 `*\|Win32` | `WIN32;_DEBUG/NDEBUG;_WINDOWS;_USRDLL;` | 随 Win32 配置一起不可达 | 【可直接修】随 3.3 一起删 |
| 第 219 行 `Debug\|x64` ResourceCompile | `_DEBUG ;_UNICODE;UNICODE;` | 值里有**尾随空格**（`_DEBUG ` 带空格）；且只有 Debug 有，Release 没有对应定义 | 【需确认】无害（rc 已 UTF-16LE 编码，版本串不受影响），仅整洁度问题 |

关于 `_USRDLL` / `_WINDOWS`：全仓搜索 `_USRDLL`、`_WINDOWS` 在 `*.h/*.cpp/*.rc` 里**均 0 命中**（前者是 MSVC 向导遗留约定宏，后者被 `windows.h` 系列间接依赖的习惯用法）。**没有把握判定为"可删"**——它们属于极低风险的常规宏，且删掉要重编全部 303 个 TU 才验证得出来。

- 【保留】`_WINDOWS`（标准 Win32 宏集，惯例保留）。
- 【可直接修（极低收益）】`_USRDLL`：确认无 `#ifdef` 使用后可以删，但收益≈0，建议随 Win32 配置一起清掉即可，不要单独动 Release|x64。

**没有发现"指向旧实验开关"的宏**：文件里不存在 `F5_*`、`F6_*`、`OPTISCALER_*`、`NEXTGEN_*` 之类的实验开关；迁移没有把旧实验宏带进项目配置（这是迁移干净的一面，如实记录，不臆造）。

---

## 5. build.ps1 自检

| # | 位置 | 问题 | 影响 | 判定 |
|---|---|---|---|---|
| 1 | 第 81/82/91 行 | `& $msbuild ...` 之后才赋值 `$exit`；配合第 22 行 `Set-StrictMode -Version Latest`，若 `&` 抛终止错误（MSBuild.exe 被删、路径变化、`Test-Path` 之后瞬间失效），`finally` 回来执行第 91 行 `Write-Host ... $exit` 会因"变量未赋值"再抛一次，**把真实错误盖掉** | 排障时看到的是"$exit 未定义"而不是 MSBuild 的错 | **【可直接修】** 在 try 前 `$exit = 1; $elapsed = [TimeSpan]::Zero` |
| 2 | 第 64 行 | `$args = @(...)` **覆盖 PowerShell 自动变量 `$args`** | 目前能跑，但脚本一旦加 `param` 之外的位置参数（或用 `$args` 做转发）行为会反常 | **【可直接修】** 改名 `$msbuildArgs` |
| 3 | 第 81 行 | `2>&1 | Tee-Object -FilePath $log`：MSBuild 输出是 GBK（中文 Windows 控制台代码页），PowerShell 7 按 UTF-8 解码 → **日志中文全部乱码**。实测 `build\msbuild.log` 首行为 `閫傜敤浜?.NET Framework MSBuild 鐗堟湰 17.14.60+43b635718`（应为"适用于 .NET Framework MSBuild 版本 …"） | 日志是唯一排障材料，现在基本不可读 | **【可直接修】** 设 `chcp 65001`/`$OutputEncoding`+`[Console]::OutputEncoding = [Text.Encoding]::UTF8`，或改用 `/flp:LogFile=$log;Encoding=UTF-8` 让 MSBuild 自己写文件 |
| 4 | 第 60 行 + 项目第 230 行 | 并行度双通道：`_CL_=/MP$Jobs /FS` 与项目 `<MultiProcessorCompilation>true</MultiProcessorCompilation>` 同时生效；且 `/FS`（强制串行 PDB 写）本身会抵消 `/MP` 的部分收益 | `-Jobs 8` 未必真的 8 倍速，行为不可预期 | **【需确认】** 建议只用 `/m:$Jobs` 一个旋钮，`_CL_` 只留 `/FS` |
| 5 | 第 43-48 行 | `-Clean` 只删 `build\obj`、`build\out`、`build\temp`；**不删** `build\msbuild.log`、也不删 `OptiScaler\x64\`（Debug|x64 的 `IntDir` 会把中间件写进源码目录） | "Clean"后仍可能有旧日志/旧中间件误导排障 | 【可直接修（低）】顺手删日志；或把 Debug|x64 的 `IntDir` 改成 `$(OutDir)obj\` |
| 6 | 第 71-73 行 | `SolutionDir` 拼接正确（保留尾部 `\`，第 62-63 行有注释说明），`IntDir`/`OutDir` 都指向 `build\` —— **实现无误** | 与 vcxproj 里 `$(SolutionDir)external\...` 的假设严格一致 | **【保留】** 这项是对的 |
| 7 | 第 33 行 | MSBuild 路径硬编码 `D:\VS2022BuildTools\MSBuild\Current\Bin\amd64\MSBuild.exe`（已核实存在，435,504 B）；无 `vswhere`/环境变量回退 | 换机/升级 VS 即断 | **【需确认】** 建议加 `$env:MSBUILD_EXE` 覆盖项，保留硬编码为默认 |
| 8 | 第 37-41 行 | 只校验 `$source/$external/$project/$msbuild` 四个存在性；**不校验** `external` 下关键子目录 | 依赖缺失时错误会晚到 cl.exe 阶段，信息更差 | 【需确认】可选改进，非缺陷 |
| 9 | 全脚本 | 工作目录未固定（无 `Push-Location $root`，旧 `BUILD-DEPLOY.ps1` 有）。因所有路径都是绝对路径、PreBuild 用的是 `$(ProjectDir)`，当前从任意 cwd 调用都能成立 | 无实际影响 | 【保留】 |
| 10 | 第 94-97 行 | 产物校验存在（`Test-Path $artifact` + 大小/UTC 打印），失败返回 1 | 与 README 承诺一致 | **【保留】** 这项是对的 |

补充（工程侧、非脚本）：构建日志里有两条与"配置"相关的告警，值得记账但均不阻塞 ——
`LNK4098: 默认库"LIBCMT"与其他库的使用冲突`（某个 vendored `.lib` 用 /MT 编、工程用 /MD，建议显式 `/NODEFAULTLIB:libcmt` 或重编来源）【需确认】；`C4744`（`InputCommon::currently_active_tech` 在 `nvapi\fakenvapi\low_latency_d3d.cpp` 与 `hooks\amdxc64_hooks.cpp` 中类型不一致，属头文件 ODR 隐患，非配置问题）。

---

## 6. 工程配置瘦身 Top 10（按"收益/风险"排序）

| # | 动作 | 涉及位置 | 风险 | 收益 |
|---|---|---|---|---|
| 1 | 删掉 3 组 Win32 配置（`ProjectConfigurations` 3 项 + Configuration 组 + PropertySheets + IncludePath/LibraryPath + ItemDefinitionGroup + 3 处 `*|Win32` 宏） | vcxproj 第 4-11、16-19、37-56、83-89、98-100、102-113、128-186 行 | 零（无 .sln、无任何入口构建 Win32） | −约 120 行配置，去掉 9 组不可达路径，`NODEBUG/WIN32` 类噪音一并消失 |
| 2 | 把 3 份逐字节相同的 x64 IncludePath/LibraryPath 合并成 1 份无条件 PropertyGroup | 第 115/121/125、116/122/126 行 | 低（改配置后需重编验证一次） | 60+24 条 → 20+8 条，未来加依赖只需改一处 |
| 3 | 删掉死路径 `$(SolutionDir)external\fakenvapi`（3 处） | 第 115/121/125 行 | 零（目标目录不存在，头文件已内联在 `OptiScaler\nvapi\fakenvapi\`） | 每个 TU 少探一个不存在的 include 目录 |
| 4 | 整段删除/注释 PostBuildEvent（29 条命令，16 条源不可满足） | 第 247-277 行 | 低：需同步决定"发行打包"由谁做（见第 5 条） | 去掉"一开就必失败"的 21 条 copy；`ReleaseDebug|x64` 的 7z 依赖（第 308 行）一并消失 |
| 5 | 用 build.ps1 里 4 行显式 copy 顶替 PostBuild，把 `D3D12Core.dll` 落到 `out\OptiScaler\D3D12_OptiScaler\` | build.ps1 第 97 行后（见 2.5 代码块） | 低（源文件确实存在） | 补上唯一真缺件；发行打包不再依赖旧根素材 |
| 6 | 给 `SolutionDir` 加 Globals 兜底，让 vcxproj 可独立/IDE 打开 | vcxproj 第 29-35 行 | 低（`/p:` 优先） | 工程不再是"只能被 build.ps1 驱动"的单通道 |
| 7 | 修 build.ps1 的 StrictMode 陷阱：`$exit`/`$elapsed` 预置初值；`$args` 改名 `$msbuildArgs` | build.ps1 第 64、91 行附近 | 零 | 失败时能看到真实错误，而不是"变量未定义" |
| 8 | 修日志编码（`/flp:LogFile=...;Encoding=UTF-8` 或设 `[Console]::OutputEncoding`） | build.ps1 第 81 行 | 零 | `build\msbuild.log` 从不可读变成可读——排障基础设施 |
| 9 | `ReleaseDebug|x64` 的 `NODEBUG` 错拼改 `NDEBUG`（全仓 0 处使用 `NODEBUG`） | 第 286 行 | 零 | 消除一个永远为假的宏；ReleaseDebug 若哪天启用会真的拿到 release STL 断言行为 |
| 10 | 清理 `Debug|x64` 噪音：空 `<StackReserveSize/>`、空 `<StackCommitSize/>`、空 `<PreBuildEvent><Command/></PreBuildEvent>`、多余 `<TargetExt>.dll</TargetExt>`、`IntDir=.\x64\Debug\` 写回源码目录 | 第 117-118、208-217 行 | 零 | 少 10 行死配置，中间件不再污染源码树 |

**不建议做的（容易误删）**：`external\streamline1` 不是 `streamline` 的重复项（代码里 `#include <sl1_reflex.h>` 真实使用）；`_DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR`、`_WINDOWS`、`streamline/freestyle/ffx_*` 那批 include 与 `.lib` 全部有实际引用；`ClCompile/ClInclude` 764 条**全部**指向真实文件，不要"清理"它们。

---

## 附录 A：验证方法（可直接复现，全部只读）

```bash
# 1) IncludePath / LibraryPath 逐条存在性（展开 $(SolutionDir)/$(ProjectDir) 后 [ -d ]）
#    见本次审计脚本 inc_lib_paths.sh：遍历第 103-126 行，按 ';' 切分，排除 $(IncludePath) 继承项
# 2) 工程条目坏项（764 条）
grep -oE '<(ClCompile|ClInclude|None|ResourceCompile) Include="[^"]+"' OptiScaler/OptiScaler.vcxproj \
  | sed -E 's/.*Include="([^"]+)"/\1/' | tr '\\' '/' \
  | while read -r f; do [ -e "OptiScaler/$f" ] || echo "MISS $f"; done   # 输出为空
# 3) PostBuild 源文件存在性：逐条按 2.2 表展开后 Test-Path
# 4) 配置行重复性
for ln in 103 107 111 115 121 125 116 122 126; do sed -n "${ln}p" OptiScaler/OptiScaler.vcxproj | md5sum; done
# 5) 日志乱码证据
head -1 build/msbuild.log        # 閫傜敤浜?.NET Framework MSBuild 鐗堟湰 17.14.60+43b635718
```

## 附录 B：关键事实出处

| 事实 | 位置 |
|---|---|
| PostBuild 29 条命令全文 | `OptiScaler\OptiScaler.vcxproj` 第 247-277 行 |
| PreBuild 生成 `resource_build_*.h`（含 `Start-Sleep 1500`） | 第 279 行（`Release|x64`）、第 311 行（`ReleaseDebug|x64`） |
| 生成头被 git 跟踪，每次构建都会弄脏工作区 | `git ls-files OptiScaler/resource_build_{date,commit}.h` 非空；当前值 `VER_BUILD_COMMIT "unknown"` |
| 旧流水线同样关闭 PostBuild | 旧根 `BUILD-DEPLOY.ps1` 第 151 行 |
| 发行打包依赖 PostBuild 目录 | 旧 worktree `package_release.ps1` 第 91 行 `$src = "$root\x64\Release\a"` |
| 迁移只拷了 2 个目录 | `tools\migrate-copy.ps1`（`OptiScaler` + `external`） |
| `D3D12Core.dll` 的运行时期望路径 | `OptiScaler\dllmain.cpp` 第 941-955 行 |
| LUT 目录缺失时的降级 | `OptiScaler\dlssnr\DlssNr_Menu.cpp` 第 134-147 行 |
| ini 的路径与自动落盘 | `OptiScaler\Config.cpp` 第 117、2628-2629 行 |

> 关于 `VER_BUILD_COMMIT "unknown"`：已核实 `git` 在 cmd/pwsh 下于 `OptiScaler\` 目录能正常返回 `b716554`，所以不是 PATH 问题；本次 DLL 生成时刻（17:14:41）早于新根首次提交（17:18），PreBuild 当时拿不到 HEAD 才写了 `unknown`。**下次 `build.ps1` 后应复查该文件**——若仍为 `unknown`，才是 PreBuild 命令本身的缺陷。【需确认】
