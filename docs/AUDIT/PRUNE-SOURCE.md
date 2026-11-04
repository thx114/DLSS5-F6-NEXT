# OptiScaler 源码树精简执行记录（PRUNE-SOURCE）

> 任务：shared `task-6`（执行者 `source-pruner`）
> 依据：`docs/AUDIT/SOURCE-INVENTORY.md`（只读审计报告）＋ Lead 拍板清单
> 执行时间：2026-11-04
> 执行方式：`rm` 直接删除（未 `git rm` / 未 `git add` / 未 commit，git 状态由 Lead 统一处理）
> 未执行 `build.ps1`（Lead 统一全量重建验证）

---

## 0. 结果速览

| 指标 | 前 | 后 | 差 |
| --- | ---: | ---: | ---: |
| `OptiScaler\` 文件数 | 1612 | **1489** | **−123** |
| `OptiScaler\` 目录数 | 211 | **184** | **−27** |
| `OptiScaler\` 字节数 | 124,903,963 (119.10 MiB) | **120,267,531 (114.70 MiB)** | **−4,636,432 B (−4.42 MiB)** |
| `genshin\tests\` 文件数 | 360 | **244** | −116 |
| `genshin\tests\` 字节数 | 5,090,217 | **2,442,903** | −2,647,314 |

删除总数：**123 个文件 / 4,636,432 B** ＋ **8 个目录**（其中 3 个是含文件的快照目录树、5 个是纯空目录壳）。
字节核对：死文件 1,989,118 B ＋ 测试快照 2,647,314 B ＝ **4,636,432 B**（与实测总体积差完全一致）。

`git status --porcelain OptiScaler` ＝ **123 条 `D`**，与删除计数逐条吻合。
`OptiScaler.vcxproj` / `.filters`：**零改动**（`git diff --stat` 无输出）。

---

## 1. 删除明细

### A. 空目录壳（5 个目录 / 0 文件 / 0 B）
结构性噪音，无任何文件、无任何引用（审计 §4.1、§5.3、§6-1）。

| 路径 | 说明 |
| --- | --- |
| `OptiScaler\F5\` | 迁移残留空壳 |
| `OptiScaler\NEXT\`（含 `work\` 两层） | 3 层空目录壳 |

> `NEXT\` 连同其 3 层子目录一并删除，故目录计数按 `F5` 1 个 + `NEXT` 链 3 个 = 4 个空目录壳。
> 审计 §5.3 列出的第 4 个空目录 `dlssnr\forwarder\OptiScaler\` **未删除**（见 §4 跳过清单）。

### B. 零引用自有死文件（7 个 / 1,989,118 B）

审计 §3.4「自有代码零引用明细」中**明确标「候选删除」**的 7 项，逐项复验通过后删除。
标「待确认」的 3 项（`ShaderRegex.cpp`、`dlssnr_forwarder.cpp`、`FSR_RCAS_Shader.h`）**全部保留**。

| # | 文件 | 字节 |
| ---: | --- | ---: |
| 1 | `menu\font\Hack.h` | 1,921,697 |
| 2 | `native\GenshinMotionDiagnostic.h` | 22,661 |
| 3 | `genshin\GenshinPassEvidence.h` | 15,616 |
| 4 | `genshin\GenshinCameraObserver.cpp` | 15,517 |
| 5 | `genshin\GenshinDepthContract.h` | 8,570 |
| 6 | `menu\MenuPhysicalTiming.h` | 4,107 |
| 7 | `dlssnr\LookUiSnapshot.h` | 950 |
| | **合计** | **1,989,118** |

**逐项零引用复验（每个文件均执行 3 重检索，全部为 0 命中）**
1. `grep -rn "<basename>" . --include=*.cpp --include=*.h --include=*.inl --include=*.c --include=*.hpp` → 0
2. `grep -n "<basename>" OptiScaler.vcxproj OptiScaler.vcxproj.filters` → 0
3. `grep -rln "<basename>" .`（不限扩展名，全树兜底，含 `.bat/.ps1/.py/.json/.md`）→ 0

**补充佐证**
- `Hack.h`：现行编译单元为 `menu/font/Hack_Compressed.cpp`（工程内），二者互斥；`Hack-Regular.ttf` 字体源仍保留，字体头可再生。
- `GenshinCameraObserver.cpp`：未进 `ClCompile`、无任何 `#include`，即从未参与编译。

### C. `genshin\tests\` 中间迭代快照（8 个目录 / 116 文件 / 2,647,314 B）

