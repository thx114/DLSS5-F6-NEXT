# F6 typeless BeforeSR 候选（2026-11-15）

本批产品保存点为 `7f15810f6b56df2aadc1d6732211ab24fb846f20`。状态为 **BUILT_NOT_DEPLOYED**；安装仍是 `3de4270`。本批于北京时间 2026-11-15 开始，宿主补充回归延续至 2026-11-16；下述构建时间统一使用 UTC。用户已确认崩溃来自错误插件，崩溃调查终止，本批没有相关产品修复。

## 改动与边界

BeforeSR 现在通过同一个 `BindBeforeSrInput` 入口处理生产者声明和端点发布。对于明确声明为 SDR、实际存储为 `R8G8B8A8_TYPELESS`、实际视图为兼容 UNORM／sRGB 的输入，保留源存储、typed view 和实际传输纹理各自的格式，不再把传输纹理冒充为 typed 分配。私有 swapchain 仍使用受支持的 typed 格式。

声明绑定本次 color、queue、stream、generation、frame 和有效区域，供阶段准入、私有 runtime 绑定和实际录制共同读取。失配或不支持的声明撤销本次旧 SDR 提示和旧 view 元数据。全局 `Resolve(TYPELESS)` 仍拒绝；未知 transfer、HDR／linear、异资源、异代次和不兼容视图继续拒绝。没有增加资源槽、复制或 GPU 等待，也没有改变实际 queue／fence／Reset 退休条件。

生产者 V1 的整图输入约束保持。局部写回测试独立验证已有 stage 矩形契约，不代表 Neo V1 已支持裁剪源声明。

## 固定候选

- 文件：[OptiScaler.dll](D:/CODE/DLSS5_NEXT-workers/f6-rewrite/build/f6-typeless-20261115/candidate/OptiScaler.dll)
- ProductVersion：`0.1.29-final (7f15810)`；32,624,640 B；mtime UTC `2026-11-15T15:12:27.2011338Z`。
- SHA256：`608BBC5CA915A1861DA6CC05DEDBADBA5A3BF58CE3A191EA3ACA9814542D3B4D`。
- Release x64 exit 0，61 秒；构建保留既有 MSVC／链接警告。身份和固定构建日志见 [candidate](D:/CODE/DLSS5_NEXT-workers/f6-rewrite/build/f6-typeless-20261115/candidate/identity.json)。本批未修改桥，未重新执行桥 NGX ON／OFF 构建。

## 验证

| 检查 | 结果与证据 |
|---|---|
| 旧准入负控 | 旧实现被 typeless 声明断言检出；`build/f6-typeless-before-negative.log`。 |
| 输入与端点 | 106 项及效果需求契约通过；`build/f6-typeless-input-binding.log`。 |
| pinned 真核／整图颜色 | 565341 项；`build/f6-typeless-20261115/final-color/result.log`。 |
| pinned 真核／局部写回 | 540764 项，区域 176×122；`final-color/result-partial.log`。 |
| pinned 真核／当帧深度 | 393422 项；`final-depth/result.log`。 |
| pinned 真核／Launchpad、MXAO | 198370 项；`final-launchpad/result.log`。 |
| 公共回归 | 全部 76 组及 shared regression，exit 0；`build/f6-typeless-20261115/common-core.log`。含 NR shader 3816 项、两项历史负控、18 组历史运行时用例和连续性、DX11／DX12／Vulkan 公共策略、Reflex／FG 调度及预览实际 queue／fence／Reset 回归。 |
| 实际 F6 DLL 宿主 | 最终 5/5、均 exit 0：interop PID204544、skip PID251328、native PID265272、before-sr PID292788、before-sr-typeless PID273356。原四项各 112 检查，新增 typeless 121 检查且六次生产准入；[最终回执](D:/CODE/DLSS5_NEXT-workers/f6-rewrite/build/f6-eval-v1-20261116-001228-072/result.json)。 |
| 旧实际 DLL 负控 | `3de4270` 的 SR 像素和 121 项宿主检查仍通过，但六次声明均被 `producer-transport-format-mismatch` 拒绝，生产准入为 0；新回归正确判失败。见 `build/f6-eval-v1-20261116-001047-955/result.json`。 |

