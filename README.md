# F6 NEXT

2026-11-12：**原神、星铁、绝区零的 `local/F6-ASI` 包及三个隔离 runtime 已原位部署 `A6D12031`，源码基于 `75d7a27` 的未提交工作树。F6 继续负责 SR／NR／FG，`External=false`；用户配置、既有 ASI、各游戏 NR 与 6×请求保留。此前菜单点击／焦点已获用户初步通过反馈，新版游戏加载与周期卡顿尚待观察，HDR 暂缓。** 详见[当前状态](docs/CURRENT-STATE.md)与[交接第0节](docs/F6-HANDOFF.md)。

本轮源码已移除旧功能入口和实现，保留通用 ASI 插件加载；新建包工具只引用用户自备插件目录，不复制插件。本次仅更新既有包 DLL，2421 项冻结构建输入、948 个导出核验通过，部署后四个 DLL alias 的哈希及原 FileId／硬链接均正确，1857 项保护文件未变。冻结身份、离线验证和本次部署回执见 `build/repair-20261112/asi-only-*`。

唯一产品写树 `D:/CODE/DLSS5_NEXT-workers/f6-rewrite`，`codex/f6-rewrite`。旧共享正式 F6、旧绝专属包、旧 runtimes、全局 state 和回退均保留。构建选择未变，由用户手动重开游戏加载新 DLL；当前加载、画面和性能尚未验证。隔离部署回执与共享 `current-deployment.json` 分开。

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

产物 `build/out/OptiScaler.dll`，UTF-8日志 `build/msbuild.log`。当前候选通用 ASI 224 项检查／22 个无害夹具场景、部署工具11项测试及普通 FG 定向回归通过；最终本地化静态清单为1999词条、2313处中文。此前菜单51,959项检查及负控记录在 `build/repair-20261112/menu-input-*`，旧证据、回退均保留。离线验证与磁盘部署不证明实际显示或性能。

## 文档

接手先读[完整交接](docs/F6-HANDOFF.md)和[外部参考仓库](../../DLSS5_NEXT/docs/REFERENCE-REPOSITORIES.md)。[诊断操作](docs/F6-GUIDE-DIAGNOSTICS.md)、[F6状态](docs/F6-STATUS.md)和[运行核查](docs/F6-RUNTIME-REVIEW.md)保留专项说明与旧证据；历史内容不覆盖当前状态。保留独立SR/NR/FG语义、用户参数、外部桥2.4 ABI及内部FSR禁用策略。
