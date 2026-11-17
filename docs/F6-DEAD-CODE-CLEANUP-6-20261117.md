# F6 第六批无用分支精简（2026-11-17）

本批基于 `43cda8affd0ae2d69ff9459a95b63978d5421f31`，只纳入修改前与该提交逐字节一致的 26 个源码文件。其他会话的 MI、Hook、DX11、NR/HUDless、菜单及维护文档改动均未纳入候选。使用 HEAD 加本批精确改动的隔离源码构建，不覆盖共享构建产物，不部署。

## 删除与收敛

- `State`／`LibraryLoad_Hooks`：删除只有插入、没有读取／遍历／清理消费者的 `modulesToFree` 集合及其写锁。保留原有模块分类、返回值、FreeLibrary 抑制／转发和模块保活语义。`<set>` 从 `State.h` 移到实际维护 timeline device 集合的 `Vulkan_Hooks.cpp`，消除间接头文件依赖。
- `State`／Present／Streamline：删除只写不读的 `presentFrameTime` 和 `dlssgLastFrame`，同时删除全部写入。实际帧时钟、日志、`lastFGFrameTime`、`fgLastFrame`、资源上报和 FG 消费链保留。
- `GuideCalibrationRuntime`：删除未被菜单、生产或测试调用的 `SetExclusions`、`SelectCandidate` 及专属存储。runtime 始终为空的排除区域不再传递，预览候选直接使用原本恒定的 `Candidate(0)`。公共校准算法的排除区域、候选评分、推荐结果及测试能力保留；worker 代次、取消、source/epoch 连续性和模块保活不变。
- FSR 输入：删除七份只写不读的 `_nvnxgInited` 副本，保留真实 NGX 初始化、返回值检查和公共 State 初始化状态。删除 DX11／Vulkan 无读取者的三组 skip 标记，以及 FSR2／FSR3 DX12 无读取者的 `_skipDestroy`；DX12 用于导出 hook 与 pattern hook 防重入的 `_skipCreate`／`_skipDispatch` 保留，所有真实 trampoline 调用和释放流程保留。
- XeSS 输入：删除 DX11／DX12 未使用的 frame counter、三份未使用的 current context、DX12 只写 device 指针，以及两份分辨率查询中无消费者的 `scalingRatio` 计算。保留质量覆盖、取整、min/max 输出、hook 注册和实际 context 映射；Vulkan 用于三槽索引的 `_frameCounter` 保留。
- `XeSSProxy`：删除未被调用／取址／注册的私有 `GetDLLVersion`、`DllPath` 及后者内部缓存；实际模块路径和 SDK 版本查询保留。
- Shader：删除未调用的 `Magnifier_Dx12::ResourceBarrier` helper、DX11 基类未用 `_counter` 和 format transfer 两个未用尺寸字段。实际 DX12 状态转换、descriptor 轮转、recording slot 的状态回滚和资源拒绝条件保留。
- `SkipSpoof`：删除未使用的 `SkipSpoofApi` 枚举与两个逻辑不可达的日志分支，保留线程／全局 scope、活动 DXGI／Vulkan 查询和可达日志。

复核时在内嵌 `nvapi/fakenvapi/low_latency_d3d.cpp` 找到了真实的 `set_forced_mode(true)` 调用，因此本批没有删除或修改 XeLL forced mode。没有以符号引用次数直接判断后端或 SDK 入口无用。

26 个源码文件增加 8 行、删除 173 行，净减少 **165 行**；不包含本报告。没有修改工程文件或生成 shader bytecode。DX11、DX12、DX11-w-DX12、Vulkan 原生和 Vulkan-w-DX12 继续参与完整 DLL 编译；实际 queue/fence 和 recording Reset/discard 退休条件不变。

## 验证

| 项目 | 结果 |
|---|---|
| 隔离完整 Release x64 | 最终 exit 0，约 99 秒；仍有既有编译／链接警告 |
| 首次构建失败 | 删除间接 `<set>` 暴露 Vulkan 实现缺少自身 include；补齐直接依赖后构建通过，初次失败日志保留 |
| 定向公共回归 | 17 套通过；完整 shared regression 显式跳过 |
| 校准相关检查 | GuideCalibration 4170 项、GuideDiagnostics 1281 项、GuideSourceMetadata 67 项、GuidePairExport 85 项通过；均不代表真实游戏输入验收 |
| 适配检查 | 7 个入口通过，包含 FSR/FG 输入、guide 方向、interop 退休、loader 和模块生命周期 |
| Windows loader／模块生命周期 | 20 个 loader 场景、2 个生产 observer 场景、生产 scheduler 检查和 21 项实际 FreeLibrary／callback／exit 检查通过 |
| 清理入口检查 | `test_cleanup_entrypoints.py` 失败；已在精确父提交源码上复现相同断言，见下文 |
| 固定构建输入 | 1937 项，构建／测试后零漂移；两个生成版本头另记，产物中的版本信息不冒充最终提交 SHA |
| 独立只读复核 | 核对 26 个路径、间接调用／include 和同步边界，未发现新增确定 blocker；独立重现了相同基线失败 |
| DLL | 33,200,128 B；SHA256 `23CE75AEF66607C51D768BBF3F1E9C357D76F859355CEEC4D9C419C82E70CD77` |

既有失败位于 `Dx11wDx12SC::HookResizeBuffers`：`HoYoShade::BridgeSourceResize sourceResize(this)` 在 `ModuleLifetime::CleanupAllowed()` 拒绝条件之前，测试要求拒绝条件之前没有其他操作。失败测试与该源文件在候选、父提交基线中均相同；未据此删除拒绝条件或改写并行 interop 工作，也没有将这一项报为通过。该断言仍待负责该调用链的主会话判定构造操作与清理边界。

证据目录为 `build/dead-code-6-20261117/`，包括 `intake.json`、精确 `before/`／`after/`、`cleanup.diff`、`source-manifest-final.json`、首次及最终构建日志／结果、`test-result.json`、`adapter-results.json`、`baseline-cleanup-entrypoints-result.json`、`verification.json` 和提交／推送回执。提交时逐项绑定隔离候选与最终 Git tree；未修改的外部 SDK 头在 archive 中发生的换行差异单列。

本批未部署，未进行真实 NGX 推理、Vulkan 设备运行、游戏加载、画面或性能验收。CPU、WARP 和 Windows loader 检查不替代上述验收。既有 FG／NR 残影、HUDless、排队、耗时和计时问题继续保留；本批不宣称修复它们或全项目已无死代码。
