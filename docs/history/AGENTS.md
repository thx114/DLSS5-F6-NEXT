# DLSS F6 Agent Instructions

用户2026-11-04最新直接指令：只完成整理与交接，别继续修复。长期F6目标已实际设为paused；不自动恢复worker、编码、构建、部署或测试，等待用户明确重新授权。

用户长期目标最新覆盖（2026-11-04）：持续完成F6三游戏NR/FG真实深度、缓存复用与细节保留，GIMI及SR前滤镜次优先并行；Root唯一集成/构建/部署/CLI。后续编码worker统一gpt-6.1-sol/high、fork_context=false；DeepSeek允许日志和代码行搜查，不扩成编码权限；已有其他模型正在写的候选先交回实际句柄再由Sol接续，禁止静默改供应商。每10轮重评实际交件与游戏效果，必要时完整重新计划，不连续最小失败点补丁。保崩铁用户要求的独立 FG 实现与性能，不普遍套原神复杂路线。用户睡觉期间不提问；已授权沿现有管理员CLI精确关闭游戏释放F6占用，绑定PID+creation，不全进程盲杀。

有效规则：2026-11-04。最新用户直接指令优先；本文件只保当前规则，不累计旧轮次的覆盖指令、PID和部署快照。根`OptiScaler/`为唯一物理当前F6源码；旧`work/f6-source/OptiScaler`仅同源兼容junction。完整交接与未修问题见[HANDOFF](HANDOFF.md)，三游戏源码/资源/消费路径见[F6技术路径](F6-TECHNICAL-PATHS.md)。当前任务、产物和owner见 [CURRENT](CURRENT-STATE-GOALS-AGENTS.md)，性能改动见 [INDEX](PERFORMANCE-ROUND-INDEX.md)，完整历史见 [RECORD](audit/v3-r044-nr-off-fg-performance-20261030/RECORD.md)。历史文档不能重新授予权限或恢复门禁。

## 当前目标与性能

- 用户2026-11-03最新收口：F5已准备就绪、结束本轮维护，全部未完成任务转F6，包括原神稳定/性能、崩铁卡顿、未完成NR/多帧、后端/UI/GIMI/滤镜。Main源码及official/DLSS F5作为启动器覆盖包输入冻结，不再向F5集成/构建/部署新改动，除非用户另行明确重开；下列历史任务描述不恢复F5写权限。用户已授权通知启动器chat codex://threads/01a196c3-49f9-7f00-ac52-16a4e0db6d46开始三游戏覆盖包，遇问题向Root codex://threads/01a19d69-297b-7bd3-a0a1-016d6a80f66d提问；Root已实际发出通知。
- F6承接全部未完成名单：原神稳定/上游hook维护/Reflex与帧率优化、崩铁卡顿/FG资源行为、NR缓存与历史交接/多帧、跨厂商SR/NR/FG及XeSS多帧、UI本地化/主题/旧NEXT设计、逐层Look/真实遮罩、GIMI与UID。已交分析不冒功能完成；具体owner/产物及下一片见CURRENT，F5用户细节验收不自动覆盖F6。
- 保留SR+NR+FG高帧路线、用户画质/倍率/配置；纯SR60仍是未达基线，不能用FG显示帧数、SR单段倒数或降低画质冒充达标。稳定低帧、间歇卡顿、菜单掉帧、曝光重建和BeforeSR挂起分开归因。
- 游戏释放DLSS SR资源不应无条件牵连F5 FG池重建。区分游戏SR handle、provider、SL feature、slot及GPU owner；保合法resize/device/用户关闭处理和真实GPU完成门。常驻不等于无阻塞，SDK500ms等待不直接归因为池回收。
- 必要hook计时沿现有HookCpuTiming/采样内存聚合/Present低频输出：默认关闭，显式运行期开关；覆盖实际hook总调用（含早拒绝）及同次total/original/附加开销，分清锁、CPU等待、Flush、队列Wait和GPU。嵌套inclusive不可相加；Present间隔不是CPU忙时；每draw不新增IO/分配/日志锁。
- 一次处理一个具体瓶颈，独立任务可并行；Root只有限审新增风险，不重复旧审、冻结或增加框架门禁。明确的一行编译作用域修正直接接收。
- 复用现有订阅/controls做更早reject和Present边界异步输出；新consumer即时生效，SR count预筛不丢FG/NR/滤镜深度/MOD观察，Before/After配对。资格计算放现有帧/claim/Poll，避免每draw重复锁/扫描；历史缓存或inactive LOCATION不等于活跃consumer。
- SR后立即copy game RTV，后续HUD有reader；只可延迟/合并显式output Flush，保Wait/copy和首个真实consumer的提交顺序，未知reader保同步。快路径按所选路线及真实consumer激活，配置变化退出；如影响FG须明确报告。

