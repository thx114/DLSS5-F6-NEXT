# external/ 依赖树精简执行记录（task-7）

- 执行者：external-pruner
- 时间：2026-11-04
- 依据：`docs/AUDIT/EXTERNAL.md`（只读审计）+ task-7 批准清单 + Lead 追加的「许可文件零删除」约束
- 执行方式：删除前逐项确认「不在 IncludePath/LibraryPath、源码 0 命中、无旁路相对路径 include」
- **未执行**：`build.ps1`（由 Lead 统一验证）、`git add` / `git commit`、任何 `git rm`
- 未触碰：`OptiScaler\`、`build.ps1`、`README.md`、`docs\` 其它文件、旧根 `D:\DATA\Documents\ChatGPT\DLSS F5 NEXT`

---

## 0. 总览

| 指标 | 删除前 | 删除后 | 变化 |
|---|---:|---:|---:|
| `external/` 体积 | **147,410,233 B（140.58 MiB）** | **35,714,743 B（34.06 MiB）** | **−111,695,490 B（−106.53 MiB，−75.8%）** |
| `external/` 文件数 | **3287** | **679** | **−2608** |
| 子目录数 | 19 | **18**（历史空目录移除） | −1 |

> 最终 34.06 MiB 高于旧报告预期的 ~30.5 MiB：因为按 Lead 约束**保留了 26 个第三方 LICENSE/NOTICE/版权文件**，且**未执行旧报告的 B 级（Vulkan-Hpp 未引用头 3.72 MiB）激进删除**——B 级不在本次批准范围内，全部保留。此差异是安全侧保守结果，非遗漏。

---

## 1. 逐库前后对比

| # | 库 | 删除前 B | 删除后 B | 回收 B | 前文件 | 后文件 | 动作 |
|---|---|---:|---:|---:|---:|---:|---|
| 1 | FidelityFX-SDK | 63,565,165 | 399,182 | **63,165,983** | 1565 | 74 | 删 `sdk/` `framework/` `samples/` 主体 + `docs/` 非许可内容 |
| 2 | vulkan | 32,019,050 | 18,215,166 | **13,803,884** | 79 | 60 | 删 `registry/` `tests/` |
| 3 | FidelityFX-SDK-v2 | 30,571,048 | 146,983 | **30,424,065** | 743 | 13 | 删 `Kits/` 非许可内容 + `Samples/` + `docs/` 非许可内容 |
| 4 | xess | 4,522,209 | 220,651 | **4,301,558** | 393 | 25 | 删 `doc/` `samples/`（保留许可） |
| — | 其余 14 库 | — | — | **0** | — | — | **完全未动** |
| | **合计** | **147,410,233** | **35,714,743** | **111,695,490** | **3287** | **679** | |

> 剩余 14 库明细（未改动，删除前=删除后）：freetype 5,463,742 / nvapi 3,378,046 / directx_agility_sdk 3,364,053 / spdlog 1,202,101 / magic_enum 1,080,601 / nlohmann 953,436 / unordered_dense 560,780 / simpleini 365,052 / streamline 164,856 / nvngx_dlss_sdk 126,561 / streamline1 33,432 / AntiLag2-SDK 17,853 / latencyflex 14,393 / peripheral_warp 7,855。

---

## 2. 各库删除依据与保留项

### 2.1 FidelityFX-SDK（63,565,165 → 399,182 B，−60.25 MiB）

**删除依据（删前已核实）**
- `OptiScaler.vcxproj` 对该库的 IncludePath 仅一条：`external\FidelityFX-SDK\ffx-api\include\ffx_api`（行 115/121/125）。
- `grep -rn 'FidelityFX-SDK' OptiScaler/ --include='*.h' --include='*.cpp' ...` 在**源码中 0 命中**，命中仅出现在 `OptiScaler.vcxproj` 的 PostBuild `copy` 行（262/263）与二进制 `.lib` 内部字符串。
- `grep -rnE '#include[[:space:]]*"[^"]*\.\./\.\./\.\./external' OptiScaler/` 全树仅 1 条旁路，指向 `peripheral_warp`，与本库无关。
- 无任何文件引用 `FidelityFX-SDK/sdk|framework|samples|docs` 路径（`vk.xml`/registry 类生成输入 0 命中）。
- 工程实际链接的 FSR 库来自 `OptiScaler\library\fsr2|fsr2_212|fsr31`，技术头来自本地 `OptiScaler\include\fsr2|fsr3|fsr31`。

**删除**：`sdk/`（19,910,499 B / 826 文件）、`framework/`（15,326,545 B / 468）、`samples/`（10,208,382 B / 110）、`docs/` 下非许可内容。

**保留（硬需求 + 许可）**
- `ffx-api/include/ffx_api/**` —— 12 文件，IncludePath 直点，`ffx_api.h` 被 10 个源文件引用。
- `ffx-api/src/**`（20 文件，137,983 B）—— 未编译，但**任务清单 C 只要求保留 include；为降低风险按「不确定即保留」处置**，未删。
- `PrebuiltSignedDLL/amd_fidelityfx_dx12.lib`、`amd_fidelityfx_vk.lib` —— PostBuild 复制源，按 Lead 指示全部保留。
- `docs/license.md`（1,142 B）—— 分发许可。
- **许可文件 17 个**（不因位于 sdk/framework 而删除）：
  - `sdk/LICENSE.txt`、`sdk/include/FidelityFX/host/backends/dx12/license.txt`
  - `sdk/tools/ffx_shader_compiler/libs/{MD5,SPIRV-Reflect,tiny-process-library}/LICENSE`、`libs/dxc/LICENSE-{LLVM,MIT,MS}.txt`
  - `framework/cauldron/framework/libs/{AGS,acs,imgui,vectormath,memoryallocator}/{LICENSE.txt,license.txt}`、`libs/json/License.txt`、`libs/dxc/LICENSE-{LLVM,MIT,MS}.txt`
  - **原址保留** `samples/thirdparty/samplercpp/readme.txt`（734 B）：含该第三方采样器的来源与版权声明，虽在 `samples/` 下仍原位保留（目录 `samples/thirdparty/samplercpp/` 因此只剩此 1 文件）。

### 2.2 vulkan（32,019,050 → 18,215,166 B，−13.16 MiB）

**删除依据**：IncludePath 仅 `external\vulkan\include`（行 115/121/125 等 6 处）；`grep -rn 'vulkan/registry\|vk\.xml\|validusage' OptiScaler/ build.ps1` **0 命中**（registry 是代码生成输入，不参与编译）。
**删除**：`registry/`（13,800,074 B / 15 文件）、`tests/`（3,810 B）。
**保留**：`include/`**全部 44 文件**（含 B 级未引用 Vulkan-Hpp 头，本次不删）、`LICENSES/`、`LICENSE.md`、全部根 cmake/README。

### 2.3 FidelityFX-SDK-v2（30,571,048 → 146,983 B，−29.02 MiB）

**删除依据（4 条独立证据）**
1. vcxproj 全文 `FidelityFX-SDK-v2` 仅 4 命中，全部是 PostBuild `copy`（行 264–267），**无 IncludePath、无 LibraryPath**。
2. `grep -rn 'FidelityFX-SDK-v2' OptiScaler/` 在源码 0 命中。
3. 源码 0 处 include v2 API 头（被用的是 v1 的 `ffx-api/include`）。
4. `build.ps1` 传 `/p:PostBuildEventUseInBuild=false`，干净构建连 copy 都不执行。

**删除**：`Kits/Cauldron2/`（2,791,858 B / 286 文件）、`Kits/OpenSource/` 非许可内容、`Kits/FidelityFX/` 除 `signedbin` 与 `docs/license.md` 外全部（api/backend/denoisers/framegeneration/radiancecache/upscalers + `docs/` 8.39 MiB 主体）、`Samples/`（777,332 B / 113，仅保留下方版权声明）、`docs/` 非许可内容（−7.38 MiB）。根目录 `ClearMediaCache.bat`、`UpdateMedia.bat` 曾误删，**已恢复原位**。
**保留**
- `Kits/FidelityFX/signedbin/amd_fidelityfx_loader_dx12.lib`（2,880 B）—— PostBuild 复制源目录，**整目录保留**。
- `docs/license.md`（61,784 B）—— PostBuild 复制为 `FidelityFX_v2_LICENSE.md`。
- `Kits/FidelityFX/docs/license.md`（61,784 B，与上者 md5 相同，冗余但属许可 → 保留）。
- `3rdpartynotice.md`（第三方组件许可总表）、`readme.md`。
- **`Kits/OpenSource` 下 6 个第三方许可文件**：`amd/acs/LICENSE.txt`、`amd/ags/LICENSE.txt`、`amd/antilag2/LICENSE.txt`、`amd/memoryallocator/license.txt`、`imgui/LICENSE.txt`、`vectormath/LICENSE.txt`（合计 7,412 B）。

### 2.4 xess（4,522,209 → 220,651 B，−4.10 MiB）

**删除依据**：IncludePath 仅 `xess\inc\{xess,xell,xess_fg}`（6/3/3 处），LibraryPath `xess\lib`；`grep -rn 'xess/doc\|xess/samples' OptiScaler/` **0 命中**。
**删除**：`doc/`（2,828,756 B / 275 文件）、`samples/` 主体（1,473,942 B / 94）。
**保留**：`inc/{xess,xell,xess_fg}/`、`lib/{libxess,libxess_dx11,libxess_fg,libxell}.lib`（4 个 .lib 与源码引用一一对应）、`bin/`（含 `basic_sample_shaders/`，PostBuild `copy bin\*.dll` 目标目录，**目录结构保留**）、`LICENSE.txt`、`third-party-programs.txt`、`README.md`、`SECURITY.md`，以及 **`samples/LICENSE.md`（1,140 B，MIT / Sascha Willems 版权声明，删除 samples 时单独上提保留）**。

### 2.5 历史空目录
历史空目录（0 文件、不在 git 索引）。已用 `rmdir`（非递归）移除，确认目录内无内容。

---

## 3. 最高优先级保护项核对结果（全部通过）

| # | 保护项 | 要求 | 实测结果 |
|---|---|---|---|
| 1 | `external\peripheral_warp\` | 整目录保留 | ✅ `SpatialWarp.h` + `LICENSE`（2 文件，7,855 B）完好；旁路 include `#include "../../../external/peripheral_warp/SpatialWarp.h"`（`DlssNr_Spatial.h:16`）路径可解析 |
| 2 | `external\directx_agility_sdk\lib\D3D12Core.dll` | 保留 external 唯一 DLL | ✅ 3,351,064 B 完好；`directx_agility_sdk` 整库 3,364,053 B / 2 文件未动 |
| 3 | `xess\inc\`、`xess\lib\`、`xess\bin\` | 保留（bin 可为空目录） | ✅ inc 3 子目录、lib 4 个 .lib、`bin/basic_sample_shaders/` 目录均在 |
| 4 | `FidelityFX-SDK-v2\Kits\FidelityFX\signedbin\` | 整目录保留 | ✅ `amd_fidelityfx_loader_dx12.lib` 2,880 B 完好 |
| 5 | `FidelityFX-SDK\ffx-api\include\` | 保留 | ✅ 12 文件（`ffx_api.h`、`ffx_api_types.h`、dx12/vk 子目录等）完好 |
| 6 | 其余 17 库 | 不动 | ✅ 14 个非目标库 0 字节变化；目标库中的 freetype/nvapi 等硬依赖库未触碰 |
| 7 | 第三方许可文件 | 全保留 | ✅ 26 个 LICENSE/NOTICE/COPYING/AUTHORS/版权文件全部在位（详见 §2） |

---

## 4. 跳过清单（未删，及原因）

| 路径 | 体积 | 跳过原因 |
|---|---:|---|
| `vulkan\include\vulkan\vulkan_raii.hpp` 等 Vulkan-Hpp 未引用头（旧报告 B 级） | 3.72 MiB | 不在本次批准清单内（仅 A 级）；且属官方头集合，将来启用需恢复。**全保留** |
| `FidelityFX-SDK\ffx-api\src\` | 137,983 B | 清单 C 只要求删 sdk/framework/samples/docs 其余；`src/` 未明确授权，按「不确定即保留」处置 |
| `FidelityFX-SDK\PrebuiltSignedDLL\` | 5,486 B | Lead 明确要求保留全部现有 PrebuiltSignedDLL |
| `FidelityFX-SDK\docs\license.md` | 1,142 B | 分发许可（PostBuild 复制源） |
| `FidelityFX-SDK\samples\thirdparty\samplercpp\readme.txt` | 734 B | 第三方采样器来源/版权声明，**原位保留**（`samples/` 仅剩此一文件） |
| `FidelityFX-SDK-v2\ClearMediaCache.bat`、`UpdateMedia.bat` | 3,335 B | 曾随根目录清理误删，已 `git checkout` 恢复原位（不在批准删除项内） |
| `xess\bin\basic_sample_shaders\` | 9,163 B | PostBuild 目标目录结构；清单要求保留 bin |
| `xess\samples\LICENSE.md` | 1,140 B | 版权声明，删除 samples 时单独保留 |
| 各库根 `readme`/`CMakeLists`/`LICENSE`/`SECURITY.md` 等元数据 | ~0.1 MiB | 骨架与许可，无体积收益 |
| `nvapi\amd64|x86\`、`Sample_Code\`（旧报告 A 级 #10/#17） | 1.44 MiB | task-7 明确「nvapi 不动」；仅审计建议，未授权 |
| `spdlog/magic_enum/unordered_dense/simpleini` 的 test/doc/example（旧报告 A 级 #12–#15） | 1.70 MiB | task-7 明确这些库「不动」 |
| `FidelityFX-SDK-v2\Kits\FidelityFX\docs\license.md` | 61,784 B | 与 `docs/license.md` 内容重复，但属许可文件，按 Lead 约束保留 |

---

## 5. 待 Lead 验证（本执行者未运行构建）

1. `.\build.ps1 -Clean` → 期望 `exit 0` 且 `build\out\OptiScaler.dll` 仍为 **34,795,008 bytes**（尺寸不变 = 编译/链接输入未变）。
2. 重点回归面：`ffx_api.h` 一族（10 个源文件）、`vulkan/vulkan.h` 与 `vulkan.hpp`（31 文件）、xess 头 + 4 个 .lib、`peripheral_warp/SpatialWarp.h`。
3. 分发路径（`PostBuildEventUseInBuild=true`）当前仍会因 XeSS/FFX v2 运行时 DLL 缺失而为空操作，与本次删除无关（审计 §7.1 已知问题）。
4. git 状态：本次仅产生工作树删除（`git status` 显示 2608 条 ` D`）；**未 stage、未 commit**。除 `xess/samples/LICENSE.md` 为删除后回填（现为未跟踪新文件）外，其余保留的许可文件均为原路径原 blob，不产生 git 变更。若需缩小 `.git`（81 MiB 历史），须另行 `git rm -r --cached` + 提交/改写历史，由 Lead 决策。

---

## 6. 执行过程说明（一次自我纠正，已完全恢复）

首次修剪 `FidelityFX-SDK-v2` 时，`rm -rf Kits/` 一并移除了受保护的 `Kits/FidelityFX/signedbin/`。发现后立即用 `git checkout --` 从索引恢复：
- `Kits/FidelityFX/signedbin/amd_fidelityfx_loader_dx12.lib`（2,880 B）已恢复并复验；
- 同批误删的 7 个第三方许可文件（`Kits/OpenSource/amd/{acs,ags,antilag2,memoryallocator}/*`、`Kits/OpenSource/{imgui,vectormath}/*`、`Kits/FidelityFX/docs/license.md`）亦已全部恢复并复验。
最终状态经 §3 逐条核对，全部保护项在位。后续修剪 `FidelityFX-SDK` v1 时改用「先备份许可 → 删 → 回填」流程，未再发生同类问题。

后续再纠正 2 项（均已复原）：
- `FidelityFX-SDK/samples/thirdparty/samplercpp/readme.txt` 曾被我上提到 `docs/` 改名保存，按 Lead「原位保留」要求已移回原路径。
- `FidelityFX-SDK-v2/ClearMediaCache.bat`、`UpdateMedia.bat` 曾被我误判为可删，已从 git 恢复原位。
