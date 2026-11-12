# DLSS5_NEXT

更新：2026-11-12。**绝区零已按最新用户指令独立改用 ASI，原神、星铁保持内置。** 新绝专属 `local/F6-ZZZ-ASI`／DLL283B6B07／源码97a2cad已构建、定向验证和隔离部署；runtime为`A5009284E9948E8D`，真实加载与卡顿改善待重启绝区零验收，HDR暂缓。详见[产品当前状态](../DLSS5_NEXT-workers/f6-rewrite/docs/CURRENT-STATE.md)和[完整交接](../DLSS5_NEXT-workers/f6-rewrite/docs/F6-HANDOFF.md)。

唯一产品写树 `D:/CODE/DLSS5_NEXT-workers/f6-rewrite`，`codex/f6-rewrite`；本根保留导航／迁移基线。启动器仅改nap_cn一个构建选择键，其他两游戏仍`official/F6`；共享1ABD9AFD四DLL原硬链接、233项保护文件、全局state和旧profiles未改。绝专属配置`External=false`，两个内置解锁关闭，既有ASI171AC18D从独立插件目录加载；SR／NR、原6×及NR版本保持。原FSR桥CC301B42未动。

新生产ASI在post-attach worker初始化，FG有界等待保留SR／NR独立；Gate28、loader context5、options1132、builtin41、Reflex19、7种无害Windows loader与16种有效SL请求组合通过，2431输入和948导出核验，独立gpt-6.1-sol/high终审无确定blocker。未操控或查询游戏，不称卡顿已修好。单游戏回执`current-game-deployment-nap_cn.json`与共享`current-deployment.json`分开；不把build/out新文件冒充共享已部署。

旧recording修复8df406c及未部署候选0C7D6709、旧日志和回退保留。GitHub目标仍`thx114/DLSS5-F6-NEXT`。新证据为产品`build/repair-20261112/zzz-asi-*`，详细约束以产品当前状态为准。

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

本根的同名 `build.ps1` 仍可构建迁移基线，但结果不能当作F6最新候选。本次仅新增绝区零隔离ASI部署；共享正式F6和其他游戏未改。实际加载、画面和性能仍需分别核验。

## 资料保留

`OptiScaler/` 与 `external/` 保持同级；[LICENSE](LICENSE)、[Licenses/](Licenses/)及各依赖许可证保留。`build/`被Git忽略，但内有候选、回退、运行证据和回执，不能整目录当临时文件删除。

旧根 `D:/DATA/Documents/ChatGPT/DLSS F5 NEXT` 与其它登记工作树保留；[docs/history/](docs/history/)按原历史上下文理解。导航和产品分别保存在 `codex/navigation-20261112`、`codex/f6-rewrite`，推送目标为用户授权的 `thx114/DLSS5-F6-NEXT`；远端提交身份见产品树 `build/repair-20261112/repair-current-git.json`。启动器历史接口见[CLI.md](CLI.md)。
