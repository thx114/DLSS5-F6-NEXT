# F6 项目交接（R1077，2026-11-04）

最终状态（用户2026-11-04直接收口）：只交接，停止继续修复；长期F6目标已实际paused。误重新分派的R1079原神/Ptolemy、R1080绝区零/Meitner、R1081 GIMI/Beauvoir已全部关闭且实际归档。R1079/1080只留下私有before/candidate，R1080明确未编辑候选；R1079中止副本未作完成核定，不得作为可集成交件。R1081未见任务目录。无新集成、编译、部署或游戏测试，最新正式版与下列未修结论不变。用户明确恢复前不重启工作。

本轮按用户要求停止等待修复、完成整理后交接。根 `OptiScaler/` 是唯一物理当前源码；源码上移与文档整理没有构建、部署或游戏运行。最新正式功能版为整理前 R1072+R1073 联合版，三游戏仍未全部修好。

先读本页与 [CURRENT](CURRENT-STATE-GOALS-AGENTS.md)，规则见 [AGENTS](AGENTS.md)，深入源码关系见 [F6技术路径](F6-TECHNICAL-PATHS.md)；历史事实仍归 [canonical RECORD](audit/v3-r044-nr-off-fg-performance-20261030/RECORD.md)，逐轮变化见 [INDEX](PERFORMANCE-ROUND-INDEX.md)。旧轮次、旧hash/manifest/批准、旧模型分工不能恢复写权限或验证门禁。

## 1. 正式版与当前源码分别是什么

|对象|准确状态/路径|
|---|---|
|当前源码|`D:/DATA/Documents/ChatGPT/DLSS F5 NEXT/OptiScaler`，从当时 `work/f6-source/OptiScaler` 整个真实目录同盘上移，保dirty、文件对象、编码与mtime；未从Git HEAD或私有候选覆盖|
|兼容构建视图|`work/f6-source/OptiScaler` → 根 `OptiScaler` 的junction；`work/f6-source/.git`及worktree注册原位保留。不是另一套可同步源码，也不是worker隔离|
|活依赖|根 `external` 与原 `work/f6-source/external` 均指 `work/full-source-f5-v11-candidate-20261018/external`，旧目录仍是活输入，禁止整体当废件搬走|
|有效增量|`work/f6-build/{obj-main,out,compiler-temp}`及PCH/PDB/tlog/log保原位；MSBuild项目/SolutionDir/工作目录仍使用字面 `work/f6-source` |
|唯一入口|根 `BUILD-DEPLOY.ps1` 默认F6，显式 `-Variant F6` 等价；显式Main在任何扫描/编译/部署前报冻结并exit1。输入扫描只根 `OptiScaler` 和活 `external`，不递归整个协调根|
|最新正式|Root交回根 `40289/cell719` actual exit0，真实compile/link/Deployed in place；artifact `work/f6-build/out/OptiScaler.dll`，mtimeUTC **2026-11-04T08:30:49.1715003Z**、**34796544 bytes**，北京16:30:49|
|部署目录|`D:/APPS/test/TEST/HoYoShadeHub_Portable_1.4.5.0_x64_Full/OptiScaler/official/F6/OptiScaler.dll`；构建日志 [R1073 joint](audit/r1073-before-sr-filter-consumption-20261104/F6-BUILD-DEPLOY-joint.log)。actual exit由Root交回，日志末见真实链接和原位部署|
|本正式包含|此前R1067共享深度/continuity、R1069 bootstrap+sustained、R1058 pause及既有GIMI/Look；本轮另含R1072十文件34hunk有限参数交易/selected执行与R1073三文件10hunk BeforeSR完整效果录制/消费接点|
|本正式不含|R1075星铁NR两文件私有候选；R1076/R1078各0-byte patch、0hunk，无修复可集成|
|整理后未运行|构建入口路径修改 **IMPLEMENTED / NOT-RUN**。没有新的build/deploy/CLI/像素验收；旧兼容路径尽量保增量，不保证下次MSBuild绝无受影响TU重编|
|冻结内容|`work/r294-r266-build` Main及正式 `official/DLSS F5` 覆盖包原位冻结；旧Main独有 `native/ZzzFinishedPictureReceipt.h`（1202bytes）保留，不并入F6或覆盖丢失|

