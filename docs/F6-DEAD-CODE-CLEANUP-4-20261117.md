# F6 第四批无用分支与接口精简（2026-11-17）

本轮以 `da9b02839b919c5f25a91de74e131ec908afefe8` 为基线，继续沿生产调用链删除无消费者的接口及其私有依赖。23 个源码／测试文件增加 7 行、删除 317 行，净减少 **310 行**，不含本文档。没有部署或操作游戏。

## 删除范围

- 移除 `IFGFeature_Dx12::GetResourceCopy` 的完整旧命令链，包括专属 command list／allocator 数组、初始化、销毁和析构调用。入口没有调用者；析构的运行状态失效通知、活动 `CopyResource`／`FlipResource`、UI／SC 命令链及 DX11 互操作复制链继续保留。
- 删除六个无调用的 FG 查询接口，以及四个 `Dx11WithDx12` getter。只删非虚查询外壳，保留后端直接使用的 distortion／async／等待执行状态、资源缓存、mask 校验和真实队列同步。
- 删除未注册、未导出的 XeSS AIL 成功 stub 和没有定义或调用的 DX11 销毁声明；实际 XeSS 销毁仍绑定公共 `hk_xessDestroyContext`，XeLL 的 AIL 动态入口保留。
- 两个 Streamline 适配器直接执行原恒真分支中的“运动缩放乘运动纹理尺寸”，删除永不可达的另一条分支。FG 派发选择器删除无符号差值的两个 `diff < 0` 判断，其他条件和回退保持。
- 删除无调用的配置解析 helper、旧 `LoadFromPath`、DLL 名称查找 helper、窗口标题查询、通用抛异常 helper、版本字符串便利接口、两个缩放 getter、RTV heap getter、旧格式转换 switch 和 NR 标量计时接口。NR 实际计时继续使用带提交证据的 `TakeFreshEvidence`。

配置旧键迁移、星铁方向设置及日志文件选择仍有实际职责，检查后保留。DX11、DX12、DX11-w-DX12、Vulkan 原生及 Vulkan-w-DX12 路线、活动 HUDless、真实所属 queue／fence 和 recording 退休条件不属于删除范围。

## Release 测试维护

`test_release_cleanup_guard.py` 原来还要求整个正常 Release 函数尾部与历史提交 `1c4e103` 逐字相同。该比较在本轮前的 `da9b028` 候选中同样失败；两个生产 Release 文件和测试本身均与基线逐字核对，证据见 `baseline-release-cleanup.json`。

现删除这一跨功能历史全文比较，保留对当前两个生产入口的保护位置、相同前缀和实际执行行为检查。28 个动态场景继续通过；没有修改生产 Release 保护或正常清理逻辑。测试也不再依赖本机保留该历史提交。

## 验证与边界

| 项目 | 结果 |
|---|---|
| 隔离候选 | 精确 Git 基线加本轮修改，未带入共享工作区其他 WIP |
| 构建 | 完整 Release x64 初次构建 exit 0；最终修改后增量构建 exit 0，111 秒；既有警告保留 |
| 编译范围 | DX11、DX12、DX11-w-DX12、Vulkan、Vulkan-w-DX12 编译项保留 |
| 固定输入 | 最终 1937 项，构建及测试后逐项 SHA256 零漂移；两项生成版本元数据单列。44 个未修改的外部 SDK 头因 archive 文本属性使用 CRLF，与 Git blob 仅换行不同；产品源码和本轮修改逐 blob 绑定 |
| 公共回归 | 最终候选 16 套定向 suite 通过，含真实 WARP guide handoff、资源退休、NR GPU 计时及 DX11 输出复制 |
| 适配检查 | 7 个入口通过，含当前生产观测方法、20 项设置语义及 28 项 Release 入口保护场景 |
| 独立复核 | 22 个生产文件的删除依赖闭包无 blocker；随后两个无符号恒假条件和测试维护由主会话核对 |
| DLL | SHA256 `1EE29F5DD1EEBACE46341CEF200039AB0F6BAE8DF43CD598E908620C7ED4C4D6`，33,203,712 B |

DLL 位于 `build/dead-code-4-20261117/candidate/build/out/OptiScaler.dll`。证据位于 `build/dead-code-4-20261117/`：`intake.json`、`change-summary.json`、`source-manifest-final.json`、`build-result.json`、`test-result.json`、`adapter-results.json` 和 `verification.json`。提交回执及 Git tree 与构建输入绑定另见 `commit-receipt.json`。

完整 shared regression 未全量运行。WARP／CPU／静态检查不执行真实 NGX 推理，不证明游戏画面、FG 显示、延迟或性能改善。本轮未改用户安装配置、profiles、启动器选择或部署 DLL；并行 MI、NR 曝光等工作保留。此前记录的 FG 固定资源槽、NR recording timeline 等活动路径优化仍待分别实施与验证。
