# DLSS5_NEXT

更新：2026-11-12。**ZZZ新局确认native四呈现槽与producer recording生命周期耦合；源码8df406c4715d已修所有权，候选0C7D6709构建／测试／独立复核通过，尚未部署。卡顿改善待实机验收，HDR暂缓。** 详见[产品当前状态](../DLSS5_NEXT-workers/f6-rewrite/docs/CURRENT-STATE.md)和[交接第0节](../DLSS5_NEXT-workers/f6-rewrite/docs/F6-HANDOFF.md)。

唯一产品写树 `D:/CODE/DLSS5_NEXT-workers/f6-rewrite`，`codex/f6-rewrite`；本根是 `codex/navigation-20261112` 导航／迁移基线。GitHub目标 `thx114/DLSS5-F6-NEXT`。正式四路径仍1ABD9AFD／51fdb7b，桥CC301B42，硬链接、配置和profiles未改；按本轮AGENTS不改正式部署。

PID185668固定日志108,242,264 B／1,267,211行，版本51fdb7b：2328次已输出busy采样全为Reset pending，594条详细行after_refusal fence采样均达target；FG启／停及最大呈现延迟1→6／6→1各1510次。源list复用帧距不等于Reset间隔，完整卡顿根因仍有验收边界。新实现分开真实录制GPU owner与四槽当前身份，保留Reset／自身queue/fence／source、execution与失败保留门。

候选SHA256 `0C7D6709A0364761BA05C9AD03E3B7EA3F2D13ABCF8BF3FD1A9DDBF712425A6E`，Release x64；2428项输入／948导出和定向回归通过。证据在产品 `build/repair-20261112/zzz-recordings-*`，日志分析 `evidence.json`；candidate指针不代表正式已部署。未查询或操控游戏；用户安排部署后手动验收，不重复关闭FG对照。旧候选、回退、其它工作树及历史证据保留。

| 入口 | 用途 |
|---|---|
| [当前状态](docs/CURRENT-STATE.md)与[交接入口](docs/F6-HANDOFF.md) | 主根／产品树边界、候选与验收 |
| [Bridge／GIMI审阅](docs/UPSTREAM-BRIDGE-REVIEW.md) | 固定上游源码和采用限制 |
| [共享逆向研究](tools/research/encore-f7/README.md) | Encore／历史NR的静态证据、数学参考与独立测试 |
| [文档索引](docs/INDEX.md)与[协作规则](AGENTS.md) | 导航、分工与生命周期约束 |

## 构建与验证

在产品工作树执行，避免误构建本根旧产品快照：

```powershell
Set-Location D:/CODE/DLSS5_NEXT-workers/f6-rewrite
pwsh -NoProfile -File ./build.ps1
pwsh -NoProfile -File ./tools/tests/Run-CommonCoreTests.ps1
```

需要 PowerShell 7、MSVC v143；本机 MSBuild 为 `D:/VS2022BuildTools/MSBuild/Current/Bin/amd64/MSBuild.exe`。默认 Release x64、最多8个编译工作进程，可传 `-Jobs N`；`-Clean`仅在明确需要清中间件时使用。产物 `build/out/OptiScaler.dll`、UTF-8日志 `build/msbuild.log`；失败返回真实退出码。普通DLL构建使用现有嵌入shader头，不必运行[Look生成工具](tools/shaders/README.md)。

本根的同名 `build.ps1` 仍可构建迁移基线，但结果不能当作F6最新候选。本轮产物在产品树构建为未部署候选；正式DLL和配置未改。实际加载、画面和性能仍需分别核验。

## 资料保留

`OptiScaler/` 与 `external/` 保持同级；[LICENSE](LICENSE)、[Licenses/](Licenses/)及各依赖许可证保留。`build/`被Git忽略，但内有候选、回退、运行证据和回执，不能整目录当临时文件删除。

旧根 `D:/DATA/Documents/ChatGPT/DLSS F5 NEXT` 与其它登记工作树保留；[docs/history/](docs/history/)按原历史上下文理解。导航和产品分别保存在 `codex/navigation-20261112`、`codex/f6-rewrite`，推送目标为用户授权的 `thx114/DLSS5-F6-NEXT`；远端提交身份见产品树 `build/repair-20261112/repair-current-git.json`。启动器历史接口见[CLI.md](CLI.md)。
