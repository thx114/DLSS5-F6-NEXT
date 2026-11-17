# 第八批无用功能与状态清理

2026-11-17；父提交 `1cbd3be139952ff7b7e0f224879bfa115644dc2e`。本批修改 38 个原本干净的源文件，新增 18 行、删除 259 行，净减 241 行；统计不含本报告。冻结候选仅包含父提交与本批修改，没有带入共享工作区的其他改动。

## 删除闭包与保留责任

| 删除内容 | 保留的活动行为 |
|---|---|
| `IFeature::IsInitParameters`、`_initParameters` 及 DX11/Vulkan 互操作转发 | 全部实际 `SetInitParameters` 调用、参数解析、配置覆盖、日志及真正的初始化结果；无人消费的返回值收敛为 `void` |
| `HasTM`、`_hasTM` 及全部后端写入和互操作转发 | SDK 的 transparency/composition 输入、reactive mask 及菜单实际读取的能力状态 |
| 无调用者的 `CallsUpscalerEndByItself` 虚接口及 10 个转发 override | 实际超分执行、GPU 计时和输出回执 |
| `DrawUiMask::Status`、文本成员和六处写入；`ResourceInfo` 中未使用的 `double lastUsedFrame` | 菜单使用的 `ReadLifecycle`、准入与拒绝分支、Begin/End、LayerLook 借用及真实资源持有；`HudlessInfo::lastUsedFrame` 的过期检查继续保留 |
| D3D12 中四组无调用的 hook 安装器、专属 getter/解析缓存；DXGI 中两个从未安装的 hook、专属 getter/slot 及卸载遍历项 | D3D12 的四个活动 `_Hooked` 导出转发、DXGI 普通导出转发、三组工厂 hook/卸载和独立 Agility hook |
| `GpuInformation::noDisplayConnected`、仅服务它的 NvAPI 查询，以及早已注释的排序方案 | 活动 DXGI 枚举顺序、LUID/架构/DLSS 能力查询及 NvAPI 模块保活；动态导出的显示器查询实现继续保留 |

FSR4 的虚表方法与 Detours 回调仍有动态调用端，本批没有删除。所有图形后端、实际 SDK 输入、SR/NR/FG 配置和 queue/fence/recording 退休逻辑保持。

## 验证

- 隔离完整 Release x64 构建 exit 0，约 155 秒；DX11、DX12、DX11-w-DX12、Vulkan 原生和 Vulkan-w-DX12 编译项覆盖。仍有编译及链接警告。
- 9 套定向公共回归通过：FeatureTransition、Dx11BackendBinding、UpscalerFrameParameters、BridgeRetirementWarp、Dx11OutputCopy、NgxDx12ResourceView、HookOwnership、D3D11On12FactoryBoundary、InteropSwapchainPolicy。
- 77 项生产输出/选择/重建检查通过。它使用提取的生产控制流与受控宿主，不是供应商 SDK 或游戏验收。
- 候选与父版 DLL 的全部 **947 项导出名称、序号及转发目标相同**；活动 D3D12/DXGI 导出源码和 `Source.def` 未修改。
- 1943 项固定构建输入在构建及测试后零漂移，38 项负责文件与冻结候选一致；CRLF 文件按 `cr-at-eol` 检查，不为检查而改写原有换行。
- 独立只读复核逐项绑定 38 个负责文件，未发现确定阻断项；该复核不替代游戏验收。
- DLL 为 **33,196,544 B**，SHA256 `5DF1039C3E306294FCFB8FE4E5C8103B4832F8D2A11F82AA754726B961C6AC30`。体积变化不代表性能收益。

完整证据在 `build/dead-code-8-20261117/`：`intake.json`、`before/`、`after/`、`owned.patch`、`source-manifest.json`、`proxy-callsite-audit.json`、构建/测试日志、`exports.json` 与 `verification.json`。生成版本头单独对待，不冒充最终提交身份。

完整 shared regression 显式跳过；未部署、未修改启动器或游戏配置、未进行游戏加载/画面/性能验收。后续合版应以当前完整源码保存点精选集成本批源码，不能直接用这份排除了并行改动的隔离 DLL 覆盖运行版本。
