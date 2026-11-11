# F6 性能审查与主会话复核

2026-11-12 续修：按用户要求停止 DS，问题检索和独立复核改用 **gpt-6.1-sol / high**，主会话核实原始日志并修改代码。当前改动与候选身份以 `build/current-{build,tests,artifact,deployment}.json` 为准；下方 11 月 11 日审查保留为旧局证据。

## 11 月 12 日确认与修复

- **已定位长等待边界，未确认 160 ms 唯一成因**：PID164004 完整日志第714831／714873／714913行的真实 copy reader 等待为83.962／98.681／22.469 ms，目标23032／23033／23034，完成值均差1。包含它的复制父区间不能再相加；后续隐藏 DX11 Present 的101.810／92.761／173.323 ms是另一边界。首个等待在失焦日志之前已开始，不能全归给后续 FG 禁用。
- **普通 interop 的三个 allocator 不等于三个颜色 slot**：`PrivateColorSlotCount(false, ...)`按游戏buffer数分配共享纹理，本局bufferCount=1、fakeIndex=0，34条已输出 copy-wait span均为slot0。只轮转 allocator 或删 wait 会失去同一共享纹理 reader 的退休保护。本轮保留此同步与用户倍率，尚未证明 fence 迟到的GPU原因；这不是通过扩大池子就验收的性能修复。
- **移除计时器重复同步**：总计时、模型及各层 timer 复用 `g_nrGpuWork` 已在真实 Execute 后记录的同 queue/fence/value，计时器不再创建 fence 或额外 Signal。query heap／readback由同一真实 recording持有；读回须GPU完成，查询槽复用还须真实Reset，重放或Signal失败仍沿用既有隔离。已完成但未Reset的槽只等事件，不反复轮询fence。修正End接受外来命令列表及guide读取失败遗漏收尾。
- **低频分段**：仅总计时前3次及每64次实际dispatch附加6个timestamp，记录encode前、encode、模型准备、模型、历史、resolve、收尾7个不重叠区间，复用现有readback与提交凭据。忙时跳过，阶段不齐不resolve未写query；它们是同queue时间跨度，不是独占GPU活跃时间，无新增CPU等待。
- **撤回一项错误归因**：此前将输出描述符误读成输入描述符，新夹具人为构造了36项“旧色”失败。生产初始target本来就是output，并已保证分离输出UAV；已撤回冗余的`separateInput ||`改动。修正夹具为提取实际target及desc拥有者后，392项生产HLSL/WARP检查通过。该错误夹具不是游戏残影根因证据，也不再宣称省去了一次生产全屏复制。

Look控件的实际修复仍保留：用户修改Look或点击“查看最终效果”时退出原始模型预览；只浏览不改设置。细节持续保留、Look游戏效果及延迟仍须在新候选实机验证，旧局日志不能证明新源码已被加载。

新timer的WARP回归包含部分阶段、错list、队列延迟、槽满、完成后缺Reset、提前Reset、旧录制重放、Signal失败及析构时在途存储；沿实际生产registry和真实WARP队列执行，不执行NGX或游戏，也不证明提速。最终检查数及构建／部署结果见当前回执。

## 11 月 11 日审查

2026-11-11（UTC+08）。工作树 `D:/CODE/DLSS5_NEXT-workers/f6-rewrite`，`codex/f6-rewrite` / `db0e646` 加既有未提交改动。按用户要求由当前会话启动 **relaycat DS / deepseek-v4.1-flash** 只读审查；主会话核对调用链、门控与真实日志，要求修订后结束实例。本轮没有修改性能调度、用户配置或正式部署。

结论：**NR 模型本身是主要实测 GPU 成本，模型外工作也值得优化；160 ms 峰值还包含明显的 GPU 开始前及报告尾段时间，具体阻塞 API 尚未定位。** 现有数据不足以把延迟归给细节保留、FPS 叠层或一个未执行的快照分支。

## 已测到的成本与边界

