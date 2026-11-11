# F6 原神：呈现分支、历史残留与延迟核查

2026-11-11（UTC+08）。产品写树：`D:/CODE/DLSS5_NEXT-workers/f6-rewrite`。最新失败局为 **07324C3D／PID162340，07:28:38—07:32:37**。新候选 **0AD66318** 于 **08:10:48** 部署，修复离线复现的分辨率重建 recording 未收尾问题；构建、测试及完整测量统一见 [当前状态](CURRENT-STATE.md)。新候选尚未实机验收，低帧数、NR＋FG 闪回／残影和高延迟仍未解决。

PID162340 的呈现入口窗口源 Evaluate 19.645 Hz，SDK LocalPresent 117.968 Hz 仅代表突发 API 回调；14 组配对模型／NR total GPU 样本中位 34.081／35.498 ms，Reflex 报告区间中位 71.053 ms。约 37.961 ms 的 copy allocator CPU 等待已包含在 copy／resolve 中，不能与它或 GPU 包含跨度相加。较小 pre-SR 模型不是同绝对尺寸的路线对照。真实显示事件与配对实际输入像素仍缺，不猜改运动 Y 或帧偏移。完整原日志为 `build/genshin-162340-OptiScaler.log`，31,053,934 B，session 的 16 MiB 副本不完整。

以下调查固定于更早 **1FB5F6E2／PID117664** 的源码、录像和日志；后续 07324C3D 实施不等于实机通过。旧行号仅用于当时证据，不能直接套作当前源码位置。

## 呈现分支调查判断（PID117664）

用户确认：**硬性启用呈现入口之前，实际 SR 前／SR 后入口的 NR＋FG 节奏正常**，腿部仍偶有外溢。把这个实测作为独立对照，不能将本次呈现分支的失败扩大为全部 NR／FG 组合失败。原有目标仍是呈现输入→NR→细节保留→FG；本轮不擅自替换用户的默认路线。

当前可以确认源帧慢、显示统计误导、高 Reflex 报告区间及历史参考随帧漂移；**尚不能确认全部人物残影／旧画面闪回的唯一像素根因**。不能通过翻转开关、倍率计数或先降模型参数宣称修复。

重构方向：先用可工作的 Evaluate 入口恢复可测量的对照，把共用 NR 处理和呈现适配拆开，再修复呈现分支及历史模块。不以整仓退回 F5 替代定位；F6B2 若实施，应作为有明确入口和验证边界的精简重构。

## 本局身份与录像边界

- 录像：`D:/DATA/video/2026-11-11 04-58-00.mp4`，2560×1440、120 FPS 编码、99.15 秒。
- 结束会话：`20261111-045630_YuanShen_117664`；完整原日志保存在 `build/genshin-117664-OptiScaler.log`，28,171,818 B，SHA256 `C34A2501141211999C51B66617C22CBE8297F46045D8F186A9A04022238B5E4F`。session 中的 16 MiB 副本不完整。
- 磁盘安装及回执关联候选：`1FB5F6E238EDE6A9530C5C6E8C167EAEEAD7722D5B87617D7C0F487C8142919C`，33,797,632 B，04:14 部署。日志进入了新 `GenshinPresentation::Apply`；精确 loaded-memory SHA 未核。
- 视频与日志按文件创建时间对齐；亚秒偏移未经认证，以下取避开选项切换的窗口。没有这局的 PresentMon／ETL 显示事件。

## 源节奏与实际显示不能混算

| 视频窗口／开关 | Evaluate 源入口 Hz | 源间隔中位 ms | SDK 输出 API 入口 Hz |
|---|---:|---:|---:|
| 3.5–7 s，SR | 82.86 | 11.786 | 82.57 |
| 14–30 s，SR＋NR | 21.50 | 46.392 | 21.44 |
| 40–52 s，SR＋FG | 51.50 | 19.096 | 309.17 |
| 57–66 s，SR＋NR＋FG | 18.22 | 54.309 | 108.67 |
| 78–92 s，SR＋NR＋历史 | 20.07 | 49.717 | 20.14 |

