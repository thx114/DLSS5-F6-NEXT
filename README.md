# DLSS5_NEXT

OptiScaler F6，Windows x64 / C++ / MSVC。**当前产品开发和构建在 `D:/CODE/DLSS5_NEXT-workers/f6-rewrite`（`codex/f6-rewrite`）**；本根 `master` 保存迁移基线、导航与共享研究，未合并该工作树最新产品改写。

2026-11-12：**F6 内置 FG 修复版 5265915D 与 FSR 桥 CC301B42 均已按用户授权部署，待新局实测。** 正式 F6 与三个游戏缓存 SHA 一致，硬链接不变；旧 DLL 已备份，2,535 项受保护文件未变。部署前用户反馈崩铁 OK、ZZZ 未检测 DLSS，旧局证据不能代替新版本验收。产品树 HEAD 9ab9686，既有未提交改动保留。接手先读[交接第 0 节](../DLSS5_NEXT-workers/f6-rewrite/docs/F6-HANDOFF.md)。

| 入口 | 用途 |
|---|---|
| [完整项目交接](../DLSS5_NEXT-workers/f6-rewrite/docs/F6-HANDOFF.md) | 架构、关键代码、候选／部署身份、测试、日志、回退与接手顺序 |
| [外部参考仓库](docs/REFERENCE-REPOSITORIES.md) | 本地位置、固定版本、已参考／已采用及待参考功能 |
| [当前状态](docs/CURRENT-STATE.md) | 主根／产品树边界与当前摘要 |
| [Bridge／GIMI审阅](docs/UPSTREAM-BRIDGE-REVIEW.md) | 固定上游源码和采用限制 |
| [共享逆向研究](tools/research/encore-f7/README.md) | Encore／历史NR的有界静态证据、数学参考与独立测试 |
| [文档索引](docs/INDEX.md) | 当前入口及历史审计 |
| [协作规则](AGENTS.md) | 写树、模型和真实GPU生命周期约束 |

## 构建与验证

在产品工作树执行，避免误构建本根旧产品快照：

```powershell
Set-Location D:/CODE/DLSS5_NEXT-workers/f6-rewrite
pwsh -NoProfile -File ./build.ps1
pwsh -NoProfile -File ./tools/tests/Run-CommonCoreTests.ps1
```

需要 PowerShell 7、MSVC v143；本机 MSBuild 为 `D:/VS2022BuildTools/MSBuild/Current/Bin/amd64/MSBuild.exe`。默认 Release x64、最多8个编译工作进程，可传 `-Jobs N`；`-Clean`仅在明确需要清中间件时使用。产物 `build/out/OptiScaler.dll`、UTF-8日志 `build/msbuild.log`；失败返回真实退出码。普通DLL构建使用现有嵌入shader头，不必运行[Look生成工具](tools/shaders/README.md)。

本根的同名 `build.ps1` 仍可构建迁移基线，但结果不能当作F6最新候选。构建、部署、实际加载、画面和性能分别核验；本次报告／Git整理未构建或部署产品、未改配置。

## 资料保留

`OptiScaler/` 与 `external/` 保持同级；[LICENSE](LICENSE)、[Licenses/](Licenses/)及各依赖许可证保留。`build/`被Git忽略，但内有候选、回退、运行证据和回执，不能整目录当临时文件删除。

旧根 `D:/DATA/Documents/ChatGPT/DLSS F5 NEXT` 与其它登记工作树保留；[docs/history/](docs/history/)按原历史上下文理解。Git整理只保存主树和F6树的本地提交，不推送、不合并、不清其它树。启动器历史接口见[CLI.md](CLI.md)。
