# DLSS5_NEXT 当前状态

更新：2026-11-12（UTC+08）。唯一产品写树 `D:/CODE/DLSS5_NEXT-workers/f6-rewrite`，分支 `codex/f6-rewrite`；保留既有未提交改动。身份与结果分别见 `build/current-{build,tests,artifact,deployment}.json`。

## 当前候选：DD53514B 已构建、未部署；正式安装仍为81DBE9A5

按用户要求停止DS，问题检索及独立复核使用 **gpt-6.1-sol / high**，核心由主会话实现。已核原始日志：PID164004尾段copy reader实际等待83.962／98.681／22.469 ms，随后另有隐藏DX11 Present的101.810／92.761／173.323 ms。普通interop虽有3个allocator，但本局游戏只有1个共享颜色slot；不能只轮转allocator或删真实退休等待。峰值166.103 ms与上述尾段属于不同报告／窗口，不能相加或宣称唯一根因已解决。详细证据与边界见 [性能审查](F6-PERFORMANCE-REVIEW.md)。

本候选的总／模型／层计时器复用真实recording已有的queue/fence/value，不再另建fence或Signal；query heap/readback由同一Use持有，读取须实际GPU完成，查询槽复用还须真实Reset。修正外来list收尾及guide失败遗漏End；重放、Signal失败与资源退休门保持原语义。前3次及每64次dispatch低频记录7个不重叠GPU跨度，无新增CPU等待、额外队列或像素读回。已完成仅等Reset时由事件推进。

Look的24次resolve均debugView=2，原始模型预览提前返回；实际修改Look或点击“查看最终效果”时返回最终画面，浏览不改设置。生产控件80、正确输入／输出描述符和最终Look HLSL/WARP392、timer WARP668、18组真实历史生命周期及FG交接／Reflex／pacing／options／热重建／shared回归通过；错误颜色域负控仍按预期24项像素失败。**此前36项“旧色”负控使用了错误的描述符身份，已撤回对应根因与省复制声明**，不是游戏残影修复证明。

Release x64 exit0，33,959,936 B，39 warning records／0 errors，14项必要NGX导出齐全。候选SHA256 **DD53514B481D4E2DCAAA5F9D95ABA269E1DEB8834CFDEB6BD3E62852AD526646**，mtime UTC `2026-11-11T16:37:07.9126767Z`；21项关键源码哈希构建前后匹配。当前构建／测试／产物回执绑定候选，正式部署回执和实际DLL仍为81DBE9A5。**本轮未部署、未运行游戏、未改配置**，loaded-memory SHA未知，延迟、画面和历史效果未验收。上一63DD构建等回执保存在 `build/latency-followup-previous-{build,tests,artifact,deployment}.json`。

## 已部署：81DBE9A5（已有新局，问题未通过）

**2026-11-11：用户反馈 4CBA8CBC 仍全屏拒绝；旧调试参数属于间隔复用，不能用来调整每源帧持续保留。** 本候选增加独立且实际接入生产 shader 的持续保留深度、颜色、运动容差与噪声下限；默认深度 0.08、颜色 0.08、运动 2 工作像素。普通 NR 页面和每帧调试窗共用控件，改值更新有效历史身份和观察配置，清除不兼容历史；不改 SR/NR/FG 方向或用户层数、分辨率等参数。

独立的**最终历史稳定时间**为 0～2000 ms（默认 0 关闭）；配合低可信比例（默认 0.65）和变化容差（默认 0.08），在坏深度或坏运动下仍可短时降噪。只平滑 NR 相对当前原画的残差，不平均旧整帧；长时保留时长或强度为 0 时仍可独立工作，总开关须开。稳定值单独保存，不回写长时原始残差/出生指纹。时间是衰减常数及平均滞后上限，不是精确时间窗或整帧冻结。仍按当前外观约束、逐通道修正上限、真实相邻源和重置门限制；高参数可能拖尾，实机未验。用同一次历史 dispatch，启用时两张模型尺寸 FP16 状态纹理；关闭不分配，预算和真实 queue/fence/Reset 退休不变，无新增 CPU 等待。

调试窗可展开**调节历史参数（与 NR 设置同步）**，显示各源帧实际阈值；新增**最终稳定权重**图，黄绿色表示低可信稳定的实际非零贡献。界面默认折叠参数，保留帧列表空间。旧间隔复用参数继续只作用于其自身路径。

最终生产 HLSL/WARP **3,281 检查、0 失败**，含可调阈值、坏/无像素 guides、零值、时长、120 Hz 时间响应、双向噪声、边界、原始历史不被稳定回写和 90 源帧有界滞后；旧出生指纹漂移 mutation 仍预期失败。135 策略检查、18 组真实历史生命周期、FG 交接/颜色事务、呈现输出、预览/采集、热重建和 shared 回归通过。实际 NVIDIA `10DE:28E0` 的嵌入 DXIL/FP16 两组各 **40 帧、329 检查**通过。模型答案是夹具，不执行 NGX/游戏。FXC 仍报 X3577/X4000，未声称零警告；缺输入/首帧/NaN/FP16 已覆盖。

Release x64 exit 0，384 warning records、0 errors；DLL **33,951,744 B**，完整 SHA256 `81DBE9A53B90990FA06BF4CB3790B240B187AAF46841906C7D665E9C0FFF8875`。状态 **已部署并核验**，记录时间 `2026-11-11T22:09:48.1977639+08:00`。正式 F6 与三个 runtime 的哈希/硬链接/File ID 一致，七份当前配置未改。 原构建/测试/产物回执保存在 `build/look-reflex-previous-{build,tests,artifact}.json`，实际部署仍见 `build/current-deployment.json`，源码范围 `build/history-tuning-source.json`。之后已有 PID153992/164004 新局，用户报告延迟升高和Look无效；日志已分析，游戏降噪/拖尾、贡献、性能与卡死恢复未验收，不能继续记成“尚未启动新局”。loaded-memory SHA 未核。

## 上一候选 4CBA8CBC（用户仍报告全屏拒绝）

2026-11-11 21:21（UTC+08）部署，完整磁盘身份、离线检查及回退见 `build/history-tuning-previous-{build,tests,artifact,deployment}.json`。用户仍报告全屏拒绝及 NR 噪声/照明跳变，不能把之前离线通过记作实机通过。

## 上一候选 8B（实机失败）

