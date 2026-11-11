# DLSS5_NEXT

用户已确认 **7C281A54** 解决 NR＋FG 的卡顿/旧帧闪回；细节持续保留、旋转发白与卡死尚未验收。正式部署仍为 **81DBE9A5**，已有 PID153992/164004 新局日志。2026-11-12 候选 **DD53514B 已构建、未部署**：修正 Look 诊断视图冲突，计时器复用真实 recording fence，移除重复 Signal 并增加低频 GPU 分段。按用户要求改用 **gpt-6.1-sol / high** 检索和独立复核，主会话核对、修改，详见 [性能审查](docs/F6-PERFORMANCE-REVIEW.md)。已定位复制 reader 与隐藏 Present 的长等待，尚未验收延迟改善。

保留 SR J/K/L/M、FG A/B/C、外部桥 HUD 对接约定及已验证的共享资源/队列交接。细节保留、轻微 FG 残影、GPU 卡死、HUD 保护、真实显示率与延迟分别验收。

原神默认 **真实源帧呈现输入 → NR → 细节保留 → FG**；原有单选条现可实际切换 SR 前／SR 后入口作对照。内部 FSR 桥继续禁用，保留外部桥 2.4 ABI、用户参数、独立 NR／FG 坐标设置及真实 queue/fence/Reset 退休门。

OptiScaler F6 隔离工作树，Windows x64 / C++ / MSVC。工作树 `D:/CODE/DLSS5_NEXT-workers/f6-rewrite`，分支 `codex/f6-rewrite`，HEAD 基线 `db0e646`；改写尚未提交。原根 `D:/CODE/DLSS5_NEXT`、旧工作区与历史恢复资料继续保留。

## 目录

| 路径 | 内容 |
|---|---|
| [OptiScaler/](OptiScaler/) | 产品源码与 MSBuild 工程 |
| [external/](external/) | 第三方头文件、库和许可说明，与源码保持同级 |
| [build.ps1](build.ps1) | Release x64 构建入口 |
| `build/` | obj/out/temp、构建与测试日志、运行日志；Git 忽略 |
| [tools/shaders/](tools/shaders/) | Look shader 再生成 recipe；普通 DLL 构建无需运行 |
| [docs/](docs/) | 简短当前说明与历史交接 |
| [LICENSE](LICENSE)、[Licenses/](Licenses/) | 主许可证与第三方署名 |
| [AGENTS.md](AGENTS.md) | 协作、模型分工和产品边界 |

## 构建与测试

需要 PowerShell 7 和 MSVC v143。本机 MSBuild 位于 `D:/VS2022BuildTools/MSBuild/Current/Bin/amd64/MSBuild.exe`。

```powershell
pwsh -NoProfile -File ./build.ps1
pwsh -NoProfile -File ./build.ps1 -Clean -Jobs 12
pwsh -NoProfile -File ./tools/tests/Run-CommonCoreTests.ps1
```

产物为 [build/out/OptiScaler.dll](build/out/OptiScaler.dll)，日志见 [msbuild.log](build/msbuild.log) 与 [common-core-tests.log](build/common-core-tests.log)；构建失败返回真实 MSBuild 退出码。

此前已实施 FG 源帧锁外 pacing、集中 SDK 统计、waitable 所有权与独立全分辨率 GPU 单选预览。最新构建、测试和部署身份分别以 `build/current-{build,tests,artifact,deployment}.json` 为准。旧局录像的编码帧率及 SDK 回调频率均不能代替真实显示率。细节持续保留的栅栏/人物跟随仍需复测，使用 [深度与运动预览](docs/F6-GUIDE-DIAGNOSTICS.md)；桥作者可参照 [HUD 输入约定](docs/FG-BRIDGE-HUD-CONTRACT.md)。

## 文档

接手先读 [当前状态](docs/CURRENT-STATE.md)；[F6转交](docs/F6-HANDOFF.md)、[F6状态](docs/F6-STATUS.md)与 [运行核查](docs/F6-RUNTIME-REVIEW.md)保留操作边界、实测证据及未确定项。[docs/history/](docs/history/)中的旧路径与流程仅供历史查阅。维护简短当前说明，不新增逐轮证据目录；保留源码、许可和用户profiles。