“108.67”只能证明 API 回调频率，不能证明均匀显示了约 109 张不同的有效图像。源入口确实只有约 18 Hz，用户感觉慢具有实际时序依据。

原生解码场景 ROI 的相邻完全相同采样比例，在 SR＋NR 为 24.1%，NR＋FG 为 21.3%。它们只描述录像像素；改变 MAD 阈值会显著改变计数，压缩及插值也会改变像素，不能换算为有效 FPS，不能由此定量宣称多少旧帧或混合帧。尚未获得能够认证生成帧身份或物理显示节奏的证据。

已确认菜单统计遗漏：`menu_common.cpp:1888` 的菜单 Present 更新被 `CalculateFrameTimes:1947` 和页脚 `:8461` 当通用 FPS。录像约 54 秒时主 overlay 的源频率约 54.8，而页脚约 325.5；修复应覆盖页脚，不能只修左上 overlay。SDK 累计呈现计数也应保留真实查询区间。

## Reflex 60–100 ms：已在日志中确认，单列优化

从 `Reflex_Hooks.cpp:928–1045` 的实际驱动报告读取完整区间对；按报告自己的时间戳分段，排除 estimated 数据。OS queue 区间包含 GPU 区间，二者不能相加。仅在同份报告满足 `queueBegin <= gpuBegin <= gpuEnd <= queueEnd` 时计算 GPU 开始前的等待。

| 录像分段 | 有效报告数 | 报告范围中位／最高 ms | GPU 区间中位 ms | GPU 前队列等待中位／最高 ms |
|---|---:|---:|---:|---:|
| SR＋FG | 22 | 32.508／36.311 | 13.855 | 18.695／21.744 |
| SR＋NR＋FG | 16 | 72.161／103.896 | 48.273 | 21.489／51.268 |

最慢一份报告 frame 6743：103.896 ms = GPU 前 50.734 ms + GPU 区间 53.162 ms；原日志第 182428 行。另一份 frame 6752：97.038 ms = GPU 前 51.268 ms + GPU 区间 45.770 ms，第 182788 行。各列中位数并非同一份样本，不应直接相加。

这些样本的 submit／driver 完整时间对缺失，Reflex frameID 也尚未与 NR 源 ordinal 逐项认证。这里的范围是有效报告端点的包围区间，**不是完整点击到屏幕延迟，也不等于模型耗时**。没有报告的 SR／NR-only 分段标未知，不能填零。

延迟列为独立失败项。需分别降低可避免的排队、CPU retirement 等待和串行交接，并保留必要 GPU 完成门；FG 高倍率不构成延迟改善。不能承诺在保留约 38 ms 实际模型工作时获得 10–20 ms 全链路延迟。

## GPU 成本：模型确实占已测 NR 区间的大部分

按相同 dispatch／epoch／queue 配对，total 包含 model：

| 分段 | 配对样本 | 模型中位 ms | NR total 中位 ms | 同次 total−model 中位 ms |
|---|---:|---:|---:|---:|
| SR＋NR | 6 | 38.205 | 38.895 | 0.668 |
| SR＋NR＋FG | 2 | 36.348 | 36.964 | 0.615 |
| SR＋NR＋历史 | 4 | 37.851 | 42.350 | 4.498 |

关闭自定义历史时，模型约占已测 NR 区间的 98.3%；不是“全局 GPU 的 98.3%”。total 不包含所有上游导入／复制／游戏工作，样本稀疏且有选择偏差。现有证据不能支持“外围 GPU 与模型同样重”，也不能支持外围零成本。

CPU `copyAllocatorCpuWait` 有约 35–40 ms 样本，外层 `dx11CopyOrResolve` 大量包含同一等待；它们可能与 GPU 工作重叠，不能作为额外同等 GPU 成本相加。呈现分支晚提交 NR、再串接 FG 是确定的调度结构；额外丢失了多少并行性尚需同参数、同绝对模型尺寸对照。

## 与正常入口相比新增了什么