**8B08F1BC 已原位部署并核验**，时间 **2026-11-11 20:19:06（UTC+08）**。Release x64 exit 0，33,823,744 B，SHA256 `8B08F1BCE4CAC5329C10C37D21A433F57E1B7BF2BFAAD7A621499DE91C824A16`；39 warning records、0 errors，14 项必要 NGX 导出及实际嵌入的 DXIL/SPIR-V 均已核验。正式 F6 与三个运行库四路径哈希、硬链接和 File ID 一致；七份当前配置未改。部署前无活跃游戏，仅见零线程/零句柄旧进程残留，未启动游戏。回退 **B9D84143** 在 `build/rollback/OptiScaler-20261111T121905655-67ca95a290f74672936acfab5a7f4fdb.dll`；上一部署回执 `build/history-inert-previous-deployment.json`。当前身份与范围源码哈希见 `build/current-{build,tests,artifact,deployment}.json`、`build/history-inert-source.json`。

用户已确认 7C281A54 的卡顿/旧帧闪回解决，但 **B9D84143 新局持续细节仍无效**。19:29–19:31 日志显示独立历史连续准入、录制并发布，截图中的准入/发布状态不证明像素贡献。本次修复下列可重复像素缺陷；实际加载、游戏画面与性能仍待新局验证，loaded-memory SHA 未核。

## 本轮修复：历史已发布但细节无效

- **正确亚像素移动误拒绝**：旧 shader 将当前已滤波像素与历史 footprint 的每个未滤波 texel 比较，正确的半像素纹理移动也会清空历史。现比较实际重投影颜色及同位移五点结构；每个正权重 tap 的深度、有限值、年龄与边界仍须通过，并用当前轮廓约束历史颜色范围，保留相反轮廓平均值负例。
- **新细节消失时旧细节也被压零**：旧幅度包络只靠当前 NR 残差/原图边缘，稳定平面上 fresh 残差为零即无显示贡献。现增加由已验证表面当前/出生颜色限定的有界残差预算，不能由旧残差幅度扩张；遮挡、出生 fingerprint、运动、寿命与源身份门不变。不同有效年龄 footprint 取最老年龄，避免精确相等要求造成反复重置，也不平均续命。
- **面板不再冒充效果证明**：分开显示“融合已录制”和真实 fused/fresh RGB 变化采样点比例、源帧及年龄。仅该页可见时请求现有异步采样，最多 2 Hz，关闭后需求过期；提前限频，菜单只读固定元数据，不逐帧复制图像。新配置/源 feature 代次及过期结果不能充当当前贡献。采样观察不参与历史准入或退休。
- **证据**：旧生产 HLSL 在新增正确移动/细节消失测试中先出现 3 项失败；修复后生产 HLSL WARP **3,087**、跨表面 **526**、历史生命周期 **17 场景**、GuideCapture **24,019**、呈现输出 **1,586** 及全部共享回归通过。额外在 NVIDIA `10DE:28E0` 上运行实际嵌入 DXIL、FP16 历史与生产发布模块，连续 **40 帧/329 检查**通过；从部署 B9 DLL 提取的旧 shader 在同一测试中按预期以零持续贡献失败。模型答案是夹具输入，不执行 NGX，不代表实机已修好。
- **测试/构建边界**：`build/history-inert-{final-tests,measurement-tests,hardware-final,old-dxil-negative}.log`。完整 DXIL 在本机 WARP 创建 PSO 返回 E_OUTOFMEMORY，因此真实 DXIL 用显式可选硬件场景，普通 WARP 生命周期与 HLSL 测试仍保留。首轮构建暴露配置哈希初始化类型不一致，改显式 uint64 数组后构建通过；初次失败记录保留。用户分辨率、层数、Look、历史时长、FG 倍率/坐标及已解决的 FG 交接顺序未改。

## 上一版：模型选择、HUD 与独立细节保留

**B9D84143 曾于 19:26:52 部署，现已由 8B08F1BC 替代**。其 Release x64 exit 0，DLL mtime 2026-11-11 15:59:34（UTC+08）；90 warning records、0 errors，14 项必要 NGX 导出齐全。范围源码哈希见 `build/model-history-source.json`，上一构建/测试/产物回执保存在 `build/history-inert-previous-current-*.json`。该版开关/生命周期修复未解决上面的像素失效。

- **SR 模型**：DLSS 设置中的单选为默认/J/K/L/M，点击“应用更改”沿现有 NGX 预设与 SR 重建流程生效；完整旧预设保留在高级折叠项。默认跟随游戏；SDK 是否实际接受指定预设仍须实机核查。
- **FG 模型**：DLSS FG 设置增加驱动默认/A/B/C，保存并重启。只覆盖本进程成功的 NVAPI 驱动设置查询，不写驱动 profile；覆盖查询有低频回执。菜单选择/配置保存不证明模型已经加载。
- **HUD**：已有 HUD-less/UI 色与 Alpha 输入标签；只在同源、同代次两项标签均成功时请求 SDK 重合成，UI 保护观察状态也使用该真实选项。外部桥 ABI/生产回调尚未接入，原神旧遮罩不等于真实独立 UI。[桥作者输入约定](FG-BRIDGE-HUD-CONTRACT.md)同时说明可选单通道 UI Alpha、颜色阶段和真实 queue/fence 交接。
- **细节持续保留**：请求与旧间隔复用开关解耦，持续保留选中时每源帧完整模型计算，并独占残差历史；被拒绝时继续当前 NR，不回落旧间隔历史。修正旧 Host 的 `keep` 赋值覆盖；活动 FG 的旧行为负控先失败、修复后通过。保留全部源身份、相位与实际 queue/fence/Reset 约束，未修改已解决卡顿/闪回的呈现发布顺序。

部署前最新日志为 **19:15:11–19:20:27、PID178728**，当时正式 F6 与原神 runtime 磁盘哈希仍为 7C281A54，尚未包含本候选。19:18:33 起采样显示持续保留开关为真、native phase/ordered queue 已验证，而 legacy 融合状态持续为假；日志没有独立 `detailFeedback*` 字段，不能据此确认独立反馈实际执行或拒绝原因。本候选在原有最多 64 行 cadence 日志内加入独立请求/准入/历史来源/录制/发布/待退休字节及原因；不再把 native 输入误报为 ordinary-picture 未验证。实际非零贡献和栅栏/人物跟随仍需部署后新局验证，本轮未新增代码改动或游戏验收结果。

离线结果：独立策略 127、实际 Host 136、生产历史 shader WARP 3,048、跨表面 shader 526、历史生命周期 17 场景/346 检查、FG options 1,116 检查通过；全部公共 CPU/WARP 套件和最终共享回归完成。首次完整运行在旧 UI 字符串静态断言处退出，保留失败日志；将断言更新为当前双输入准入后，复跑 FG options 与全部共享检查通过。日志为 `build/model-history-tests.log` 与 `build/model-history-shared.log`。离线结果及构建不证明新候选的 NVIDIA 模型、画面、真实显示率或性能已通过。

