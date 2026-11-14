# F6 当前状态

## 当前：轮廓稳定与线程归属修复（2026-11-15）

产品保存点 **`1f07b0b464be381951cd9fe220507ccbe525f44e`**，嵌入标记 `1f07b0b`。Release x64 exit0 / 98秒；DLL **32,599,552 B / SHA256 `D6976017E368FEC38F6021C4EC2CED619202CFF741262E70398BE5AB05C908DD`**，mtime UTC `2026-11-14T17:13:38.9097987Z`。UTC17:17 原位覆盖既有 `local/F6-ASI`，部署回执 `DEPLOYED_IN_PLACE`；原 FileId `0x000000000000000000020000001b42ff`、6条硬链接和核对的6份配置/内容哈希保持，无 `.next`。构建前后2084项输入哈希一致，946个导出名称与先前安装DLL一致。本节覆盖下方历史部署身份。

- **细节稳定：** 短稳定器现在检查紧邻源帧 t7 的实际采样跨度，拒绝双线性均值恰好相同的消失轮廓。floor 不覆盖结构失配。复用长历史已验证的当前四邻；cross 判据将拒绝时，再以当前3×3范围和本次footprint最强tap补足二维拐角/单像素的采样衰减。平坦路径不增加纹理读取；不改绑定、常量ABI或退休权限。DXIL/嵌入头字节一致，重新生成的Vulkan字节码不变。
- **Reflex与调度：** PCL递归保护从进程共享数组改为按SDK枚举大小的TLS；Sleep和marker均以RAII恢复嵌套状态。限帧节拍和自动复位timer归同一线程，线程退出关闭HANDLE。ViT `Split` 仍逐项执行原有Drop/TakeGap，仅实际需要改链时物化输出数组；原样提交不分配、不复制launch。timer竞态仅在启用限帧且并发时成立，不能解释旧cap=0局。
- **已验证：** 生产HLSL/WARP正向4197项检查通过，包含长历史开关、floor 0/.65/1、FP16/FP32、三轴消失轮廓及独立CPU双线性重投影的corner/point。单独恢复cross-only的负控336项中96项预期失败，准确捕获有效细节被误拒绝；此前只做cross的候选从未部署。28个历史生命周期场景与3个NVIDIA `10DE:28E0`/嵌入生产DXIL场景通过。Reflex保护31项、ViT等价与原样零分配6712项、真实并发timer48次、Reflex计时/报告与FG pacing/源调度定向回归通过；shared未全量重跑，未运行游戏或Vulkan实卡。
- **阶段成本：** 同宿主、2144×1344工作图、1536×960 guides、平坦合成输入，当前/旧/当前各80样本GPU P50为 **2.475008 / 2.432512 / 2.478080 ms**；本夹具增加约 **0.044 ms**。D3D12 debug errors=0、GBV关闭，exe和两版CSO哈希前后绑定。不能将阶段成本当游戏FPS或完整Reflex改善。

**用户已明确“稍后再测，先继续代码工作”。** 不再催测或运行隐藏FG宿主；只由用户操作游戏/选项。本版实际加载、细节画面、ReShade游戏执行、FPS与完整Reflex≤60ms仍未验收，旧约101–113ms保持未达标。保留SR/NR/FG、用户配置、ASI与既有dirty；没有拆分生产queue。

下一代码项为 [Execute提交与录制代次闭包](F6-SUBMISSION-LIFETIME.md)：已确认公共post按当前裸list查找代次，Execute之后成功Reset但post迟到可能错认旧/新代次；各独立capture/readback也须同步处理。KernelProfile的“90次evaluation即完成”仍未修，不能先接一个不完整的fence判断后宣称安全。此问题未被证明是旧局延迟或残影根因，代码仍保持原行为，须完整实现和定向验证后另行构建。

本轮证据 `build/code-followup-20261115/`：`artifact.json`、`shader-identity.json`、`source-before-build.json`、`build.log`、`deployment.json`、`installed-{before,after}.json`、`protected-{before,after}.json`、`history-corner-regression.log`、`history-runtime.log`、`history-hardware.log`、`pacing.log`、`source-scheduling.log`、`benchmark-*.log`及`benchmark-identity.json`。两项旧稳定器负控和新轮廓/拐角负控保存在`build/common-core-tests/NrDetailHistoryWarp/`。本轮只读Sol high均由当前主会话创建并关闭，未复用旧会话或过期实例。

## 历史：BeforeNR同源颜色授权

更新：**2026-11-14T12:53Z**。当前产品保存点 **`f4e65ad58ec3d09becbc8b0935fe4d1747ab7d67`**，保留细节稳定、深度复用、SDR 单写和 `54b422f` 的实际 SDK Present 后 owning fence 观察，补齐 BeforeNR 的同源颜色授权。Release x64 exit0，67秒；DLL **32,584,704 B / SHA256 `B0B5C60B44DF9BCC8DA74A32A9EFE8371B132DCF7D7EC5CFBC1C0701BD50AE15`**，嵌入标记 `f4e65ad`，mtime UTC `2026-11-14T12:50:26.7831915Z`。12:52Z 原位部署既有 `local/F6-ASI`；原 FileId `0x000000000000000000020000001b42ff`、6 条硬链接和核对的 6 份配置/内容哈希保持，无 `.next`。本节覆盖以下旧部署身份。

- BeforeNR 原本已能取得最终颜色；缺口是滤镜授权，而非缺少私有颜色纹理。统一 `SwapchainColorSpace` 严格检查本链读取成功、长度精确且明确 G22/P709。桥的成功 SetColorSpace1 声明绑定实际 `_real` 源；BridgeScope 按本次原 buffer、child buffer、线程/epoch 与真实 copy queue/fence 核对后读取源声明。删除公共 BeforeNr 的格式推断 `sdr` 入参，不借 child 输出声明授权原图。
- Native DX11/DX12 使用独立 `filterSdrAllowed`；保留 NR/光流原有 `colorSpace` 映射与用户选项。未知/HDR 仅拒绝当前 SDR 滤镜，继续现有 NR；源和深度分别用原真实 fence 退休。Vulkan 映射及默认滤镜拒绝保持，没有新增外桥 ABI。
- 固定真实 ReShade 核心/WARP：颜色 **467264** 检查（含桥与原生 DX12 下游像素、缺失/失败/畸形/错链/HDR、实际 wrapper 方法抽取），深度 **98517**、不确定提交 **24688** 通过；颜色事务 8 路由/16 成败/4恢复、效果深度需求通过。测试局部全局配置的首次提示污染像素已修，失败日志保留且断言未放宽；未改用户配置或核心。两位只读 Sol high 已关闭，最终复核无具体阻断；shared 未全量重跑，未执行 native Acquire/实际 SDK DLL/游戏或 Vulkan GPU。
- **生产队列仍未拆分，无新增 GPU Wait 或复用/退休权限。** `F6 SDK input Present observation` 仍每5秒最多一次，明确 `queue_separated=false` / `receipt_usable_for_retirement=false`；不把数字地址转成对象。上一轮 1471 检查/90 SDK 脚本调用及5项负控见 `owning-input-fence/`，无需重复。
- 活动原神日志仍为 UTC08:06:43 / 60,804,125 B。**当前加载、细节稳定及完整 Reflex ≤60 ms 未验收，约110 ms仍不及格。** 用户选择仍为 BeforeSR，低分辨率桥输入的未知 transfer 不能借最终 backbuffer 声明放行；也没有本版 BeforeNR 游戏执行证据。下一新局核对 `f4e65ad`，不沿用先前请求中的旧候选标记；仅用户操作游戏和选项。

证据：`build/genshin-reshade-20261114/detail-feedback-20261114-140355/before-nr-transfer/`，含 `validation.json`、`review.md`、三种真实核心模式的 `result.log`、`common-targeted.log`、`build.json`、`deployment.json`、`installed-identity.json` 和 `protected-{before,after}.json`。本次授权修复不构成性能改善，60ms目标仍未完成。

### 后续：真实 SDK 独立采样（2026-11-14T13:25Z）

