# F6 性能审查与主会话复核

2026-11-12 续修：按用户要求停止 DS，问题检索和独立复核改用 **gpt-6.1-sol / high**，主会话核实原始日志并修改代码。当前改动与候选身份以 `build/current-{build,tests,artifact,deployment}.json` 为准；下方 11 月 11 日审查保留为旧局证据。


## 2026-11-12：排队候选修复（92D1BACE／8C9418B6，未部署）

从PID97772的GPU前排队继续沿真实链路调查，修复两处frame-latency所有权缺口：应用未请求WAITABLE时，setter原先仍能覆盖SDK pacer内部队列限深；互操作getter选择native等待对象时，setter／限深查询却可能指向provider。现在三个API共用同一resolver和转发器，确认无权时返回INVALID_CALL且不触底层，显式请求时命中同一对象，SDK／未知来源及非SDK路径保留既有路由和真实HRESULT。分类仍只将主EXE认作Application，引擎／覆盖层DLL未知来源继续透传，不能声称覆盖所有调用者。

自有Reflex选项改由公共策略构造，保持LowLatency／Boost选择及其余默认值，`useMarkersToOptimize=false`。本地 `external/streamline/sl_reflex.h` 要求特殊标记调优仅在经Reflex团队指导的场景启用；仅观察到标记不足以建立该条件，也不保证和当前SDK设备／源帧同域。保留锁外原token Sleep、用户限帧、共享source queue及真实资源退休。`eBlockNoClientQueues`只支持Vulkan，D3D12保持原阻塞合同；GetState消费区间及完成门没有降频或删除。

验证：抽取生产wrapper方法的1380项CPU接口检查、1065项源调度检查通过；恢复9ab9686基线setter后，SDK所有权拒绝与显式请求的receiver一致性两个负控均失败。负控固定历史版本，避免未来提交改变HEAD后失效。六套定向回归通过：options1132项、pacing21场景／323项、Reflex12例、interop207项，以及真实WARP的FG交接15场景／564项、桥退休15场景／494项；options静态连接95项。独立gpt-6.1-sol/high复核无确定代码blocker，回执为 `build/queue-latency-{tests,review}.json`。

两种Release实际重链、53个既有NGX导出／版本资源与826项源码指纹均核验，候选私有92D1BACE／普通8C9418B6，均未部署；安装仍为8FCB8632。源码缺陷已修正，但旧日志没有证明这些setter调用命中，也未证明特殊标记调优导致末尾排队。下一步须保持原参数、6×及焦点状态做候选前后对照，分开记录GPU前队列、GPU跨度、NR分段和源率；未获得运行证据前不宣称降低了毫秒数。

## 2026-11-12：末尾技能时段调查（PID 97772）

06:20:50–06:31:53 原神完整日志封存于 `build/f6-skill-latency-97772.log`，58,000,728 B／532,561 行，SHA256 `8CBD5FD878AEC50C965F25CD63BCFA0E1D1C3A0A3B25A15A0D11CF760D4697DD`。该局晚于 8FCB8632 部署，日志版本 `9ab9686`，未读取加载内存哈希。第222行记录私有模块初始化调用返回；第27638／28260行记录 SDK 支持5张生成帧、接受总6×配置且选项提交成功；实际生成像素和扫描输出仍未知。后续 `submitted=false` 是该条选项未重新提交，不能据此认定 FG 未执行。

896条合法 Reflex 日志去掉26条重复端点报告，得到870份；195份 NR 分段均满足同 recording、同队列和真实 fence 完成条件。日志没有技能事件标记，以下以最后菜单关闭且保持焦点的区间近似定位；不是同参数受控 A/B，06:31:26 还发生 SR 后端重建和 FG 资源重建。表中结束时间为排除端点；末尾行窗为521896–531355，最后一行时间06:31:50.639713，下一行531356的06:31:50.641639是失焦边界。

| 日志窗口（UTC+08） | 唯一报告 | 报告中位／P95 ms | GPU前队列中位／P95 ms | GPU跨度中位 ms | NR总段／模型／历史各自中位 ms |
|---|---:|---:|---:|---:|---:|
| 06:30:56.529–06:31:14.022，菜单关闭／有焦点 | 32 | 61.807／81.342 | 16.149／36.670 | 44.840 | 32.170／25.517／3.196（6份） |
| 06:31:38.853–06:31:50.641，末尾菜单关闭／有焦点 | 21 | 76.115／105.031 | 26.371／50.270 | 46.490 | 30.193／24.273／3.154（4份） |
| 上行再排除39秒附近SDK清理，06:31:40–06:31:50.641 | 19 | 71.779／105.031 | 23.054／55.557 | 46.207 | 同上4份 |

