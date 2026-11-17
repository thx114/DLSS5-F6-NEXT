# F6 第二批死代码精简

日期：2026-11-17。本批在第一批提交 `8cd99b9` 之后，继续删除已确认没有调用者的旧接口及孤立头文件。八个生产文件只删除 182 行；两批合计净减少 527 行生产源码。

## 修改范围

| 文件 | 删除内容及活动路径核对 |
|---|---|
| `hooks/Advapi32_Hooks.h`、`Crypt32_Hooks.h`、`Gdi32_Hooks.h`、`Wintrust_Hooks.h` | 删除四个仅有定义的静态 `unhook*` 函数。保留实际 Hook 安装、事务提交和失败回滚。当前模块通过 `ModuleLifetime::PinCallbacks` 固定回调代码，`DLL_PROCESS_DETACH` 只通知关闭，不执行这些函数。 |
| `inputs/FG/Streamline_Inputs_Dx12.h`、`.cpp` | 删除无人调用、仅记录日志并返回 true 的 `dispatchFG` 声明及定义；保留 `evaluateState`、`reportResource`、`setConstants`、`markPresent`。 |
| `upscalers/fsr2_212/FSR2Feature_Vk_212.cpp` | 删除无人调用的 `transitionImageToShaderReadOnly`。实际 FSR2.1 Vulkan 初始化、Evaluate、资源包装及 Vulkan-w-DX12 后端保持。 |
| `integration/HoYoShadeStageBinding.h` | 删除完整孤立头文件。没有 include、消费者或项目条目，实际 HoYoShade owner／epoch 认证继续由现有活动接口承担。 |

上表路径均相对于 `OptiScaler/`。全项目源码、工具及导出定义检索未发现上述函数的调用、取址或导出。修改前八文件均无已有 Git diff，且 SHA256 与第一批隔离快照一致；编辑保留原文件编码及行尾。

已通知 F6 主会话，并读取主会话、TAA／HDR 与 MI 会话的最新进度。本批没有修改它们正在处理的公共状态、菜单、配置、计时、TAA、NR、GIMI 或工程项目文件。

## 编译和回归

从第一批固定的 `EE231043 + cleanup batch 1` 隔离快照复制全部输入，只覆盖本批七个修改文件并移除孤立头文件。最终 1911 项源文件与 37 项静态库，共 1948 项输入；构建后逐项 SHA256 核对零漂移，负责的共享树文件也零漂移。

- 完整 Release x64 编译 exit 0，用时约 2 分 37 秒。编译涵盖 DX11、DX12、DX11-w-DX12、Vulkan 原生及 Vulkan-w-DX12 的现有项目项。编译器／链接器警告仍存在。
- 验证 DLL：`build/dead-code-cleanup-2-20261117/isolated/build/out/OptiScaler.dll`，33,230,336 B，SHA256 `4E418DB3134A9BA50649F79C9FC5415498635117C7F550B27AA25824B5BABDCB`。
- 四套公共定向回归通过：`HookOwnership`、`MethodHooks`、`LoaderStartupContext`（4 检查）、`PluginStartupGate`（33 检查）。覆盖实际 Detours 的安装、重复安装、失败回滚、卸载及重装，以及 Windows 主线程／工作线程／TLS 启动上下文。完整 shared regression 显式跳过。
- `framegen/dlssg/tests/GuideFgWiringTests.py` 静态接线检查通过；它不执行 SDK 或游戏。
- 八文件 `git diff --check` 通过。

额外运行的 `runtime/tests/test_cleanup_entrypoints.py` 在 `Dx11wDx12SC::HookResizeBuffers` 的“veto precedes locks/logs/COM/SDK and state changes”断言失败。用第一批未修改的隔离快照复现相同失败；两版测试脚本及 `with_dx12/dx11_with_dx12_sc.cpp` 字节和 SHA256 完全一致。本批不修改该并行负责文件，不报告完整回归全绿。已将该基线问题通知主会话。

## 证据和交付边界

证据位于 `build/dead-code-cleanup-2-20261117/`：

- `before/`、`before-manifest.json` 保存八文件修改前身份；`cleanup.patch` 保存源码差异。
- `source-manifest.json` 固定隔离构建输入；`verification.json` 汇总零漂移、产物身份和验证范围。
- `build-console.log`、`common-core-console.log`、`guide-fg-wiring.log` 保存通过项。
- `cleanup-entrypoints.log` 与 `cleanup-entrypoints-baseline.log` 保存本批及基线的相同失败。

本批只交付源码与验证，未部署、未改用户配置、未操作游戏。DLL 比第一批小 512 B，不据此推断运行性能或延迟改善。产品合版由主会话把本批精简纳入其最新源码及测试；本验证 DLL 不用于覆盖后续并行成果。