新增独立宿主保存点 `d043e65480b8260febc720dd328b5f42bef7c97a`，入口 `OptiScaler/framegen/dlssg/tests/input-completion-sdk/run.ps1`。**产品 DLL 仍为上方 f4e65ad，没有因工具提交重建或部署。** 真实 NVIDIA D3D12、自有 1920×1080 SDR/深度/MV/HUDless 及同帧 token/constants/Reflex/PCL，自动代理链上 24 次 Present 均 S_OK；每次紧随调用线程查询 ABI4，QI fence 与输入设备相同，但 target 全为0、实际呈现间隔计数全为1。SDK 明确记录 `DLSS-G disabled: window not focused`。结论为 **`NO_CONSUMED_INPUT_PROOF` / exit4**，不能授权队列拆分，也不能据此说 D3D 不支持完成回执。

实际 GetState 位于 `C:/ProgramData/NVIDIA/NGX/models/sl_dlss_g_override_0/versions/134672/files/190_E658700.dll`，SHA256 `3ED9538D…` 与本轮 bundled `sl.dlss_g.dll` 完全一致；路径不同不等于内容不匹配。六份安装 SDK 哈希保持；三项 SDK disable/free/shutdown 均返回0。代理释放先于 SDK shutdown、DLL卸载最后；错误 GPU drain 或 SDK 清理以独立进程 exit5 结束，不能把先前成功样本冒报成功。

宿主 `-Visible` 仅供用户手动启动/聚焦：显示自身窗口但不抢焦点或合成输入，45秒未获手动焦点即无证明退出；本轮**未执行可见模式**。原神活动日志仍停 UTC08:06:43，没有新局。已异步询问用户方便手动原场景还是独立窗口测试，不自动操作游戏或焦点。下一步需要真实活跃 FG 的非零递增回执与当前游戏画面/延迟；不要重复隐藏模式来冒充进展。证据 `build/genshin-reshade-20261114/detail-feedback-20261114-140355/real-sdk-input/` 的 `validation.json`、`result.json`、`result.log`、`runtime-{before,after}.json`、`review.md`。两位本轮只读 Sol high 已完成并关闭；一项 SDK 清理失败退出码漏报已修，未在硬件运行触发失败。

## 历史：共享 SDR 编码部署

更新：**2026-11-14T11:00Z**。当前产品保存点 **`0e599810c053801451d323b047ed91a2d1b57b2e`**，包含下述细节稳定、深度复用与 SDK 观察修复，并让同格式 SDR passthrough 的原图与代理图共享编码输出。Release x64 exit 0，72 秒；DLL **32,572,416 B / SHA256 `9504586CDD1657DE018678AE00F437DAA2BF35F2CD4F7E210074CCAD649337EA`**，嵌入标记 `0e59981`，mtime UTC `2026-11-14T10:59:01.6915523Z`。10:59Z 已原位部署既有 `local/F6-ASI`，原 FileId、6 条硬链接和核对的 6 份配置哈希保持，无 `.next`；别名包括三游戏 cache。本节覆盖下方旧部署身份。

- 只在 passthrough、目标支持 UAV、两张实际存储格式相同时共享；校准和 resolve 读取同一只读编码原图。HDR、不同格式和非 UAV copy-back 保留双输出。调用方 mode 0 与历史编码 contract 不变，仅上传副本选 mode 15；未使用的 keep 槽绑定 typed null UAV。没有减少资源预算、扩大槽、改变队列或放松退休条件。
- 793 项实际 HLSL/WARP 检查通过：UNORM8 / RGB10A2 / FP16 / FP32、舍入边界、alpha、变化源、生产 null UAV 与共享 SRV resolve；输出逐位等价。旧 encoded-domain 负控的 24 项预期失败单列，不算正向失败。颜色事务通过 8 路由、16 成功/失败和 4 个同一组纹理成功→失败→新源恢复场景；28 个历史生命周期场景、Look 控件 80 和输出 WARP 1596 检查通过。完整 shared 未重跑。DXIL / SPIR-V 与嵌入头已同步生成，Vulkan 实卡未验收；只读 Sol high 复核无具体阻断，已关闭回收。
- 同宿主完整生产 DXIL、2144×1344 FP16 平坦合成输入，单写 / 旧双写 / 单写各 80 个样本，GPU P50 **0.098816 / 0.111104 / 0.099328 ms**，本夹具收益约 **0.012 ms**；实际存储逐位一致，debug errors=0，GPU-based validation 关闭，包含转为可读的屏障。不是游戏 FPS 或整体 Reflex 改善，不能解释几十毫秒差距。
- 活动日志仍停在 UTC08:06:43 / 60,804,125 B。**当前候选的加载、细节稳定与整体 Reflex ≤60 ms 仍未验收，用户约110 ms反馈保持未达标。** 下一局先核对 `0e59981`；游戏启动、进入、操作和退出由用户手动完成。生产/呈现队列仍未拆分，后续须按 SDK 实际输入完成与真实 buffer 代次闭合依赖。

本轮证据：`build/genshin-reshade-20261114/detail-feedback-20261114-140355/shared-encode/`，包含 `pixels.log`、`runtime.log`、`transaction-recovery.log`、`output-regression.log`、`benchmark-*.log`、`benchmark-identity.json`、`build.json`、`deployment.json`、`installed-identity.json`、`protected-{before,after}.json`；shader receipt 在 `build/shaders/evidence/shared-encode-20261114/`。

后续队列拆分的具体接线已收敛到同总目录 `completion-observation/queue-separation-plan.md`：实际 buffer 独立代次、SDK 原始 Present 返回后且 teardown 前的 owning fence capture、最后 writer 与 SDK reader 的双向依赖、未决输入及 resize/detach 退休。仅设计，未激活；本轮只读 Sol high 评审已完成并关闭，现有阶段测试与部署无需因该计划重复。

## 历史：SDK 观察部署

更新：**2026-11-14T10:13Z**。产品保存点 **`8390a8735ca6880c3626fdc5c77f897ec600167d`**，保留下述细节稳定修复，增加现有 SDK 查询的输入完成字段观测，并修复异常路径的 LastError。Release x64 exit 0，62 秒；DLL **32,570,368 B / SHA256 `07B80733D1741A5F35400CB641479AE942397810918697A2819CAED7CABCE900`**，mtime UTC `2026-11-14T10:11:57.8610540Z`。10:12Z 原位部署既有 `local/F6-ASI`，原 FileId、6条硬链接和核对的6份配置哈希保持，无 `.next`；别名仍包括原神与崩铁 cache。本节覆盖下方旧部署身份。

- `F6 SDK input completion observation` 每线程最多5秒一条，记录实际查询的线程、序号/时段、调用前后ABI、原始fence地址/目标数字及当时provider身份。不新增SDK/COM/fence调用，不改变排队或等待。调用前后均为已知v3/v4结构才读新增尾字段；其它可选字段同样受调用前版本限制。异常在SDK边界保存LastError，观察后恢复并原样重抛。
- 真实生产查询函数抽取夹具 **1243检查、40次脚本化SDK调用通过**，覆盖尾部不可访问的旧ABI、版本/类型改写、失败残留、零目标、异常、递归、缓存与跨线程。两项隔离负控分别准确触发越界访问和LastError丢失；源调度1065、真实延迟适配780检查及guide wiring通过。未全量重跑shared，未执行SDK DLL或游戏。
- 查询通常在Dispatch、早于SDK Present；source frame只是查询时的上下文。**Present/buffer join、SDK是否写入字段、GPU完成仍未知，不能据此退休或跨队列复用。** 后续拆分必须按swapchain代次和实际buffer identity，闭合最后颜色writer→下一producer及SDK reader→下一producer；共享copy槽不是SDK fake index。SDK要求的present线程回执不能由CPU Present返回、copy fence或物理输出tail替代。此次未拆分队列，未产生游戏降延迟证据。
- 活动日志仍为 UTC08:06:43 / 60,804,125 B 的旧局。**细节稳定游戏验收与整体Reflex ≤60 ms仍未完成**，约110 ms的用户反馈保持未达标。仅用户操作游戏；下一局核对实际加载 `8390a87` 后再分析，不重复此前夹具/部署或拿旧局作当前候选结果。