末尾可见增量主要落在 **GPU开始前的队列区间**，NR稀疏样本没有显示对应的模型／历史耗时增长。NR工作尺寸2144×1344、显示2560×1600；末尾模型占NR总段的逐样本比例中位约83%，是其主要常态成本。不同指标中位数不相加；NR分段是同队列时间跨度，不是独占GPU活跃时间，也不能按相邻日志配成某个Reflex报告的子段。Reflex约500 ms抽样、NR为first3_then64且完成后回读；19份窗口的P95就是该窗口最大值，不能当作全体游戏帧的P95。SDK原始时间未校准到日志墙钟，转换日期也不用于跨时钟归因。

第530299行、06:31:49.270625打印的报告帧12277为 **105.031 = 55.557 GPU前队列 + 49.474 GPU跨度 ms**，前队列入口／GPU尾段均为0；GPU活动字段为47.853 ms，仍不把报告总长称为NR耗时或鼠标到显示延迟。最后完整关闭菜单窗口峰值113.435 ms位于第522273行，拆为26.371＋51.091＋35.973 ms，紧邻SDK清理／呈现状态转换，不能直接归为技能稳态。

末尾还有不同边界的CPU长调用：06:31:14.164 `hiddenRealPresent` 99.670 ms、06:31:28.880 `fgPresent` 212.573 ms、06:31:33.492 `copyAllocatorCpuWait` 113.313 ms（目标15030、完成15029）。第一／第三项处于失焦边界，第二项在菜单开启及FG重建期；113.313 ms被外层 `dx11CopyOrResolve` 113.375 ms包含，不能相加。最后菜单关闭窗口未记录这类百毫秒复制／隐藏Present样本，但日志限频，不能据此宣称没有等待。SDK的最大帧延迟6→1→6与资源清理只证明状态转换，尚不足以断言某次flush、6×倍率或技能本身是排队根因；下一步应获取同source的pacing、SDK Present和真实queue提交区间，而不是删除真实完成门。

CPU侧独立复核：行521086→529304的累计快照覆盖06:31:37.899–47.966，copy wait调用增加63，但≥20／≥50 ms计数194／5未变，hidden Present对应计数4／1也未变；末段后续未刷新累计值，不能推广为整窗零等待。末尾三份 `fgPresent` 55.251／33.245／41.537 ms均被对应 `interopPresent` 包含，内部SDK／pacing／锁等待没有独立耗时；ReflexSleep在FG锁及执行保护释放之后，GetState串行门又在query计时之前取锁。现有证据不支持把CPU同步判为主要增量，也不能反向排除CPU或SDK的作用。

全局、窗口和逐行复算分别保存在 `build/f6-skill-latency-97772-{evidence,final-evidence,analysis}.json`，复算脚本为 `build/analyze-skill-latency.py`。最后21份窗口已用现有 `Get-F6RunEvidence.ps1 -LatencyFirstLine 521896 -LatencyLastLine 531355` 交叉核对；当前会话另建两名gpt-6.1-sol/high分别独立复算Reflex／NR、复核CPU调用链，数值及包含关系一致，已补齐排除端点、稀疏采样和未校准时钟的限制。本次只分析和更新说明，不构建、部署、修改配置或操控游戏；不据此宣称已修复性能。

## 2026-11-12：AE53A7CC 实机记录（PID 175744）

封存日志 `build/f6-run-20261111T193336850Z.log`，SHA256 `BB52E32B49DF919A558886D3668A084DAEB8B9A7DDF13D4ACCACE18876EA4333`；03:25:51 启动，晚于 AE53A7CC 部署，未读取加载内存哈希。388 条 Reflex 日志只有 384 份唯一报告：报告跨度中位／P95 为 71.402／106.253 ms，GPU 开始前队列跨度为 15.811／50.178 ms。启停边界的 1075.994 ms 峰值分为 **940.165 ms 报告开始至队列入口＋85.398 ms GPU 前队列＋50.431 ms GPU 区间**，不能称为 1 秒 GPU 排队。解析器现保留这四段、去掉重复端点报告，13 项合成检查通过。

两个排除启停和末尾失焦的窗口（非受控 A/B）：

| 窗口（北京时间） | 唯一报告 | 报告中位／P95 ms | GPU 前队列中位／P95 ms | NR total／model／history 各自中位 ms |
|---|---:|---:|---:|---:|
| 03:27:15–03:28:15，行 61937–103968 | 113 | 66.122／82.228 | 13.945／30.435 | 40.135／29.816／4.264（18 份分段） |
| 03:28:40–03:30:10，行 123163–181144 | 156 | 71.711／97.734 | 14.656／41.224 | 37.508／29.702／4.077（24 份分段） |