## 已部署：呈现 NR＋FG 共用资源时序

- **隔离 SDK 输出队列**：失败局日志显示游戏 client queue 与 SDK physical-output queue 不同。SDK 嵌套 DXGI factory 曾将物理输出队列发布为全局 source queue，令 SR／共享复制与呈现 NR／FG 分属不同队列。现用真实创建角色抑制这类发布，九处 factory 接点共用一个入口；输出 swapchain resize 只更新自己的队列。作用域即使没有可选 endpoint 准入也生效，不能充当资源权限或完成证明。
- **在长 NR 之前封存本源帧 guides**：先前 FG 对共享深度／运动的读取在 Evaluate 后录制，却拖到呈现 NR 后才执行，也未在下一次 DX11 覆写前交回读所有权。现于真实 Evaluate 接点提前提交该实际 recording，在准确的 reader queue 上 Signal 已有共享 fence，并向 DX11 排入 Wait；后续最终颜色仍按原源身份汇合。正常路径不新增 CPU 等待，也不让下一帧等待后续整段 NR／FG。预览 guides 亦在原 recording 内读取，封存后不再读取裸共享 guides。
- **失败保留真实所有者**：仅在首次实际读入前 pin 共享资源，CPU 校验／预热失败不永久阻止上传；部分提交、错误 queue／fence 域或 Signal 失败则阻止新共享写入并隔离 provider。资源仍须实际 host／SDK 完成与 executable recording 收尾后释放，不借别的 fence、不扩池、不用旧帧补齐。

保留此前分辨率重建收尾、独立细节历史、失败保留本帧与输入复制精简。内部 FSR 桥／探测继续禁用，保留实际外部 DLSS Evaluate、桥 2.4 ABI、用户模型尺寸／层数／Look／历史时长／FG 倍率及独立 NR／FG 方向。默认呈现输入 → NR → 细节保留 → FG，SR 前／SR 后入口仍可选；本轮不重写 shader 或自动校准。

## 离线证据与边界

新增 **FgHandoffWarp 15 场景／564 检查、9 项静态接线检查**通过。提取实际生产创建／队列发布、提交、封存和共享 fence 交接函数，运行真实 D3D11＋D3D12 WARP 队列、共享纹理和像素读回。以显式 GPU copy 表示 SDK ValidNow 快照，不执行 NVIDIA SDK／NR 模型／swapchain。三项旧行为负控均按预期 exit 17：旧队列发布覆盖源队列；旧延迟读取和缺少 writer Wait 两例均将应读的 source 1 读成 source 2。修复版像素匹配。这证明本地跨帧读取缺陷及修复，不等于实机全部残影已找到唯一原因。

受影响 runtime 回归通过，包含 bridge retirement 15／494、呈现输出 1,586、Evaluate guides 22、pacing 21／323、GPU 预览 1,059,448 与重放 58、菜单 992、方向 837、interop 207、参数 17 组及 options 1,111。原组合运行随后停在迁移后的静态断言，不能把该日志记为全过；修正断言跟随共用 SubmitUICommandList 后，最终 focused 与完整 shared 回归通过，见 `build/fg-handoff-final-focused.log`、`build/fg-handoff-shared-regression.log` 及当前测试回执。

最初 WARP 夹具使用可 CPU 访问共享内存的 UpdateSubresource，绕过了排入的 GPU Wait；已改为保留上传纹理并执行真正的 CopyResource。另两次中途失败是静态断言仍查找旧 Present 内联提交位置，现沿实际委托核验顺序、失败和完成门。这些是测试宿主／断言修正，不能当作游戏像素证据。

## 最新反馈与下一步

7C281A54 之前的主要复现是呈现 NR＋FG 联合产生混合／闪回帧，SR 前／SR 后配 FG 节奏正确。该卡顿/闪回复现以用户本次“已解决”为最新结论；轻微 FG 残影、人物/UI 保护、60—100 ms Reflex 延迟以及真实显示率不因此自动验收。SDK client-queue 保护范围见本地 `external/Streamline/sl_dlss_g.h`，不把它当对其它队列的保护。

失败局 PID162340 在 07:29:59—07:30:30 的源 Evaluate 为 **19.645 Hz／间隔中位 50.075 ms**，SDK LocalPresent **117.968 Hz／间隔中位 0.809 ms** 只是突发回调。14 对 GPU model／NR total 中位 **34.081／35.498 ms**，Reflex 区间中位 **71.053 ms**。copyAllocator 37.961 ms 被 copy／resolve 38.020 ms 包含，不能相加。较小 pre-SR 模型的约 9 ms 不是固定模型尺寸的路线对照。完整日志 `build/genshin-162340-OptiScaler.log` 为 31,053,934 B，SHA256 `5EDA9370FD8EDDB2F1B8DA194A6783BA49D0C05A09F190647C6672C7BB511CCF`；session 16 MiB 副本截断。

本轮独立处理 SR J/K/L/M、FG A/B/C、HUD 输入对接约定及失效的细节持续保留。新候选需验证细节保留的实际非零贡献与栅栏/人物跟随；FG 模型须核查真实 NGX/驱动查询，HUD 须取得桥作者具体 ABI 后接入同源输入。实际同一对源帧的最终颜色/guides 与物理显示事件仍缺。保持 SR 前/SR 后对照入口，三游戏分别验收。

以下为较早候选与历史记录。

## 2026-11-11：NR 残影、调参后持续掉速与预览修复

最新结束局 PID155136 为 2026-11-10 22:05:19—22:26:32（UTC+08），launcher exit 0。完整日志 `build/genshin-155136-OptiScaler.log`，93,383,658 B，SHA256 `1A244CAD199DF772FC3677F027F0A5A709B3B4064A851FA869AC33486CDF2C46`。磁盘候选为 8027EF17；精确 loaded-memory SHA 未核。原神本局使用原生深度残差历史（TemporalContent=0），不是星铁／ZZZ 的 light-edit 内容路径。日志证明深度／运动及融合录制，不证明重投影像素正确或实际显示率。

离线已复现：相同深度下，旧颜色方差框会接受偏离薄边的残差。新增原生残差结构 veto；旧生产 shader 的四方向负例 117 项中 8 项失败，当前 526 项通过，包括 1 秒／25 帧栅栏移动、错误运动拒绝和有效运动下真实非零贡献。Vulkan、legacy、picture-flow 和 light-edit 原逻辑保留；新增纹理采样的 NVIDIA GPU 成本尚未实测。

确认并修复命令列表跟踪溢出后的永久全局慢路径，保持精确身份并在 Reset 后移除；本局没有溢出占用证据，不能认定它已解释 110→80。旧资源退休仍绑定全 registry 的保守快照；是否在本局造成滞留未知，未删完成门或借其它 fence。重建日志补入现有退休条数、recording 数和 halted 状态，已有低频 cadence 补入溢出占用／峰值；无新逐帧长日志或等待。