正式DLL原对象与三个runtime的hardlink关系不变：`cache/games/{nap_cn,hk4e_cn,hkrpg_bilibili}/OptiRuntime/2E415BFEBE7E11B2/OptiScaler.dll`。整理没有写正式/缓存DLL或profiles；后续只能沿现有入口流写原对象，不能Copy/rename替换破坏链接。磁盘正式存在/mtime相同不证明同PID加载内存或像素正确。

## 2. 三游戏实际结果与未修问题

**下述最新CLI都早于08:30:49联合正式版，不能当R1072/R1073的新运行验收。用户FG可见失败保持有效。**

|游戏/证据|已观察到的事实|仍未通过|
|---|---|---|
|原神51780；`20261104-161924_YuanShen_51780`；t0北京16:19:14.423；creation UNKNOWN|旧08:17:31.9259590UTC/34790400bytes版，180s CLI59484/terminal32087/cell706 actual0；回执游戏不运行，管理员精确GetProcessById51780 exit1/notrunning，退出原因UNKNOWN，不称Root强杀或自然退出。实际NR低频模型depthtrue仅3个样本，false0；FG DispatchOk23/Deactivate0、physical-content16/owner-content15，同token Reflex Start/End/Sleep返回0|用户直接 **USER_FAIL：FG没有效果，打开Win菜单才见左上监控**；仅3个depthtrue不能判持续world深度/像素通过。SDK有失焦和同步短暂shown，不能反推整局始终hidden，也不能按API计数算插帧|
|ZZZ48964；`20261104-162553_ZenlessZoneZero_48964`；t0北京16:25:43.874；creation134382543438743436|120s CLI17936/terminal42141/cell714 actual0，明确forced kill收尾。selected SR1591，NR cadence/modelDepth非空；无 `dlssnr_call_evaluate_v2` 特定字符串不能当NR未执行。FG DispatchOk0/Deactivate113，旧partial-publication0|L37043 source guide frame23成功提交/sr_succeededtrue，L37044 Reflex改23→4882，L37048 current-guide-pair拒绝，L37049停用；4893→4896后重复。FG未恢复；深度方向/曝光/动态细节和性能也未完整通过|
|星铁34648；`20261104-153414_StarRail_34648`；t0北京15:34:03.886；creation134382512438861274|90s CLI49468/terminal55386/cell635 actual0，forced fallback；FG真实Dispatch Ok2058、Deactivate0，NR guides format41/1280×800，真实GPU completed total/model_interval各135组；保持用户独立FG|L73103 sceneSource1801对published1799、feedback requested/active却historyUsable/fusion=false，slotBusy44；44/44原日志未证明发布者fence对象相同。total中位56.5535ms包含model53.8952ms，不可相加或倒数冒world性能；动态曝光/残影/细节连续性与净性能未通过|

原神full/session在 `audit/r1069-genshin-presentation-order-20261104/genshin-pid51780-*`；星铁同目录 `starrail-pid34648-*`；ZZZ在 `audit/r1067-genshin-world-terminal-20261104/followup-continuity/zzz-pid48964-*`。Root已交回本波CLI/游戏操作句柄，整理未重启游戏或重新查询进程。旧51132进程“查询空但后续残留”教训保留：一般CLI退出0/普通查询空不能冒精确收尾证据。

旧28532 DRED887A0006首未完成SR slot的首因仍UNKNOWN；47188旧帧/深度闪USER_FAIL和51132深度13/26不能被51780短样本覆盖。未复现设备错误不等于已修。

## 3. 留给下一接手者的候选与调查

