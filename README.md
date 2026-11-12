# F6 NEXT

2026-11-12：**原神、星铁、绝区零均已选择 `local/F6-ASI`，三游戏隔离 runtime 已部署 `6D377DD4`／源码 `3665de5`。ASI 负责 MFG 解锁，F6 继续负责 SR／NR／FG，`External=false`；原 NR 版本与 6×请求保留。菜单输入候选已通过离线回归，冷开点击与周期卡顿仍待实机验收，HDR 暂缓。** 详见[当前状态](docs/CURRENT-STATE.md)与[交接第0节](docs/F6-HANDOFF.md)。

唯一产品写树 `D:/CODE/DLSS5_NEXT-workers/f6-rewrite`，`codex/f6-rewrite`。新包与三 runtime 的 DLL 原位更新并保留硬链接；旧共享正式 F6、旧绝专属包、旧 runtimes、全局 state 和回退均保留。激活新选择须从托盘完整退出启动器，再重新打开；仅 CLI 写 DB 或关窗口不刷新旧 GUI 缓存。隔离部署回执与共享 `current-deployment.json` 分开。

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
python OptiScaler/menu/tests/test_menu_input.py
pwsh -NoProfile -File OptiScaler/framegen/dlssg/tests/run-native-guide-notifications.ps1
pwsh -NoProfile -File OptiScaler/runtime/tests/run-module-lifetime.ps1
python OptiScaler/runtime/tests/test_cleanup_entrypoints.py
python OptiScaler/hooks/tests/test_fg_hook_install.py --cpu
pwsh -NoProfile -File ./tools/tests/Run-CommonCoreTests.ps1
```

产物 `build/out/OptiScaler.dll`，UTF-8日志 `build/msbuild.log`。本次2435项声明输入及948导出核验通过；菜单51,959项检查、8个执行负控和调用清单负控、部署42项检查通过。证据 `build/repair-20261112/menu-input-*`；旧ASI和recording证据、回退均保留。离线验证与磁盘部署不证明游戏内点击、实际显示或性能。

## 文档

接手先读[完整交接](docs/F6-HANDOFF.md)和[外部参考仓库](../../DLSS5_NEXT/docs/REFERENCE-REPOSITORIES.md)。[诊断操作](docs/F6-GUIDE-DIAGNOSTICS.md)、[F6状态](docs/F6-STATUS.md)和[运行核查](docs/F6-RUNTIME-REVIEW.md)保留专项说明与旧证据；历史内容不覆盖当前状态。保留独立SR/NR/FG语义、用户参数、外部桥2.4 ABI及内部FSR禁用策略。