预览维持原有九项单选：在物理输出保存菜单绘制前的全分辨率 GPU 图，短暂获取失败仍显示同域缓存；epoch／配置／方向切换失效。输入封存权限在进入菜单前固定，不能因菜单内切换 FG 获得。仅元数据显示清除不授权资源退休。FG-only 接点独立请求真实 Execute／Reset 通知；FG 颜色可使用当前已接受颜色标签与本 slot 已完成 allocator，不借 NR guides；深度／运动仍要求其实际拥有 recording。SDR／后端限制继续明确。

本轮交付：DXIL／SPIR-V 及嵌入头同步；完整公共套件分段覆盖完成。新增缓存测试漏关 recording 的夹具问题已修正，真实生产完成／Reset 门不变；随后输出 WARP 994 项通过。预览生产 WARP 1,059,482 项、首用重放 61 项、捕获 WARP 23,996 项通过；NR 热重建 30 场景／191 项与三项不安全负控通过；FG admission 8 项、ZZZ 公共链路 101 项静态检查通过。Release 208 warning records／0 errors。部署回退为用户失败的 8027EF17，不是已知良好版。CPU/WARP、静态检查和构建不证明 NVIDIA SDK、实际显示或性能通过；用户自行开启新局验收。

以下 2026-11-10 小节保留当时分析，残影归因以本节用户最新确认优先；原神、星铁、ZZZ 仍需分别实机验收。

## 2026-11-10：FG 源帧时序与独立 GPU 单选预览

当前身份、测试与部署分别见 `build/current-{build,tests,artifact,deployment}.json`。最近结束的 **PID152408** 关联磁盘候选 D12D64AC，性能失败；精确 loaded-memory SHA 未核。13:41:37–13:41:43.5 日志有 118 次源 Evaluate，约 18.14 Hz、间隔中位 55.109 ms；录像相邻近似不变采样约 57.6%，都不能代替 PresentMon 真实显示事件。分析保留在 `build/video-134134/analysis.json`。单次约 38.96 ms NR 包含跨度与约 85.8 ms FG 停顿不相加，也不能解释整段慢速。

本轮修改：direct-DLSS 源角色独立于内部 FSR 桥；不可变 pacing 决策持有原始 SDK token 及会话准入，释放 FG/execution/continuity 锁后等待；API 变更须避开在途调用。SDK GetState 统一串行入口，显示真实累计区间；overlay 只读缓存，实际显示率无事件时明确未知。保留应用原始 swapchain flags，区分应用和 SDK waitable 所有权。

预览复用原有九项单选条，独立全分辨率 GPU 快照、每源帧一次且上限 30 Hz。关菜单继续，选择关闭停止新增；NR 关闭可看实际 FG 标签及 SDK 缩放。快照在封存输入后的物理输出层绘制；生产和读取均要求所属 queue/fence 加成功 Reset，延迟回执绑定 recording 代次，窗口重建亦须结束旧读取 recording。关闭后在途回执仍处理，空闲无匹配 recording 不扫描；普通预览不使用 CPU 格子画布。当前仅 DX12 DIRECT 与 SDR 输入／输出受支持。

有界事件关联 Evaluate、NR 结果、最终颜色、FG 常量/token、SDK 查询和输出；显式双帧导出与离线八组合分析不自动改参数。FG 残影像素根因仍未知，NR 1 秒栅栏历史单独复测；CPU/WARP、构建与部署不等于游戏画面或固定参数性能通过。下方较早实现与实测保留为历史。

用户对上一部署 **795599E1／PID56932** 实测失败：文字闪烁、卡顿／伪影加重；NR 效果与 NR 历史全部关闭后，FG 人物残影仍存在。另有独立 NR 长时历史问题：设置 1 秒时，向前走会留下栅栏分身，地板保留较正常。不能将两个问题互相归因，也不能用模型成功或历史融合录制替代像素正确。

诊断为关闭／NR 编码前颜色／NR 输入颜色／NR 深度／NR 运动／FG 颜色／FG 深度／FG 运动／历史贡献单选，默认关闭，一次仅请求一路；固定状态与真实年龄，详情折叠。取消转头向导及正常面板评分 worker；冻结或选择关闭停止新增图像，关闭菜单继续预览。FG-only 预览独立请求实际 Execute／Reset hook，不依赖先开 NR 或原始预览。

原生深度历史确认两处跨表面污染：背景借邻近前景的深度／运动通过准入；中央 2×2 深度包围区间接受混合表面后硬件双线性仍混入前景细节。旧生产 HLSL 的真实 WARP 像素负例为 8 项中 3 项失败，初次修复后 8 项全过。修复限定 DX12 temporalMode=1：所有非零权重中央 tap 必须与当前表面相容；donor 也必须相容并用接收像素 ownDepth 验证，平局优先自身。拒绝时使用当前 fresh NR，不缩短用户保留时间；legacy、Vulkan、picture-flow 模式 2/3 维持原语义。DXIL／SPIR-V 及嵌入头已同步。扩展生产 shader WARP 像素检查 105 项全过，包含完整反馈链、±XY、region／scale／phase、缺深度／NaN、500ms／1000ms cohort 交接；采样 WARP 23,947 项及历史重建／公共接线回归通过。测试只执行 HLSL 的 WARP 版本，不等于 NVIDIA SDK 或实机画面通过。

FG 人物残影根因仍未知。审查未证实内部双重翻转、额外缩放或固定一帧偏移；外部 NGX 的像素运动方向、最终颜色相位与 SDK 插值仍需实际预览／画面确认。未自动改 NR／FG 方向及用户配置。无 HUD 输入继续 FG 并标 UI 未保护。内部 FSR 桥和探测保持禁用，来源为实际 DLSS Evaluate。

构建、部署与离线通过不代表实机恢复。原神先分别验收 SR、SR＋NR、SR＋FG、全开，重点检查栅栏／人物遮挡、原地转镜头、模型热调节；星铁、ZZZ 各自回归。没有测得诊断 GPU 开销或游戏提速，不把性能退化唯一归因于面板。下方是历史记录。
## 2026-11-10：FG 独立坐标控制

FG 旧 `ResourceFlip` 把深度行翻转、运动行翻转和运动 Y 取反绑在一起。现新增 `[OptiFG] DepthTextureFlipY / MotionTextureFlipY / MotionYInverted` 三项独立覆盖；`auto`/缺失逐项继承旧开关，显式 false 保存后仍有效，不联动 NR。菜单显示有效组合，变更走既有 FG 重建/真实退休；有效方向与 offset 进入 owner 身份。DLSSG、FSRFG、XeFG 共用解析策略，分量符号只在转换纹理中应用一次，NGX scale/jitter 保持原约定。

