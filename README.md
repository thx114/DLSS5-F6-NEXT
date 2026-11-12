# F6 NEXT

2026-11-12：**新局确认四呈现槽与producer录制生命周期耦合，已修recording资源所有权。候选0C7D6709通过Release x64、导出、定向回归和独立复核，尚未部署；实际卡顿改善待验收，HDR暂缓。** 详见[当前状态](docs/CURRENT-STATE.md)与[交接第0节](docs/F6-HANDOFF.md)。

唯一产品写树 `D:/CODE/DLSS5_NEXT-workers/f6-rewrite`，`codex/f6-rewrite`，候选源码 `8df406c4715d`。正式及三缓存仍1ABD9AFD，桥CC301B42；本轮AGENTS要求不改正式部署或profiles。FG内置，SR／NR／FG语义、6×参数和监视报告策略保留。

PID185668已输出2328次busy采样均Reset pending，594条详细行after_refusal fence采样均达target；FG启／停及最大延迟1↔6各1510次。修复让GPU转换资源随真实recording保留至Reset＋自身producer fence且CPU释放；四槽只保留当前身份，未扩大BUFFER_COUNT、删退休门或借旧帧。

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

产物 `build/out/OptiScaler.dll`，UTF-8日志 `build/msbuild.log`。本次冻结2428项输入，核验全部948个导出；定向验证和独立复核见 `build/repair-20261112/zzz-recordings-*`。候选0C7D6709尚未部署，旧1ABD9AFD、ADFF3486和回执保留。构建、部署、实际加载、画面和性能分别验收；本次尚未证明卡顿改善。

## 文档

接手先读[完整交接](docs/F6-HANDOFF.md)和[外部参考仓库](../../DLSS5_NEXT/docs/REFERENCE-REPOSITORIES.md)。[诊断操作](docs/F6-GUIDE-DIAGNOSTICS.md)、[F6状态](docs/F6-STATUS.md)和[运行核查](docs/F6-RUNTIME-REVIEW.md)保留专项说明与旧证据；历史内容不覆盖当前状态。保留独立SR/NR/FG语义、用户参数、外部桥2.4 ABI及内部FSR禁用策略。