证据：`build/genshin-reshade-20261114/detail-feedback-20261114-140355/completion-observation/`，含`wrapper.log`、两项`negative-*.log`、`source-scheduling.log`、`guide-wiring.log`、`build.json`、`deployment-console.json`、`installed-identity.json`及前后配置指纹。当前主会话新建的只读Sol high ABI/观察审计已完成并关闭，没有复用旧智能体。

更新：**2026-11-14 17:21（UTC+08）**。当前产品源码保存点 **`c5901203eff7f466964029c607536f0f1f5118b5`**，在下述细节稳定修复上复用已经验证的当前深度，避免短稳定器第二次读取同一深度纹理。Release x64 exit 0，50 秒；DLL **32,566,784 B / SHA256 `B780EB543B4EE2B43A1497B76D3B215A62025DEF8808D3509CCC5146609A36EC`**，嵌入标记 `c590120`，mtime UTC `2026-11-14T09:19:29.6803385Z`。17:21 已原位部署至既有 `local/F6-ASI`，原 FileId、全部 6 条硬链接与核对的配置哈希保持，无 `.next`。这些别名包括原神和崩铁 cache；未创建包、切换选择或改动用户选项。此处部署身份覆盖下述 16:58 条目的身份。

- 候选与保存的前版 HLSL 在 927 次真实 WARP dispatch 的 u0…u4 **8,464,384 个原始 GPU 存储字节完全一致**，覆盖现有 FP16 / FP32、跨帧、无效输入、深度和 inspector 场景；3861 检查零失败。只改 u4 的独立负控于 dispatch 1 被准确检出。28 个生命周期和 3 个实卡 / 嵌入生产 DXIL / FP16 用例重跑通过；CSO / 嵌入头字节一致，重新生成的 Vulkan 字节码不变。只读 Sol high 复核无源码阻断，已关闭回收；未全量重跑 shared，也未做 Vulkan 实卡验收。
- 同宿主 / 同资源 / 平坦合成输入，候选 / 旧 / 候选各 80 个 GPU 时间样本的 P50 为 **2.741760 / 2.777600 / 2.741248 ms**，本次减少约 **0.036 ms**；debug errors=0，GPU-based validation 关闭。仅为细节阶段，不是游戏 FPS 或整体 Reflex 改善；仍包含正常历史到期分支。
- 模型链只读审计未发现重复模型调用：旧 PID225400 日志为单有效层，10,800 dispatch / 10,800 modelCalls / 0 failures。SDR passthrough 存在两张同格式全图的重复 encode 写入，可考虑统一物理输出；尚未实施，整个 encode 阶段 P50 仅约 0.209 ms，不能把它当几十毫秒差距的解释。
- **整体 Reflex ≤60 ms 与细节游戏画面仍待验收。** 原神活动日志仍是 UTC08:06:43 的旧局，不能拿旧 PID225400 或旧两段 101.489 / 113.347 ms 当本候选结果。继续沿 source pacing 和实际 GPU 工作定位，不改 SR / NR / FG 参数，不控制游戏。

本轮证据：`build/genshin-reshade-20261114/detail-feedback-20261114-140355/depth-reuse/`，含 `validation.json`、`bitwise-warp.log`、`comparator-negative/result.log`、`runtime.log`、`benchmark/identity.json`、`build.json`、`deployment.json`、`installed-identity.json` 和前后配置指纹。下方细节稳定修复、旧局定位、ReShade 与桥的未闭环项仍保留。

`Get-F6RunEvidence.ps1 -Latency` 现可统计有效 GPU active、完整 Reflex 的默认 60 ms 阈值，并按线程 / swapchain 分组解析新 source pacing 的成对 QPC；等待子项不与包含它们的收尾区间重复相加。22 项合成解析回归通过，覆盖不同线程 / 链、同设备重复报告与异设备保留；独立只读复核无阻断。旧 PID225400 的完整保存日志复核为 1299 份唯一报告，83 份 ≤60 ms、1216 份超标；全日志混有不同场景，不能取代前台窗口或候选验收。旧32条 pacing 缺新字段，全部保持不可解析而非填零。该工具和文档更新不改变已部署 DLL；使用方法见 `F6-GUIDE-DIAGNOSTICS.md`。

当前源码的队列复核确认 SR 和 DLSSG presenting 共用 WithDx12 queue，且 DLSSG 选择 `eBlockPresentingClientQueue`；这是下一项待证的串行瓶颈，尚未拆分，不能把旧 allocator 27.970 ms 等待直接认作可回收成本。shared writer 的返回 Signal / DX11 Wait 已在本帧 NR / FG 前提交，必须保留；完整拆分还需闭合生产→reader、独立 writer 返回 fence、SDK backbuffer / UntilPresent 与各实际队列退休。主会话核对了源码及随树 SDK，不以历史建议代替证据。四位本轮只读审计智能体均已完成并关闭，结论汇总于 `depth-reuse/read-only-reviews.md`。

更新：**2026-11-14 16:58（UTC+08）**。当前优先级是细节稳定；整体 Reflex **60 ms 为及格线**。下面的 15:26 及更早条目均为历史，不能覆盖本节。

## 当前：稳定修复已部署，游戏画面和整体延迟待验收

唯一产品树 `D:/CODE/DLSS5_NEXT-workers/f6-rewrite`，分支 `codex/f6-rewrite`。源码保存点 **`7c2f11ed7503b8aaf117729541c32fc3a36e45a8`**。Release x64 exit 0，50 秒；嵌入构建标记 `7c2f11e`，DLL **32,567,808 B，SHA256 `3B9EDFBE5D222BB4FC18BD79C85EF8308E573C9C542EAA11D3676FF772EC8F20`**，mtime UTC `2026-11-14T08:56:45.0743464Z`。16:56 已通过 `Deploy-F6.ps1 -BuildId local/F6-ASI` 原位覆盖；原 FileId `0x000000000000000000020000001b42ff` 和全部 6 条硬链接保持，别名逐一同 SHA，核对的 INI / profile / Rocket 配置 / ReShade core 哈希未变。没有切换启动器选项或创建新包。

- **本次稳定修复：** 长细节历史的 RGB 保存可能较老的出生颜色，短稳定器此前误用它判断相邻帧外观。现在用独立 `stabilityContext[2]` 保存紧邻源帧的原始 RGB / 有效位，现有出生颜色和当前高精度深度语义不变。同一 dispatch 写出，跟随既有 source identity、真实 queue/fence 和成功 Reset 退休，预算计入新增纹理；没有扩大录制槽或放松深度 / 运动准入。DX12 表为 8 SRV / 5 UAV；生产 DXIL / 嵌入头逐字节一致，重新生成的 Vulkan 字节码与此前相同。实际 Vulkan GPU 未验收。
- **可复现的像素结果：** 先播种一次强细节、随后弱输出并缓慢改变光照，避免测试不断更新出生颜色。生产 HLSL/WARP 3861 检查通过；恢复滚动出生颜色和恢复短稳定读取老出生颜色，两项独立负控均准确失败。28 个生命周期用例加 3 个实卡生产 DXIL / FP16 用例共 2678 检查通过；新外观场景连续 48 个源帧通过，恢复旧读取在第 8 帧失稳。另过 NR 公共颜色 / 输出 / Look、Reflex 和 FG pacing 定向回归；没有全量重跑 shared。以上不执行完整游戏 NR 模型，不等于游戏画面验收。
- **阶段成本：** 修正计时夹具的 CPU non-visible UAV clear 描述符和异常持有后，当前 / 旧 / 当前在同宿主、2144×1344 工作图、1536×960 guides、平坦合成输入下各测 80 个样本。GPU 时间 P50 为 **2.780160 / 2.698240 / 2.779648 ms**，当前增加约 **0.082 ms**；三局 D3D12 debug layer errors=0，GPU-based validation 关闭，exe / CSO SHA 前后绑定。包含正常历史到期分支；不是持续完整分支的独占成本，也不是游戏帧数。早期 `stability-benchmark-*.log` 的 clear 描述符用法错误，已作废，不作性能结论。
- **整体 60 ms 尚未通过：** 已成功运行的 PID225400 加载的是上一版 `b0eb017` / 磁盘 A9DDE228，不是本候选。两段前台显示采样中，总 Reflex P50 **101.489 / 113.347 ms**，线性插值 P95 **132.8955 / 141.1946 ms**；GPU 活跃时间 P50 **65.043 / 66.662 ms**。总值按同份有效报告最晚结束减最早开始，不重复相加模拟 / 队列 / GPU；不能拿排队子项替代总值，也不能称完整输入到显示延迟。没有有效新旧游戏 A/B；旧版对照失焦，11.09 Hz 无效。
- **下一局定位：** `F6 source pacing` 已改为每线程每 5 秒采样，持续记录真实 SDK Present、PresentEnd 后收尾、解锁、实际 leased Sleep / limiter 的成对 QPC、原 token 和返回值；消除原来 32 条后停止的诊断盲区。不改变等待、准入或用户选项，不声称已降延迟。`present_tail_ms` 是 SDK 返回后的包含性尾段，不能再与其中子项相加。native phase 也区分 legacy history 与独立 feedback，不再拿旧 decision 的 false 误报反馈未工作。

