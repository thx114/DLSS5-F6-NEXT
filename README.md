# DLSS5_NEXT

更新：2026-11-12。**原神、星铁、绝区零的 `local/F6-ASI` 包及三个隔离 runtime 已原位部署 `A6D12031`，源码基于 `75d7a27` 的未提交工作树。F6 保留 SR／NR／FG、`External=false` 和各游戏 NR／6×请求。此前菜单点击／焦点已获用户初步通过反馈，新版加载、画面和周期卡顿尚待观察，HDR 暂缓。**

本轮源码已移除旧功能入口和实现，保留通用 ASI 插件加载；新建包工具只引用用户自备插件目录，不复制插件。本次仅更新既有包 DLL，没有复制或运行用户 ASI；2421项冻结构建输入、948个导出核验通过，四个DLL alias的新字节及原FileId／硬链接完整，1857项保护文件未变。源码、离线验证和部署证据见产品树 `build/repair-20261112/asi-only-*`。

唯一产品写树 `D:/CODE/DLSS5_NEXT-workers/f6-rewrite`，`codex/f6-rewrite`；本根仅导航／迁移基线。旧共享正式部署、旧绝专属包／runtimes、全局 state、用户配置／插件及回退保留。本次构建选择未变，由用户手动重开游戏加载新版；离线回归和磁盘部署不能替代游戏画面或性能验收。

详见[产品当前状态](../DLSS5_NEXT-workers/f6-rewrite/docs/CURRENT-STATE.md)和[完整交接](../DLSS5_NEXT-workers/f6-rewrite/docs/F6-HANDOFF.md)。

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

本根的同名 `build.ps1` 仍可构建迁移基线，但结果不能当作F6最新候选。本次三游戏均选择隔离ASI包，通用插件候选已部署；共享正式F6保留。此前菜单点击／焦点已有用户初步通过反馈；新版实际加载、画面和性能仍需分别核验。

## 资料保留

`OptiScaler/` 与 `external/` 保持同级；[LICENSE](LICENSE)、[Licenses/](Licenses/)及各依赖许可证保留。`build/`被Git忽略，但内有候选、回退、运行证据和回执，不能整目录当临时文件删除。

旧根 `D:/DATA/Documents/ChatGPT/DLSS F5 NEXT` 与其它登记工作树保留；[docs/history/](docs/history/)按原历史上下文理解。导航和产品分别保存在 `codex/navigation-20261112`、`codex/f6-rewrite`，推送目标为用户授权的 `thx114/DLSS5-F6-NEXT`；远端提交身份见产品树 `build/repair-20261112/menu-input-git.json`。启动器历史接口见[CLI.md](CLI.md)。
