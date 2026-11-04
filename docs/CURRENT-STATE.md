# DLSS5_NEXT 当前状态

更新日期：2026-11-04。当前工作根是 `D:/CODE/DLSS5_NEXT`；从旧根迁入的 F6 源码已完成第一轮结构整理，并通过清缓存的 Release x64 构建。

## 目录与入口

| 位置 | 用途 |
|---|---|
| [OptiScaler/](../OptiScaler/) | 当前产品源码；[工程](../OptiScaler/OptiScaler.vcxproj)含 303 个编译条目 |
| [external/](../external/) | 实体第三方依赖，与 OptiScaler 保持同级；已裁去不用的 SDK 主体、样例与文档 |
| [build.ps1](../build.ps1) | 当前构建入口，Release x64，默认最多 8 个编译工作进程 |
| `build/` | obj/out/temp、UTF-8 构建日志，以及可选 shader 生成输出；Git 忽略 |
| [tools/shaders/](../tools/shaders/) | 从既有 Look recipe 迁入并适配新根的生成工具 |
| [LICENSE](../LICENSE)、[Licenses/](../Licenses/) | 主许可证与第三方署名；依赖自身的许可文件也保留 |
| [AGENTS.md](../AGENTS.md) | 当前协作规则和模型分工 |
| [history/](history/) | 11 份旧交接文档，保留原文；旧路径和运行结论须按原上下文理解 |
| [AUDIT/](AUDIT/) | 本次整理的审计和实际删除记录；后续不按轮次继续堆叠报告 |

## 构建和已验证结果

需要 PowerShell 7、MSVC v143；本机 MSBuild 在 `D:/VS2022BuildTools/MSBuild/Current/Bin/amd64/MSBuild.exe`。

```powershell
pwsh -NoProfile -File ./build.ps1
pwsh -NoProfile -File ./build.ps1 -Clean -Jobs 12
```

脚本把 `SolutionDir` 指向本根，obj/out/temp 收在 `build/`。失败返回真实 MSBuild 退出码，日志由 MSBuild 文件记录器直接写 UTF-8。Release 的 PreBuildEvent 自动生成 `resource_build_date.h` / `resource_build_commit.h`，二者不跟踪到 Git。

**本次精简后的全量构建**：2026-11-04 19:33–19:36，北京时间；先删除旧 obj/out/temp，再实际编译、链接，`exit 0`，耗时 **2 分 56 秒**。

- [产物](../build/out/OptiScaler.dll)：**34,795,008 bytes**，Release x64。
- 上述全量构建完成时间：`2026-11-04T11:36:13.4421189Z`；版本主号 `0.1.29-final`。
- 上述全量构建汇总：**335 个警告、0 个错误**。警告涉及编码、类型转换、继承和链接，不宣称零警告。[日志](../build/msbuild.log)对应最近一次构建，后续构建会覆盖它。
- 整理提交 **`8a34d8c`** 后再做增量编译、链接：`exit 0`，耗时 **42 秒**，**62 个警告、0 个错误**。最终 DLL 仍为 **34,795,008 bytes**，版本 **`0.1.29-final (8a34d8c)`**，mtime **`2026-11-04T11:57:48.9510802Z`**；构建后 Git 工作树干净。之后的记录更新只涉及文档。
- 独立复核：764 个工程条目全部存在；受保护依赖与基线一致；本次删除没有触及在用的测试输入。

本根只构建 DLL，未部署、未做完整分发包，也未运行游戏。旧 PostBuild 分发流程保持关闭；XeSS/部分 FidelityFX 运行 DLL 原本就不在此依赖快照中，不能把本根直接当完整安装包。

## 这轮实际精简

| 范围 | 整理前 | 整理后 | 实际减少 |
|---|---:|---:|---:|
| 产品树（执行者统计，含当时两个生成版本头） | 1612 文件 / 124,903,963 B | 1489 文件 / 120,267,531 B | 123 文件 / 4,636,432 B |
| external | 3287 文件 / 147,410,233 B | 679 文件 / 35,714,743 B | 2608 文件 / 111,695,490 B |

两项合计移除约 **110.94 MiB** 工作文件。Git 基线仍保存原文件，删除并不缩减已有 Git 历史的体积；上表是执行时快照，生成版本头的时间与 commit 文本会随构建变化。

删除了七个确认零引用的自有文件、八组无引用的中间测试输出，以及未使用的 FidelityFX SDK 主体、Vulkan registry、XeSS 样例/文档。移除了 F5/NEXT 空壳，清掉三处失效的 `external/fakenvapi` 头文件搜索路径。

保留了 Debug 链接库、实际使用的头文件/库、所有许可证、第三方参考、独立 forwarder 工程、shader 源码与原位工具链，以及测试驱动/断言仍引用的最终和失败样本。具体记录见 [源码执行记录](AUDIT/PRUNE-SOURCE.md)和[依赖执行记录](AUDIT/PRUNE-EXTERNAL.md)。

Look shader recipe 的路径已改为随仓库定位，输出放到 `build/shaders/evidence/` 与 `build/shaders/backups/` 的新子目录；语法、帮助和错误路径拒绝已验证。本轮没有重新生成 shader，也没有修改现有嵌入字节码。

## 旧根与后续工作

旧根 `D:/DATA/Documents/ChatGPT/DLSS F5 NEXT` 保持原位，未删除或重命名。历史 audit、worktrees、work、回退件、原 Git 工作区和正式部署硬链接未搬入本根；需要调查旧候选或证据时仍从旧根取。

本次完成的是源码迁移、可证冗余清理和构建闭合，不能据此判断三游戏 NR/FG 已修好：原神异步 FG 显示、ZZZ source/SDK 时钟、星铁 NR 历史交接仍是旧交接指出的后续问题。R1075 私有候选没有自动集成。

已知历史测试限制：`genshin/tests` 的旧驱动仍引用旧轮次的 MSVC 环境捕获辅助文件，个别 checks 也记录不在当前树中的旧头文件。保留这些回归材料不等于已验证它们可在新根直接运行；恢复测试时先整理驱动路径，不能冒用旧结果。

下一步核心重构应按三游戏的实际输入、GPU 所有权、消费与退休链路分工；普通审计/文档使用 DS，核心代码设计按用户指示可使用 `gpt-6.1-sol`。构建成功、运行验证、画面正确和性能收益分别报告。