RF shader 修正高度偏移映射、X/Y padding 边界和错误使用全局 feature 尺寸的问题，按各 guide 实际区域派发；仅运动分量取反不使用行偏移。运行时源码与预编译源、字节码有一致性检查。内部 FSR 桥保持关闭，用户配置保留。当前构建部署身份和回归以 `build/current-{build,tests,artifact,deployment}.json` 为准，离线通过不代表 UI 外漂已修复。

候选 **FAAEAE93** 已于北京时间 **2026-11-10 10:39:07** 原位部署，SHA256 `FAAEAE935028E085FB818561C7DF755B8BEC10CEC2B5E00F26A4377A6AC4DF65`，33,042,944 B，DLL mtime UTC `2026-11-10T02:34:22.9831218Z`。Release exit 0，382 warning records／0 errors；四条硬链接、原文件对象及七份当前配置保留，部署前无活跃游戏。837项方向/真实配置类型检查、5组接线、公共CPU/WARP/静态回归及100项ZZZ接线通过；RF真实WARP跑过源码与预编译两套pipeline，240组派发／117,408个精确像素覆盖R32深度、RG16/RG32运动、行/符号/偏移组合和非法区域。独立静态复核未见新增阻断；候选未启动游戏、未验收显示或性能。回退0EE5BFD3是UI外漂报告对应候选，不是已知良好版。

最新已结束局为 **0EE5BFD3 关联磁盘候选 / PID112124**，北京时间10:10:55–10:17:17，launcher exit 0，精确 loaded-memory SHA 未核。完整日志 `build/genshin-112124-OptiScaler.log`，33,811,754 B／340,107行，SHA256 `893A6FE8D0406C204D3F9EC03DA3170DE0E2FC9960DDFA574DF2C7E342E44D18`，解析 `build/f6-112124-summary.json`。FG Dispatch及depth/MV标签各7,989次成功，但SDK enabled/disabled各9次、11次flushAll、3次async-present mutex失败；没有HUDless/UI成功标签，不能单凭Y解释外漂。NR direct NGX采样22/23 ran=true且全部不被光流替换；模型cadence采样15/15成功，但场景元数据/相位认证仍拒绝历史反馈，**历史效果尚未恢复**。digest的179次“错误”含普通Info字段；完整日志实际仅1条Exposure缺失错误，随后自动曝光回退。新候选的游戏画面与性能尚待用户验收。

## 2026-11-10：直接 NGX 运动、历史细节与 NR 热重建

510AA6F5 的新局 PID144892（09:32:44–09:42:21，北京时间）实测失败：生成/原始画面交替闪、历史细节几乎不生效、调模型分辨率需失焦恢复。固定完整日志 `build/genshin-144892-OptiScaler.log`，49,784,657 B，SHA256 `58F6F41CC4D0EC6ED94AC486A0664C831E748FDB68EEB4B981621F2B4E4FB46F`；launcher exit 0，精确 loaded-memory SHA 未核。本局没有上次 SDK fence 超时隔离证据；三段 NR 重建等待 10.821 / 5.761 / 2.990 秒，期间 SR 和 FG Dispatch 继续，不能称 SR 同时掉线。

确认 generic DX11 FinishedPicture 会无条件计算光流并替换 NR 模型运动；日志 `motionIsPictureFlow=true`、`nativePhaseVerified=false`、`feedbackActive=false`。F6 direct-DLSS 现复用已有 NGX scene contract：真实 Evaluate 捕获当前深度/MV、最新 capture/源 epoch/producer handoff 认证及实际消费者行翻转/向量解码后相位检查，模型不再被最终画面光流替换。独立源 epoch 修复整局全局 Present epoch=0 的身份缺口，不赋予生成帧源权。场景运动不声称覆盖 HUD/postprocess；缺元数据/深度、jittered 或高分辨率 MV 仍拒绝 temporal。

历史严格对应上一实际观察输入，包含 GPU-busy 早退，防止 picture1800 对 source1798；native 源步长不再学习物理 FG 步长。模型重建在公共入口非阻塞推进已完成且闲置的私有 recording，保留全部实际 queue/fence 完成、真实 Reset、借用 list 隔离及 quarantine 规则；不通过放宽退休门、旧帧或扩大池子恢复。新增回归已接公共 runner。源码修复、构建部署和新局像素/性能验收分开；交替闪烁的唯一像素成因仍未确定。最终当前 DLL/部署身份以 `build/current-{build,tests,artifact,deployment}.json` 为准。

Release x64 构建 exit 0，84 warning records / 0 errors；DLL 33,035,264 B，SHA256 `0EE5BFD3FD9DB008452C578D30B788954B8D54E993FD10F168D84E0A4B1D13F5`。北京时间 **2026-11-10 10:09:19** 原位部署完成，产物与四条链接哈希相同，文件对象及七份当前配置保留；部署前无活跃游戏。十组针对性套件和公共 CPU/WARP/静态检查通过，新增历史连续性104项、native host104项、直接 NGX 接线17项、私有 recording退休30场景191项及三个unsafe负控；另100项ZZZ接线通过，独立静态复核无确定阻断。汇总 `build/f6-scene-history-fix.json`；本版尚未启动新局，FG闪烁/历史效果/调参恢复/性能待实测，回退510AA6F5为本局失败版。

## 2026-11-10：直接输入的 FG 输出停顿

用户报告再次卡顿、闪烁，最后 DLSS 或 FG 掉线。对应 `20261110-084556_YuanShen_115336`：08:45:55.249 启动，日志到 08:46:53.886132，启动器退出码 0。磁盘候选 `C8200104...`；精确 loaded-memory SHA 未核。固定完整日志 `build/genshin-115336-OptiScaler.log`，4,349,238 B，SHA256 `BA71A3F179025B8A0B2964370DA22BEC50529A32339849898BE6F9BE53393C42`；解析 `build/f6-fg-output-runtime.json`。

预热修复已使当前输入成功发布：SDK 于 08:46:32.898314 进入 enabled，最后 FG Dispatch 为 frame154。随后同一源线程连续四次 SDK GPU fence 500 ms 超时，08:46:40.678094 暂停 FG；实际 client queue 的 host drain 又等待 5 s 超时，保留引用并隔离 FG 至进程退出。此后 SR Evaluate、RCAS 和 NR 继续，证据支持 FG 掉线，不能说 DLSS 停止，也未确定闪烁的唯一像素成因。