**操作边界以用户本次提供的最新 AGENTS.md 为准：游戏启动、进入场景、操作、调整和退出由用户手动完成，AI 构建、原位部署、读取日志和分析。** 本次续接没有操作游戏。下一局先验收细节跳变 / 残影，再以相同场景核对整体 Reflex ≤60 ms 和显示采样；持续性能目标仍未完成。

证据目录：`build/genshin-reshade-20261114/detail-feedback-20261114-140355/`。当前构建 / 部署分别见 `stability-source-bound-build.json`、`stability-deployment.json`、`stability-installed-identity.json`、`stability-protected-{before,after}.json`；测试见 `stability-validation.json`、`stability-context-warp-final.log`、`stability-final-runtime-and-hardware.log`、`stability-final-hardware-negative.log`；成本与延迟见 `stability-final-benchmark/identity.json` 和 `stability-latency-observations.json`，分析脚本同目录。`native-current-final.log` 与 PID / creation time 绑定；`before-ab` 是其前缀，不能重复统计成独立样本。

ReShade 仍受未知颜色域拒绝，尚无实际前置滤镜执行；当前用户设置仍是 BeforeSR。上一版自然 runtime 的缓存已正常命中，不能据此宣称 BeforeNR 滤镜成功。桥在 14:57 已推送的历史回执保留于下节，本候选没有部署新桥。保留无关 `pelican_bicycle.html` 和既有 `Invoke-ExactGameEntry.ps1` 未提交改动，不 reset / clean。

## 历史：15:26 的缓存修复候选与当时操作条件

更新：2026-11-14 15:26（UTC+08）。当前产品源码为 `b0eb017f1781e73b10f70a39b25d2f6db5ed9098`，包含 `bd79e40` 的 NR 反馈录制恢复与本次 ReShade 缓存事件修复，并保留此前 D3D11On12 / resize / BeforeNR 接线。Release x64 exit 0，1 分 40 秒；DLL **32,558,592 B，SHA256 `A9DDE228CCB4A1F2C144521CE2D2A4B125F462DC0AF094C5E1230BC98B688B70`**，mtime UTC `2026-11-14T07:26:00.5577369Z`。15:26:47 已原位部署到既有 `local/F6-ASI`，`DEPLOYED_IN_PLACE`；原 FileId `0x000000000000000000020000001b42ff`、6 条硬链接及核对的 INI / profile / Rocket 配置 / ReShade core 哈希保持。**新候选加载、游戏画面及性能仍未验收。**

唯一产品树：`D:/CODE/DLSS5_NEXT-workers/f6-rewrite`，分支 `codex/f6-rewrite`。文档提交不改变 DLL 源码身份。2026-11-14 本会话用户重新明确授权 CLI 启停、管理员进入世界，并要求尝试电脑技能点击 Rocket；该授权覆盖下文历史手测限制，但不授权改启动器选择、INI、preset、profiles、数据库或 ASI。用户已确认没有 Rocket CLI；Hub `launch_game` 在 Rocket 模式只准备配置，直接启动游戏 exe 的试验没有加载注入链，不能用于基准。电脑技能和独立控制入口经重置后均在连接前报 `failed to write kernel assets: 系统找不到指定的路径。(os error 3)`，未获得 Rocket 窗口，当前无法自主发起有效新局。不重复裸启动，不把工具失败写成游戏失败。

## 最新：NR 反馈恢复、ReShade 缓存与桥推送（2026-11-14）

- `bd79e40`：已完成 retained generation / source gap 后允许重新录制反馈，保留真实 queue/fence、Reset 关闭与独立历史邻接准入，不扩池、不复用不相邻帧；新增反馈录制 / busy / recovery 计数。28 个隔离运行时用例、policy 135、生产 shader WARP 3763、continuity 104 检查通过；两个真实 NVIDIA / 生产 DXIL / FP16 像素用例各 328 检查通过。修正的是宿主三 list teardown 的 Drain / Reset / Collect 顺序，未放松生产退休条件。这些不执行完整游戏 NR。
- `b0eb017`：删除 `SelectionEvent` 中悬空 Destroy 条件，所有 Init / Reload / Preset / Order / Destroy 均发布代次，包括 owned 重入。保留 `reloadObserved`、revision / generation 及资源拒绝条件。真实固定 ReShade 核心回归覆盖两个顺序敏感 technique、非空 uniform 实时同步、owned 缓存、真实重排、否决 / 改写后的安全拒绝和恢复、二次 reload 与 destroy；713,775 像素 / 状态检查通过，4 次必要 graph build / 2 次私有快照后稳态无重复重建或快照 I/O。隔离副本重新加入 `!ownedOperation` 后准确触发负控失败；旧悬空条件也在真实 Init 断言被检出。BeforeSR 颜色 / 去重 270,398、BeforeNR 颜色 / 去重 147,586 检查通过，均不是游戏性能结论。
- 外部桥同步已完成：隔离树 `D:/CODE/DLSS5_NEXT-workers/fsr-bridge-sync-20261114`，基于当次 fetch 的上游 `2db978753eb82ae8e4f90b8a0a13023db832b721` 合并当前四项有效补丁，NGX ON / OFF 各 18/18 通过；已普通快进推到 `thx114/genshin_fsr_brigde` 的 `main`，提交 `a03e99dc182430a605791f76487bc94acb5e21a8`，14:57:54 核对远端一致。未 force push、发布、部署新桥或动旧 fork 的 49 个未提交文件；本轮 F6 未推送。15:25 再次联网核对遇网络连接中止，不将 14:57 的快照称为持续实时最新。
- 最新可用固定局仍是 `20261114-140355_YuanShen_230344`，实际加载 `8ca0623`，不是新候选。127 个同录制 GPU 样本：NR 总 P50 / P95 30.2756 / 35.6465 ms，模型 28.3832 / 33.408 ms；旧反馈多数被拒绝，history P50=0 不能作为正常低成本基线。582 个去重 Reflex 报告的 GPU 开始前排队 P50 / P95 20.9185 / 43.249 ms，不是受控 A/B，不能据此认定桥回退。
- 该旧局 `Stage=1`，即 BeforeSR；自然 reload 确实到达，但坏事件发布使 `graph_builds=8046` 且缓存始终未命中。实际滤镜还被 `color-transfer-unknown-or-hdr` 拒绝，没有执行像素。源码 / 日志指向全局输出颜色域缺失；`IsHDR=false` 或 8-bit 存储格式不能单独证明低分辨率桥输入的 transfer。BeforeNR 已接线，但私有颜色信息读取仍有格式默认值授权的待修缺口；保持 unknown / HDR 拒绝，不能靠删条件宣称滤镜可用。用户反馈 GIMI 似乎正常，尚未独立确认新候选。