| 项目 | 证据 | 能确认什么 |
|---|---|---|
| 用户所述约 160 ms | PID153992，22:46:55.912340，报告帧33333：166.103 ms | 同报告端点拆分为 GPU 开始前62.987、GPU跨度58.481、GPU结束到报告末端44.635 ms；不是纯 NR 或完整点击到显示测量 |
| 峰值附近 FG 状态 | 22:46:55.313959 重新 enabled、生成数5；55.364612 SDK 最大帧延迟1→6 | 与峰值有时间关联，尚未证明具体等待点；6不是六张源帧排队的证明。flush 在57.144217，发生在峰值之后 |
| 上一局 GPU 配对 | PID153992 尾4分21秒，85对同dispatch样本 | NR总区间中位42.893、模型区间35.601、逐对差值中位6.413 ms；三项中位数不能直接相加 |
| 最新局 GPU 配对 | PID164004，317对相同sampleDispatch、sampleEpoch、queue和list的已完成样本 | NR总区间中位39.630、模型区间31.759、逐对模型外差值中位5.288 ms，差值P95为11.391 ms；是稀疏样本，配置期间有变化，不能冒充整局平均或历史单项耗时 |
| FPS 叠层 CPU | PID164004，23:00:00–02，菜单隐藏、仅FPS需求，108/108/111次回调 | UI构建均值0.250–0.262、录制0.042–0.044、Execute调用0.031–0.032 ms；合约0.32–0.34 ms/物理输出回调。不是GPU时长或扫描帧率，不能单独解释160 ms |
| 资源积累 | 最新局34条cadence样本：pendingRecordings=0、retiredEntries=0、trackedRecordings=1或2、registryHalted=false | 未见这些计数持续堆积；不是全局无泄漏证明。细节状态占用上界约238.6或306.7 MB，需要区分启用状态与泄漏 |
| 调试日志 | 最新局74,581,959 B /717,054行；本局配置LogLevel=1 | 确实有逐帧DEBUG格式化/入队。尚无日志阻塞时长或对照，不把日志量直接换算为延迟 |

最新局有1,848次Reflex报告、1,835个不同的PID/帧号/报告区间。重复报告不能算新帧；尾段帧18665的297.169 ms两次上报是同一结果，GPU跨度140.030 ms、active55.176 ms也不能被当作140 ms纯计算。全局最大报告包含长的队列入口前区间，缺完整输入端点，不能直接宣称为输入到显示延迟。

## 源码复核与 DS 结论取舍

| 链路 | 主会话复核结果 | 处理 |
|---|---|---|
| 物理输出snapshot/restore | `FinalOutputEndpoint.cpp:571` 在未选中HoYoShade FinalOutput时直接呈现；日志第216、24471行均为滤镜enabled=false，后者stage=before-sr。`physical_output=true`只是叠层调用角色 | 撤回本局每物理帧全屏snapshot的归因；不为此路径新增常驻计数，也不删除正确性快照。DS的“可能P1”不采纳为本局优先项 |
| NR encode/resolve/copy-back | `DlssNr_Dx12.cpp:4838` encode与`:6612` resolve确有工作；`:6623` copy-back受`composed && !targetSupportsUav`限制 | DS第二版仍写“每帧必copy-back”，主会话再次否定；必须按实际资源权限测量，不能凭调用存在估算带宽 |
| 光流/trust | `DlssNr_Late.inl:1971–1974`受bridgeScene、freshColorFallback等条件控制；34条实际cadence均motionIsPictureFlow=false、每次模型调用数1 | 撤回细节持续保留必跑额外光流的说法；不自动更换用户运动来源或坐标设置 |
| SceneCut/Calibration | `DlssNr_SceneCut.inl:342`按mode/帧契约早退；Calibration入口`:225`仅主动run时执行 | 调用入口存在不代表每帧dispatch/读回；native判定还须看`Run`本身的MotionMatchesPicture条件，不能只看BeginFrame返回值 |
| SDK GetState | `DLSSG_Dx12.cpp:766`插件确实主动查询；`:3132`非原神每源查询、`:4784`原神同呈现线程查询真实输入完成fence | DS第一版“只有游戏调用”不成立。缓存只服务Read，不能消除Query；必要完成查询保留。非原神查询还控制当次最大生成数/状态，未证明可安全降频 |
| pacing/Reflex | `FG_Hooks.cpp:2528–2529`先释放本次FG锁及execution，再由独立token租约执行`:2576` Sleep | 先前锁内等待已有修复，不能当本轮新发现；尚无此次峰值的实际阻塞栈 |
| NR录制锁与生命周期 | Dispatch持g_nrMutex；Submitted在watched gate后取同锁。resize退休共享一个5秒总预算 | 存在争用风险但未量到等待；菜单slot_wait=0不证明g_nrMutex无争用，5秒不是每slot预算。不得缩减真实fence/Reset条件 |
| 关闭后的后台工作 | ExposureScan的NoteResource/NoteUav受NR/Wanted门控，唯一Tick调用在实际NR Dispatch中；Calibration受主动run门控 | 未证实这些扫描在关闭NR后常驻。不能把单一入口早退扩大为所有诊断均零成本 |
| 资源与日志缓存 | NR scratch按shape复用；FPS统计文本在source/SDK revision或时效变化时更新（menu_common.cpp:2388）；异步日志可能按所选模式阻塞或覆盖旧项 | DS的“增加FPS文本缓存”已有实现。可继续查整条ImGui构建成本与实际日志模式；不能只反复加文本缓存，不能假设异步日志一定不阻塞 |