- **R1075 Sartre** `01a1a5a3-a236-7a72-aaa5-cb53c53b32fd`：已最终交还并close/archive；[RESULT](audit/r1075-starrail-nr-history-continuity-20261104/RESULT.md)及同目录before/candidate/source.patch完整原位。唯一两文件 `shaders/dlssnr/DlssNr_DetailReuse.inl`、`DlssNr_Dx12.cpp`，**IMPLEMENTED / PRIVATE-SOURCE-ONLY / NOT-INTEGRATED / NOT-BUILT / NOT-RUN**。候选使用消费端真实 `nativePhase.verified` 授权既有ordered native history，绑定真实发布fence对象并在实际完成后由原池owner Reset旧executable；不删单步ordinal/contract/fence/MAX门，不改FG/SL/PTX/profile。合法反馈恢复仍可能增加真实既有GPU工作，不能提前宣称加速。下一步Root有限审相邻source→发布→真实消费→退休，局部集成前核当前dirty前像，不整文件覆盖。
- **R1076 Locke** `01a1a5a9-2b51-7f12-8e44-567d76243978`：已close/archive，[RESULT](audit/r1076-genshin-foreground-output-20261104/RESULT.md)，0-byte patch、产品无改动。51780 L26681 render52060返回submitted但尚无同线程physical，随后L26948起SDK thread61148才raw Present；`PresentPermit::current_`为TLS，`PhysicalOutput::Bind`核当前线程/同source，因此异步raw hidden-only、返回处立即Hide。同步失焦时曾短暂shown，不能简单删除前台门或认定都是失焦预期。需要真实source token→SDK→异步raw→准确queue/fence/owner交接；原SL input-processing fence与numFramesActuallyPresented都不能冒exact-output source/完成。已有async marker接口只调查，未证明同局实际绑定，**NOT-IMPLEMENTED**。
- **R1078 Newton** `01a1a5ae-bbbe-7411-895b-683f6d9defa9`：已close/archive，[RESULT](audit/r1078-zzz-current-guide-clock-20261104/RESULT.md)，0-byte patch、产品无改动。当前native ZZZ的Reflex PRESENT_START仍可直接改provider `_frameCount`，guide receipt/tag、GetIndex槽、constants/SDK token随之错域。应按 `UpscaleStart→StartNewFrame→SetUpscalerGuidePair→NativeGuideSource/Complete→Execute/Signal→Reflex→OrderNativeGuides→Dispatch/SetConstants→SDK Present`闭合同一事务；source ordinal和SDK marker counter分开，serial+list+generation+epoch回到原source。不取最近marker/FIFO猜配、不重标旧pair、不删Reflex/current-pair、不扩大池/延长pause。必要扩大NGX/marker接点先交Root协调，**NOT-IMPLEMENTED**。
- R1072/R1073均已Root局部集成并随08:30:49正式部署、作者close/archive；报告原“未集成”描述是交件时状态，不改写原证据。R1072完整MOD Load、页数/VRAM/像素未运行；最坏immutable页payload从32MiB到256MiB，原512MiB preload总预算仍保持。R1073没有改shader/recipe，没有宣称三游戏滤镜像素成功。
- R1074 [DESIGN](audit/r1074-f6-effectiveness-review-20261104/DESIGN.md)为效果重评输入，worker已close/archive。继续统一真实源、保NR必要picture适配；下一片按持续输入→历史交接→真实消费闭合，停止反复只补孤立拒绝点、共用全游戏FG状态机或新增验证框架。旧DESIGN的当时进行中任务已由本交接更新。

三份候选/调查目录及R1072/R1073、R1067/R1069本波before/patch/运行证据都保护不搬，不因worker结束就叫废件。后续编码统一gpt-6.1-sol/high、fork_context=false；DeepSeek仅日志/代码行搜查，不静默换供应商，不递归委派。

## 4. 三游戏源码、consumer与坐标

以下路径均相对根 `OptiScaler/`，符号比历史行号更稳定。

