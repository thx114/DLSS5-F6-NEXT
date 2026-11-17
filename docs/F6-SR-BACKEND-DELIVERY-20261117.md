# FSR／XeSS 输出与后端选择修复

2026-11-17；父提交 `8e5d37c2358cebb5ad38ca00ebf3fac6157f166c`。用户报告原神、星铁、绝区零选择 FSR／XeSS 后失效或退回 DLSS。本轮修复公共输出契约和 DX11 已提交选择的生命周期；隔离候选已经构建并完成实际 SDK 宿主验证，未部署，三游戏画面与性能仍待手动验收。

## 问题与实现

1. 旧公共接口用 bool 同时表达“SDK 已记录输出”和“请求重建但没有输出”，另由默认 false 的 `SuccessfulEvaluateRecordsOutput()` 决定能否提供输出回执。只有 DLSS／DLSSD 显式覆盖，FSR／XeSS 即使执行成功，TAA／桥也可能拿不到当前帧输出。本轮改为调用内的 `FeatureEvaluation::{Failed, NoOutput, OutputRecorded}`，转换全部 11 个 DX11／DX12 实际 provider。SDK dispatch 成功才返回 OutputRecorded，重建早退返回 NoOutput；只有当前输出和最终处理链均完整才提供输出回执。
2. DX11 菜单之前只写共享草稿，尺寸／曝光等重建会再次自动选择后端，可能回到 DLSS。现在仅在用户点击“切换超分”时提交对应 API 偏好，构造／fallback 不覆盖用户 DX11 请求。Reset 不提交偏好，尚未 Apply 的草稿不影响内部重建。
3. DX11 三阶段重建目标现在归属每个 ContextData。显式 Apply 增加请求代次；旧重建完成时发现新代次，就保留该 handle 的重建请求，按正常 Retire／Construct／Initialize 处理新选择。排队 fallback 后的新 Apply、同一选择再次 Apply 和多个 handle 均覆盖。菜单允许显式重选当前显示的后端；Create 立即登记每个 handle，未首次 Evaluate 的实例也能收到 Apply。旧实例和创建参数仍按原生命周期退休。
4. TAA 的 DX11 auto 可把已配置的 DX12 XeSS／FSR21／FSR22／FFX／DLSS 偏好映射到相应 on12 路线；普通 NGX auto 策略及显式 DX11 请求的优先级保留。

`OutputRecorded` 只证明向调用者提供的 context／command list 记录了输出命令。最终 pass、资源身份、DX11 immediate/device COM identity、桥 submission 及实际 queue／fence 退休门保持，不把 CPU 返回成功视为 GPU 完成。Vulkan 原生 bool 接口保留，Vulkan-w-DX12 消费同步更新的 DX12 接口。DX12／Vulkan 原生多 handle 选择仍沿用原有共享状态，本轮没有宣称将其全部隔离。

## 验证与证据

固定目录 `build/backend-output-20261117/`。最终 Release x64 DLL 为 `candidate/build/out/OptiScaler.dll`，33,199,616 字节，SHA256：

```text
8CCD18AFD726FDB1ADE56807B5B6FC3368802B27C7B3A8F5F039D8F7F6163FC4
```

- `source-manifest.json` 固定 1943 项输入和 53 项本线改动；`input-drift.json` 检查为零漂移。候选来自精确父提交及本线字节，排除并行工作。`IFeature_Dx11.cpp`、`menu_common.cpp` 仅提取本线 hunk，保留工作区中他人的 guard、before-SR、NR 和 UI 改动。
- `build-result.json`：最新构建开始 11:29:04 UTC、结束 11:29:37 UTC，exit 0。完整工程包含 DX11／DX12／DX11-w-DX12／Vulkan／Vulkan-w-DX12；编译通过不等于这些路线均已实际运行。
- `test-result.json`／`tests.log`：10 套定向公共回归通过，包括 FeatureTransition、NGX DX12 参数视图、CPU timing、DX11 backend binding、WARP bridge retirement、frame parameters、FG status、Reflex queue／forwarding 和 interop policy。完整 shared 回归显式跳过。
- `revision-tests-v2/`：提取生产 DX11／DX12 delivery 尾部、完整 DX11 ChangeFeature、菜单 Apply、创建注册入口及 11 个 provider 的返回契约；77 项检查。包含真实 WARP device identity，SDK factory 使用受控 seam。
- `gpu-entry-final/receipt.json`：同一最终 DLL 的 24 项实际 SDK／像素读回全部通过。八种配置为 fsr22、fsr31、fsr22_12、fsr21_12、xess_12、dlss、dlss_12、auto-xess，各测 pixel、pixel-upscale、compute。每项核实际 backend、PID、DLL／运行库 SHA 和连续 18 帧实际回写，辅助输出保持。
- `parent-gpu/fsr22/pixel/receipt.json`：精确父版本 DLL 的负对照复现故障，18 帧 gray=0／original=18；最终 DLL 同场景 gray=18／original=0。该负对照是预期失败，不计正向通过。
- DX11 shutdown 的既有受控 fixture 缺少生产代码已读取的 `taaAttachedDevice`，现补齐 stub。修正版正向 39 检查通过；移除退休的负变异按预期在第 2 检查失败。原失败日志保留。

第一版 SDK fixture 只请求 FL11_0，导致原生 FSR3.1 回退，其失败不计通过。正式宿主显式请求 FL11_1／FL11_0，并核实际 backend，避免用回退掩盖测试结果。较早 `gpu-final/` 的 DLL 早于请求代次修正，不作为本版最终证据。

## 交付与剩余边界

本线不部署、不改游戏／启动器配置、profiles 或数据库，不操作游戏。实际 SDK 宿主使用独立 INI，NR／FG 关闭；这些结果证明所测 SR 路线的输出和偏好解析，不证明三游戏输入、画面、NR／FG 组合或性能已经验收。原生 XeSS、FFX 和 Vulkan 的 SDK 执行没有纳入这次 GPU 矩阵。

`FeatureProvider_Dx11::ChangeFeature` 移除了无效 Upscaler 参数，相关调用点同步更新。集成方应按精确提交或固定 after 字节接入，不能整文件覆盖共享工作区。第七批死代码清理见 [清理报告](F6-DEAD-CODE-CLEANUP-7-20261117.md)，MFG-Unlocker 的适用范围见 [评估报告](F6-MFG-UNLOCKER-ASSESSMENT-20261117.md)。