本轮证据总目录：`build/genshin-reshade-20261114/detail-feedback-20261114-140355/`。NR 见 `source-bound-validation.json`、`recovery-teardown-final.log`、`hardware-pixels-teardown-final.log`；当前部署见 `selection-cache-build-identity.json`、`selection-cache-source-bound-deployment.json`、`selection-cache-installed-{before,after}.json`、`selection-cache-protected-{before,after}.json`；定向正负控见 `selection-cache-validation.json` 及其固定日志。完整旧局快照 `OptiScaler-snapshot.log` SHA256 `2DE83794F78BEF027A367A496F58AB6AA49BE42F7CA4F1F230C5603704829510`，28,241,340 B / 289,227 行。桥推送回执为桥树忽略目录 `local-only/push-receipt.json`。

接续优先级：先补清楚颜色事实来源与读取失败语义；只在有效注入新局实际出现后绑定 PID / creation time / 加载身份和当前日志采样。桥普通 draw 的静态审计未发现新增锁、堆分配或原子 RMW，但存在重复 TLS scope；没有路由计时或游戏 A/B 前不做收益断言，也不凭静态计数贸然优化。

## 历史：白屏后退出与 D3D11On12 内部工厂（2026-11-14，13:49）

新局 `20261114-130257_YuanShen_240456`：13:02:54 加载 F6 `d0ca697`，ReShade `6.8.0.2155` 真实加载，桥路径由转储确认是 `temp3/Dx11FsrBridge (2).dll`；本局三个 Rocket 插件均开启，不能沿用此前 ReShade 关闭局的状态。13:03:37.823912 触发异常，13:04:05 收尾记录退出码 `-2147483645`。本局没有 NGX Evaluate、BeforeSR / BeforeNR 或实际 Present 入口，不以初始化的 NGX cache fallback 或 NR 架构警告替代退出根因。

实际转储 unwind 为 `D3D11On12 → 内部 factory EnumAdapterByLuid → ReShade 适配器构造 → 内部 adapter SetPrivateData → dxgi.dll+0x95512`，末端是实际 `int3`。ReShade 在没有 D3D12 proxy 时直接转发 On12，内部 `CreateDXGIFactory2(flags=0x80000000)` 因而进入普通代理链。独立宿主先证明原生 WARP On12 / GPU 读回成功，再加载当前原样 ReShade，复现相同地址和异常；只在系统层补返回边界仍会复现。

F6 现在用可嵌套线程 scope 与本局观察到的内部 flag 共同区分内部 factory。系统 factory2 与 ReShade 自身 factory2 export 的返回分别处理，复用按真实 callable 保存 original 的 MethodHooks，覆盖两种 hook 顺序。按已知 native-object QI 契约逐层持有引用，最终重新 QI 原请求 IID 并转移一个调用者引用；失败清空输出，内部工厂不执行普通包装 / spoof / Luma 策略。On12 使用独立但继承原 Returned 观察的路由 Domain，避免普通 D3D11 创建内嵌套 On12 时漏执行 scope。没有改变公共工厂、队列 / fence 或资源退休契约。

真实宿主另检出了既有 owner 校验对加载后 NT ImageBase 的误拒绝：现在只接受磁盘 preferred base 或精确映射模块地址，其他头字段 / export / 代码仍逐字节验证；错误 ImageBase 负控被拒绝。没有修改 ReShade 核心实现或安装文件。

| 项目 | 本轮证据与边界 |
|---|---|
| 固定核心 / 真实 WARP | `on12-factory-final/results.json`：两项负控均按预期 `0x80000003`；两种加载顺序各 104 检查通过，含重复 On12、GPU buffer 读回、普通代理保留、并发 scope 与 owner 错误 ImageBase 拒绝 |
| CPU / Detours | `boundary-cpu-final.log`：3,026 项 COM 所有权 / 偏移 IID / 失败 / 环 / 线程 / 嵌套创建；MethodHooks 与 HookOwnership 回归通过 |
| 公共设备身份 | `device-identity.log`：814 检查 / 46 场景与 59 项快照 / 接线检查通过；native-object GUID 抽取未改语义 |
| Release x64 | `f6-build.log`，exit 0，1 分 42 秒；保留后端参与编译，既有 C4744 / LNK4098 警告仍在，未做 Vulkan GPU 验收 |
| DLL | 32,555,008 B，SHA256 `BF6B85E99A976F54236116B7730E75EC72F64AE8C1DAA31FDBBD78B69F278755`；mtime UTC `2026-11-14T05:48:45.3595959Z`，含 `8ca0623` |
| 原位部署 | `DEPLOYED_IN_PLACE`；`local/F6-ASI` 与原神 runtime alias 同 SHA，原 FileId `0x000000000000000000020000001b42ff` 和 6 硬链接保持，无 `.next`；核对的用户 INI / Rocket / 桥 / ReShade core 哈希未变 |
| 游戏验收 | `PENDING_USER_TEST`；本轮没有启动 / 操作游戏，没有新候选的实际加载或视觉成功证据；旧局 loaded-memory 完整 SHA 未测 |

新局全部固定证据在 `build/genshin-reshade-20261114/white-screen-exit-20261114-130257/`：原 session / dump、`provenance.json`、`dump-unwind.json`、`run-evidence.json`、`validation.json`、`build-identity.json` 和部署前后回执。真实核心宿主为 `OptiScaler/hooks/tests/on12-factory/`；这些检查不执行 NVIDIA SR / NR / FG，也不能替代游戏画面和性能。全量 CommonCore 的历史结果仍属于下面对应源码，本轮完成的是表内定向回归。

## 原神 BeforeNR 接线与部署（2026-11-14，12:53 历史）

目标顺序已接入 `SR → 游戏最终合成 → ReShade BeforeNR → NR → FG`。先确认本次 source / metadata / slot，再领取单次 source；冻结原 capture serial / epoch 后，在真实 present queue 等待捕获 fence，并以 BridgeScope 原始队列身份进入 `slot.nrFilter.BeforeNr`。该事务先于 NR 私有 allocator / list Reset；公共路径与原神共用 `FilterDepthCapture`，继续使用真实深度 snapshot / reader fence 退休。原神 BeforeNR 不再绕过公共入口。

阶段归属扩展到 BeforeNR：已登记的源链和物理链在自然 Present / worker 仍抑制滤镜，预热或缺必需深度时不挪到末尾补画。`FailedUncertain` 在外层 wrapper 直接阻止后续 FG join / 子链 Present，不能仅靠缺 final-colour tag 或 slot quarantine 允许消费。

| 项目 | 当前证据 |
|---|---|
| 固定核心 BeforeNR 颜色 / 去重 | `before-nr-color.log`，147,594 检查；真实公共 PresentTransaction、当前 DX12 buffer 和下游 GPU copy 像素，含源 / 物理 runtime、worker、resize、关闭后的恢复 |
| BeforeNR 深度 | `before-nr-depth-final.log`，98,512 检查；生产 NrConsumerDepth snapshot、非对称方向与 A/B 交替，真实 capture / filter / consumer fence |
| BeforeNR LP → MXAO | `before-nr-launchpad.log`，129,632 检查；首批不写回、后续改色、法线随当前深度变化、缺深度 / 错帧不写回 |
| 不确定提交 | `before-nr-uncertain.log`，24,679 检查；真实 filter Execute 后注入 Signal 失败，停止消费者，关闭阶段仍不能解除颜色隔离 |
| BeforeSR 回归 | `before-sr-color-after-nr.log`，270,396 检查，真实核心颜色及源 / 物理 / worker 去重仍通过 |
| 生产接线检查 | Direct DLSS scene 37 项、FG guide wiring、interop policy wiring 26 项通过；包括实际 active claim、BeforeNR / Reset 顺序与外层失败阻断，源码检查不等于游戏执行 |
| 完整 CommonCore | `white-screen-common-tests.log` / `white-screen-common-result.json`，exit 0；基于 `ec1fead` 启动，BeforeNR 后续改动由上列专项与新 Release 构建覆盖，没有宣称该套件全程对应同一最终源码 |
| Release x64 | `before-nr-f6-build.log`，MSBuild exit 0，1 分 34 秒；32,541,184 B；mtime UTC `2026-11-14T04:51:17.8794446Z` |
| DLL / 原位部署 | SHA256 `8970947E37DC465DAB4F61B13A41E387306E40B45D1C9AF5B893F2E7C26E1D7C`；12:53 `DEPLOYED_IN_PLACE`，目标 `local/F6-ASI`；FileId / 6 硬链接保持，原神 alias 同 SHA，无 `.next` |