DS完整修订稿在 `build/relaycat-performance-review.md`，作为检索材料；与本页冲突时，以本页已经复核的条件和边界为准。没有直接移植F5的同步/所有权实现。

## 下一步按证据排序

1. **先拆峰值等待**：复用现有source/token关联与CPU阶段计时，匹配实际Present、ReflexSleep、必要GetState、DX11/12队列交接；按真实source/配置代次区分FG启停、失焦、稳态。没有对应区间不能把SDK flush或frame-latency setter当因果。
2. **拆模型外GPU时间**：在已有异步GPU timer及其真实提交/fence生命周期内，按同dispatch/queue测encode、guide转换、实际执行的历史/稳定、resolve和条件copy-back；低频汇总、忙时跳过，不增加CPU等待或像素读回。只对已执行阶段计时，包含区间不相加。
3. **确认CPU浪费再缓存**：计g_nrMutex等待、日志队列/格式化耗时；FPS已缓存文本，若整条UI构建占比仍明显，再缓存对应布局/绘制状态，同时保留每次renderer recording的真实读取退休。
4. **保持固定参数验收**：相同场景/用户模型尺寸、层数、历史和FG倍率，分别报告源Hz、SDK计数、实际显示事件与延迟；缺显示采集就标未知。不靠降低画质、删同步、扩大池或旧帧回填完成优化。

当前审查没有宣称160 ms已解决。保留用户确认有效的7C281A54共享source queue、Evaluate提前封存guides与最终颜色汇合；无需恢复转头校准向导或新增复杂面板。

## Look 修复与交付状态

PID164004的24次`NR look resolve`均为debugView=2，参数已上传。生产shader的`:1825–1828`原始模型预览提前返回，`:2126`才应用最终Look；这是当前看不到滑条效果的确定原因。

`NextgenLookControls.h`现在在用户实际改Look或点击“查看最终效果”时返回最终画面；仅浏览不改参数，原始预览shader语义保持。生产控件80检查、Look HLSL/WARP246检查和FG交接/颜色事务/Reflex/pacing/options/shared回归通过，旧错误颜色域负控仍预期失败。Look WARP入口显式恢复成功退出码，未放宽像素断言。

11月11日旧候选为 **63DD753C27FA17F71E9A197DB0CA5D5D8840FBC1A25C1411CF1B0BAD6667B38B**，33,951,744 B，Release x64 exit0，14导出齐全，未部署。现由11月12日的 **DD53514B** 候选取代；正式安装仍为81DBE9A5。当前回执绑定新候选，旧回执保留在`build/latency-followup-previous-*.json`；两候选均不证明Look、历史或延迟实机通过。

测量回执：`build/reflex-latency-latest.json`（PID153992），`build/performance-review-{latest,paired}.json`（PID164004）；当前构建／测试身份见`build/current-{build,tests,artifact,deployment}.json`，新源码快照为`build/latency-followup-source.json`，旧63DD快照为`build/look-reflex-source.json`。
