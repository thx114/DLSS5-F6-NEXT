# F6 NEXT

2026-11-12：**ADFF3486 经用户实测仍有 ZZZ 周期卡顿；1ABD9AFD 诊断版已部署。已修监视器旧报告选择并增加guide阻塞状态，卡顿根因尚未证实。HDR暂缓。** 详见[当前状态](docs/CURRENT-STATE.md)与[交接第0节](docs/F6-HANDOFF.md)。

唯一产品写树 `D:/CODE/DLSS5_NEXT-workers/f6-rewrite`，分支 `codex/f6-rewrite`，本次源码提交 `51fdb7b19f09`。正式及三个游戏缓存均为1ABD9AFD，原文件身份／硬链接、用户配置及profiles不变；FSR桥保持CC301B42。FG使用内置实现。退出保护、完整英文已保留，相关游戏验收独立记录。

新局PID190880确认selected DLSS输入，也记录FG反复启停及呈现延迟1↔6切换；槽忙的Reset／fence原因需要新诊断日志核实。Reflex选最新有效完成报告，缺报告实例凭据的Bridge／NR enrichment保持unknown；独立NR计时未改。桥缺独立HUDless/UI ABI，见[输入契约](docs/FG-BRIDGE-HUD-CONTRACT.md)。

## 目录

| 路径 | 内容 |
|---|---|
| [OptiScaler/](OptiScaler/) | 产品源码、MSBuild工程及定向测试 |
| [external/](external/) | 同级真实依赖及许可 |
| [build.ps1](build.ps1) | Release x64构建入口，默认最多8个编译工作进程 |
| `build/` | Git忽略的候选、回退、日志和运行证据；不能整目录删除 |
| [tools/shaders/](tools/shaders/) | 既有shader recipe，普通构建无需重新生成 |
| [docs/](docs/) | 当前说明与历史交接 |
| [LICENSE](LICENSE)、[Licenses/](Licenses/) | 主许可证与第三方署名 |
| [AGENTS.md](AGENTS.md) | 协作及产品边界 |

## 构建与测试

需要PowerShell7和MSVC v143；本机MSBuild位于 `D:/VS2022BuildTools/MSBuild/Current/Bin/amd64/MSBuild.exe`。

```powershell
pwsh -NoProfile -File ./build.ps1
python OptiScaler/menu/tests/test_ui_localization.py --runtime
pwsh -NoProfile -File OptiScaler/framegen/dlssg/tests/run-native-guide-notifications.ps1
pwsh -NoProfile -File OptiScaler/runtime/tests/run-module-lifetime.ps1
python OptiScaler/runtime/tests/test_cleanup_entrypoints.py
python OptiScaler/hooks/tests/test_fg_hook_install.py --cpu
pwsh -NoProfile -File ./tools/tests/Run-CommonCoreTests.ps1
```

产物 `build/out/OptiScaler.dll`，UTF-8日志 `build/msbuild.log`。本次冻结2428项输入，核验全部948个导出；定向验证和独立复核见 `build/repair-20261112/zzz-followup-*`，部署核验 `zzz-followup-deploy-verified.json`。旧ADFF3486及回执保留。构建、部署、实际加载、画面和性能分别验收；本次尚未证明卡顿改善。

## 文档

接手先读[完整交接](docs/F6-HANDOFF.md)和[外部参考仓库](../../DLSS5_NEXT/docs/REFERENCE-REPOSITORIES.md)。[诊断操作](docs/F6-GUIDE-DIAGNOSTICS.md)、[F6状态](docs/F6-STATUS.md)和[运行核查](docs/F6-RUNTIME-REVIEW.md)保留专项说明与旧证据；历史内容不覆盖当前状态。保留独立SR/NR/FG语义、用户参数、外部桥2.4 ABI及内部FSR禁用策略。