|链路|真实源与消费入口|坐标/完成域|
|---|---|---|
|通用选择/生命周期|`dllmain/Config`、`inputs/NVNGX*`、`upscalers/FeatureProvider_Dx11/12`、`IFeature_Dx11wDx12`、`native/NativeDriver/VirtualUpscalerDriver`；实际所选SR factory/回退/构造，不把菜单枚举当全部后端已通|设备/resize/config/provider切换各自失效与退休。原神认证桥实际DLSS_on12限制仍按真实factory/transport，不宣称非NVIDIA/所有XeSS路线已验证|
|原神SR|`genshin/GenshinLegacyCapture`、TypedGuides、`GenshinProducerTransport`与数值packet→所选wDX12 provider→实际SR Evaluate→立即copy真实game RTV|源低分辨率/subrect/MV/jitter与输出picture分开；后续HUD reader可依赖RTV，保Wait/copy/首consumer提交顺序。只可合并已证明允许的output Flush，不重造第二桥|
|原神FG|NativeGuide/current-previous SR packet→共享 `PreparedDepthSource`/immutable packet→`genshin/GenshinFgGameMotion.h` / `GenshinFgGameMotion_Hlsl.h::NormalizeNative`→`inputs/FG/GenshinBridge_Present_Dx12`/sealed Present→`framegen/dlssg/DLSSG_Dx12`；`with_dx12/GenshinVisibleChild`、`dx11_with_dx12_sc`管理physical/owner|FG guides归一化不是NR picture映射；SDK输入退休fence、physical queue/tail和owner display提交各自证明。R1069同token Reflex与SDK-return/display-commit分层保留；当前异步显示断链未修|
|原神NR|同一PreparedDepthSource/packet→`GenshinDepthSemantic`真实clear/完整writer VP/W数值证明→必要Scene→Picture mapper→PreparedDepthLease→`GenshinPresentNr`/`NativeDriverDx11::OnPresent`→`NativeDriver/NativeProducer`→最终模型|scene资格≠picture资格；映射绑定内容/源serial/epoch/真实坐标和reader双fence。SR独立运行，FG开关不决定NR资格；当前保FinishedPicture/Present，原神低分辨率BeforeSR NR未实现，不能删`!finished`或切placement冒修复|
|星铁FG|`inputs/FG/Upscaler_Inputs_Dx12`及所选DLSS wDX12→`DLSSG_Dx12::PrepareStarRailPresent/PresentStarRailInputGap`→`hooks/FG_Hooks::FGPresent`|保用户F5-DS独立owner/provider/input-gap语义；source/generated Present不混一个源，不因游戏SR handle释放无条件重建FG池。不套原神显示认证/复杂等待|
|星铁NR|真实SR scene depth/MV→`shaders/dlssnr/DlssNr_Late.inl::Capture/ApplyToFinishedPicture`→FinishedPicture/Present→`DlssNr_Dx12`模型/DetailReuse|sceneSource与published sourceOrdinal必须按实际单步消费；星铁当前row controls在归一化前后不同，R1075使用消费端已证contract候选尚未集成；不能拿一步MV跨两步历史|
|ZZZ FG|所选SR→`Upscaler_Inputs_Dx12`/`NVNGX_DLSS_Dx12` source receipt→`ResTrack_dx12`真实Execute/Signal→F6仍称F5-owned `DLSSG_Dx12`→Present/Dispatch|FGInput=Upscaler、FGOutput=DLSSG意图不代表实际成功；不静默退游戏FG。R1058原生pause早拒绝保留，但Reflex23→4882身份断链仍未修；tag/constants/token必须同事务|
|ZZZ NR|真实当前backbuffer Late，Ready时用同次selected SR scene guides；`AcquireFreshColor`保R30 NotReady/无guide fallback|本次scene-depth/低分辨率region/reversed-Z不冒已映射picture depth；flow-only temporal可无depth。必须新鲜color，没有slot就跳过，不重放旧纹理/guide；Late不被FG active/paused无条件阻断|

`native/NativeProducer`在实际ready fence后录制并Execute/Signal，`shaders/dlssnr/DlssNr_Dx12.cpp::Dispatch`才决定最终modelDepth/region；本次读取实际 `g_nr.evaluate(...)` 在约L5788，初始化解析 `dlssnr_call_evaluate_v2`。上游输入非null、FG tag成功、预览Ready都不能替代最终模型本次实参/坐标。普通inline Before/AfterSR、FinishedPicture Late和原神Present NativeInput是不同真实callsite，不能用一个标签合并。

深度与MV的subrect、base extent、jitter、reversed-Z、row normalization/vector scale独立传递；NR picture depth不套FG取样翻行，picture-flow不重复Y反射。保未知ABI、device/queue/owner/epoch变化时拒绝；不能借上一帧source/旧相机填空。

## 5. GPU所有权、历史/cache及性能边界

