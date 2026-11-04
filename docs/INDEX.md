# 文档索引（docs/INDEX.md）

> ## ⚠️ 除本索引外，`docs/` 下全部是**历史交接文档**
>
> 这些文件是从旧根 `D:/DATA/Documents/ChatGPT/DLSS F5 NEXT` 逐字节复制过来的（SHA256 已核对一致），内容未做任何修改。
>
> - 文中出现的路径（`OptiScaler/…`、`work/…`、`audit/…`、`BUILD-DEPLOY.ps1`、`PERFORMANCE-ROUND-INDEX.md` 等）指向的是**旧根**，本根不一定存在。
> - 文中的构建/部署/验证结论属于旧根当时的状态，**不能作为本根的构建说明或验收依据**。
> - **当前唯一构建入口是本根 `build.ps1`**（`pwsh -NoProfile -File .\build.ps1`），产物 `build\out\OptiScaler.dll`；见根 `README.md`。
> - 历史文档不授予任何权限，也不能恢复任何门禁/批准状态。

## 文档清单

| 文档 | 大小 | 原位置 | 内容 | 现状 |
|---|---|---|---|---|
| [HANDOFF.md](HANDOFF.md) | 26,160 B | 旧根 `HANDOFF.md` | 最新正式版、未修问题、三游戏全链路、配置/CLI、整理结果（R1077，2026-11-04） | 历史交接；路径指旧根 |
| [CURRENT-STATE-GOALS-AGENTS.md](CURRENT-STATE-GOALS-AGENTS.md) | 4,160 B | 旧根 `CURRENT-STATE-GOALS-AGENTS.md` | 短状态、owner、下一步；长期 F6 目标已 paused | 历史交接；状态已冻结 |
| [AGENTS.md](AGENTS.md) | 18,317 B | 旧根 `AGENTS.md` | 旧根的代理规则、资源安全、Root/worker 职责 | 历史规则；不适用于本根 |
| [F6-TECHNICAL-PATHS.md](F6-TECHNICAL-PATHS.md) | 46,906 B | 旧根 `F6-TECHNICAL-PATHS.md` | F6 源码符号、consumer、坐标/owner/fence/history 分析 | 技术参考（路径需按新根重映射） |
| [BUILD-ENVIRONMENT.md](BUILD-ENVIRONMENT.md) | 3,308 B | 旧根 `BUILD-ENVIRONMENT.md` | 旧构建入口 `BUILD-DEPLOY.ps1 -Variant F6`、增量/部署说明 | **已过时**；以本根 `build.ps1` 为准 |
| [REGRESSION-HISTORY.md](REGRESSION-HISTORY.md) | 225,478 B | 旧根 `docs/REGRESSION-HISTORY.md` | 回归索引：失败签名、逐轮现象与结论 | 历史事实；裸路径按旧根解释 |
| [BRIDGE-SOURCE-REFERENCE.md](BRIDGE-SOURCE-REFERENCE.md) | 36,185 B | 旧根 `docs/BRIDGE-SOURCE-REFERENCE.md` | 原神 Bridge 上游跟版与源码映射 | 历史参考；行号属旧树 |
| [LAUNCHER-CLI.md](LAUNCHER-CLI.md) | 10,885 B | 旧根 `docs/LAUNCHER-CLI.md` | 启动器 CLI 接口与启动加载核对（V3-R042） | 历史接口导航，非运行授权 |
| [GENSHIN-FSR-BRIDGE-UPSTREAM-UPDATE.md](GENSHIN-FSR-BRIDGE-UPSTREAM-UPDATE.md) | 11,249 B | 旧根 `docs/GENSHIN-FSR-BRIDGE-UPSTREAM-UPDATE.md` | 原神 FSR Bridge 上游跟版维护流程 | 维护流程参考 |
| [OPTDRN1-v1.md](OPTDRN1-v1.md) | 12,583 B | 旧根 `docs/OPTDRN1-v1.md` | OPTDRN1 v1 日志格式与只读解码工具 | 工具格式说明 |
| [LOW-WORKSPACE-PREPARATION.md](LOW-WORKSPACE-PREPARATION.md) | 1,706 B | 旧根 `docs/LOW-WORKSPACE-PREPARATION.md` | `scripts/Prepare-LowWorkspace.ps1` 交接工作区准备流程 | 旧流程；`scripts/` 未迁移 |

合计 11 份，约 488 KB。

## 一并保留在文档层之外的入口

- 根 [README.md](../README.md)：项目概览、目录结构、构建方式、产物位置（**当前有效**）。
- 根 [CLI.md](../CLI.md)：启动器 CLI 要点（原样保留，其中仍写旧根的绝对路径 `…/docs/LAUNCHER-CLI.md`）。
- 根 [build.ps1](../build.ps1)：唯一构建入口（**当前有效**）。

## 未迁移的内容及原因

| 未迁移 | 原因 |
|---|---|
| `PERFORMANCE-ROUND-INDEX.md`（旧根根级，536 KB） | 逐轮性能索引，正文大量链接 `audit/…` 证据目录；`audit/` 未迁移，单独保留会得到大面积死链。需要时从旧根只读取用。 |
| `audit/`、`worktrees/`、`work/`、`_evidence/`、`recovered-temp/`、`错误/`、`_nextgen_ui/`、`references/`、`cleanup/` | 历史/临时/证据目录（合计数十 GB），不属于干净新根。 |
| `scripts/`、`BUILD-DEPLOY.ps1` | 旧构建/辅助脚本，已被本根 `build.ps1` 取代；保留会造成双入口歧义。 |
| 旧根 `README.md`、`.gitignore` | 已按新根结构重写，不复制。 |

## 已知链接失效（复制未改正文，属预期）

- `BRIDGE-SOURCE-REFERENCE.md`、`REGRESSION-HISTORY.md`、`LAUNCHER-CLI.md` 中的 `../HANDOFF.md`、`../CURRENT-STATE-GOALS-AGENTS.md`、`../PERFORMANCE-ROUND-INDEX.md`：这些文件现在与它们同处 `docs/`，或未迁移，链接可能失效。
- 正文中 `audit/…/RECORD.md`、`work/…`、`scripts/…` 之类的旧根路径在本根不存在。
- 处理原则：**读内容，不依赖链接**；需要当前事实时以本根 `README.md` + `build.ps1` 为准。
