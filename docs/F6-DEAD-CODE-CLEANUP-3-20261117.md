# F6 第三批精简与所有权收敛（2026-11-17）

第三批完成了已确认的旧 NR 分支、孤立接口、无消费者配置和辅助状态清理，并修复 DeferredSR 私有 NGX 的退休所有权。此次构建没有部署；已安装版本仍以原部署回执为准。

## 提交边界

旧 Git HEAD 为 `237f2ce`，它尚未保存后续已部署的 TAA/NR/呈现合版。先保存冻结的 `4E8D796F` 源码为独立基线，再叠加本批精简。直接把删除补丁放在旧 HEAD 上会留下 `NrPresentationSource` 的旧调用者，编译已确实检出；最终 Git 候选的完整构建已经通过。

基线来源与边界见 [Git 源码保存点](F6-GIT-SNAPSHOT-20261117.md)。最终候选不包含共享工作区未完成的 MI、FactoryHookTarget 或其他后续修改。用独立 Git index 生成精确 tree，不整文件提交并行 WIP，也不改用户安装配置、profiles 或启动器选择。

## 完成的精简

- 删除永久关闭的 ResidualFG、HalfRate、sample-and-hold、近似相机及无调用的私有异步入口；保留实际当前输入的 NR 残差超分和 Finished Picture 路线。同步移除配置、菜单、旧 shader modes，并重新生成 DXIL/SPIR-V。
- 删除未分配的 NR 内嵌 guide preview、旧校准/readback 和预算占位。活动 `runtime/GuidePreview`、GuideCalibration、曝光校准、meter 与真实退休同步继续使用原路径。
- 移除无消费者的设置、菜单 helper、手动低延迟 setter、HUDfix 只写计时和无效参数、FG `_resourceFrame` 写入、无调用 ResTrack helper/旧 hook 指针及孤立 headers/旧 bytecode。
- 移除仅测试实例化的 ProducerProgress、旧 NrPresentationSource 模型和两个薄 copy wrapper；保留真正的 `RecordCopy`、颜色输入准备、像素与 fence 验证。生产 `ZzzSourceClock` 保留。
- 删除 DrawUiMask 中未接入 FG backend 的 BuildHudless 路线、FG 专用配置和第三套观察槽。NR/Filter 仍由实际消费者订阅；GenericDepth 的安装需求使用真实 Enabled，避免仅有旧 FG 设置时继续捕获。
- 删除无消费者的 DXGI `childReady` 计算，保留实际创建结果、原始 COM 调用、provider 输出观察和 frame-latency owner。
- 一个 marker 只读取一次时钟；HUDfix 资源查询和原生颜色消费者查询各做一次，消除重复工作。

80 项源码/测试/模板变化，非生成文件增加 324 行、删除 2852 行，净减少 **2528 行**；不计本报告和性能审计。生成 bytecode 的逐行变化另计，不能用它夸大源码精简。

## DeferredSR 的实际退休

旧实现用 timestamp readback 非零作为槽复用与 generation 释放条件，既没有 recording Reset/discard 通知，也不能证明 executable recording 已关闭。

现在 Before/After 共用公共 `NrRecording` token。私有 NGX 创建、全部使用与 scratch 的生命周期依赖真实 post-Execute 所属 queue/fence；创建完成使用 CreationReady，退休还要求 Reset/丢弃关闭 recording。未提交即 Reset 可以丢弃创建；重复 Execute、Signal 失败等不确定状态继续隔离持有，Shutdown 不提前释放它们。独立 query heap/readback/marker 状态机已删除，普通一对 Before/After 不再额外录制两次 timestamp/resolve。

复核同时修正了上轮未完成删除中的悬空 `if (placementChanged)` 与 HUDfix 多行调用；尺寸或 seam 改变时 output 和 colorCopy 一起退休，调参仍保留 scratch。

## 验证

| 项目 | 最终结果 |
|---|---|
| Git 候选完整 Release x64 | exit 0，130.5 秒；DX11、DX12、DX11-w-DX12、Vulkan、Vulkan-w-DX12 编译项覆盖 |
| 固定构建输入 | 1941 项，零漂移；其中 1326 个 Git 源码/测试/模板 blob 与最终 tree 完全一致 |
| DLL | SHA256 `3F56060C27D093A6C3F7C11F138C351EC0697648A33DA07A0AC4F781886C760B`，33,203,712 B |
| 与部署基线比较 | 比 4E8D796F 的 33,251,328 B 小 47,616 B；这不是性能测量 |
| 定向公共回归 | 30 套通过；测试源码与最终候选逐项一致 |
| 新所有权/需求 fixture | 88 检查；真实 WARP queue/fence/Reset，NGX teardown 为计数 stub |
| 新负控 | GPU-only 退休、错误尺寸 guard、无消费者也激活 mask 三种错误均按预期 exit 1 |
| 既有定向检查 | FG/input/方向/scene/interop/ZZZ/菜单/英文及 Reflex monitor 共 9 个入口通过；菜单与 Reflex 含执行型 CPU 负控 |
| 独立复核 | 提交依赖 blocker 已用已部署基线保存点及最终 Git 完整编译消除；高风险 token/mask 修改未发现其他确定 blocker |

完整 shared regression 未全量重跑。构建仍有编译/链接警告，不能表述为零警告。WARP 不执行真实 NGX 推理，不证明游戏画面或性能；本轮没有新游戏加载、截图、显示节奏或延迟验收。

精确证据位于 `build/dead-code-final-20261117/`：`source-manifest-final.json`、`git-source-manifest-final.json`、`git-tree-source-binding.json`、`git-build-result.json`、`test-result.json`、`negative-results.json`、`static-results.json` 和最终 `verification.json`。两份隔离 DLL 仅用于构建/验证，不是新部署包。

## 后续边界

已确认的性能项和 DXGI/GIMI 收敛边界见 [后续审计](F6-DEAD-CODE-PERFORMANCE-AUDIT-20261117.md)。不能把这批完成等同于全项目已无死代码；MI 并行开发接口不在本次删除范围。