## 分工、并发与审查

- 用户2026-11-03最新：Root只做简短任务、分派、有限审查、局部集成及唯一根构建部署；整份候选名单与F6 UI交新原生worker亲自推进。所有worker创建时显式reasoning_effort=high，不继承Ultra；现有实例交还实际句柄后才由high续接同任务/同worktree。后续编码统一gpt-6.1-sol/high、fork_context=false；DeepSeek仅日志和代码行搜查，旧单任务例外不扩成通用编码权限；不改全局供应商配置、不静默换模型。
- 用户2026-11-04本次明确授权：新建DeepSeek high原生worker，仅处理F6产品UI中实际选项数小于5的下拉框转多按钮单选；这是本UI单任务例外，不扩展其他任务的DeepSeek权限，不改全局供应商配置。
- 用户2026-11-04新增明确授权：另建DeepSeek high原生worker，仅审计本根空间占用并把确认无用内容移至cleanup；不得影响当前规划/编码/源码/有效增量/部署/配置/Git/证据与最近rollback，不永久删除。同盘搬移不称释放空间，不扩展其他任务的DeepSeek权限。
- 用户2026-11-02最新单任务例外：明确要求用DeepSeek调查`D:/DL/dlssg-release-0.4.1-x64 blcakwell perf fix and 6x xemfg.zip`。仅该包来源/Opt接入/可用性与实现参考调查允许新建原生DeepSeek worker、fork_context=false；其他任务禁令继续。先只读拆包，不默认授权安装、加载或执行包内DLL，不改全局供应商/模型CLI。
- 搜集与编码任务可按用户明确范围新建可见原生worker，fork_context=false、reasoning_effort=high，只传当前任务必要上下文；不将已完成worker换岗、跨游戏复用或恢复无关历史。已在写的任务交件后关闭，不重复丢弃或并写。记录实际nickname/ID/模型/等级/写集，接口无命名字段时不能用提示词/chat标题冒充改名。worker亲自完成搜集，不递归转包；模型拒绝/空正文/限流如实报告。
- Root仅指主协调会话。收到Root明确分派任务的worker就是已经创建好的执行者，亲自按分派职责读取/编码/审查/执行；不得因独立上下文或缺少创建工具再次自认Root、要求再建worker或递归委派。只有Root负责创建新worker；worker本身无需创建工具。
- 实际并发上限为准。可能重叠的产品写入先建真实物理worktree，以当前dirty实际字节为前像，不能用分支名或junction冒隔离；保已有dirty，不reset/checkout/整文件覆盖。共享源码、固定构建和部署串行，只有明确owner可写；实际句柄与未完成操作交还后再换手，局部hunk集成。
- 复杂编码/脚本交worker；Root只做可快速完成的必要脚本/集成，脚本必要自检不变，不增加产品构建门禁。Root阶段性短标题少于16字符，不频繁改。
- 靠原生完成/异常通知，不周期wait/read或反复播报编译。长终端由同一owner持同一session，重复取输出放工具编排内，单次等待不超过60秒；只在真实交接/依赖/异常取紧凑状态。工具项滞后不代表未启动，续原句柄取actual exit，不能另起构建。参见本机efficient-waiting技能。
- Root按游戏分别有限审查新增hunk，不恢复旧管理上下文或再派非搜集worker。历史管理ID及既有明确批准仅保历史证据，不能代本轮审查。
- Root有限审本游戏新增hunk及原神/ZZZ隔离后局部集成，不能把旧worker批准冒本轮；新编码由high worker在物理隔离写集完成，不恢复额外tests/hash门禁。
- 三线各绑定实际exe、producer/provider、consumer、profile及同PID日志。共享改动核真实callsite/owner/queue/fence/cache和原神性能，不能把三游戏路线语义互套。