| 位置 | 当前实现及需要处理的问题 |
|---|---|
| `DlssNr_Dx12.cpp:6801,6941` | 原神 direct-DLSS 无条件进入固定呈现分支，Evaluate NR 提前返回；旧 placement 不再代表实际执行入口。用户的正常对照来自硬性启用之前，不能拿本局假作其运行证据。 |
| `IFeature_Dx11wDx12.cpp:1254,1266,1364` | 正常入口在 SR 前／后同个实际 producer recording 运行 NR；固定呈现入口只捕获 guides。 |
| `dx11_with_dx12_sc.cpp:873–925` | 固定分支经过 DX11 成品→共享纹理→DX12 颜色交接后才运行 NR，再把最终颜色交 FG。需查同源颜色阶段及真实队列依赖。 |
| `DlssNr_GenshinPresentation.inl:188–218` | 标记 FinishedPicture／NativeSceneRoute／FullModelRequired；颜色是呈现阶段，guides 仍来自 Evaluate。源身份相同不自动证明 SR／后处理后的轮廓与低分辨率 guides 在像素上相同。 |
| `DlssNr_Dx12.cpp:5082,5274` | 呈现 scene 分支模型 MV scale 为 2560,−1600；同尺寸普通 post-SR 基线为 1536,−960。差异约 1.667 倍，确有边界差异，尚不能只凭数值认定 SDK ABI 哪边正确。 |
| `DlssNr_Dx12.cpp:5329–5336` | 当前历史显式保持原始 NGX scale 1536,−960，再除 guide rect。不能把模型 scale 的疑点重复“修”到历史或 FG 上。 |
| `DLSSG_Dx12.cpp:2402–2499` | final colour join 有逻辑源／queue／receipt 验证，并明确 pixel lineage 未认证；不把逻辑 join 通过宣称为像素配对通过。 |

本局实际 depth／MV 为 1536×960，最终颜色 2560×1600，FG 实际归一化 scale 为 1,1。04:58–04:59 的 Velocity／Depth／PresentColor 各有 1255 次成功 tag；没有 UI／HUD-less tag。说明 FG 收到了资源，不说明资源像素、方向、相位必然正确。UI 未保护是独立已知缺口，不足以解释人物外甩。

Ada 4060 Laptop、请求 6×且硬件 flip metering 仍开启的提示需保留调查；未证明它是原因，不自动打补丁或对每个生成回调加 Sleep。57–66 秒窗口无 flush 或反复停用证据，不能拿录像结束后的 Reflex 错误解释该段。

## 自定义细节关闭与模型历史分开

当前自定义独立历史的有效请求为 `DetailReuse && PresentedTemporalDetail && !vulkanGame`（`DlssNr_Dx12.cpp:5299`）。关闭总开关后，`DetailHistory::Before:292` 使历史失效并返回，不再录制新历史融合；固定呈现分支也在旧 `DetailReuse::BeforeModel:1024` 禁止 legacy 跳帧、spread 及旧 cohort。

但这不会关闭 NR 模型内部自己的时序状态。配置最终值、旧 cadence 的 `feedbackRequested=true` 和上一份完成快照都不能反证某段自定义历史实际融合。当前日志缺少每个开关阶段的完整独立历史有效状态，不能把用户不确定的关闭状态擅自改写为全程关闭。下一版直接显示该有效门、最后融合源及贡献年龄，关闭时明确“自定义历史关闭”；与模型时序状态分开。

腿部外溢仍须区分：NR-only 新鲜结果已有外溢、仅 FG 新增外溢、或只在自定义历史开启后产生。现有信息不能把三种情况合成一个原因。

## 历史的可复现设计弱点

`dlssnr_detail_history.hlsli:276–297` 保留残差 anchor 和年龄，确实没有把显示融合结果回写为历史；但 `DetailHistoryCapture:224–230` 与 host `DlssNr_DetailHistory.inl:428` 每帧覆盖参考 proxy／depth。结构验证 `:122–162` 因而对比的是相邻帧外观，而非原残差生成时的外观。**残差年龄与外观参考不属于同一个原始样本。** 渐变每步都小，也可能保留早已偏离原始外观的残差。