- 实际源slot、SR provider、FG provider、NR picture/import/reader、SDK input tail、物理输出queue/tail和显示owner是不同对象。COM/list/allocator/texture/descriptor只有所属真实fence完成且recording replay权限关闭才可退休。`UINT64_MAX`、device loss、Execute后Signal失败都不当完成；保quarantine与完整owner，不用CPU返回、Ready或window ACK释放GPU资源。
- `DlssNr_OrderedLifetime`、recording/submission lifetime、Late slot及shared immutable packet保实际queue/fence/token。输入完成不替输出完成，已有SR成功不授NR picture mapping，FG显示许可也不授SDK退休。真实partial failure不能重试original造成第二提交。
- DetailReuse/Host/cadence/HistoryContract按同source、extent、坐标、曝光/编码域、config/Look revision、device/queue/epoch、interval/reset及实际flow/trust/latents决定；本次多帧/反馈计数不等于融合像素正确。保现有512MiB寿命/预加载相关各自预算，不能把它们混为进程总峰值上限。
- 原神picture clock、星铁source/generated clock、ZZZsource receipt分域。R1075不得将1801直接借1799，不缩小contract或伪造ordinal。完成旧executable仍需原池owner真实Reset，不能仅看fence数值丢root。
- 三游戏持续稳定、细节/遮挡/曝光/残影、FG净收益均未完整验收。pureSR60仍未达基线，不用FG显示帧数、SR单段倒数或降画质冒完成。HookCpuTiming默认关闭，CPU total/original/附加、锁/等待/Flush、队列Wait和GPU区间分别看；嵌套inclusive不可相加，Present间隔不是CPU忙时。

## 6. 滤镜、逐层Look、真实mask、GIMI、UID与UI

- 只用既有HoYoShade core和Opt内明确接口，不换ReShade core、不加second loader/side DLL/proxy。主路线BeforeSR，BeforeNR是独立后置选项；`integration/HoYoShadePolicy::Stage`的BeforeSr、BeforeNr、FinalOutput不能靠文案生成独立AfterSR。
- 原生BeforeSR：`NativeProducer::SubmitBeforeSrPrefix`（本次约L220）持真实color/input/output/depth/motion/provider，prefix Execute/Signal→同core owned scratch真实效果/copyback→原allocator/list continuation→`VirtualUpscalerDriver::BeforeSrInputMatches`及真正selected `_feature->Evaluate`。最终tail退休整个HeldFrame，中间filter tail不释放provider；外来NGX list不具有Close/Execute权限。
- wDX12 BeforeSR：`IFeature_Dx11wDx12::Evaluate`取得真实selected color/transport和原queue endpoint，`HoYoShade::RenderStage`后实际selected provider再次核Color/同次Evaluate；DX11未知reader仍保同步、原神即时gameRTV copyback不动。R1073修准备空lease经原Submit取得真实tail、不永久隔离，并要求完整selected techniques的同runtime/list/RTV有序begin/after/finish；不完整不copyback、不压掉自然效果，原scratch仍要真实退休。实际三游戏像素/净成本未知。
- 逐层Look：`dlssnr/LayerLookProfile/LayerLookConfig`→`DlssNr_Dx12`每个启用pass后Mode_LayerLook→独立scratch→下pass/最终resolve，不重复global Look。真实R8 mask/t4只在同帧、native picture坐标和真实消费者下保护本层Look前模型答案；proxy/temporal/reuse或未知映射保拒绝，Vulkan保旧global。既有mask版本Params312bytes/consumer CB512，历史308bytes不回滚现声明；本轮不生成shader。
- Look唯一recipe仍 `worktrees/r5-shader-tool-path/audit/r5-shader-tool-path-20261018/regenerate-look-shaders.py --source <根OptiScaler绝对路径>`，保现有DXIL/SPIR-V输出/reflection/offset/receipt及同套dxc/dxcompiler/dxil；该工具/旧DXC路径原位保护，不新造生成/验证管线。构建工具说明见[BUILD-ENVIRONMENT](BUILD-ENVIRONMENT.md)。
- GIMI：`mods/gimi/GimiRuntime`的Config→include/required→CompilePlan→native资格→PreloadResources→实际DX11资源/fence→CommandState/Session→exact-match draw→状态恢复/Present退休，all-or-nothing。R1072同完整immutable页/native tape的PRE/POST有限参数提交、原始binding selected callback admission已正式；页容量4096、64KiB/page最坏256MiB，原512MiB总preload仍收费完整资源/键/元数据，不能把上限当实际消耗。未知连续参数/ABI/shader/reader继续拒绝。完整用户MOD Load/服装/骨骼LOD/shadow/FSR共存及成本仍UNKNOWN，默认关闭，不per-draw IO/hash/readback/upload。
- UI mask：`ui_mask/DrawUiMask`真实draw、consumer订阅、坐标/retain分别满足FG/NR/filter，不拿scene depth/假图代替。UID `uid/DrawUid`独立默认关闭、显式按需有界内存消费；识别draw不等于glyph解码或授权，真实decoder/verifier缺失仍拒绝。无registry/cache/network/OCR/明文日志/落盘fallback。
- UI保稳定ID/INI/默认路线：调试在设置二级；Pro含既有HoYoShade/GIMI/UID与授权面板；NR仅启用层、多帧在NR子标签；FG HUD/alpha高级、帧率归FG；各向异性归SR图像。第一层未逐层时控制global，不造假独立阶段。监控可独立常开，不强开FG；无样本显示暂无数据。FSR从左端、NR从Present右端示意，长度真实耗时；派蒙文本限本次启动前120秒。UI可见不能冒真实GPU功能。