本轮修复公共源帧/物理输出职责：SDK 创建作用域和真实 DIRECT 队列认证物理输出身份，独立于游戏名和可选 filter 准入。Reflex 限帧在源 FGPresent 的 Dispatch 后、SDK Present 前更新；输出 worker 不再请求源 continuity，也不推进源 epoch、修改源设备/队列/描述或执行源 tick、GPU timer、NR/capture。普通源 wrapper、物理 overlay/raw Present 和真实退休边界保留。并发 CPU 回归用实际生产块复现旧原神 worker 等源锁，修复后通过；这不是实际 SDK GPU-fence 根因或游戏显示通过证明。

共享纹理先等待该 slot 上一次真实 DX12 reader 退休，再执行 DX11 写入，allocator Reset 消费同一次、绑定 owner/ring/fence/allocator/slot/value 的局部准入，不做第二次等待。实际 Execute 后 Signal 失败时，全部路径保留 wrapper、实际 queue/device 和取得的目标纹理，阻止后续复用；native 可选路径仍以非阻塞 Busy 返回。原生 DX12 若只有 FG proxy，则由去重的 `SourcePresent::Scope` 补齐同一套源时钟、tick 和 epoch；存在外层 source wrapper 时不重复，物理 worker 不获得源职责。

Release x64 构建成功，MSBuild exit 0，118 warning 记录 / 0 errors；DLL 33,034,240 B，mtime UTC `2026-11-10T01:18:00.9822192Z`，SHA256 `510AA6F5F8CC0823A9C1B3CFBC2364CE9124BF0A22E15772E14229135FD6DE88`。北京时间 **2026-11-10 09:18:31** 已原位部署，产物和四条部署链接哈希一致，原文件对象、硬链接拓扑及七份当前配置保留；部署前无活跃游戏。八组针对性套件及公共 CPU/WARP/接线检查通过，含 9 项 source scope、19 项 FG 并发、30 项 interop 接线 / 74 项实际生产块 CPU 检查，另有 100 项 ZZZ 静态接线。旧 worker 的并发负控按预期失败；交叉静态复核无确定阻断。回执 `build/current-{build,tests,artifact,deployment}.json`、`build/f6-fg-output-fix.json`。新候选未启动游戏，画面/FG 显示/性能仍待用户新局验收，loaded-memory SHA 未核。回退为本局失败的 C8200104，不能称为已知良好版；内部 FSR 桥保持禁用。

## 2026-11-10：直接 DLSS 输入的 FG 预热循环

用户报告 FG 似乎未启用。最新已结束会话 `20261110-083440_YuanShen_79484`，北京时间 08:34:38 启动、08:36:01 日志结束，启动器退出码 0；磁盘候选 `71AB73EB...`，精确 loaded-memory SHA 未核。完整日志 `build/genshin-79484-OptiScaler.log` 为 6,320,505 B / 66,882 行，SHA256 `B8BDB12E4F289FB3DD40A672C58E24A9F08BD872B195E7A0BCCF978BBC625163`；配置 FG Enabled=true、Upscaler→DLSSG、请求 5 个生成帧。统计见 `build/f6-fg-warmup-runtime.json`。

确认新阻断：激活在 frame 11 设置 target 21，frame 12 仍处预热，`SetResource` 正常拒绝发布却被 `SetUpscalerGuidePair` 当作部分发布失败，触发 Off/drain/free，随后每 11 个输入帧重新激活。日志记录激活、物理 teardown 与 SDK FreeResources 各 178 次；无 depth/MV tag 或 FG Dispatch 成功证据。这些是日志计数，不代表生成帧数。上次透明故障的 presenter resize 本局返回 0，`proxyBuffer is NULL` 与 `BAD00005` 均未出现；无独立像素验收。

修复将原先仅 native ZZZ 使用的预热推迟规则统一到全部上采样器 guide pair：inactive/paused 时在转换、原生凭据保留与 SDK tag 之前返回，继续靠真实上采样器帧推进到 target；按本局时序 frame 22 才可发布。真实部分发布失败的 Off/queue/fence/SDK 退休链保持。新增静态接线回归已在旧代码下失败，修复后 13 项输入接线、七组针对性 CPU 套件及公共 CPU/WARP/接线检查通过，另有 100 项 ZZZ 静态接线通过。独立静态复核无阻断，未发现已核对直接链的额外确定阻断；这些检查不证明 SDK 或游戏生成成功。

Release x64 构建成功，MSBuild exit 0，78 warning 记录 / 0 errors；DLL 33,033,216 B，mtime UTC `2026-11-10T00:42:57.5569151Z`，SHA256 `C8200104B8A8D226483555AC651F167659B68C9374876F9BE7B680432D04B0B9`。北京时间 **2026-11-10 08:44:23** 已原位部署，产物及四个部署链接哈希一致，原文件对象、硬链接拓扑及七份当前配置保留；部署前无活跃游戏。回退为本次 FG 失败候选 `71AB73EB...`，路径及完整核对见 `build/current-deployment.json`。本次修复回执 `build/f6-fg-warmup-fix.json`，构建/测试日志 `build/f6-fg-warmup-{build,tests}.log`。F6 内部 FSR 桥保持禁用；新候选未启动游戏，FG 生成/显示/性能仍需用户新局验收。

## 2026-11-10：外部 NGX 输入透明故障

用户本局使用外部带 NGX 的 FSR 桥 + F6，报告整个游戏窗口透明、仅保留游戏鼠标样式且有声音；未做独立像素捕获。对应会话 `20261110-080852_YuanShen_124564`，北京时间 08:08:50 启动、约 08:10:19 退出，启动器记录退出码 0；本次读取未见活跃游戏。本局日志确认 F6 的 direct-DLSS 策略生效、真实 DX11 NGX Evaluate 已进入，不能把外部桥的输入与已关闭的 F6 内部 FSR 捕获混为一谈。部署候选为 `AB1A1C33...`；精确 loaded-memory SHA 未核。

完整固定日志 `build/genshin-124564-OptiScaler.log` 为 29,706,442 B / 285,994 行，SHA256 `8A5A44384C7691396185FA05E92AA9AFBD3AD7265BB8853863AF2D052FD7B613`。启动器 session 中的产品日志副本仅 16,777,148 B，诊断采用完整已关闭日志；身份解析 `build/genshin-124564-evidence.json`，本轮摘要 `build/f6-transparency-runtime.json`。

确认两处断点：FG 初次 resize 把原始 sRGB format 29 传给创建时使用 UNORM 28 的 DX12 presenter，返回 `80070057`，随后 `proxyBuffer is NULL` 共 47,394 条；SR 则报 Color 非 Tex2D / `BAD00005` 共 11,380 次。计数是日志行数，不能当生成帧数。本局尚未建立 NR 模型或 FG 生成成功证据，透明画面的唯一成因未获像素验证。