工作尺寸 2560×1600，每 dispatch 一次模型调用；各中位数不相加。完整 65 份同队列分段的 total／model／history 中位为 38.625／29.706／4.050 ms。当前主要实测 GPU 成本仍是模型，历史是最大的模型外区间；CPU FG Present 的 pacing 与 GPU 工作重叠，不能再加到报告总跨度。两个稳态子窗口中实际 copy 事件等待仅 1／1050 和 6／1478 次，均无 ≥20 ms；不沿用旧局百毫秒 copy/hidden Present 归因。

本轮性能改动让 `OrderedHistory` 复用先于它执行的 Registry 对**同一实际 list/generation/queue** 的成功 Signal 凭据，移除历史的独立 fence 和重复 Signal；timer 使用相同凭据查询接口。真实队列完成、Reset、重放和失败隔离门保留。Registry 的每代实际 Signal、copy reader 等待和 Reflex pacing 保留；不做扩大池、降低用户参数或批量猜完成。减少调用已由代码路径证明，节省毫秒数和实机排队改善仍未测量。

用户报告 NR 勾选仍在但效果消失；本局后段仍有模型成功和最终发布录制，不能当画面有效证明。已补呈现端点的拒绝状态，完整页与摘要统一显示及翻译，避免旧 GPU 成功计时遮住当前失败；应用关闭与最终输出失败分开。另修最终稳定在明确跨深度表面／坏运动时仍按 floor 混入旧残差的路径。稳定器独立验证运动一致性，长历史关闭、强度零或过期也不能跳过；正常长历史路径复用已读向量的验证结果，有效低可信表面仍保留用户 floor。草残影与 FG 的实际关系仍需关闭 FG 的同景对照。当前 FG 模型选择仍要求重启；本局 SDK 明确报告请求 C、实际 C。

输出链额外复核确认：`dlssnr.hlsl` 对接近全黑的模型亮度或非有限 Look 结果保留原色，C++ 的 `successfulDispatches` 只证明 resolve 录制成功，无法识别这些逐像素回退。本局是否触发仍未知，安全回退保留。若新版本复现，应按同一 source 对比原始模型／proxy、历史后的结果和最终输出，分清模型、历史与最终合成；本轮未加入常驻像素读回。

复算全局或窗口：

```powershell
pwsh -NoProfile -File ./tools/runtime/Get-F6RunEvidence.ps1 `
  -Log ./build/f6-run-20261111T193336850Z.log -ExpectedPid 175744 -ExpectedCommit 9ab9686 `
  -Latency -SampleLimit 2 -Output ./build/ae53a7cc-latency-analysis.json
# 窗口另加 -LatencyFirstLine 61937 -LatencyLastLine 103968
```

## 2026-11-12：EA3059BA 实机记录（PID 180096）

02:58:59 启动日志为 `9ab9686`，结合该局前的 EA3059BA 部署回执绑定版本；没有读取加载内存哈希。02:59:48–03:01:54 的 219 条合法 Reflex 报告中，总跨度中位／P95／最大为 **70.532／116.792／143.743 ms**，GPU 前队列跨度为 **18.330／49.666／82.980 ms**，GPU 区间为 **51.129／53.616／63.223 ms**。报告峰值是 `49.666 + 53.000 + 41.077 ms`，不是将重叠的 SDK 区间相加，也不是鼠标到屏幕延迟。

同一真实队列的 39 份 NR 七段采样（2560×1600）：总区间中位 **35.758 ms**，模型 **28.902 ms**，历史 **3.816 ms**，resolve **0.528 ms**，encode **0.169 ms**。这些是各阶段各自的中位数，不相加；模型是主要常态成本，历史是最大的模型外区间。GPU 前排队峰值和另一次 **382.002 ms** `fgPresent` CPU 调用不属于同一项测量，不能据此认定归因；本局没有超过采样门槛的 `copyAllocatorWait`／`hiddenPresent` 行，也不证明没有等待。

用身份校验后的封存日志重跑，原始行号和样本口径保留：

```powershell
pwsh -NoProfile -File ./tools/runtime/Get-F6RunEvidence.ps1 `
  -Log ./build/ea3059ba-run-20261112-0301-OptiScaler.log `
  -ExpectedPid 180096 -ExpectedCommit 9ab9686 -Latency -SampleLimit 2 `
  -Output ./build/ea3059ba-latency-analysis.json
```

可用 `-LatencyFirstLine`／`-LatencyLastLine` 分别统计明确的固定设置窗口。下一局固定场景、画质和 FG 状态，预热后各取 30 秒 NR 关／开，再单独改变 FG；不要把开关切换、载入或失焦混进稳态样本。当前未获得该受控对照，**不把 143.743 ms 与旧局 166.103 ms 的差值当成提速**。本轮 SR 菜单修复和离线统计不改稳态队列或渲染算法。

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
