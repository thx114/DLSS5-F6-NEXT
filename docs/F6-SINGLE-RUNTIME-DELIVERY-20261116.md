# F6 单 runtime BeforeSR 与 NR 呈现调查交付

2026-11-16。执行依据：[Sol 执行交接](D:/CODE/DLSS5_NEXT-workers/f6-rewrite/docs/F6-SOL-EXECUTION-HANDOFF-20261116.md)。FSR→DLSS 接入沿用用户已确认修复的线路；本轮不修改桥路由、启动器、用户配置、profiles、ASI 或 ReShade core。

## 已完成的产品实现

- F6 自管 BeforeSR 使用唯一 DX12 效果 runtime。DX11 游戏设备继续存在，但不依赖 DX11 效果 donor；选择、顺序和静态参数来自该实例，按真实事件 revision 缓存，不导出私人配置、不复制 preset、不把参数同步回自身。外部核心模式保留原有兼容路径。
- runtime 在借用效果句柄、录制前完成每源帧一次的更新；Submit 不重复更新，仍在真实 queue 上提交并 Signal。reload 会撤销旧选择，深度定义与真实 runtime/reload 代次关联。退休、隔离或完成未知的旧实例仍占用唯一实例名额。
- BeforeSR 有效且没有已加载核心时，单个后台任务只从明确安装布局寻找 pinned `HoYoShade/ReShade64.dll`，使用 exe 旁已有主 `ReShade.ini`。临时设置并恢复 `RESHADE_DISABLE_GRAPHICS_HOOK`；用 Windows DLL 首次加载通知证明本次加载所有权，外部先到不能被认领为自有。已有错误、重复或不兼容核心明确拒绝。
- subscriptions 建立完才发布 accepted；连接按设置代次去重，失败后可通过切换阶段或关开重新请求。没有在 Evaluate/Draw/Present 中加载核心或扫描磁盘。

## 源码与构建身份

| 项目 | 身份 |
|---|---|
| 产品源码保存点 | `f88e2193a8be974196a014069f8665762b0f8852` |
| 最终测试保存点 | `87843d8dcbb9c20d47915b2b70b8d7943b46acf9`，仅测试/runner 变化，不改变产品 DLL |
| 独立构建工作树 | `C:/Users/thx11/AppData/Local/Temp/f6-owned-707e6bb`，实际 HEAD 为 `f88e219` |
| Release x64 DLL | `C:/Users/thx11/AppData/Local/Temp/f6-owned-707e6bb/build/out/OptiScaler.dll` |
| SHA256 | `F37A8ECB414F6652D6CC42F0C6444A9F4DC1B749E2E9E5CCB237E8A8669141CD` |
| 大小 / mtime UTC | 32,648,192 B / `2026-11-15T21:50:05.6621887Z` |
| 构建 / 加载标记 | Release exit 0；实际 DLL 宿主记录 `(f88e219)` |
| 导出检查 | V1 与旧 NGX Evaluate 保留；正式 DLL 无 `F6_Test_SetPresentationProbe` |

构建日志与退出文件在上述独立工作树的 `build/f6-owned-release-20261116.log` / `.exit`。Release 编译包含 DX11、DX12、DX11-w-DX12、Vulkan 原生与 Vulkan-w-DX12；不把编译闭合当作所有设备的实际运行验收。既有 `LNK4098` 等构建警告仍在。

主工作树出现了另一批并行 TAA、hook 与测试修改。本轮保留全部未提交内容，只保存本轮明确拥有的文件。`175c049` 曾误收五条 TAA 工程引用，已由 `707e6bb` 在索引修正，工作树没有回退。早期 `f6-managed-release-175c049/out/OptiScaler.dll` 混入并行源码，**不用于部署或最终产品身份**。

## 验证结果

| 验证 | 结果与证据 |
|---|---|
| 原生 Windows 首次加载所有权 | 7 checks PASS，`build/f6-core-load-20261116.log` |
| 单实例颜色、typeless、局部区域 | PASS，最终局部组 49,277 checks，`build/f6-single-color3-20261116.log` |
| 单实例深度与真实 reload | PASS，73,965 checks；另有反向 Z 正控，`build/f6-single-depth-reload-20261116.log` |
| 单实例 Launchpad→MXAO | PASS，74,224 checks，坏/错帧深度拒绝，`build/f6-single-mxao4-20261116.log` |
| uniform、开关、顺序、reload、真实 resize/rebuild | PASS，737,538 checks；DX11 init=0，DX12 顺序 init=2 / destroy=2，峰值 live=1，`build/f6-single-lifecycle2-20261116.log` |
| 外部核心旧路径 | color / depth / selection-cache / Launchpad-MXAO 均 PASS；对应 `build/f6-external-*-20261116.log` |
| 独立最终 DLL 九例 | **9/9 PASS**，见下述矩阵回执 |
| 完整公共回归 | session 98097 返回 exit 0，`build/f6-managed-common-20261116.log` |
| 只读 Sol 最终复核 | 指定产品变更未发现确定 P1/P2；实例 `01a1e137-5318-7451-8f8f-90cc11ab916c`，已关闭 |

九例回执：`C:/Users/thx11/AppData/Local/Temp/f6-owned-dll-final2-20261116/result.json`。覆盖自管显式开启、默认开启、显式关闭、缺核心、错误核心、外部 typeless、interop、skip、native。自管正例由 F6 自己加载核心，宿主不替它设置禁 hook 环境；Probe 红通道 +32/255 后的当前输入被真实 SR 消费并读回。五个自管设置组均只有一份主 INI，初始配置/preset 的已写入设置保持语义相同；允许 core 补默认键、正规化路径。