回执位于 `build/genshin-reshade-20261114/`：`before-nr-build-identity.json`、`before-nr-deployment.json`、`before-nr-installed-before.json`、`before-nr-installed-after.json`。核对的原神 INI、Rocket 配置、桥 DLL 和 ReShade core 哈希未变。磁盘 `[HoYoShade] Stage=1` 仍为 BeforeSR；需要测试 BeforeNR 时由用户自己选择阶段并启用 ReShade，AI 未替用户修改。阶段当前保留 8-bit SDR 契约，所有 Vulkan 后端完整参与 Release 编译，未做 Vulkan GPU 验收。

这些真实核心 / WARP 测试执行公共滤镜事务和模拟的 transport seam；没有直接执行 NVIDIA NR 模型或整个游戏 `GenshinPresentation::Apply`。新专用入口由实际 Release 编译及生产接线检查覆盖，游戏正确性仍待用户。首次深度 probe 的缺深度断言失败是测试把可选深度效果当成必需深度；已按实际契约修正后重跑，LP/MXAO 的必需深度拒绝仍由原样效果测试覆盖。

## 原神桥 + F6 白屏诊断与首轮修复（2026-11-14）

- 本局为 `20261114-113109_YuanShen_227276`，`t0=11:31:08.214+08:00`，F6 日志加载版本 `44380f8`。Rocket 开关为桥 / F6 开、ReShade 关；本局没有 ReShade 自然日志，外层 BeforeSR 明确记录 `core-not-loaded`。11:30 的 ReShade 日志属于前一局，不能混用。
- 11:31:37：游戏源链 count=1、sRGB format=29、flags=2；DX12 显示链创建时转换为 count=2 / UNORM，并在 DLSSG provider 中加入 `ALLOW_TEARING`。旧 resize 只转换 count / format，却把源 flags=2 传回 presenter，试图删除创建时固定的 tearing 位。真实 WARP / DXGI 负控以创建 `0x802`、resize `0x2` 复现相同 `0x80070057`。
- 失败状态让 `Dx11wDx12SC::Present` 在颜色 copy、NR 和最终 Present 之前返回。与此同时，本局 NGX Evaluate 仍有 5,884 条入口日志，桥持续 `DISPATCH_OK`；这些不能证明输出像素或显示成功。11:34:04 另出现 DX12 `0x887A0006` fence fault，不能倒推为白屏最早原因。session 最终记录退出码 0。
- 修复统一从该 presenter 的一次真实 `GetDesc` 适配 resize：保留 tearing / waitable / foreground 创建位，继续排除 DX12 不支持的 GDI 位，保留现有 XeFG 全 flags quirk；源 DX11 参数不改。健康且等价的 presenter 可跳过无用 resize，失败状态仍要求实际成功 resize；零宽高交给真实窗口尺寸解析，不当作旧缓冲区尺寸跳过。

| 项目 | 当前证据 |
|---|---|
| 显示链专项 | 259 项 CPU policy、55 项真实 DXGI / WARP 检查；旧 flags 负控 `0x80070057`，修正后真实 resize 成功；26 项生产 wiring 检查 |
| Release x64 | `white-screen-f6-build.log`，MSBuild exit 0，1 分 43 秒；保留后端正常参与构建，未进行 Vulkan GPU 验收 |
| DLL | 32,541,184 B；SHA256 `D6BABF08EAA3917BAB79BB2C28B40A663193DEE1A2AB5CB3B2496E9D37324A0A`；mtime UTC `2026-11-14T04:02:02.9011353Z`；含 `ec1fead` 构建标识 |
| 原位部署 | `DEPLOYED_IN_PLACE`，目标 `local/F6-ASI`；FileId `0x000000000000000000020000001b42ff`、6 个硬链接及当前原神 runtime alias 保持；本轮未产生 `.next` |
| 配置与依赖 | 核对的原神 INI、Rocket 配置、桥 DLL `4BC59AD0…`、ReShade core 哈希未变；不切换启动器选中构建 |
| 完整 CommonCore | `white-screen-common-tests.log`，exit 0；BeforeNR 后续源码的专项覆盖见上一节 |
| 实际验收 | 仅旧局日志绑定 PID / 加载版本；新 DLL 尚无用户新局加载、画面或性能结论，加载内存 SHA 未采集 |

证据位于 `build/genshin-reshade-20261114/`：`white-screen-20261114-113109/` 内为固定 session / bridge / F6 / Rocket 快照与解析回执；另有 `white-screen-build-identity.json`、`white-screen-deployment.json`、`white-screen-installed-before.json`、`white-screen-installed-after.json`。没有覆盖桥源码、桥 DLL、ReShade core 或用户配置。