用实际生产 HLSL 的 WARP 夹具已复现：TemporalMode=2、duration=.911 s、maxAge=1.529 s、strength=.95、sigma=.017；固定零运动、均匀深度，raw 每帧增加 .006。第 20 帧 raw 从 .4 变成 .52，旧 residual 仍为 .02、age=1 s，显示额外贡献约 .001154；第 24 帧外观差 .144，仍有旧残差及非零贡献。编译／运行 exit 0，未改生产 shader。

这是可执行的渐变参考漂移诊断，不是原神人物落地面的像素复现；不能宣称已唯一解释录像。现有 16×8、整数运动栅栏测试不足以覆盖低分辨率 guides、分数像素轮廓及后处理场景。

只读对照 Skynizz `fc702f8c` 的 `dlssnr_cache.hlsl:81–84,528–534`：原始 fingerprint 与 residual 一起传递，明确不每帧更新。可吸收这一不变量，不整体搬入其多纹理缓存／跳模型系统。F5 对照固定为 `f10dedf9`；本地 Encore 逆向固定 SHA `06a7fd63...a5f1217`，可参考独立 guide rect、失败不解码回写、真实跨 API 同步，不能当游戏验收或照搬全部光流管线。

## 实施顺序与完成门槛

1. **恢复可信对照与统计。** 统一入口选择，显示“实际执行入口”；同一源只允许一个 NR writer。保留原有单选预览，不恢复校准向导。页脚改读与 overlay 相同的源／SDK 缓存；显示率没事件则未知。自定义历史关闭必须零新增历史 dispatch，状态不借旧完成值冒充当前。保持外部桥 2.4 启动兼容边界及内部 FSR 桥禁用。
2. **一次性补齐像素证据。** 复用既有成对导出，采同一源颜色阶段的连续两张 SR／present-before-NR／NR-final／实际 FG 颜色与实际 guides、有效 rect、scale、jitter、token 和变换。按需 GPU 异步采样，忙则跳过，完成后停止；任何导出失败明确缺项。确认模型 scale 和颜色阶段差异，再决定修边界，禁止凭“腿向外”猜改 Y 或 frame offset。
3. **精简单源处理与呈现适配。** 共用一次 encode→模型层链→可选历史→Look／resolve→一次发布；Evaluate 和呈现只是不同的合法生产接点。扩展已有不可变源描述的颜色阶段／guide 单位，避免增加多套互相转换的状态表。当前 NR 失败／跳过保留本帧 SR／原图，不发布旧 NR；FG 只使用最终已封存的同源结果。呈现保留为目标路线，SR 前／后成为能实际选择的对照入口，禁止互相偷偷覆盖。
4. **重写历史内容合同。** 把原始 residual、出生参考 fingerprint、年龄／有效性作为同一份样本传递；双线性 taps 的全部有效性和来源须一致，不平均年龄或混配参考。上一帧 depth／proxy 的几何匹配与原始 fingerprint 分开：当前到上一帧用于重投影／遮挡，出生参考用于判断原残差是否仍适用。拒绝落点用当前 fresh NR，绝不从邻近人物 donor 补地面；曝光／Look／变换代次变化失效。该历史模块不能修改送进模型或 FG 的运动。
5. **降低排队和延迟。** 以同源 queue 时间线分开导入、encode、模型、历史、resolve、输出及 CPU 等待；核对 Reflex frameID、marker 位置、Sleep／限帧 owner 与真实源。只在真实接点移动 pacing，且每源一次；生成回调不推进源，不重复限帧。检查晚提交、copy slot 复用及 hidden／SDK Present 的阻塞，减少能证明多余的串行交接；跨队列用真实 Wait／Signal，退休继续要求真实完成与 Reset，不删门、不扩池、不加旧帧缓存。6× metering 只有形成调度证据后才决定处理。
6. **分开验收。** 同场景、同绝对模型尺寸／层数／Look／历史时长／FG 倍率，对照 SR、SR＋NR、SR＋FG、SR＋NR＋FG；无需转头校准。每组预热 5 s 后记录 30 s，源 Hz、真实显示事件、Reflex 中位/P95/最高及原始有效端点分别报告。继续长时运行及模型参数往返切换，检查队列年龄、资源数量和延迟是否持续增长。

