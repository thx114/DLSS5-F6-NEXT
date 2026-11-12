# 外部 NGX FSR 桥的 HUD 输入约定

2026-11-12 核对：当前已部署桥固定提交 `1865c585e02679d38881b9020611dfee8d44a9de` 没有独立 HUD-less／UI 色和 Alpha 发布 ABI。现有 SR 颜色、reactive／transparency mask 不满足下述输入契约，不能改标签冒充 UI 保护。此项保持不可用，不继续从桥侧猜测提取；只有上游提供真实资源及完整交接约定后才重新接入。当前 F6 源码只有 `Hudfix_Dx12`，旧文档所述 `Hudfix_Dx11` 不能当作本版本可启用能力。

F6 已有 `FG_ResourceType::HudlessColor` 和 `FG_ResourceType::UIColor` 输入，分别映射 Streamline 的 `HUDLessColor` 和 `UIColorAndAlpha`。现有 FFX/FSR FG 输入也会提交这两类资源。原神旧 `DrawUiMask` 属于基于遮罩构造 HUD-less 图像的路径，不能视为已获取真实独立 UI 图层。

本地 Streamline 头文件另有 `kBufferTypeUIAlpha`：桥若可提供可靠的单通道 UI Alpha，请优先说明，不必为了 F6 预先生成 RGBA。当前 F6 公共资源类型尚未接入 alpha-only，需要在双方 ABI 确定后补相应标签与生命周期；SDK 有该标签不等于 F6 已收到它。

本轮已准备接收端：只有同一源帧、同一输入代次的 HUD-less 与 UI 标签均提交成功，才请求 `enableUserInterfaceRecomposition` 并显示 UI 保护。缺少任意一项仍允许 FG，状态为 UI 未保护。该状态表示输入和重合成选项已接受，不证明最终 UI 像素正确。

桥作者请提供以下约定及实际调用位置：

| 数据 | 要求 |
|---|---|
| HUD-less 场景色 | 当前真实源帧、叠加 HUD 之前的完整场景色；应明确与最终 NR/FG 颜色所处阶段的关系 |
| UI 输入 | 优先可靠的单通道 UI Alpha，或单独 UI 色＋Alpha；无 UI 处 Alpha 为 0。二值启发式遮罩不等于真实 UI 透明度 |
| 源帧身份 | 与本次 DLSS Evaluate、最终颜色、深度和运动一致的身份；不能使用生成帧回调计数 |
| 有效区域 | 两张图各自的尺寸、有效 rect、非零起点；必须描述同一输出区域 |
| 像素约定 | DXGI 格式、色域/HDR、是否只包含 HUD；色＋Alpha 路径须为预乘色，即 Final.rgb = UI.rgb + (1 - UI.a) × Hudless.rgb，直通色需显式转换 |
| 设备与状态 | D3D11 或 D3D12、设备、生产队列/上下文，以及交接时的资源状态 |
| 完成与寿命 | 实际所属 queue/fence 交接或可证明的同队列顺序；消费结束之前不得覆写或销毁 |

两张图的有效尺寸须与最终 backbuffer 区域一致，场景色需匹配其色域及后处理。呈现 NR 已改动最终颜色，不能直接将 NR 前的场景色标成 NR 后的 FG 实际输入；适配时须明确 UI 提取、NR 与最终合成的阶段。

若桥生产 D3D11 纹理，需要沿现有 DX11→DX12 共享资源的读写交接机制接入；不能将 D3D11 指针直接作为 DX12 标签。保持已修复的 guides 提前封存和真实源帧呈现顺序。

适配器在同一 F6 source slot 注册两项资源，使用 `FG_ResourceValidity::UntilPresent`，由现有 `DLSSG_Dx12::SetResource` 提交实际 SDK 标签。接入前须核对颜色阶段、区域、Alpha 及资源交接；观察快照或 CPU 成功返回不能授权资源使用/退休。

具体导出函数、结构版本、回调时机和完成回执须与桥作者确认。目前尚未实现桥侧生产回调，也没有新增或猜测 ABI；保留现有外部桥 2.4 ABI，F6 内部 FSR 桥及探测继续禁用。