## 7. 配置、日志与后续CLI入口

正式配置根：`D:/APPS/test/TEST/HoYoShadeHub_Portable_1.4.5.0_x64_Full/OptiScaler/official/F6/profiles/`，本机实际文件为 `hk4e_cn.ini`、`hkrpg_bilibili.ini`、`nap_cn.ini`。启动投影到runtime、退出合并；保用户当前质量/倍率/SL/PTX、NR/FG/Look/MOD开关，不套PureSR预设、不整份恢复旧INI、不把根INI/cache-only当当前权威。实时内存保存仍只有产品保存设置动作，无等价CLI，不为声称已保存直接写profile。

启动器源码 `D:/CODE/HoyoDLSS5`；已存在安装根入口 `D:/APPS/test/TEST/HoYoShadeHub_Portable_1.4.5.0_x64_Full/HoYoShadeHub.exe`。本次只读确认文件存在，没有运行它或确认其启动后app选择；`app-1.4.8-local-*`多目录并存不能按mtime擅自换入口。`docs/LAUNCHER-CLI.md`保历史接口核对，其旧app-1.4.6.12/旧F5/cache/hash/备份流程不当当前命令授权。

Root现有命令族：`testgame --biz hk4e_cn|hkrpg_bilibili|nap_cn --seconds <有界秒数>`；精确停止沿 `stopgame --biz <当前biz> --expected-pid <真实PID> --expected-creation-filetime <原long值>`，不猜creation。管理员activate_game/click_game复用既有helper，真实枚举ResolveWindow，绑定PID/creation/window；actual stdout/result/windows/failure才是操作回执，不要求每次有entry-record，不加MainWindowHandle/重复foreground门禁或盲点。实际管理员入场helper接点是 `D:/CODE/HoyoDLSS5/scripts/Invoke-ExactGameEntry.ps1`（F5GameTest调用约L187，带ExpectedPid/ExpectedCreationFileTime与ResolveWindow/ExpectedWindow），复用它而不新造输入路径。相关源码接点保在 `scripts/F5GameTest.psm1`、`scripts/Invoke-F5GameTestAdmin.ps1` 和启动器实际CLI实现；旧脚本含旧F5路线/人工确认/恢复说明，**不是本次可直接自动运行的F6总入口**，未重写或执行。

Windows shell固定 `C:/Program Files/PowerShell/7/pwsh.exe`、login=false。GUI CLI沿Start-Process -WindowStyle Hidden -PassThru，有界WaitForExit，RunAs不混输出重定向、launch不无界-Wait；空LASTEXITCODE不算成功。只Root后续拥有游戏操作，精确PID+creation收尾，不全进程扫描/新kill路径，不处理登录秘密/付费；world/输入正确/视觉需另有实际证据。本轮整理不恢复任何CLI/游戏。

日志先 `D:/APPS/test/TEST/HoYoShadeHub_Portable_1.4.5.0_x64_Full/log/sessions`、`log`及active Opt，再固定audit副本。按exe、session.txt中的PID/creation/t0/配置与具体full日志行对齐。日志写official不独自证明读了哪份INI或加载内存；DLL版本、磁盘候选、启动成功、API返回、模型输入、GPU完成、实际像素与净性能逐级分开。