真核固定 SHA256 为 `0CEE63F9C9F13F3AC909C5B4903F4DBB4B719A7AB3B4F13B0DEAF83C814B94F7`。夹具覆盖颜色通道及非不透明 alpha、边界外不写回、typed 私有 swapchain、当帧深度、反向 Z、缺失／失配深度及未知颜色域拒绝。真核夹具自行提供声明，不经过真实 Neo；实际 DLL 宿主的可执行文件模拟桥查询，也没有自然 ReShade donor。这两类离线证据分别验证自己的接缝，均不能证明游戏中的 Neo→F6→MXAO 已接通。

实际 DLL typeless 夹具加载固定核心，使用与真核夹具相同的隔离环境变量和绝对路径，并通过 NGX 现有日志回调等待生产连接回执，避免读缓冲文件或固定等待时间猜测连接完成。初次缺这些前置条件的夹具失败保留在 `actual-dll-host.log`，最终仅更新测试代码，没有改生产准入。V1／旧 ABI、改色、尺寸往返和逐次输出均按真实 GPU 读回验证；未执行 vendor `Shutdown1`，不将每 handle Release 验证扩大成完整 vendor 清理证明。

本批总回执为 [final-validation.json](D:/CODE/DLSS5_NEXT-workers/f6-rewrite/build/f6-typeless-20261115/final-validation.json)。`regression-exits.json` 仍保留第一次含夹具失败的原始退出码，不覆盖成成功；最终状态由各固定原始回执汇合。补充测试未进入产品 DLL，无需重建已固定的 `7f15810`。Vulkan 本轮验证为完整 Release 编译和公共策略／资源语义回归，没有 Vulkan GPU 或游戏验收。

## 安装与未解决项

只读核对见 [installed-readonly.json](D:/CODE/DLSS5_NEXT-workers/f6-rewrite/build/f6-typeless-20261115/installed-readonly.json)：安装 F6 仍为 `3de4270`／SHA256 `B81F992EB024EFCA91729CDA6CBFFC24F2E5E194D61937D8E1CA7235229BA3EF`，文件 ID `0x000000000000000000020000001b42ff`、六条硬链接，无 `.next`。安装桥仍为 `13617E0FAAF7DDCAD227A52EA5127844EE9F92D93FC26D0C9345A21FB9C554E9`。本批没有部署或推送，没有修改启动器选择、配置、profiles、ASI 或 ReShade core。

旧局 PID272960 的 typeless BeforeSR 在颜色准入处停止，且桥记录 `input_query=no-export`。本批兼容修复仍要求实际生产者提供合格的 typed view 与 SDR 声明；不能据离线通过宣称旧桥已可用。无 ReShade 的派发断点继续由 Neo 跟进，用户此前在 Neo 之前的测试不算 Neo 验收。

Neo 本轮同步的原生 AO 放在桥内同帧 NGX Evaluate 前，不依赖 ReShade，也无需改 F6 ABI。已只读核对其 `build-neo-release/providers-20261115-234330-819/result.json`：NVIDIA／F6 AO 开关和 native 用例均通过，但该宿主使用的 F6 SHA256 为 `0EBF10D0AD3592B1FCAA5A4D69FD632AF121BC528FA4E1D57231F478096D226F`，与本批 `608BBC5C…` 不同，不能算本候选验证。该同步仍明确未部署，PID230904 的无 ReShade 游戏局未定因。temp3 的 `NgxProvider=nvidia` 是 Neo 同步的当时状态；本批没有更改该选择，F6 始终使用独立宿主配置验证。

关闭细节持续保留后的微弱残影仍未定位。现有 NR 输入、pre-encode、history fresh 和 FG 基帧预览不是一组同源的裸模型输出；下一项需复用现有固定槽建立明确请求的分阶段观察，先核实实际 SR 输出接线、同源汇合及退休条件。子智能体给出三槽批次建议，尚未实现或验证。现有接口没有可认证的生成帧纹理，须明确记为不可观测。NR 算法本批未改，长期性能任务继续暂停。

实机仍分别等待新局 PID／加载标记、当帧 SR、BeforeSR 实际深度与 MXAO 画面、残影及计时验收；不据本批离线结果要求用户重复测试。