修复：DX12 presenter resize 与创建统一 sRGB/UNORM、单缓冲计数规则，原 DX11 描述保持；失败后不再继续使用可能已丢失 proxy 的 presenter，必须真实成功 resize 才能恢复。普通 DX11-on12 为六项资源使用同步 DX12 参数视图，保留调用方 DX11 原表和未知标量/元数据，SR 失败不得发布 FG guides。本机真实 `_nvngx.dll` 的 D3D11 参数表 + WARP 纹理已复现旧 typed-DX12 Set 被忽略，且新视图 typed/void 读取均为正确 DX12 纹理，原 DX11 资源保持；见 `build/f6-native-ngx-parameter-probe.log`。该探针不执行 SDK Evaluate 或游戏。

修复版 Release x64 构建成功，MSBuild exit 0，96 warning 记录 / 0 errors；DLL 33,033,216 B，SHA256 `71AB73EB02D76A30C6065839568AB4F75E9934DD6B545FFAF5D853DE2DD4FDF5`，mtime UTC `2026-11-10T00:27:02.5113389Z`。七组针对性套件及公共 CPU/WARP/wiring 检查通过；新增 207 项交换链策略检查、23 项接线检查及真实 NGX 参数探针通过，独立静态复核无阻断。

北京时间 2026-11-10 08:30:11 已通过既有 `Deploy-F6.ps1` 原位部署，四条 DLL 硬链接哈希均匹配 `71AB73EB...`；原文件对象、硬链接拓扑和七份部署前当前配置保留。部署前无活跃游戏，未启动游戏；新候选仍需用户新局验证。构建/部署回执 `build/current-{build,tests,artifact,deployment}.json`，本次修复摘要 `build/f6-transparency-fix.json`；回退备份在部署回执中，其哈希为本局失败候选 `AB1A1C33...`，不代表已知良好版本。若 presenter 真正 resize 仍失败，当前仅安全拒绝，不会把隐藏 DX11 源链当作可见回退；完整可见回退未实现。

## 2026-11-10：DLSS 直接输入路径

本次按用户要求禁用 F6 的全部 FSR 输入桥（包括 FSR2/FSR3/FSR FG API 拦截、模块/EXE 扫描、原神 shader/Draw/深度采集、native provenance 探测、专用 Present producer/child owner 和桥 UI/提示/计时）。统一策略在 `OptiScaler/runtime/F6InputPolicy.h`；旧 INI、捕获和调试请求不能重新启用。FSR 输出后端和正常 DX11→DX12 互操作保留。

NR/FG 从实际 DLSS 上采样器 Evaluate 的当前帧输入和输出接入，保留独立开关及原有资源身份、queue/fence 完成约束。没有实际 DLSS 调用或有效输入时，不通过 FSR 桥或旧帧补造来源。旧配置保留。下述 `AB1A1C33...` 是初次部署记录，已在 PID124564 出现透明故障，由上方 `71AB73EB...` 修复候选取代。

直接输入边界：必须有真实 DLSS/NGX Evaluate 调用，仅加载 DLL 不会生成输入。DX11（含原神）的本次 DX12 NR 路径需选择 `DLSS_on12`，普通 DX11 `DLSS` 不等同于此路径；FG 使用当前上采样器资源缓存与 provider 自有 recording。原生 DX12 ZZZ 保留已有真实 source receipt/提交/完成链；原生 DX12 原神的外部 recording 当前不支持 FG，没有相应完成凭据时继续拒绝 `conversion-producer-completion-unavailable`。以上为源码接入边界，尚未证明 SDK 或游戏运行成功。

初次 Release x64 构建通过，MSBuild exit 0，96 warning 记录 / 0 errors（最终增量构建）；当时产物为 33,030,144 B，SHA256 `AB1A1C335FF85BA32C28A451140F9AC36A47091CED180CB4300106237638202A`，mtime `2026-11-09T23:56:34.507941Z`（北京时间 2026-11-10 07:56:34）。统一策略/旧配置保留、feature transition、帧参数、NR guides、FG options、fence/backpressure 的七组针对性套件及现有 wiring/CPU/WARP 检查通过。额外 ZZZ 实际链路 100 项静态检查通过；其测试中此前失效的 sampler/options 源码定位同步至现有适配器，未改变运行实现。独立代码静态复核通过；部署后的 PID124564 游戏实测失败，见上方透明故障记录。

部署完成：北京时间 2026-11-10 08:02:20（UTC `2026-11-10T00:02:20.7687222Z`），通过 `tools/runtime/Deploy-F6.ps1` 原位写入正式 `OptiScaler/official/F6/OptiScaler.dll`。产物与正式目录、三游戏 runtime 共四条硬链接 SHA256 均为上述 `AB1A1C33...`；原文件对象、硬链接拓扑及七份当前配置哈希保留。部署前未见活跃游戏，仅有零线程/零句柄旧进程残留；未启动游戏。上一部署 `25D1E42A...` 已备份至 `D:/CODE/DLSS5_NEXT-workers/f6-rewrite/build/rollback/OptiScaler-20261110T000220102-c902aefc2d5a451ca96768f9c8892fa0.dll`，完整身份与核对结果见 `build/current-deployment.json`。

下方为先前迁移与实测记录；其中 FSR 桥的当前流程/下一步描述已被本次输入路线调整覆盖。

更新：2026-11-08。唯一产品写树 `D:/CODE/DLSS5_NEXT-workers/f6-rewrite`，分支 `codex/f6-rewrite`，HEAD 基线 `db0e646`；历史未提交改动保留。原根、旧根、Encore／sm86／F5 参考树和许可证继续保留。

## 当前目标与实测

用户最新实测 **10503898／PID107608 失败**：**大世界**两层、50% NR 先约 7 FPS，用户“切了下画面”后约 27 FPS；FG×6 仍慢放，关闭 FG 后新增画面冻结。目标至少 35 FPS，未达到。完整接手入口、问题清单与日志读取命令见 [F6 转交](F6-HANDOFF.md)；构建、CPU／WARP 和部署不证明游戏性能通过。

最新 session `20261108-134312_YuanShen_107608`：UTC+08 启动 13:43:10.919，末条产品日志 13:45:34.693762；13:45:34.638013 请求关闭 FG，随后记录 Off+retain 与 native fallback，末行截断，冻结根因未知。launcher 于13:46:23收尾，退出码 **-805306369**，不能写正常退出。固定日志 `build/genshin-107608-OptiScaler.log`，6,749,117 B／42,099行，SHA256 `0A9AD2E956CB1FD06DEC8A2C663164917544E174EA9D16A69E7C6BC51FFEA242`；解析 `build/genshin-107608-evidence.json`，会话回执 `build/active-genshin-session.json`。两层／1280×800模型输入已记录，重型R448记录为零。FG段17次SDK状态变化、8次flushAll，原因未知；同局世界FEAC producer持续到关闭前。本次交接未改产品代码或重新构建部署。