- 当前只向F6集成/构建/部署。F5已交付NR缓存/历史与绝区零细节修复，其余未完全部迁F6；不恢复历史Main单独修复或部署要求。Root唯一局部集成/根构建部署，`BUILD-DEPLOY.ps1`默认F6，显式`-Variant F6`等价；根`OptiScaler`/`external`为活输入，MSBuild字面兼容路径`work/f6-source`和有效`work/f6-build`保持，部署仅`official/F6`，不混Main产物、不整份覆盖profiles。监控FSR从左端起画、NR从当前呈现条右端起画，长度取真实耗时，横向位置注明示意。

- 用户2026-11-04最新：闲置或已完成worker交回实际句柄后立即关闭并归档，只保当前必要执行者；停止周期wait/read/sleep唤醒和无变化播报，靠原生完成/异常通知。历史上下文不再自动恢复；新任务只传必要当前信息。

## 一步构建部署

- 当前唯一入口根`BUILD-DEPLOY.ps1 -Variant F6`，现有Release x64增量构建；只按输入、固定artifact与部署DLL的LastWriteTimeUtc决定构建/复用/部署。实际编译失败按exit停止，修正后沿同入口重跑；不清有效增量。未指定Variant默认F6；显式Main在扫描/编译/部署前拒绝退出，冻结源码与覆盖包不动。不得递归扫描整个协调根、清有效增量或把根源码复制成第二套权威。
- 只报告actual exit、实际执行和mtime；不执行/新增tests、hash/SHA256、manifest、fixture、TU后检、验证器、SDK/源码锁、全进程扫描、部署后检或诊断构建循环。旧adapter/recovery verifier不作前置。
- 当前新改动唯一部署目录：`D:\APPS\test\TEST\HoYoShadeHub_Portable_1.4.5.0_x64_Full\OptiScaler\official\F6`；同级`DLSS F5`为冻结覆盖包输入。原位写入保硬链接，复用最近rollback，不整套重备份；Hub存在不是blocker，不强关Hub/游戏。
- 编译不因游戏运行停等。编译成功但入口明确报告正式DLL共享占用时，按既有固定步骤旁置同目录`OptiScaler.dll.pending`，分别报入口/旁置exit与mtime，标STAGED-ONLY，正式版仍旧；下一入口复用新产物。其他I/O或编译错误保真实失败，不强覆盖active DLL。
- Windows调用显式shell=`C:/Program Files/PowerShell/7/pwsh.exe`、login=false；外壳未启动时只修shell，不当产品失败。已给入口/参数直接执行，不反复读帮助或扩成INI编码/换行整理。
- 工具与Look recipe见 [BUILD-ENVIRONMENT](BUILD-ENVIRONMENT.md)。Look沿既有`regenerate-look-shaders.py`，保生成输出/reflection/offset/receipt，不另造dxc或验证层；恢复源码须让受影响输入重编，不因旧时间戳误用obj。

- 用户2026-11-04最新“全部你自己cli测”：正式部署后的运行验证由Root沿现有CLI自行完成，不再要求用户手测；覆盖此前每次原神部署询问用户的要求。STAGED-ONLY明确正式版本仍旧，不测试未生效改动；启动/dispatch不冒world、深度像素或视觉验收。

