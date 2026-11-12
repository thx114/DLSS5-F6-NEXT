# DLSS5_NEXT

更新：2026-11-12。**产品修复候选ADFF3486已构建、未部署；HDR暂缓。** 当前工作包含FG排队锁环修复、退出入口保护和完整英文，详见[产品当前状态](../DLSS5_NEXT-workers/f6-rewrite/docs/CURRENT-STATE.md)与[交接第0节](../DLSS5_NEXT-workers/f6-rewrite/docs/F6-HANDOFF.md)。

唯一产品写树 `D:/CODE/DLSS5_NEXT-workers/f6-rewrite`，`codex/f6-rewrite`，源码提交 `b1ee21020a58`。本根 `D:/CODE/DLSS5_NEXT` 为 `codex/navigation-20261112` 导航／迁移基线，不在本根构建最新产品。GitHub目标 `thx114/DLSS5-F6-NEXT`，main未改。

ZZZ旧局186740已出现selected DLSS接入；用户实测关闭FG后周期卡顿消失，仍有GPU等待。新修复、退出和英文布局尚待游戏验收。16组定向验证及Release x64通过；2425项输入构建前后相同，53必要NGX导出及全部948导出核验一致。新候选SHA256 `ADFF3486F21C52FCD0FE72D73A14BB3AFA93E599A2A73AD72DCC7EAD1F790372`。

正式F6保持5265915D，桥保持CC301B42。本轮不写正式部署或profiles，不查询、启动或操控游戏。HUDless/UI缺真实桥输入，不以SR颜色或reactive/transparency mask伪造。

回执在产品树 `build/repair-20261112/repair-current-*`，构建指针与安装指针分开。HDR已归档撤出候选。保留真实queue/fence/Reset、独立SR/NR/FG语义及用户参数；不扩大池子或借旧帧。旧根、其它工作树、build候选／回退／证据和许可证全部保留。来源见[仓库清单](docs/REFERENCE-REPOSITORIES.md)。

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

本根的同名 `build.ps1` 仍可构建迁移基线，但结果不能当作F6最新候选。本轮候选在产品树构建，未部署、未改配置；实际加载、画面和性能仍需分别核验。

## 资料保留

`OptiScaler/` 与 `external/` 保持同级；[LICENSE](LICENSE)、[Licenses/](Licenses/)及各依赖许可证保留。`build/`被Git忽略，但内有候选、回退、运行证据和回执，不能整目录当临时文件删除。

旧根 `D:/DATA/Documents/ChatGPT/DLSS F5 NEXT` 与其它登记工作树保留；[docs/history/](docs/history/)按原历史上下文理解。导航和产品分别保存在 `codex/navigation-20261112`、`codex/f6-rewrite`，推送目标为用户授权的 `thx114/DLSS5-F6-NEXT`；远端提交身份见产品树 `build/repair-20261112/repair-current-git.json`。启动器历史接口见[CLI.md](CLI.md)。