| 目录 | 文件数 | 字节 |
| --- | ---: | ---: |
| `fg-depth-camera\r851-source` | 15 | 365,193 |
| `fg-depth-camera\r851-source-v2` | 15 | 365,794 |
| `fg-depth-camera\pre-fix-diag` | 17 | 390,090 |
| `fg-depth-camera\final-r852` | 15 | 365,480 |
| `fg-depth-camera\final-r852b` | 17 | 387,053 |
| `fg-depth-camera\final-r852c` | 17 | 387,051 |
| `fg-depth-camera\final-r852d` | 3 | 1,714 |
| `fg-depth-camera\final-r852e` | 17 | 384,939 |
| **合计** | **116** | **2,647,314** |

**删除前的取代性验证（关键，按 Lead 指示先读 `run.py` 再定）**
- 两个 `run.py` 均以 `--phase <name>` 建目录 `E = A/args.phase` 并**当场生成** `.log`/`stages.json`/`receipt.json`/夹具 `.exe`，**不读取**任何既有 phase 目录。故快照目录是纯输出，不是输入。
- `fg-depth-camera\run.py` 唯一的目录型输入是 `A/'pre-fix/hardware/r849-embedded.dxbc'` → **`pre-fix\` 已因此保留**（从原删除清单中撤下）。
- `fg-depth-camera\checks.json` 中 `cleanup.removed` 记录历史删除的 `fixture.obj`，属重生成产物，`pre-fix\` 内已不存在该文件，删除 8 个快照目录不影响该记录的可解释性。
- 8 个目录名在**保留文件**（`README.md`、`checks.json`、`inputs.json`、`preimage.json`、各 `receipt.json`）中均**未被引用**；两个 README 的叙述只指向 `perspective-v1/v2/v3`、`release-fp-fast`、`pre-fix`、`candidate-v*`、`final` 等保留项，未提及 `r851-source*`、`pre-fix-diag`、`final-r852*`。
- `build.ps1` / `OptiScaler.vcxproj` / `.filters` 对 `genshin\tests` 零引用。

---

## 2. 保留清单（明确未删 / 已撤销删除计划）

| 项 | 原因 |
| --- | --- |
| `genshin\tests\fg-depth-camera\pre-fix\`（15 文件 / 365,703 B） | **`run.py` 硬依赖**：`shutil.copy2(A/'pre-fix/hardware/r849-embedded.dxbc', …)`。删了测试驱动直接崩。虽在审计「中间快照」名单内，本次**不删**。 |
| `genshin\tests\fg-depth-camera\` 6 个 `.log`（`final\` 内） | `final/receipt.json` 的 SHA256 断言对象，属测试证据，**不是**一次性输出。 |
| `genshin\tests\fg-animated-character\` 10 个 `.log`（`final\`、`final-embedded\` 内） | 同上，被 `receipt.json` 的 `artifacts` SHA256 钉住。 |
| `pre-fix\` 内 6 个 `.log` | 同一 `receipt.json` 证据机制（`stdoutSHA256`），随 `pre-fix\` 整体保留。 |
| `genshin\tests\fg-depth-camera\GenshinFgGameMotion.before.h`（42,700 B） | **保留**：`checks.json` 的 `preimageSHA256: 6ade62f4…` 就是它的哈希，`README` 也明示其为「exact product preimage」。属测试断言依赖，按 Lead 指示不能按 `*.before*` 批删。 |
| `genshin\tests\fg-animated-character\product.patch`（2,869 B） | **保留**：`checks.json` 的 `files` 表钉了它的 SHA256 `0546f465…`，是产品补丁的正本，非草稿垃圾。 |
| `fg-animated-character\candidate-v1/v2/v3\`（87 文件 / 790,779 B） | **保留**：两个保留的 `README.md` 显式论述 v1/v2 失败、v3 通过，是回归断言的来源；带 `hardware-candidate`/`warp-negative-control` 四象限子目录签名。 |
| `fg-depth-camera\perspective-v1/v2/v3\`、`release-fp-fast\`（56 文件 / 264,333 B） | **保留**：`README.md` §45 显式论述「intermediate failures are not product-build or game failures」，是失败样本证据。 |
| `fg-depth-camera\final\`、`fg-animated-character\final\`、`final-embedded\`（73 文件） | 最终快照，明确保留。 |
| `shaders\shader_tools\`（10 文件 / 21.06 MB） | **本轮不迁移**（Lead 指示：搬走非必需工具路径会扩大变更面，当前以可复现优先）。原位、原样、零改动。 |
| `library\*_x64d.lib`（33 MB） | 不批准，保留。 |
| `shaders\` 41 个未引用 `.hlsl/.hlsli` | 不批准，保留。 |
| `include\` 12 个未用 SDK 头 | 待确认，保留。 |
| `dlssnr\forwarder\`（含独立子工程 + `forwarder\OptiScaler\` 空目录） | 待确认 / 独立工程，保留。 |
| 全部许可证、第三方说明、`third_party\reference\` | 保留（含 `ShaderRegex.cpp`）。 |
| `resource_build_date.h`、`resource_build_commit.h`、`build\` | 构建生成物，不在清理范围。 |

---

## 3. 修正后的 `genshin\tests\` 可复现入口

删除后目录仅剩：两个夹具根（`run.py`、`fixture.cpp`、`README.md`、`checks.json`、`inputs.json`/`preimage.json`、`R852_*_Hlsl.h`、`.before.h`、`.patch`）＋ 6 个快照目录（`pre-fix`、`final`、`perspective-v1/v2/v3`、`release-fp-fast`、`candidate-v1/v2/v3`、`final-embedded`）。共 **244 文件 / 2,442,903 B / 86 个 `.log`**。

`run.py` 的输入依赖复验（删除后逐一 `test -f`）：

| 依赖 | 状态 |
| --- | --- |
| `OptiScaler\genshin\GenshinFgGameMotion.h` | OK |
| `OptiScaler\genshin\GenshinFgGameMotion_Hlsl.h` | OK |
| `OptiScaler\native\F5LowShaderBytecode.h` | OK |
| `OptiScaler\genshin\GenshinTypedGuides.cpp` | OK |
| `OptiScaler\genshin\GenshinLegacyCapture.cpp` | OK |
| `tests\fg-depth-camera\fixture.cpp` / `run.py` | OK |
| `tests\fg-depth-camera\pre-fix\hardware\r849-embedded.dxbc` | OK |
| `tests\fg-animated-character\fixture.cpp` / `R852_GenshinFgGameMotion_Hlsl.h` / `preimage.json` | OK |

> 唯一 MISS：`genshin\GenshinFgDepthCamera.h`（被 `fg-depth-camera\checks.json` 引用）。经 `git ls-tree HEAD` 确认**从未被提交过**，属**本次操作之前就存在的既有缺失**（旧根迁移遗留），与本次删除无关，未做任何处理，仅在此登记。

---

## 4. 跳过清单（有疑问一律不动）

| 项 | 跳过原因 |
| --- | --- |
| `dlssnr\forwarder\OptiScaler\`（空目录） | 审计 §5.3 列为空目录，但 `dlssnr\forwarder\` 整体是**独立子工程**且本轮标"保留"；删其内部空目录会污染该子工程的目录语义，收益 0 B。**不动**。 |
| `mods\gimi\third_party\reference\ShaderRegex.cpp`（24,869 B） | 审计 §3.4 标 **待确认**（非"候选删除"），按 Lead 指示「只删明确候选」→ **保留**。 |
| `dlssnr\forwarder\dlssnr_forwarder.cpp`（49,044 B） | 同上，标 **待确认**，且属独立 vcxproj 子工程 → **保留**。 |
| `shaders\output_scaling\fsr1\FSR_RCAS_Shader.h`（46,601 B） | 同上，标 **待确认**（可能服务离线着色器再生成流程）→ **保留**。 |
| `include\imgui\imgui_demo.cpp`（583,267 B） | 审计 §6-7 列低风险，但**不在**本次批准清单 B/C/D 内，且涉及上游 SDK 完整性 → **未动**。 |
| `include\d3dx\D3DX11*.h`（4 个 / 42.5 KB） | §6-8 明确标 **待确认** → **保留**。 |
| `menu\font\*.ttf`（721 KB） | §4.9 标 待确认，且 `Hack_Compressed.cpp` 依赖其可再生性 → **保留**。 |
| 130 个 `.log` 中的 **86 个** | 非"全部可删"：被 `receipt.json` SHA256 断言钉住或位于 `final*`/`pre-fix` 保留目录内 → **保留**（仅随 8 个快照目录删掉 54 个）。 |
| `*.before*` / `*.patch` | 经 `run.py`/`checks.json` 验证为**测试断言依赖**（preimage 哈希正本），非过程垃圾 → **保留**。 |
| `shaders\shader_tools\` 迁移 | Lead 本轮指示**暂不迁移**，避免扩大变更面 → 原位保留，脚本路径零改动。 |

---

## 5. 未执行项（明确排除）

- 未运行 `build.ps1` / 未做任何编译（避免抢锁，Lead 统一验证）。
- 未 `git add` / `git commit` / `git rm`：全部用 `rm -rf` 删除，工作区留 123 条 `D` 待 Lead 统一提交。
- 未修改 `OptiScaler.vcxproj`、`.filters`、根 `build.ps1`、`README.md`、`docs\` 下其它文件。
- 未触碰 `external\`、`library\`、`build\`、旧根 `D:\DATA\Documents\ChatGPT\DLSS F5 NEXT`。
- 未修改任何 `.cpp/.h/.inl` 内容（仅删除 §1-B 列出的 7 个文件）。
