# F6 NEXT

2026-11-12：**绝区零已按用户要求单独切到 `local/F6-ZZZ-ASI`；原神、星铁保持 `official/F6` 内置。隔离 DLL 283B6B07／源码97a2cad已构建和部署，真实加载与卡顿改善待重启游戏验收。HDR暂缓。** 详见[当前状态](docs/CURRENT-STATE.md)与[交接第0节](docs/F6-HANDOFF.md)。

唯一产品写树 `D:/CODE/DLSS5_NEXT-workers/f6-rewrite`，`codex/f6-rewrite`。绝专属配置保留SR／NR／FG、原6×参数，仅关闭内置Ada／Ampere解锁并从独立插件路径加载既有ASI。独立post-attach startup与FG有界等待保持SR／NR独立。旧正式F6、原神／星铁、四DLL硬链接、233项保护文件、全局选择和旧profiles未改；单游戏回执 `build/current-game-deployment-nap_cn.json`，共享部署仍由原 `current-deployment.json` 记录。

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

产物 `build/out/OptiScaler.dll`，UTF-8日志 `build/msbuild.log`。本次2431项冻结输入、948导出和ASI定向测试通过，证据 `build/repair-20261112/zzz-asi-*`；候选路径不能代替共享正式部署身份。旧0C7D6709及回执保留，构建／部署不证明游戏性能改善。

## 文档

接手先读[完整交接](docs/F6-HANDOFF.md)和[外部参考仓库](../../DLSS5_NEXT/docs/REFERENCE-REPOSITORIES.md)。[诊断操作](docs/F6-GUIDE-DIAGNOSTICS.md)、[F6状态](docs/F6-STATUS.md)和[运行核查](docs/F6-RUNTIME-REVIEW.md)保留专项说明与旧证据；历史内容不覆盖当前状态。保留独立SR/NR/FG语义、用户参数、外部桥2.4 ABI及内部FSR禁用策略。
