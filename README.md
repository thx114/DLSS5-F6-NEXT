# DLSS5_NEXT

更新：2026-11-12。**ADFF3486 经用户实测仍有ZZZ间歇性卡顿；1ABD9AFD诊断版已原位部署并核验。监视器旧报告选择已修正，新增native guide阻塞原因记录；真实卡顿根因及改善未证实。HDR暂缓。** 详见[产品当前状态](../DLSS5_NEXT-workers/f6-rewrite/docs/CURRENT-STATE.md)和[交接第0节](../DLSS5_NEXT-workers/f6-rewrite/docs/F6-HANDOFF.md)。

唯一产品写树 `D:/CODE/DLSS5_NEXT-workers/f6-rewrite`，分支 `codex/f6-rewrite`，本次源码提交 `51fdb7b`；本根 `D:/CODE/DLSS5_NEXT` 为 `codex/navigation-20261112` 导航／迁移基线。GitHub目标 `thx114/DLSS5-F6-NEXT`。

新固定日志PID190880（版本b1ee210）记录160次SDK插帧状态切换、159次最大呈现延迟变化，包含缺源后eOff／6→1及下一帧恢复eOn／1→6。原节流日志不能区分Reset未完成与真实producer fence未完成。新日志 `F6 native guide reuse blocked:` 只观察，保留原拒绝／退休门；不扩大池子、借旧帧或修改6×参数。Reflex选最新有效完成报告，缺实例凭据的Bridge／NR enrichment为unknown，独立NR计时链未改。

Release x64通过，2428项输入构建前后相同，53必要NGX及全部948导出一致，定向回归和独立复核通过。诊断SHA256 `1ABD9AFDDD605B3D55EF3466EDBDD7946DCA6FD8B362A4C2030A4FCC61DE9ABC`。正式及三个游戏缓存一致，原File ID／硬链接、2,535项受保护文件及profiles不变；桥保持CC301B42，旧ADFF3486已备份。退出保护及完整英文保留，HDR暂缓，HUDless/UI仍缺真实输入。

当前证据在产品树 `build/repair-20261112/zzz-followup-*`，部署核验 `zzz-followup-deploy-verified.json`；旧 `repair-current-*`／ADFF3486证据保留。用户手动启动ZZZ并复现后，再读版本51fdb7b的新局日志；不重复要求关闭FG对照。未查询或操控游戏；新版本性能、画面、退出与英文布局分别验收。旧根、其它工作树及build候选／回退／证据保留。

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

本根的同名 `build.ps1` 仍可构建迁移基线，但结果不能当作F6最新候选。本轮产物在产品树构建，并按用户要求原位部署；未改配置。实际加载、画面和性能仍需分别核验。

## 资料保留

`OptiScaler/` 与 `external/` 保持同级；[LICENSE](LICENSE)、[Licenses/](Licenses/)及各依赖许可证保留。`build/`被Git忽略，但内有候选、回退、运行证据和回执，不能整目录当临时文件删除。

旧根 `D:/DATA/Documents/ChatGPT/DLSS F5 NEXT` 与其它登记工作树保留；[docs/history/](docs/history/)按原历史上下文理解。导航和产品分别保存在 `codex/navigation-20261112`、`codex/f6-rewrite`，推送目标为用户授权的 `thx114/DLSS5-F6-NEXT`；远端提交身份见产品树 `build/repair-20261112/repair-current-git.json`。启动器历史接口见[CLI.md](CLI.md)。