本次用户确认只加载 OptiScaler。此前把 Bridge 或其它模块归入当前性能原因来自旧日志，已撤回；启动器 `mod Bridge` 配置条目也不证明模块实际加载。日志与部署磁盘身份已绑定，精确 loaded-memory SHA 仍未核。

上一候选8240EE70／PID116868：大世界两层50% NR约25 FPS，NR＋FG×6慢且卡，实测失败。北京时间 2026-11-08 13:00:50 启动、13:03:37 正常退出。原始日志 `build/genshin-116868-OptiScaler.log`，9,382,056 B / 51,832 行，SHA256 `07B4B80CC99627EA6D6007D3DB6208740F009743D31AF883BA00340A2702A8ED`；解析 `build/genshin-116868-evidence.json`。输出 2560×1600、模型 1280×800、两层；重型 Hook CPU 记录为零。下列细节仅属于该局：

- 仅 NR 末段 Present 入口间隔约 39～42 ms，Present 本体约 0.1～0.24 ms；Native Run CPU 通常约 1.5～1.9 ms，allocator wait 很小。
- 前处理 flow 通常约 4～5 ms；模型已完成区间约 15～17 ms，详细样本偏向启动期。它们不是同一输入的完整排他分解，不能相加硬算根因。
- 54 条 child 调用记录多为整数 0 ms，后段夹有 15～32 ms，另有初始化／切场景长调用。区间包含整个 hook 和 ReflexSleep，不能写成持续 31 ms 的原始 SDK 成本。
- SDK 有 47 次 interpolation state changed：24 次 enabled、22 次保持 `eOn/5 generated` 的 disabled、1 次 `eOff` disabled。22 次 `eOn` 停用紧邻 flushAll，触发原因 **UNKNOWN**；没有证据支持盲改 Reflex、选项或源资格检查。
- 世界 FSR 变种 `FEAC669E48FD931E` 持续生产。切场景后可选 NR depth 曾因 `producer-depth-without-picture-uv-mapping` 缺失，flow 仍有；不得借旧深度或删除映射约束。FG capability／配置缓存实际命中；SDK ordinal 与查询 presented batch 均不等于显示 FPS。

## 本批结构改动

- 绘制观测：ticket 返回只更新轻量字段，前一完整快照的绑定仅清理一次；有效捕获直接填入唯一 Raster，复用同次已验证 DSV／深度状态及保留的 Context1。保留参数、context/thread/epoch、间接原调用返回、数值证明和预算／COM 生命周期。
- FG 私有 copy 事务：同 owner、同 queue/fence/value 且首次成功的两个 Wait 合并成一次；不同身份仍实际提交。重入门之后才获取调用局部引用，覆盖整个 Wait，再发布回执引用。回执只证明队列顺序；Signal／Flush／Execute、生命周期 drain 和实际退休不变。
- 默认 engine capture：只处理本次捕获 Draw 的返回确认；下一次 Begin 清除未匹配票据，普通场景 Draw 不再复制 receipt／查询 Render token，也不能补认旧调用。
- 诊断：跨 cadence owner/config 重置的模型调用差值明确 unknown，避免无符号下溢；GPU 完成证据每 64 个 sequence 补一条低频详细样本，没有新增等待。

既有结构继续保留：统一 FSR 创建字节认证与新旧 profile 查询；新变种真实 native RTV0／RTV1 alpha 后 RGB 合成；实际 query／Flight／paired lease；重型 Hook CPU 每会话默认关闭、轻量 FSR HUD 独立；同一输入前处理 GPU 与 Native CPU 的分域计时。未减少 NR 层数、分辨率或强度来冒充优化。

## 验证、构建与部署

完整公共 runner exit 0：返回事务 43 项、捕获 27 项、绑定 133 项、输入观察 1081 项、资源观察 73 项、私有 Wait 46 项及既有公共回归。100,000 次 ticket 捕获／100,001 次返回均无 Raster 构造／移动／复制；有效捕获为一次构造、零移动／复制。WARP／CPU 使用的数值和 owner 夹具有明确边界，不执行真实游戏或 SDK 画面。

独立静态复核已确认最终 Draw／Lead 改动未见新增具体语义缺陷。初版 Wait 去重的异对象重入引用缺口已修复并复核，回归覆盖引用保持和零嵌套提交。新增捕获及 Wait 测试已接入公共 runner；首次 runner 的新测试 CMD 引号错误已修正，最终完整重跑通过。

Release x64：MSBuild exit 0，1 分 43 秒，144 warnings／0 errors；DLL 35,017,216 B，mtime UTC `11/08/2026 05:35:44`，完整 SHA256 `10503898606F5CBD6C0951A9BE4AEEE014815633715348139187EF0EF4538D5B`。原位部署完成 UTC `11/08/2026 05:36:54`：产物与四个部署链接哈希相同，四条 hardlinks／文件对象及七份配置均保留，未改 F6／F8 选择。

回退为已测失败的上一候选 **8240EE70**：`D:/CODE/DLSS5_NEXT-workers/f6-rewrite/build/rollback/OptiScaler-20261108T053654413-fc3599deff9040709cdf7489449189e9.dll`。该位置与 SHA 已核；部署前仅见 0 线程／0 句柄旧 PID51144、60652，无活跃游戏。未启动游戏或操控提权窗口。

单一回执：`build/current-{build,tests,artifact,deployment,performance-review}.json`。构建和离线回归通过仍保留，10503898的游戏验收更新为失败；最新已结束会话为PID107608，退出方式未知。旧候选、恢复点与崩铁／ZZZ 边界见 [F6 状态](F6-STATUS.md)。

## 工作方式与边界

构建 `pwsh -NoProfile -File ./build.ps1`；回归 `tools/tests/Run-CommonCoreTests.ps1`；部署既有 `tools/runtime/Deploy-F6.ps1`。仅 Lead 统一构建／部署，测试保持单一编译 owner；不重新生成既有 Look shader，不新增逐轮证据目录。

源帧、SDK 与 display owner 分开；资源只有在实际所属 queue/query/fence 完成后退休，不以 CPU 返回、窗口 ACK、扩大池子或借旧帧替代。保留用户配置和独立 SR／NR／FG 语义。

用户自行启动、进入世界、切换场景与正常退出；不点击提权游戏、不强退。子智能体只在当前主会话创建，超过 30 分钟不调用，默认新建；规则已写入工作树及主根 `AGENTS.md`。