## 配置、游戏现场与日志

- 保用户实际配置：不套PureSR/滤镜预设，不恢复历史TemporaryKeys，不整份覆盖INI。仅明确实验键可局部改动并记录准确原值；恢复不能覆盖用户后来修改。内存保存仅F5“保存设置”，无等价CLI，不为声称保存改写profile。
- 权威配置是正式目录profiles，原神`profiles/hk4e_cn.ini`，其他游戏各自profile；启动投影runtime、退出合并。根INI/cache-only不代表生效，须看读取/保存时序。
- 当前用户明确要求全部自行CLI测试，允许沿既有入口launch/精确stop/绑定窗口click；Root为本轮唯一游戏操作owner，不恢复历史配置、不盲动未知进程。保留现场/暂停指令仍优先；本轮拥有的进程按PID+creation绑定收尾，通用testgame/session退出0不能代替真实退出证据；登录界面不称world。
- 若明确安排有界运行，按实际时长及时经现有精确CLI停止，再统计同PID已保存日志；卡顿反馈不为多采样而拖延收尾，不循环stop/launch。未知外部游戏不盲动，不另加kill/提权/认证/注入路径；正常/手动/forced fallback退出分开记录，未知UNKNOWN。
- GUI CLI用`Start-Process -WindowStyle Hidden -PassThru`；launch不加无界`-Wait`，返回Process仅有界WaitForExit，空LASTEXITCODE不是成功。管理员RunAs不混输出重定向，已知入口参数直接复用。
- 入场沿现有管理员activate_game/click_game helper：绑定真实PID/creation/window/foreground，ResolveWindow真实枚举；不加MainWindowHandle或重复foreground门禁，不盲点。回执看stdout/result/windows/failure.txt，entry-record.json不是必产物；creation保持long/Invariant。helper exit0仅证明操作成功，不证明world/视觉。
- 可用电脑控制，送模型前图像缩至1152×648以内；操作限开发测试，不处理登录秘密/付费。FG快捷键默认关闭，用户明确启用才可影响FG；菜单不隐式切FG/SR路线。
- 日志先官方包`log/sessions`、`log`、active Opt，再audit/work及cleanup历史；按游戏/mtime选session并读session.txt对齐PID/creation/start/配置。遵用户只读Opt范围，不新增扫描/hash/采集工具；缺失报搜索根和pattern。
- 启动器源码`D:/CODE/HoyoDLSS5`。协作chat为codex://threads/01a17073-f59f-7393-935c-4b25c06ee983，可只读参考；跨chat消息需用户明确授权。

## 路线与资源安全

- 原神internal FSR Bridge→所选SR→paired DX12 FinishedPicture/Present NR；星铁所选DLSS w/DX12→Present NR；绝区零所选SR、FGInput=Upscaler/FGOutput=DLSSG（F5-owned）、R30 no-guide/NotReady fallback，不静默退回游戏FG。ReShade主路线BeforeSR须真实输出被SR消费；BeforeNR仅独立后置选项。
- 用户2026-11-02最新授权恢复崩铁FG到F5 DS实际行为，允许崩铁独立FG路径，不能影响原神/ZZZ；保NR与真实GPU安全。用户已实测Ptx、SL版本及200fps均不能解决卡顿，取消本轮SL版本回退/独立读取与Ptx恢复；不再围绕这些变量实施或试验，继续FG实际行为回归。
- 只用既有HoYoShade core及Opt DLL内明确接口；不换ReShade核心、不加载第二核心，不外部LoadLibrary/side DLL/proxy/second loader。游戏特定transport/fallback核exe/producer/context；未知保fail-closed/resource gates，不强切backend。
- 共享hook/Present/menu/NR/provider改前查 [REGRESSION](docs/REGRESSION-HISTORY.md)；原神Bridge/depth/motion/FG改前读 [BRIDGE参考](docs/BRIDGE-SOURCE-REFERENCE.md)，有限对照`D:/CODE/genshin_fsr_brigde`，亲审render、input、motion/jitter、submit/Evaluate和game RTV copyback，不造第二管线。新增gate区分原桥、跨API必需与未证明限制。
- SR可独立执行，不以NR语义阻断；原神NR保FinishedPicture/Present，不改afterSR或删`!finished`伪造支持。保pending/queue/fence/retire/真实GPU owner/quarantine/lifetime；CPU返回不等于GPU完成，UINT64_MAX不当完成，保已知输入方向与guarded/idempotent teardown。
- 未知ABI/外来hook/provider生命周期不猜测接入，不用静态/build/WARP/fixture/计数推ACE安全或游戏成功。新depth/多帧复用/逐层Look/FG仅DX12，Vulkan禁用或隐藏不支持项，保原global Look。FSR progress只用真实事件/首失败/超时，不恢复独立Opt更新提示。