## 8. 实际整理数量与保留项

本次 **21次Move-Item**：15个根旧文件→`cleanup/r1077-project-cleanup-20261104/<原相对路径>`；3棵空目录脚手架`$base`（6目录）、`$cleanup`（3目录）、`R043`（1目录）→同任务原相对位置；2份活参考md→`docs/`；1棵真实F6源码→根。另建2个junction，保11份入口/文档原字节前像→`cleanup/r1077-document-before-20261104/<原相对路径>`，新增本HANDOFF。移动数量以顶层操作计，未把源码子文件算成额外move；同盘归档不是释放空间。

15项根文件：AGENTS.md.bak、CURRENT-STATE-GOALS-AGENTS.md.bak、DLSS-F5-NEXT-DELTA-REPORT-20261016.md、DLSS-F5-NEXT-REVIEW-CONCISE.md、GENSHIN-BRIDGE-MIGRATION-TARGET.md、RESHADE-COMPAT-ASSESSMENT.md、RESHADE-HOOK-CHAIN-298.md及.evidence.json、fg-auto-enable-source-map.md、ui-reactive-mask-location.md、R682-FILE-HASHES.txt、R682-MANIFEST.json、旧OptiScaler.dll、menu_intent_lifecycle_seam.obj、WcpDelta.obj。每项先确认普通文件、指定hardlink仅自身、可独占只读打开；不永久删除/覆盖。3棵脚手架实际0文件/0reparse且不是注册worktree，再整目录移走。

`docs/REGRESSION-HISTORY.md`与`docs/BRIDGE-SOURCE-REFERENCE.md`保原正文历史证据，修正相对链接并标明裸历史路径按项目根解释。F6技术文档旧连续状态追加保在完整前像，现页保全链路主体、以本HANDOFF/CURRENT覆盖旧“当前”快照。具体前像和移动理由见[R1077 PLAN](audit/r1077-project-cleanup-20261104/PLAN.md)。

原位保护：冻结Main/F5、旧Main独有文件/20个junction目标、活full-source/external、`.git`/index/config及284个注册worktree位置、既有dirty与暂存、`f6-build`、正式F6/runtime硬链接/profiles、全部R1075/1076/1078私有目录与本波回退依据、shader recipe/DXC/输出/receipt、唯一运行证据和最近rollback。最近rollback唯一绝对文件本次未查明，全部候选保留。

未知而保留：旧 `OptiScaler-f5-nextgen-ui-20261014`、大部分 `work/worktrees/audit`、`recovered-temp`、`_evidence/_nextgen_ui`、`错误/fsr-opt-black-screen-20261018`、`scripts/test-output`及旧workspace/shader检查工具。不能凭旧日期/目录名判无用，也不迁移注册worktree或删除第三方license/source说明。根Git目前不跟踪OptiScaler/external；上移后可出现untracked产品，未用ignore/stage/commit/reset隐藏或清洗历史dirty。

## 9. 下一步及所有权归还

用户本轮要求的整理/交接完成即停止，不等修复通过。下一接手顺序按R1074：优先有限审R1075真实相邻历史消费候选；再分别解决原神异步FG显示身份与ZZZ source/SDK clock，维持游戏隔离，不递归恢复旧worker或再堆管理门禁；是否重新编码/构建/CLI由Root另行安排。

本worker R1077 Hooke `01a1a5a9-2d91-7531-8854-6d3f7a69d40f`，仅亲自完成已授权整理。没有创建子worker/新chat、换模型供应商或发外部消息；没有执行新build/deploy/shader生成/tests/hash/manifest/fixture/验证器或游戏。所有文件工具同步返回，无活PTY/session/running cell、后台进程、打开文件流、构建/部署/game/GUI/GPU句柄或未完成操作。交件后停止写入，整个整理写权交回Root，由Root有限读取入口/链接/HANDOFF后关闭归档。

Root最终接收：已有限读取入口、真实目录/兼容链接及交接边界；R1077 Hooke已close并实际archive，当前无活跃子智能体。整理完成即收口，未恢复修复、构建或游戏测试。
