# DLSS5 F6 NEXT

基于 OptiScaler / F5 的实验性 Windows x64 分支，整合超分辨率（SR）、
Neural Rendering（NR）、细节持续保留与帧生成（FG）。

原神默认链路为：真实源帧呈现输入 → NR → 细节持续保留 → FG。
保留 SR 前／SR 后 NR 入口供对照，NR 与 FG 的配置独立。
F6 内部 FSR 桥及其探测保持禁用；实际深度和运动输入来自 DLSS/NGX Evaluate，
保留外部 NGX FSR 桥兼容接口。

## 当前功能与状态

- SR 模型预设 J / K / L / M，FG 模型预设 A / B / C；实际可用性取决于所用运行时与硬件。
- NR 多层与 Look 控件、源帧颜色汇合，以及独立的深度／运动／颜色 GPU 预览。
- 细节持续保留的容差、最终稳定滑条与每源帧调试界面。
- 呈现 NR 与 FG 的主要卡顿／旧帧闪回问题已有用户测试改善反馈。

本分支仍在开发中：细节持续保留、转镜头发白／噪声变化、卡死恢复、
Look 实际效果、轻微 FG 残影、HUD 保护及延迟改善仍需实机验证。
没有 HUD 输入时 UI 未受保护。构建或离线测试通过不等于游戏画质、
真实显示帧率或性能已经通过验收。

## 构建

需要 Windows x64、PowerShell 7、Visual Studio 2022 的 MSVC v143 C++ 工具和 Windows SDK。
测试及 shader 再生成还需要 Python 3。
`OptiScaler/` 与 `external/` 必须保持同级，仓库已包含当前构建使用的依赖。

```powershell
pwsh -NoProfile -File ./build.ps1
pwsh -NoProfile -File ./build.ps1 -Clean -Jobs 8
```

构建脚本从 PATH 或 Visual Studio Installer 的 `vswhere` 查找 MSBuild，
也可通过 `-MSBuildPath` 显式指定。输出为 `build/out/OptiScaler.dll`，
日志为 `build/msbuild.log`。普通构建使用已有嵌入 shader，无须先重新生成。
构建不会自动部署或修改游戏配置。本仓库不附带已部署成品、模型或游戏文件。

## 离线测试与开发工具

```powershell
pwsh -NoProfile -File ./tools/tests/Run-CommonCoreTests.ps1
```

测试入口自动寻找 C++ 工具链，也接受 `-DevCmd` 与 `-Python`。
单独运行测试脚本时，可先把 `VSDEVCMD` 环境变量设为 `VsDevCmd.bat` 的完整路径。
测试使用合成输入、CPU 夹具与软件 WARP，不需要私有采集文件。
运行时观测工具见 [tools/runtime](tools/runtime/README.md)，
Look shader 再生成见 [tools/shaders](tools/shaders/README.md)。

## 许可与署名

OptiScaler 主体遵循 [GPL-3.0](LICENSE)。
第三方代码、字体、SDK 与工具保留各自许可，参见 [Licenses](Licenses/README.md)、
`external/` 及各第三方子目录。
本项目是社区分支，与 NVIDIA、AMD、Intel 及游戏开发商没有隶属关系。
