# DLSS5_NEXT

2026-11-12：**普通内置 FG 修复版本 5265915D 已按用户授权部署，待实机验证。** 正式 DLL 与三个游戏缓存的 SHA256 一致，硬链接不变；FSR 桥 CC301B42 保持已部署。设备身份路由、生命周期重入和星铁锁外 pacing 的定向回归及独立静态复核完成，详见[交接第 0 节](docs/F6-HANDOFF.md)。FG 使用内置实现，新版本跳过残留 MFGUnlocker.asi。产品 HEAD 9ab9686，既有未提交改动保留，不 reset/clean。

**部署前用户反馈：崩铁 OK，绝区零仍未检测到 DLSS。** ZZZ PID180352 固定日志显示游戏确实运行了 DLSS，两个句柄均被旧版 8FCB8632 归为 backend。这局不能作为刚部署的 5265915D 的验收证据。详情见[交接第 0 节](docs/F6-HANDOFF.md)。

本次 F6 回退为原安装 8FCB8632，精确备份与核验见 build/current-deployment.json；2,535 项受保护文件哈希不变，未改配置／profiles 或操控游戏。旧版 ASI 运行反馈不能认证新版本内置 FG。current-artifact/build/tests 对应 5265915D；原冻结清单遗漏 FgDepthContract.h，身份验证边界保留于交接。

保留 SR J/K/L/M、FG A/B/C、外部桥 HUD 对接约定及已验证的共享资源/队列交接。细节保留、轻微 FG 残影、GPU 卡死、HUD 保护、真实显示率与延迟分别验收。

原神默认 **真实源帧呈现输入 → NR → 细节保留 → FG**；原有单选条现可实际切换 SR 前／SR 后入口作对照。内部 FSR 桥继续禁用，保留外部桥 2.4 ABI、用户参数、独立 NR／FG 坐标设置及真实 queue/fence/Reset 退休门。

OptiScaler F6，Windows x64 / C++ / MSVC。唯一产品写树 D:/CODE/DLSS5_NEXT-workers/f6-rewrite，codex/f6-rewrite，HEAD 9ab9686，历史累计保存点 ea8c304；最新改动尚未提交。build/repair-20261112/verification-next/source.json 记录 1,245 项声明输入；旧 input-integration／queue-latency 指纹保留原身份。旧根与恢复资料保留。

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

当前仅用普通构建与内置 MfgUnlock.cpp；私有 ASI 参数、工程与启动入口已移除，残留源码／离线测试保留历史。普通 Release x64 已重新链接，核验 53 必要 NGX 导出、PE 版本及声明输入指纹；构建不等于部署或游戏通过，详情见交接第 0 节。

此前已实施 FG 源帧锁外 pacing、集中 SDK 统计、waitable 所有权与独立全分辨率 GPU 单选预览。历史构建、测试和部署身份分别以 `build/current-{build,tests,artifact,deployment}.json` 为准。旧局录像的编码帧率及 SDK 回调频率均不能代替真实显示率。细节持续保留的栅栏/人物跟随仍需复测，使用 [深度与运动预览](docs/F6-GUIDE-DIAGNOSTICS.md)；桥作者可参照 [HUD 输入约定](docs/FG-BRIDGE-HUD-CONTRACT.md)。

## 文档

接手先读 [完整交接](docs/F6-HANDOFF.md)和 [外部参考仓库](../../DLSS5_NEXT/docs/REFERENCE-REPOSITORIES.md)，再读 [当前状态](docs/CURRENT-STATE.md)；[F6状态](docs/F6-STATUS.md)与 [运行核查](docs/F6-RUNTIME-REVIEW.md)保留操作边界、实测证据及未确定项。[docs/history/](docs/history/)中的旧路径与流程仅供历史查阅。维护简短当前说明，不新增逐轮证据目录；保留源码、许可和用户profiles。