自动化新增重点：原始 fingerprint 漂移负例；低分辨率 guides＋分数位移＋非零 rect／jitter 的腿／栅栏遮挡；8 种方向及双重变换负例；自定义历史关闭时零新增工作；同源发布／FG join；队列延迟、Signal 失败、错误 queue 和缺 Reset；Reflex 端点缺失／区间包含及 frameID 错配。已有生命周期／桥 ABI／shader 回归保持。

像素错误、持续掉速、高排队或显示事件缺失，均不能用构建成功豁免。原神通过后，星铁／ZZZ 各自回归。若呈现输入不能和 Evaluate guides 建立可信像素映射，应明确保留限制并修接点，不能把元数据“已知”当映射证明。

## 07324C3D 历史交付与当时未完成项

测量脚本和结果在 `build/video-045800/`：`analyze_video.py`、`analyze_log.py`／`runtime-analysis.json`、`analyze_latency.py`／`latency-analysis.json`、`probe_anchor_reference.cpp`／`anchor-reference-probe.txt`。这些保持旧局分析身份。后续主会话曾实施、离线验证并部署 **07324C3D**，此候选随后在 PID162340 实测失败。以下仅说明该历史候选的改动，当前部署以文首及当前回执为准。

- `DlssNr_DetailHistory.inl`／`dlssnr_detail_history.hlsli`：历史改为单次三输出，原始残差与出生指纹、年龄共同迁移，保留当前深度作下一帧几何验证；逐 tap 验证出生来源及外观，移除 mode25 独立捕获，不新增纹理。生产 HLSL WARP 3,048 项通过；测试副本恢复滚动参考后按预期失败，证明新回归能检出此前遗漏的设计缺陷。
- `DlssNr_Dx12.cpp`：确认编码、模型输入缩放、最终合成三个关键 `DispatchPass` 返回值曾被忽略。描述符忙或录制失败时可能沿用旧 scratch，这是额外的确定代码缺陷，不能据此宣称唯一解释录像。现逐级停止错误发布、恢复资源状态并 reset；10 个抽取实际生产分支的故障场景通过。
- `NrPresentationOutput.h`／呈现适配：普通兼容输入直接编码，省去一次全图复制；LUT／hold 需要写入时按需复制，PQ 保留私有解码。实际 DX12 WARP 1,586 项验证源图保持、成功发布、失败／未执行 recording 与队列延迟。实际入口单选恢复 SR 前／后对照，默认仍为呈现路线，不自动修改 guides 方向或缩放。
- `menu_common.cpp`／`ReflexQueueTiming.h`：页脚与 overlay 统一源／SDK 缓存并复用格式化；Reflex 仅用同一报告的合法嵌套端点显示 GPU 前排队，12 项通过，没有额外 SDK 查询。尚未证明真实排队或端到端延迟减少。

历史 runtime 16 隔离场景、热重建 30 场景／191 项及不安全负控、Look／预览／GuideCapture／FG join 和公共回归通过。DXIL／SPIR-V 已同步，Release x64 exit 0；外部桥 D3D11／D3D12 各七个必需导出仍存在。构建、测试和四路径部署身份见 `build/current-{build,tests,artifact,deployment}.json`，实机与 NVIDIA SDK 验收仍空缺。

本次 DeepSeek 原生检索路由失败，未获得可用调查正文；以上发现由主会话核对本地代码／固定参考和日志得出。没有复用过期智能体。联网未取得新的可核对材料，不声称参考仓库为当前最新。

仍缺：实际 FG／NR 配对像素、此局真实显示事件、正常非呈现入口在相同绝对模型尺寸下的完整新对照，以及可靠的全输入到屏幕延迟。**当前不是“全部根因已查清”或“修复通过”的交付。**