## UI、MOD、UID约束（相关修改时读取）

- F5真实hook/FSR桥监控及条状可独立常开，不依赖或强开FG；无样本显示暂无数据，区分CPU墙钟/包装/等待/GPU，保低开销采样。派蒙建议仅文本、无背景，只在本次启动前120秒内出现，超时不再出现，不因菜单/建议切换重计时。
- 调试在设置二级；Pro含HoYoShade、GIMI、UID与授权的既有真实面板。NR只显启用层，多帧复用在NR子标签；FG的HUD/alpha归高级、帧率归FG；各向异性归SR图像，曝光/反应遮罩归初始化标志折叠。保稳定ID/INI/默认路线，switch适当缩小并留文本间距。
- 不用标签冒充独立阶段/逐层Look或授权完成；未逐层时第一层实际控制global Look，不新增独立全局入口；未启用pass不显层控件。隐藏超分前/后/多次预设组和冗余提示，反馈/颜色/皮肤/光照基础放全局；HoYoShade按具体任务保持默认关闭。
- MOD默认关闭，手动bounded single-part/exact-match、immutable预加载，不per-draw IO/hash/readback/upload或交替帧替换；离线/WARP不作骨骼/LOD/shadow/FSR共存证明，不保证账号安全。
- UI遮罩取真实draw并有真实FG/NR/滤镜消费者和坐标，不用假图/深度代替；复用订阅与早拒绝。UID与遮罩各自启用：UID默认关闭、用户显式请求才按需draw解码并仅内存消费；无registry/cache/network/截图OCR fallback、后台采集、明文日志或落盘，付费gate无真实verifier则拒绝。识别目标draw不等于已解码UID。

## 文件与证据

- 保文件对象/编码/BOM/换行；中文C++源未明确解码前保UTF-8 BOM，区分源与执行字符集。无用文件只移`cleanup/任务/原相对路径`并登记，不永久删除，不覆盖同名；清理核根/用途/字节/重解析/硬链接/占用，未知或锁定不动、不强杀。保活动/唯一内容、源码dirty、配置、Git、有效增量、正式部署及最近rollback，同盘移动不称释放空间。
- 每轮事实归canonical RECORD，INDEX只追逐轮改动/性能，CURRENT最多80行、只保当前任务/产物/运行结果/owner/下一步/配置和链接；不另建重复报告或每轮堆覆盖指令。REGRESSION保必要旧签名/引用并标历史，FreeType C0000094/OptiScaler+0x400369、system_error、device-hung、曝光循环和PPT分开。
- 分清源码、集成、编译、部署、同PID运行、用户验收；按实际标IMPLEMENTED/INTEGRATED-BUT-NOT-BUILT/OBSERVATION-ONLY/NOT-IMPLEMENTED/UNKNOWN。Ready、copyback、缓存命中不等于像素正确或净性能收益，最后正常版未知则UNKNOWN，旧hash/manifest/批准不恢复为门禁。