首轮九例中缺核心/坏核心因宿主查询预期遗漏而失败，失败证据保留在 `f6-owned-dll-final-20261116`，修正夹具后上述两例通过，未因此放宽产品校验。额外“外部核心先于 F6 加载”组 **PASS**：PID 297144，6 次准入，实际核心保持外部 ownership，回执 `C:/Users/thx11/AppData/Local/Temp/f6-owned-core-first2-20261116/result.json`。首版夹具漏写 Enabled=true 的失败也保留在 `f6-owned-core-first-20261116`。

公共回归运行期间主工作树有并行修改，因此该结果是当次工作树的回归结果，不冒称完全对应独立干净树；本轮 `OptiScaler/integration/` 源码与产品保存点一致。最终 DLL 的产品身份由独立工作树构建及上述真实 DLL 矩阵闭合。

## NR 闪烁：已复现，生产修复未交付

测试专用观测在实际交给 FG 的 backbuffer 上，分别采集 raw copy 后、NR 后及 FG 输入；实际所属 queue 排序，并等待读回自己的 fence。正式 DLL 没有该导出，也没有常驻逐帧读回。

- FG 关闭观测通过：`build/f6-present-model-20261116/result.json`，PID 287980，真实模型写入 11 次。FG 开启观测通过：`build/f6-present-fgon4-20261116/result.json`，PID 260516，真实模型写入 12 次。后者只证明请求开启时的基帧输入观测，**生成帧及实际显示像素仍未知**。
- 相同源 Evaluate 后多次 Present，已确认 raw copy 覆盖该源的 NR 成品。严格保持测试 `build/f6-present-strict4-20261116/result.json` **仍然失败**：`unchanged second Present must retain this source's completed NR pixels`。普通观测 PASS 不表示闪烁已经修复。
- 反例覆盖同一纹理改 HUD、整幅改写，以及同 handle 的无有效 Color 输入 B 后恢复 C。B 旧接口返回 Success + unproven；不把 Success 当当前输出。B Present 保持当前紫色原图，不重放 A 的 NR。
- SourceClock 只认证 SR 来源，不能认证最终 HUD/整图写入版本。当前缺少最终生产者授权，无法安全决定何时发布同一 NR 成品。按交接决策门记录阻点，**没有缓存重放、按奇偶重跑 NR、跳过 Present、添加全局 Draw hook、常驻 GPU 图像比较或额外池**。后续须先在实际最终写入边界建立内容版本。

测试专用 DLL 为 `build/f6-present-probe-20261116/OptiScaler.dll`，SHA256 `2232A24C20C44862643F4362F9D96F27C1777B05C7A045BC7DBE25A4E4FC1361`，**不可部署**。实际 NR runtime SHA256 为 `3E83DFE5910FE9609A51197531FF446B336762C7EF273F1092BBCBD0D6DFE9B9`。

## 游戏 BeforeSR 的独立阻点

Neo `NgxBackend.cpp` 的当前生产声明仍为 `color_transfer=unknown`，测试也要求 unknown。AO 关闭时的 SRV/CopyResource 与 AO 开启时的 UNORM 乘 AO 都不能证明源内容编码；本轮没有改邻库或用最终 swapchain / typed view / NgxHdr=0 填 SDR。

因此本轮证明了 F6 单核 BeforeSR、同帧深度以及真核 MXAO 的隔离宿主链，**没有证明游戏 Neo→BeforeSR/MXAO 已能准入**。要推进该项，须在实际颜色生产者处建立编码契约，再测真实 Neo→F6→当前滤过输入→同次 SR 像素。

## 崩铁 TAA 附加研究

已使用只读 `gpt-6.1-sol high`，结论见 [崩铁 TAA 研究](D:/CODE/DLSS5_NEXT-workers/f6-rewrite/docs/F6-STAR-RAIL-TAA-20261116.md)。上层现有材料是 Lecram 通用逆向与绝区零历史绑定，缺少当前崩铁专属 shader、CB、有效 rect、depth、MV 与 reset 证据，不能确认崩铁是 TAAU，也不能宣称能直接支持 FG/SR/NR 历史。

研究方向是把 TAA pass 作为来源适配器，分别验证放大算法与 guides；不把普通 TAA 自动当放大器。另一批并行 TAA 实现没有纳入本轮候选。

## 部署与游戏验收

**已原位部署 `local/F6-ASI`**。安装 DLL 与独立候选同为 SHA256 `F37A8ECB414F6652D6CC42F0C6444A9F4DC1B749E2E9E5CCB237E8A8669141CD`，32,648,192 B。FileId `0x000000000000000000020000001b42ff` 保持；全部 6 条硬链接保持且内容一致，12 项受保护文件哈希保持，没有 `.next`。没有推送，也没有改启动器选择、配置、profiles、ASI 或核心。

精确回执：`build/f6-single-runtime-delivery-20261116/before.json`、`artifact-and-tests.json`、`deploy.json`、`after.json`。部署只按目标文件实际占用处理，没有查询游戏进程，也没有启动或终止游戏。混合构建和测试 probe DLL 均未部署。

本轮没有游戏新局验收，没有性能收益结论。NR 弱残影、Reflex/长期性能、HUDless、XeFG 不混入本批，长期性能任务保持暂停。
