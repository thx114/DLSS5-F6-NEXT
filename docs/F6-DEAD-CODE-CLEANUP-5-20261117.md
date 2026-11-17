# F6 第五批无用分支精简（2026-11-17）

本批基于 `744a191ca273804b8a94f14b5ac40d93c5a66956`，仅纳入修改前与该提交完全一致的 23 个源码／测试文件。其他会话的 MI、Hook、DX11、NR/HUDless 和维护文档改动均未纳入候选。使用隔离源码构建，不覆盖共享构建产物，不部署。

## 删除与收敛

- `runtime/HookCpuTiming.h`：删除无人读取的逐种类数值快照、增量聚合数组和旧 Bridge GPU 计时发布／读取接口。活动的 `ConsumePresentDrain` 仍依赖原来的 version、end、epoch 发布，星铁调用与最终 Present 日志仍按这一回执消费。保留每秒回执、五秒累计报告、关闭时尾部发布和已结束线程的数据；无详细日志到期时不再扫描无人使用的数值行。
- `dlssnr/DlssNrFgTrace.h`：删除无消费者的 `Output`／`Snapshot`／`lastOutput` 及其持久 fence 引用。保留当前 copy 身份、采样日志、导出解析原指针返回和实际 `Identity` 调用者；提交日志仅在采样时查询身份。该诊断快照不是生产资源的退休凭据，生产 queue/fence、recording Reset/discard 与所有权实现没有修改。
- `DlssNr_ExposureScan.*`：删除无人调用的旧 `Headline` 文本生成器和 `AnchorClear`；活动菜单仍使用 `Where`、`Report`、`Status`，多点曝光锚定的编辑、序列化和迁移保留。
- `runtime/GpuWorkDomain.h`：删除没有实例或外部方法消费者的整个旧 `GpuWorkDomain` 类，保留 `GpuWitness` 和头文件登记。当前 DX11-w-DX12 文件只有一个未使用的 witness 局部声明；本批没有把它描述为已执行的所有权校验，也没有修改该并行文件。
- Vulkan Shader：删除 RCAS 的两个无调用 buffer 包装及基类无人调用的 `SetBufferState`，删除 ResourceCopy／DepthTransfer 的四个无调用 image 包装。基类实际 buffer/image 创建和布局变换、RCAS DA 常量缓冲继续保留。
- ResourceCopy／DepthTransfer／Magnifier Vulkan：删除只比较和回写自身的六个 view 缓存成员、三个 `InitializeViews` 及其调用。descriptor 始终来自本次参数；Magnifier 的空输入／输出 view 拒绝条件移到 `Dispatch` 入口，其余实际 dispatch、descriptor 更新与帧槽轮转保留。
- FSR3.1 DX11：删除旧 `_context`／`_contextDesc`、无人调用的 typeless 格式转换、尺寸和四个调参缓存。实际派生类的 `_upscalerContext`、配置 API 使用的 `_velocity`、计时、版本和虚接口保留。
- XeSS Vulkan：删除未用版本字符串、只写 init flags 副本和只在注释中读取该副本的旧 reactive-mask 分支；局部 init flags、实际 SDK 初始化／执行和其他后端的活动 flags 保留。
- Vulkan-w-DX12：删除只写 `VulkanGIPA` 和始终为空的 `OutCopy2`；保留入口参数、实际 `VulkanGDPA` 函数加载及 `OutCopy` 复制链。原生 Vulkan 共享资源仅删除两个无调用访问器，保留字段、`Matches`、`Next` 和实际同步。
- HUDless／UI：删除两份没有实例、布局或模板消费者的 CPU `CompareParams` 类型；活动 shader 字符串、实际上传常量和产品路径保留。

以上目标均核对了生产调用、宏、虚接口、导出／动态注册及测试消费者。不是按标识符引用次数直接删除后端。DX11、DX12、DX11-w-DX12、Vulkan 和 Vulkan-w-DX12 均继续参与完整 DLL 编译。

23 个源码／测试文件增加 43 行、删除 569 行，净减少 **526 行**；不包含本报告。没有生成 shader bytecode 变化。

## 验证

| 项目 | 结果 |
|---|---|
| 隔离完整 Release x64 | exit 0，约 154 秒；仍有编译／链接警告 |
| 定向公共回归 | 20 套通过，完整 shared regression 显式跳过 |
| Hook 发布测试 | 19 项检查；覆盖累计报告、worker 尾部、关闭和重开，以及 drain 重复消费、错误 epoch 和发布中拒绝 |
| 适配检查 | 10 个入口通过，包含 FG SDK Present、源调度、星铁清理、输入与方向、运行观察和 interop 退休 |
| 固定构建输入 | 1937 项，构建及测试后零漂移；两个构建生成版本头另记 |
| 独立只读复核 | 核对 23 文件、真实消费者及同步边界，未发现确定阻断项 |
| DLL | 33,202,176 B；SHA256 `82A04E2C5C4578C8E195BF7FC718A8FFD6EE83AF175F1A948B8D439DFCAD4840` |

证据目录为 `build/dead-code-5-20261117/`，包括 `intake.json`、精确 `before/`／`after/`、`source-manifest-final.json`、`build-result.json`、`test-result.json`、`adapter-results.json`、`verification.json` 和提交／推送回执。候选中的生产源码和测试与本批提交逐项绑定，未修改外部 SDK 头的 archive 换行差异单独记录。

本批没有真实 NGX 推理、Vulkan 设备运行、游戏加载、画面或性能验收；WARP／CPU 检查不替代这些结论。移除无消费者的采集和状态可减少静态额外工作，尚无相同配置下的实测收益。既有 FG／NR 残影、HUDless、排队、耗时和计时问题继续保留；本批不宣称修复它们，也不宣称全项目已无死代码。