原神 NR 前滤镜的实现、实际队列 / 当前源 / 深度 reader 生命周期与验证范围见 [兼容性文档](F6-RESHADE-COMPATIBILITY.md#原神-nr-前滤镜实现与验证边界)。本节 `ec1fead` 的白屏候选已被包含该修复及 BeforeNR 的 `d0ca697` 产物原位更新，当前部署身份以上一节为准。

## 本轮实现

以下为此前 `44380f8` 的 BeforeSR / GIMI 阶段记录，当前 DLL 身份以顶部 BeforeNR 部署一节为准。

- `Enabled=auto, Stage=BeforeSR` 正确启用，显式 `false` 优先。外部原神桥继续直连普通 NGX；没有恢复内部原神 FSR 捕获链。
- BeforeSR 接入同源帧 / 代次的颜色与深度，保留真实 producer / reader queue-fence 交接。私有深度 copy / SRV 按尺寸和格式复用，源 A/B 轮换不重建绑定。自然 Present 在登记的源链和物理输出链持续去重，覆盖 worker，不永久修改用户 effects。
- 修复私有 runtime 首次 preset reload 前写深度宏导致 `MXAO_USE_LAUNCHPAD_NORMALS=1` 丢失的问题；等待真实 reload、核对有效定义，避免反射 graph 计数触发重复编译。LAUNCHPAD → MXAO 首批在私有 scratch 预热 IPC，保留实际 completion fence，随后才允许写回 SR 输入。缺深度 / 错帧不写回。
- 内置 GIMI 独立开发提交 `a76896977d3e28a4e58bd71f45dcd261d5bebc8b` 已集成为 `06b0724`。主树补齐 native device / context / swapchain 源代次、晚 HWND 登记、选源与确认共用 owner 锁、可撤销 token，以及嵌套 proxy → native 的 shader / factory 观察。辅助链只退休，不能冒充游戏源；interop global PRE / POST 仍未支持。
- Loader 保留 Win32 / native 原参数、实际初始化状态、last-error / NTSTATUS；仅成功 Hook 才记 applied，目标模块 pin，失败由单个延时 worker 重试。修复 FSR3 重复安装和 OTA 角色漏观察；新增 queue-create / interop / hidden DX11 创建边界日志，为 Rocket 新局定位提供证据。
- 保留 SR / NR / FG、普通 FSR、DX11 / DX12 / DX11-w-DX12 / Vulkan / Vulkan-w-DX12，以及真实 queue / fence、Reset、资源退休和 quarantine。崩铁 SR 对象缓存仍由用户自备 `NgxFeatureCache.asi` 负责。

LAUNCHPAD 为本机 MXAO 提供基础 / 平滑法线，需要当前深度。它也能按请求估计图像光流；F6 当前没有读取其 flow，也没有向 ReShade 发布游戏 motion semantic。API20 可反射该纹理，但图像光流不能直接当成 SR / FG 的游戏几何运动矢量。完整范围与路径限制见 [兼容性评估](F6-RESHADE-COMPATIBILITY.md)。

## 最终构建与部署

本轮证据：`build/genshin-reshade-20261114/`。

| 项目 | 已确认结果 |
|---|---|
| Release x64 | `f6-build-release-final.log`，MSBuild exit 0，3 分 35 秒；已有 C4744 / LNK4098 / u8path 警告仍在 |
| 产物 | `build/out/OptiScaler.dll`，32,540,672 B |
| SHA256 | `082361809BA0256E1AD898CFE280AC21610F9601E59AB680B11191E8380A5F49` |
| mtime UTC | `2026-11-13T23:57:44.4550690Z` |
| `local/F6-StarRail-GapPacing` | `DEPLOYED_IN_PLACE`，FileId `0x000000000000000000070000001df2d3`，2 硬链接保留 |
| `local/F6-ASI` | `DEPLOYED_IN_PLACE`，FileId `0x000000000000000000020000001b42ff`，6 硬链接保留 |

两个既有目标已原位覆盖，实际安装哈希均与候选一致；本轮没有产生 `.next` 待替换。部署只判断目标实际文件可写状态，没有查询游戏进程。前后 FileId、完整硬链接集合、核对的用户 INI 和 ReShade 核心哈希均未变。回执：`f6-build-identity-final.json`、`f6-installed-before-final.json`、`f6-deployment-final.json`、`f6-installed-after-final.json`。

这是磁盘部署证据。需用户手动重开游戏后，用新 PID / 时间 / 加载身份和本局日志验证实际运行。

## 最终验证

| 范围 | 日志与结果 |
|---|---|
| 完整 CommonCore | `common-final3.log`，exit 0；CPU / WARP 主套件及最后的 FG / Reflex / interop / 设置共享回归通过 |
| 真实固定 ReShade 核心颜色 BeforeSR | `real-core-color-final.log`，270,405 检查；含自然链与 worker 去重 / 恢复 |
| 同帧深度与 reader 退休 | `real-core-depth-final.log`，221,337 检查；双源轮换、方向、错帧拒绝与 reader completion |
| 原样 LAUNCHPAD + MXAO | `real-core-launchpad7.log`，75,588 检查；首批不写回、MXAO 改色、真实法线随深度变化、缺深度 / 错帧拒绝 |
| GIMI 公共入口 | `gimi-present-final2.log`，真实双 WARP 设备 / 三条 chain、晚 HWND、源确认锁交错和嵌套 VS / PS；`gimi-routing-final2.log`，93 内容检查、100,000 次并发发布 |
| 真实 Windows loader | `loader-final5.log`，20 个参数 / 嵌套场景、生产 observer、自动失败重试与 OTA |
| MethodHooks | `method-hooks-final.log`，实际 original、native / proxy、嵌套 factory 返回观察、rollback / reinstall；本专项显式跳过 shared，完整 shared 由 CommonCore 覆盖 |
| NGX 生命周期 | `ngx-final.log`，15 router suites、7 release suites，通过 |
| UI / Menu input | `ui-final.log`：6,019 检查；`menu-final2.log`：51,959 检查、8 个执行负控被检出 |
| 工程闭包 | 784 个编译 / 头文件条目，无缺失或重复；最终 Release 编译覆盖全部保留后端，未执行 Vulkan GPU 验收 |

完整 CommonCore 开始后补充的 GIMI token 与嵌套 factory 修改，另由表中最终 GIMI / MethodHooks 专项覆盖；没有将开始时间不同的套件伪称为同一瞬时源码的全量重跑。日志中预期失败的 mutation / negative control 用于检出不安全改动，不是未处理的测试失败。早期 `menu-final.log` 的 vcvars 超时已由 `menu-final2.log` 实际重跑通过覆盖。

以上固定核心、Windows / WARP、CPU fixture 与源码检查各自说明覆盖范围，不能证明实际 NGX SDK / 游戏显示、Rocket 稳定性或 FPS / 延迟改善。

## FSR Bridge PR15 与上游

最终 GitHub API 复核时间为 `2026-11-14T00:09:01.7895314Z`，回执 `upstream-final-recheck.json`：main `c37d3a54ee736595bd9d461e11ab475dd137a00e`；PR15 head `97d2b06af415a6e8b4d4660b3dd6b5c1ae23db95`，仍 open / 未合并。PR head 缺 main 分叉后 10 个提交。

没有整包合入 PR15：其清除低分辨率 MV 标志但继续提交低分辨率纹理，违反 F6 FG 输入契约，新增字段默认值 / 测试也未闭合；预测合并候选的真实 NgxParamMap 测试为 3,657 检查、18 失败。安装器改配置、复制 ReShade、自签和信任根行为不在集成范围。

实际采用 main 加独立 `Ffx12SingleInstance` 接管配置控制，隔离源码保存点 `3ec47e3de096fd7f64872f819d108a0535938884`，归档 `bridge-source-3ec47e3.zip`。Release x64、NGX ON、release runtime ON、diagnostics OFF；4/4 CTest 及 NgxParamMap 的全部 3,657 检查通过。

早期部署的 `B5966CFF…` 桥已被下述提供者转发修正版覆盖；`bridge-deployment.json`、`bridge-installed-after.json` 只保留该次历史部署。

### 续接：提供者与 COM 转发修复

用户补充“只注入 FSR 也会崩溃”。实际旧桥 DLL 的独立宿主复现了两个同名 `d3d11.dll` 共用 original：第二提供者的 GPA / IAT 调用进入第一提供者。成功返回设备后，COM 方法也存在同类错配；同址 Factory2 / Factory 的重复安装还可能保存自身 hook。

运行时修复保存点 `f53873a7943b502e7739f5361c8b8f65c714b602` 按实际 callable 统一创建入口与 COM original，逐线程恢复嵌套调用；用标准 QI 定位实际 context，保留代理的绘制前后处理，内部 bypass 不再全局切换 vptr。接口表按实际 SDK 能力保留完整长度，并纠正 SwapChain4 的 HDR 方法槽 40，保留槽 39 的 `ResizeBuffers1`。最终完整源码为 `586f7f0fd77be27f9ed9884537fff3180b88bbb9`，只追加本局 `ResizeTarget` 回归与 fixture 的 UTF-8 构建设置，生产源码与前一提交一致。未改 Rocket / ReShade 核心或用户配置。

最终 `bridge-routing-final-verified.log` 为 Release x64 / NGX ON / release runtime ON / diagnostics OFF 的构建与 15/15 CTest 通过证据；包含真实 DLL 多提供者、嵌套成功路径、页边界接口表、SwapChain3 / 4、不同布局交换链的 `ResizeTarget` 正反创建顺序和 WARP 字节读回。旧桥负控 `bridge-route-negative-old.log` 检出 5 项错误；SwapChain4 修正前的 `bridge-routing-hdr-negative.log` 检出两个提供者的槽位错配；`bridge-resize-routing-negative-old.log` 与 `bridge-resize-routing-reversed-negative-old.log` 各检出 2 次错误转发。`DllMain` 归属 `Dx11FsrBridge.cpp.obj`，产物含新增提供者绑定日志。首次新增 ResizeTarget fixture 因 provider 未开 `/utf-8` 失败的日志保留，使用既有统一测试配置后实跑通过，不归因于生产路由。

2026-11-14 10:18（UTC+08）已原位覆盖既有 `temp3/Dx11FsrBridge (2).dll`：SHA256 `94D3B633195B55EE8F398FDD837CAB4A732FD718962FD2BEA81FD7FC56C275C0`，818,176 B。原 FileId `0x000000000000000000040000001d8630` 与两条硬链接保留，两个别名哈希一致；桥 INI、ReShade 核心和 F6 DLL 哈希未变。最终回执 `bridge-routing-final-build-identity.json`、`bridge-routing-final-deployment.json`、`bridge-routing-final-installed-before.json`、`bridge-routing-final-installed-after.json`；未生成待替换 `.next`。较早 `bridge-routing-*` 的 A49014B2 是本轮中间部署记录。

完整源码归档 `bridge-source-586f7f0.zip` 留在本轮证据目录；可重建差异随 F6 保存在 [外部桥补丁](../patches/genshin-fsr-bridge/README.md)。独立复核记录了 COM 同址重建与克隆表退休的继承缺口，已列入桥的 `docs/OPEN-ISSUES.md`，不据此解释首次启动退出。

08:54 局的真实 dump 已把该次访问冲突绑定到旧桥原方法 / receiver 错配，具体见下节；更早的 `0xC0000135` 仍无同等调用栈证据，不能归为相同根因。新桥加载及启动稳定性仍需用户手动新局。

## 已锁定游戏证据与待验收

### 最新：仅桥失败局与 GIMI 创建边界

2026-11-14 10:54:47（UTC+08）PID242576，12.4 秒后 `0xC0000005`。Rocket 配置的插件顺序为桥 / F6 / ReShade，启用值 `1|0|0`；本局无新的 F6 / ReShade 日志或加载证据，实际桥日志在本局截断为空。session 的 `mod` 清单为候选路径，不能据此声称 F6 实际加载。证据副本及逐文件 SHA 位于 `bridge-only-1054-20261114/`；此时磁盘桥为 `94D3B633…`，不等于已核对其加载内存身份。本局没有异常栈，根因仍未知。

用户指定的 `thx114/genshin_fsr_brigde` 远端固定源码为 `2c8a91e98d8d93a7ea4a9375c1402c75b9aa4848`；本地历史树 HEAD 为 `366e778221f5b0521ba9c95838f1e731d9db26db`，另有 47 条状态，保持不动。只借鉴本地旧桥的嵌套创建隔离概念，未复制其实现、DLL 黑名单、全局 original、固定长度虚表、加载器或同步读回。旧树的卡顿体感与静态成本点不作为本候选性能结论。

外部桥源码保存点 `5ce2efbf4651a4df7d5e3bd1b0ccfe6a1f45fd5b` 将两个 D3D11 创建 API 纳入同一线程 scope：嵌套调用保持原参数和返回值，最外层包装完成后统一观察返回的设备 / 工厂 / context。原有标准 QI 的执行 context 接入与代理绘制路径保留，未改变每帧路径。新宿主在包装器仍未返回时检查方法身份：旧 `94D3` 在三个用例中分别有 3 项提前改写，新候选均为 0；覆盖 Device → Device、Combined → Device 和失败后重试。combined 用例未创建真实 swapchain，代理 fixture 未执行真实 GIMI BeforeDraw / AfterDraw。首次编译的辅助函数声明顺序错误已修正，失败日志保留。

`bridge-creation-boundary-final2.log` 确认实际重编生产 CPP 和新头，Release x64 / NGX ON / release runtime ON / diagnostics OFF，18/18 CTest 通过，包含此前 15 项。只读独立复核无已知源码阻断。最终 DLL 815,616 B，SHA256 `4BC59AD014795022A2E11A91F7E994A043507F76B68B87EBC114FBC17F3E4883`，11:23 原位覆盖既有 `temp3/Dx11FsrBridge (2).dll`，FileId `0x000000000000000000040000001d8630` 和两条硬链接保留，两别名哈希一致，无 `.next`。桥 INI、Rocket 配置、ReShade core 和 F6 DLL 哈希未变，未操作游戏。详见 `bridge-creation-boundary-result.json`、`bridge-creation-boundary-deployment.json` 与 installed-before / after；可重建差异为桥补丁目录的第二个补丁。此回归证明创建时机改正，不证明本次游戏启动崩溃已解决。

下述会话时间为记录中的 UTC+08，不再追加八小时；均为本轮部署之前的旧候选，不能代替新版本验收。完整统计和 SHA 清单见兼容性文档及 `locked-files.json`。

| 会话 | 已确认事实 |
|---|---|
| 普通局 `20261114-024410_YuanShen_57172` | 1,446 个 Evaluate 入口；816 个前台且 Opti 菜单关闭的源间隔，P50 / P95 47.408 / 58.934 ms，中位源间隔倒数约 21.1 FPS |
| ReShade 局 `20261114-024706_YuanShen_230352` | 1,571 个 Evaluate 入口；341 个对应源间隔，P50 / P95 59.198 / 72.326 ms，约 16.9 源 FPS；旧 F6 `auto + BeforeSR` 实际关闭 |
| Rocket 局 `20261114-042258_YuanShen_228728` | 约 38.3 秒退出 `0xC0000135`，SR / NR / FG 入口全 0；XXMI 的主动 `DoubleBeepExit()` 也使用此码，未定位实际退出分支，无对应 dump / WER |
| Rocket 局 `20261114-082043_YuanShen_227916` | 确认加载 `44380f8`，约 31.1 秒后 `0xC0000135` 退出；隐藏 DX11 swapchain 创建尚未返回，未进入 SR / NR / FG |
| 仅桥对照 `20261114-084512_YuanShen_238544` | 约 8.6 秒后 `0xC0000135` 退出，桥日志停在创建设备接管后；该局不能用旧 F6 日志证明加载了 F6 |
| 新局 `20261114-085448_YuanShen.exe_240612` | 加载 `44380f8` 和旧 B596 桥；隐藏 DX11 链和 DX11-w-DX12 wrapper 创建成功后，旧桥把 F6 wrapper 交给 ReShade 的 `ResizeTarget` 原方法，在 `ReShade64.dll+1804B7` 发生 `0xC0000005`；UEF dump 146,284,432 B，已逐文件复制并 SHA 锁定 |

本局 dump 的真实展开栈为 `ReShade64+1804B7 ← Dx11FsrBridge (2)+16B7C ← YuanShen+AAB844`，活动栈没有 F6 帧，不能把栈扫描的残留地址冒充活动调用。旧桥基址 `7FF871EC0000`，timestamp `6AF7870F` / image size `AA000` 与保留的 `bridge-only-lock-20261114/bridge-baseline-B596.dll` 匹配。主会话重读 dump，核验了全局原方法位置 `7FF871F5EA10` 指向 `ReShade+180480`，receiver 为 F6 wrapper `2A033225A50`；ReShade 把其 `+8` 的 Operations `7FFFF0039FB8` 解释成自己的原对象，再把 F6 函数地址当虚表，在 `call [rax+70h]` 访问冲突。寄存器、模块、指令和匹配 PE 身份见 `dump-resize-routing-analysis.json`；完整加载内存 SHA 未核，wrapper 虚表槽页未保存在 dump 中。本次无需修改 F6 wrapper 布局。新增异构布局专项通过、旧桥负控失败，说明修正版覆盖了这个调用错配；仍不替代新桥游戏验收。

没有这些会话的 Display / PresentMon 数据，不能报 FG 后显示 FPS。两局场景和设置不同，不能把差值直接归因 ReShade。新源码修复和测试不等于 Rocket 崩溃已实机修好。

下一轮由用户手动验证 Rocket / GIMI 进入场景、BeforeSR 的 LAUNCHPAD + MXAO 画面、SR / NR / FG 与实际性能；AI 读取同局新证据。[AGENTS.md](../AGENTS.md) 和 [README.md](../README.md) 的 9 项已知问题、7 项 TODO 继续保留，不据本轮源码或离线测试划掉。

## 历史保存点

- 阶段 0：`5d0f0aebd5cfe690703d89a5012c6c52a5226079`。
- 阶段 1：`2052553`，整体移除内部原神捕获链，将 Hook / 身份 / 计时移至公共模块，拆分 DLSSG Inputs / Lifetime 和 NR GPU execution lifetime。实际 queue / fence 退休保护保留，Vulkan 两条后端完整保留。
- 阶段 1 DLL `E454F55C1A8E185F0B6703BBA6AF1E10F6AED310B2D4931DF91BEECD8E051418` 已被上述 `08236180…` 候选原位覆盖。历史构建 / 回归 / 部署证据保留在 `build/fg-runtime-followup-20261113/phase1-*.json` 与日志中，不代表当前安装内容。
