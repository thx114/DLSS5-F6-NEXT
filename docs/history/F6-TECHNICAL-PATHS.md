# F6 技术执行路径

2026-11-04，R1077整理。当前源码/正式版/未修问题/配置与CLI的完整交接见 [HANDOFF](HANDOFF.md)，短状态见 [CURRENT](CURRENT-STATE-GOALS-AGENTS.md)，事实归 [RECORD](audit/v3-r044-nr-off-fg-performance-20261030/RECORD.md)。本页保之前三游戏全链路分析；旧PID/行号/带日期结论是历史导航，不代表本轮通过。

## 1. 当前权威、正式版与维护边界

根 `OptiScaler/` 是唯一物理当前产品，下面裸源码路径均相对它。旧 `work/f6-source/OptiScaler` 为同源junction，MSBuild字面调用仍经 `work/f6-source`，有效 `work/f6-build` 原位；根external指既有活依赖，不能清旧full-source。默认 `BUILD-DEPLOY.ps1` 仅F6，显式Main冻结拒绝；本次改入口未运行，没有新增构建/部署/游戏。

最新正式为R1072+R1073联合版，Root根40289/cell719 compile/link/deploy actualexit0、mtimeUTC2026-11-04T08:30:49.1715003Z/34796544bytes；日志 `audit/r1073-before-sr-filter-consumption-20261104/F6-BUILD-DEPLOY-joint.log`。此前R1067 continuity/R1069 sustained等已包含；R1075候选未集成，R1076/R1078均0hunk未修。Root唯一集成/构建/部署/CLI，F5覆盖包冻结、profiles/画质/倍率/SL/PTX不变。

|游戏/消费者|真实链|最新未完边界|
|---|---|---|
|原神SR/FG|internalFSR→paired selected SR→立即gameRTV；同源PreparedDepthSource/packet→NormalizeNative→DLSSG/physical/owner|SR不依赖NR资格；FG异步SDK raw与render TLS显示许可断链，用户Win菜单才见监控USER_FAIL|
|原神NR|同源packet→完整writer数值证明→Scene→Picture→PreparedDepthLease→FinishedPicture/Present最终模型|PID51780仅3个低频depthtrue样本，不代表持续world/像素；FG有guide不授NR picture资格|
|星铁FG/NR|所选DLSS wDX12→独立adamfg式FG；真实scene depth/MV→Present NR/history|FGOk2058保留；scene1801/history1799未融合，R1075两文件私有候选未集成，不套原神复杂显示路线|
|ZZZFG/NR|所选SR→F5-owned DLSSG；current-backbuffer Late NR/R30 fresh-color独立|48964 FG0/停用113、source23→Reflex4882身份拒绝，R1078零hunk；不因缺某日志字符串判NR0|
|滤镜/Look|既有core BeforeSR→prefix/core/continuation→同次selected SR；每pass真实Look/mask|R1073完整selected录制已正式但像素/净成本未运行；原神BeforeSR NR不是该滤镜功能|
|GIMI/UID/UI|immutable预加载→精确draw/恢复/真实退休；UID独立显式按需|R1072已正式但完整MOD Load未知；默认关闭，UID decoder/verifier不冒完成，监控不等于插帧|

以下保技术主体及历史证据。既有Detailed实现/行号早于部分当前补丁时，用HANDOFF最新状态和实际源码符号判断，不把旧“当前/最新/请用户测”恢复为任务或权限。

## 2. 三游戏路径目录与矩阵

|层|原神|崩坏：星穹铁道|绝区零|
|---|---|---|---|
|精确exe|YuanShen.exe / GenshinImpact.exe|StarRail.exe|ZenlessZoneZero.exe|
|本机只读profile例|`profiles/hk4e_cn.ini`|`profiles/hkrpg_bilibili.ini`|`profiles/nap_cn.ini`|
|producer|paired IL2CPP Render + 原生FSR accumulate真实绑定/返回|游戏NGX DLSS输入，所选DLSS w/DX12|游戏所选SR实际输入与source/output receipt|
|SR实际入口|GenshinFsrDx11→FSR2_Dx11→NGX Evaluate→认证scope强制DLSS_on12（见3.1）|NVNGX_DLSS_Dx11→selected feature|按实际游戏API/所选feature；不能用profile推实际设备API|
|深度语义|真实所选t2→R32 prepared，NativeGuide；另有ReturnedRaster全writer→相机/phase→最终picture映射|游戏NGX depth/MV/jitter，独立SR_input profile投影|游戏SR guides；Ready优先，R30 NotReady fresh-color可无guide|
|跨API|GenshinDx12Transport事务/实际owner|IFeature_Dx11wDx12 + Dx11wDx12SC copy slot|若实际走w/DX12则复用原导入，否则实际DX12资源；不套原神语义|
|NR位置|FinishedPicture/Present→NativeDriver→NativeProducer→ApplyNativeInput→真实model Evaluate|Present NR / DlssNr_Late FinishedPicture|DlssNr_Late当前backbuffer；R30 no-guide fallback|
|FG guides|NativeGuide + 当前/前一SR packet→NormalizeNative→DLSSG格式depth/MV|Upscaler inputs→StarRail独立F5 DS owner/provider|FGInput=Upscaler / FGOutput=DLSSG，F5-owned provider在F6中仍沿此名|
|完成域|SR transport、mapped NR reader、native FG job、provider/output分别有真实fence|D11共享源、D12 copy/allocator、provider呈现/UI reader独立|成功source receipt、Late slot、flow/trust历史、provider output独立|
|重建边界|device/context/owner/config/history/picture与对应consumer retire|SR handle释放不无条件重建FG池；resize/device/显式关闭仍处理|NotReady不借旧DLSS纹理；source/output epoch失效与slot retire各自处理|
|最新运行边界|PID51780仅3个低频NR depthtrue样本；FG可见USER_FAIL，R1076零hunk未修|PID34648 FG2058成功，但history1801/1799；R1075私有候选未集成|PID48964 FG0/停用113，source23→Reflex4882拒绝链；R1078零hunk未修|

`upscalers/GameRoutePolicy.h::IdentifyExecutable` 是精确exe入口；`UseGenshinBridgeTransport` 还要求实际producer scope。`native/OpticalFgNrPolicy.h::OpticalFgNrEnabled` 是启动锁存且只限StarRail的独立路线，不能推广给原神/ZZZ。真实source frame、generated output和物理Present是三个时钟。

## 3. 启动、订阅、配置和保存

`Config.cpp::Config`（约L115）从实际DLL父目录读取OptiScaler.ini；`Reload`（L129）、`LoadGenshinControls`（L1382）解析当前配置，`EditGenshinControls`（L1369）发布控制变化。正式profiles由Hub启动时投影到所选OptiRuntime，游戏实际读取runtime INI；退出由Hub合并profile。不能以根INI、cache-only或formal文件的一项值直接断言当前进程实际值。候选PID90152有实际iniPath和profile activated/stored-on-exit日志，见第10节。

`Config::SaveIni`（L2619）→`PersistSettings`/`WriteSettings`（L2625/L1529）；`menu/menu_common.cpp::RenderMainMenuBottomBar`（L8616，保存按钮约L8644）实际调用SaveIni。保存成功才推进已保存快照；当前没有等价“内存保存”的CLI。StarRail `ReloadStarRailInputProfile`（L2950）/Apply（L2932）独立投影volatile `[SR_input]` 翻转，不能作为原神深度修复变量。

历史profile快照（非当前运行值）：原神Dx11Upscaler=dlss_12、NR/FinishedPicture、FG Upscaler→DLSSG；StarRail所选w/DX12路径；nap_cn的Dx11Upscaler=auto、Dx12Upscaler=dlss、NR/FinishedPicture=true、FG Upscaler→DLSSG、InterpolationCount=5。auto不是运行成功。HoYoShade=auto在候选原神启动锁存false/stage1；不能为排障强开。SL/PTX、倍率、质量及用户开关原样保留。

共享观察入口 `resource_tracking/DrawObservation_Dx11.h::Subscribe/NeedsDraw`（约L84/89）、`resource_tracking/GenericDepth_Dx11.cpp` 与 `hooks/D3D11_Hooks.cpp` 按活跃SR/FG/NR/滤镜/MOD/UID消费者早拒绝。订阅即时生效；缓存LOCATION和旧历史不算consumer。draw Before/After成对，原调用返回事实与GPU完成分开。现有HookCpuTiming默认关闭、按用户运行期开关；不每draw追加日志/IO/上传/分配。当前原神profile显式true不修改。

### 3.1 所选SR实际factory、回退和跨厂商边界

`inputs/NVNGX_DLSS_Dx11.cpp::NVSDK_NGX_D3D11_CreateFeature`约L622先读Dx11Upscaler，但 **`GenshinSrReceipt::active`时L635显式改为DLSS_on12**，并以`preserveDx12Fallback=!active`调用`FeatureProvider_Dx11::GetFeature`。因此当前原神认证桥不是“UI任选所有SR后端都同等接入”；dlss_12本机配置与该强制路径一致，不能据此宣称XeSS/FSR/非NVIDIA原神整栈已通。

`upscalers/FeatureProvider_Dx11.cpp::{GetFeature,ChangeFeature}`与`.h::FallbackUpscaler`实际构造DLSS、DLSS_on12、XeSS/XeSS_on12、FSR2/FSR2_on12/2.1、FSR31、FFX_on12；GPU能力/模块不可用走原fallback，显式普通on12可保FSR22_on12，Genshin active不借fallback授DLSS-specific receipt。模块构造后实际ModuleLoaded检查及config active choice更新、changeBackend分阶段retire/construct/init均有接线，不能绕实际producer能力静默恢复历史供应商路线。

`FeatureProvider_Dx12.cpp::GetFeature/ChangeFeature`构造DLSS/DLSSD、XeSS、FSR2.1/2.2、FFX；真实模型调用分别在`dlss/DLSSFeature_Dx12.cpp::EvaluateInternal`（L82，NGX call L105）、`xess/XeSSFeature_Dx12.cpp::EvaluateInternal`、`fsr2/FSR2Feature_Dx12.cpp::EvaluateInternal`。DX12模块失败分支构造FSR2.1后直接`loaded=true`、没有再次ModuleLoaded，是当前factory的明确实现假设，未证明为本轮运行故障；不应称“所有后端成功”。`FeatureProvider_Vk`单独选型，新增NR深度/多帧/逐层Look只DX12，Vulkan不借支持。

FG输出不是跟SR factory同一枚举：`hooks/FG_Hooks.cpp`创建swapchain路径约L579/719按activeFgOutput构造`FSRFG_Dx12`、`XeFG_Dx12`或`DLSSG_Dx12`；各provider拥有SDK context、实际swapchain/queue和retire。`framegen/ffx/FSRFG_Dx12.cpp::{CreateSwapchain,ReleaseObjects}`、`framegen/xefg/XeFG_Dx12.cpp::{CreateSwapchainContext,CreateSwapchain,ReleaseObjects}`是真实接点；XeFG从真实SDK属性取maxSupportedInterpolations，不把UI请求当支持。三游戏当前Upscaler→DLSSG不证明另外provider、XeSS多帧、generated-child映射均已验收。`native/NativeDriver.cpp::VirtualUpscalerRequested`另受NativeMotion/NativeUpscaler等开关限制，OpticalFgNr还有精确StarRail startup latch；不能将虚拟feature存在冒全游戏能力。

## 4. 原神：原桥、SR与实际资源

### 4.1 paired Render到selected SR及copyback

|入口/源码|真实输入→输出与约束|
|---|---|
|`genshin/legacy/Il2CppCallSiteHook.cpp::on_render_enter` L90；`GenshinBridgeRecipes.h`|paired instance/context/generation/frame/render/display/jitter；方法pattern/序言/layout独立验证，旧RVA不能兜底。|
|`genshin/GenshinLegacyCapture.cpp::ReadBindings`；`GenshinNativeMrtContract.h::Validate/ApplyProfile`|accumulate PS真实t0 color、t2 depth、t3 motion、RTV1所选输出、companion RTV0及CB布局。尺寸/格式/mip/device/token匹配。当前固定槽比原bridge动态资源识别更窄。|
|`PrepareShader` L440；`CompleteNativeReplacement` L1508起|真实深度提取到R32_FLOAT，motion为R16G16_FLOAT；`-sign(d)*(abs(d)*2)^2`、中心127/255，depth/MV Y flip独立。Render jitter及真实SR output-grid phase不由纹理尺寸推断。|
|`CapturePipeline::BeforeDraw/AfterOriginalDraw/CompleteNativeReplacement`|optional-guide常用路线先让原native draw返回，保companion MRT；SR-only前置替换有独立优化资格，不能一概互换。`Evidence::Associate` 在prepared scratch前抓原selected source内容。|
|`inputs/GenshinFsrDx11.cpp::Dx11Owner::Submit`→`inputs/FSR2_Dx11.cpp`→`NVSDK_NGX_D3D11_EvaluateFeature`|复用实际所选feature；不另建第二SR/加载器。|
|`upscalers/IFeature_Dx11wDx12.cpp::Evaluate` L691，producer Scope约L705，selected `EvaluateForRecordingSlot` 约L1060|Genshin专属transport与普通w/DX12分支分离；SR可单独成功，optional深度/相机失证不能阻断SR。|
|`GenshinLegacyCapture.cpp::CompleteNativeReplacement` 约L1766|私有SR输出立即CopySubresourceRegion到真实game RTV/subresource，后续HUD有reader，不能延到Present。|

只读参考 `D:/CODE/genshin_fsr_brigde` 当前本地dirty：`Dx11FsrBridge.cpp::try_fsr2_translation_draw` 构造真实FrameInput、paired jitter/MV后调用 `ffx12::dispatch`（当前约L11687）；`Ffx12Backend.cpp::dispatch`（L2703）、depth/motion提取shader（L379/L403）、pending/finish_pending。它是参考，不是当前F6所链接的另一个桥。`BRIDGE-SOURCE-REFERENCE.md` 的上游固定提交与旧O根是历史导航；本次没有请求远端latest或执行包内DLL。本地bridge的DSV fallback、jitter定制、异步pending不能整段搬到F6，尤其不能覆盖当前所选资源和即时game RTV写回。

### 4.2 GPU transport、完成和重建

`genshin/GenshinDx12Transport.cpp::Lease` 的input Signal(D11)/Flush→D12 queue Wait（约L734）→Execute（L768）→输出fence→Restored/Copyback/Poll构成同一事务。保实际设备、队列、原/规范COM身份转换及escaped packet roots。CPU Evaluate返回不等于GPU完成；Execute后Signal失败必须quarantine并持有真实命令/资源owner，不能把failure当未提交后释放。

池仅复用真实已retire且未escaped的slot。device/context、producer/config epoch、extent/format或合法关闭使对应新claim失效；旧reader仍沿原fence退休。停止/重建是guarded/idempotent；UINT64_MAX不是完成。`GenshinProducerTransport.h` 的QueuedTransfer/CompletedTransfer是不同资格，不能互相改名。

## 5. 原神：FG能拿到什么，NR为什么仍可能没有深度

这不是“GPU没有任何深度”。当前代码可把同一真实SR深度packet成功交给FG，同时因缺少最终picture坐标映射而不给NR模型depth；候选PID90152正有这一类模型输入。FG成功本身也必须有当局provider/提交证据，本次未把用户所述FG表现强行绑定该PID。

### 5.1 FG实际读源、归一化和provider资源

1. `GenshinTypedGuides.cpp::Receipt::NativeGuideForPresent`（L188）从当前原生accumulate、真实相机、selected SR grid和 `GameMotionReferenceForPresent` 生成NativeGuideBundle，provenance=`ActualNativeAccumulate`。它不调用 `DepthFactsForPresent` 的全writer解析。`GenshinGameMotionReference.h::NativeReference` 绑定实际CB28、revision/loss、Render相位与输出history UV；这是native consumer ABI证明，不是最终picture的UV证据。
2. `GenshinProducerTransport.h::ProducerFrame::PreparedDepth`（L639）以同一raw_[1] R32 prepared source、owner和lease建立nativeTransfer/nativeLease（约L674–701）。FG/NR联合路线用真实QueuedTransfer并在读取context/queue上排依赖；NR-only保CompletedDepth/ProducerReady，不宣称CPU已完成。
3. `ProducerFrame::PrepareGameMotion`→`GenshinFgGameMotion.h::Lease::Prepare` 消费当前及直接前一SR事务 **transport-owned Prepared(1)深度与Prepared(2) motion**，不用下一SR会覆盖的scratch。当前/前一packet必须不同且owner、generation、camera/grid连续。`GenshinFgQueryIsolation::PacketPair/PacketLane` 持实际packet和队列，隔离NormalizeNative作业；不升级游戏context的UnknownPast query资格。
4. `GenshinFgGameMotion_Hlsl.h::NormalizeNative`：display-resolution `GameDepth` R32、`GameMotion` RGBA；从native producer grid做五点最近深度donor，按clear/reversed约定过滤。**FG输出行坐标对native取样做Y反射**，MV转picture pixels并负Y；history采样为`previousNative=nativeUv+nativeMotion`，不以静态相机Z淘汰骨骼运动。输出深度已是donor膨胀结果，缺失motion用65504 sentinel。
5. `native/NativeDriver.cpp::captureGameMotion`（约L379–406）ImportReady/BeginRead后构造独立`fgFrame`，明确 `fgFrame.depth=motion->Depth()`、`motion=motion->Motion()`；**没有修改NR传入的frame.depth**。
6. `inputs/FG/GenshinBridge_Present_Dx12.h::Recording::Capture`（约L667–754）核当前FinishedPicture ticket、相机、consumer list/queue及同一gameMotion lease，把上述资源交给DLSSG tags/provider；`s.sourceDepth`和`s.depth`都来自该fgFrame。`CaptureGuidePreview`（约L1247调用）快照的是该FG输入depth/MV，不是NR模型depth。provider提交、present/copyout与packet reader分别退休。

### 5.2 NR实际读源、最终picture映射与null-depth分支

`GenshinLegacyCapture.cpp::TakePresentNr`（L2138，公开入口约L3381）取得实际final picture及selected SR对应关系，调用同一个`PreparedDepth`。已有共享源和共享完成机制，不需要重建第二套深度采集。

映射链：`GenshinReturnedRaster.h::Evidence::Associate`→`GenshinTypedGuides.cpp::DepthFactsForPresent`→`GenshinDepthSemantic::Resolve`→`GenshinDepthMapping.h::Capture::Resolve`→`PreparedDepthLease`。Scene/Picture真实draw replay建立UV，ResolveDepth输出 **RGBA32_FLOAT，x=DeviceZ、y=scene-valid**；clear/HUD/sky/越界/非有限值保无效，不把“纹理覆盖整个屏幕”当有深度。`Ticket::DepthIdentityMatches/DepthMatches`（PresentNr.h约L279/293）同时核selected output、backbuffer/pictureSerial、source frame、history/owner；`SelectFgPicture`（L343）保`!finished`拒绝。

`native/Dx11FrameSource.cpp` 约L287–355从mapped lease真实Ready/BeginReader/CopyResource→共享D12；约L412发布DepthCoordinates::Picture及paired-validity。如果只有FG可用native packet，则L423–434保留R32原producer资源、nativeLease、真实reader完成，明确 **DepthCoordinates::UnmappedProducer**。`native/NativeProducer.cpp` L419/483/489仅将Picture guides交TrustMask/NR；UnmappedProducer会报告`producer-depth-without-picture-uv-mapping`，不会把扩大后的宽高当最终picture。

因此“FG拿到depth、NR modelDepth为空”在当前代码中有明确可定位的分叉：**NativeDriver把NormalizeNative输出只赋给fgFrame；NativeProducer不把UnmappedProducer送进NR。** 这不是丢了一次普通指针赋值。

### 5.3 可共用与不能直接共用的真实原因

|可以复用/应统一|不能用来替代的内容|
|---|---|
|同一selected source/原生accumulate、prepared packet、owner/epochs、相机/Render、SR grid、CB28相位、真实提交/完成依赖|NativeGuide的output-history UV不自动证明之后Scene/Picture合成、HUD遮挡和backbuffer对应|
|QueuedTransfer/CompletedTransfer、现有NativeQueuedDepthLease、reader退休协议|某FG作业的fence完成不代表NR reader已完成；两个消费者要各自登记完成并共同延迟pool复用|
|已有可信坐标/相机数值事实，以带来源的共同证明传递|`NumericalMaximumWins`仅为reviewed accumulate donor数值域，不等于forward/reversed DeviceZ或clear/投影矩阵|
|原prepared深度作为mapping唯一采样源；每个picture只做一次现有映射|FG的五点最近donor R32及其垂直基准不等于NR的逐pixel picture DeviceZ+validity，边缘、HUD/天空、phase不能丢|
|Evaluate和preview共用最终实际guide receipt|FG debug显示成功不能证明NR model guide存在；共享源不要求共享同一格式/分辨率输出texture|

C已沿同一Receipt实现producer事实复用：`GenshinTypedGuides.cpp::NativeGuideForPresent` L191缓存一次成功的不可变NativeGuide；先核线程/retire，再委托parent或取cache。`DepthFactsForPresent` L233复用同source/camera/grid并核冲突；native guide缺失时独立用原OutputGrid，不依赖FG开关。`GenshinDepthMapping.h::Capture::Resolve` L621对同owner/picture/source/content/mutation返回同一不可变mapped结果，consumer lease仍分别登记实际完成。未缓存会反向拥有ProducerFrame的强depth receipt，避免生命周期环。最终RGBA picture映射与FG donor R32的坐标/完成域继续分离；详见第6节和交件RESULT。

## 6. 原神ReturnedRaster/语义/映射：前像与隔离候选

### 6.1 B/S前像限制及证据边界

规划前像有4个track、8个弱Evidence lease及既有byte retention。Snapshot拒绝GS/HS/DS，VS/PS各14 CB；Snapshot失败清整个interval；semanticWriters有1024硬界。Semantic只解析VS终端，ReplayState只保14 VS CB，Mapping只校VS→PS。旧R855报告和source.diff不是当前支持或运行证明，候选没有整文件回放旧片。

depth6 `Submission::{Unmap,Returned}` 真实sub0原Unmap返回后恢复mappingKnown/mapped、先回写shadow及owner/thread/loss/revision约束保留。PID90152原selected source首个Snapshot分支仍UNKNOWN，不能反推是1024或HS/DS；后段query reason16也不能删门绕过。正式USER_FAIL与第10节原同局证据不变。

### 6.2 C交件时的完整受支持路径（后已随R1028集成部署）

|C文件/稳定符号和本次行号|实现及边界|
|---|---|
|`GenshinTypedGuides.h::DepthSource` L40；`GenshinReturnedRaster.h::Refusal` L42 / `DescribeSelected` L918 / `Evidence::Associate` L932|source/origin/prepared/copy ancestry、资源allocation stamp、loss/content/clear/return、total/covered/unresolved和第一拒绝跟同一receipt走。ObservedContent与全writer资格分离；Snapshot失败不抹掉最初内容，后续好writer不能复活未证interval，真实clear才重开。claim原因单列，不覆盖第一内容拒绝。|
|`GenshinReturnedRaster.h::SnapshotCandidate/Snapshot` L399/L486；`BeforeRaster/RasterReturned` L799/L857；`CopySubmission` L555|同次VS/PS/HS/DS对象、stage组合、topology、全56 CB绑定/range、原IA/RS/DSV配对；每阶段一次14槽getter，VS-only仍两批，其他游戏不增加56个逐槽COM。每个返回writer恰入covered或unresolved。whole-copy返回传播有效interval；失证copy只传观测首因，绝不授GPU/语义资格；region写仍撤销完整证明。|
|`GenshinDepthSemantic.h::Equivalent/WriterGroups` L669/L685；`Resolve` L1290|相邻精确语义等价组+checked multiplicity，无1024总writer硬界。键含终端program/真实stage/PS、owner/epoch/thread/loss、compare/viewport、instance域、全部实际依赖CB对象/range/revision/immutable payload；几何XYZ只因原Resolve对任意XYZ通用证明而不入数值键，同次原返回仍逐draw严格配对。组数组容量与distinct Draw元数据计原16MiB Payload/4096登记；64MiB Buffer预算不变。共享immutable storage、写时分离，不扫历史writer组求合并；总数须与covered一致，pending不借后来的CB。|
|`hooks/D3D11_Hooks.cpp::hkCreateHullShader/hkCreateDomainShader` L215/L223，`HookGenshinTessellationShaders` L231；`GenshinDepthEvidence.h::ReadShaderBytes/TessellationSignature` L153/L181|实际SDK slot16/17、原调用先行、成功对象才保实际创建bytes，复用Own Batch/AddMethod及teardown。精确原神进程只启观察，producer资格仍须实际nativeDevice/scope/epochs。缓存原字节预算不变；不靠MOD开关。VS→HS、patch constant、HS→DS、domain及输入/输出control-point数按真实reflection校验。|
|`GenshinDepthSemantic.h::Stage/ProgramFailure/Analyze` L109/L110/L339，`Capture` L916|VS-only终端保原；成对HS/DS以DS最终SV_Position做数值解析。真实ds_5_0、常量索引control-point/patch/domain输入保持symbolic；未知不置零。DS终端14 CB+VS上游14+HS上游14共42依赖都保同call payload/revision/range/retention，并在Returned/Resolve核验。GS、class linkage、非CB pre-raster依赖及未证明dataflow精确拒绝。|
|`GenshinDepthReplayState.h::Raster::{CaptureCandidate,Returned,Preflight,Materialize,Apply}` L260/L365/L383/L408/L429|完整VS/HS/DS、42实际CB/first/count/revision/payload，保geometry mutation watch与原query/deferred/owner/loss。Materialize只用原immutable bytes，Apply真实stage/topology，不卸tessellation或替换几何。Buffers metadata容量扩至完整集合，原字节预算不提高。|
|`GenshinDepthMapping.h::Signature/Replay/Resolve` L332/L366/L621|实际终端→现有生成PS签名；cache为强VS/HS/DS对象+topology+recipe，8项，避免地址重用和跨DS借PASS。replay真实stage/42 CB持到原mappingDone/reader退休，96 raster-root边界保留。KnownQuiescent/PrivateWork、mutation/picture epochs不松；同owner/picture/prepared source/content/version只映射一次，其他身份拒绝。RGBA x=DeviceZ/y=validity语义及原shader不变。|
|`GenshinProducerTransport.h::ProducerFrame::PreparedDepth` L641；`NativeDriver.cpp` L453；`NrInputObservation.h::SourceReceipt` L77|沿原native packet和mapped lease：联合路线OrderMappingRead，NR-only CompletedDepth/ProducerReady，独立reader保原Acquire/Wait→model→Signal/copyback。source收据跟本次mapped/imported/model资源传递；FG的NormalizeNative donor仍仅fgFrame使用，不替NR。只扩既有Present publication，非逐draw采集。|

资源allocation stamp仅在真实clear/full-copy返回赋值、copy/selected边界读取，随D3D对象销毁；墓碑必须匹配非零stamp，地址重用不带回旧内容。首拒绝中的shader地址是历史观测标签，不解引用、不授生命周期/执行资格；实际可消费shader与GPU资源仍由原强根及完成门保护。

### 6.3 仍真实不支持或UNKNOWN

支持域是CB-only pre-raster、真实配对HS/DS及现有有界直线DS表达式，不是任意曲面细分。GS、dynamic class linkage、pre-raster SRV/UAV/sampler、未知控制流/动态control-point索引/未知opcode、无法证明的camera matrix或W仍拒绝；不能猜常量/ABI。现有2048指令/8192节点、64KiB创建bytes、有限program cache仍是精确边界。实际PID90152 shader是否在此域、首个query/CB/阶段依赖是什么仍UNKNOWN。

活跃/未知/unhooked query继续禁止PrivateWork；已有deferred只有原picture/owner/version、geometry与immutable CB都完整时才可Flush，不另建隔离replay管线。4 tracks、8弱lease、96 mapping roots、原buffer/payload预算可能精确拒绝，不承诺所有场景可用。该候选没有运行样本，不宣称modelDepth或像素已修复。

第7/8节崩铁/ZZZ核心文件未改：普通w/DX12/NGX、StarRail F5 DS owner/slot真实等待/Present NR/池重建，ZZZ Ready优先/R30 fresh-color无guide、source/flow/trust/reset及F5-owned Upscaler→DLSSG均保原。共享hook只增原神shader创建观察与现有失效操作名；共享model只增现有收据字段，不新增全局depth必需条件。三游戏新运行/性能验收均UNKNOWN。

## 7. 星穹铁道：独立producer、Present NR、FG与寿命

`inputs/NVNGX_DLSS_Dx11.cpp::{CreateFeature,ReleaseFeature,Evaluate}`（约L575/704/802；selected Evaluate约L922）接真实游戏NGX。`IFeature_Dx11wDx12::Evaluate`非Genshin分支使用独立资源导入和selected feature；约L1032捕获FinishedPicture桥，不能把Genshin的Render/ReturnedRaster作为它的输入资格。

`inputs/FG/Upscaler_Inputs_Dx11wDx12.cpp::UpscaleStart/UpscaleEnd`（L72起）从NGX和已支持profile取得真实camera flags/MV/jitter；同frame ID可复用实际prepared cache，否则PrepareFgResourceCache。`with_dx12/dx11_with_dx12_sc.cpp` 的D11共享color在覆写前必须等待实际reader：StarRail `_sourceFrameLatencyOwner` 路线约L2035用`_WaitForCopyAllocator(copySlot,_copyAllocatorFenceValues)`。这是实际共享color-slot hazard，不是看到allocator数组就可任意删等；Genshin native parent的poll-only CopySlotBusy是另一条语义。

共享color交接为D11 Signal/Flush→D12 queue Wait→copy→Signal。copy queue、selected SR queue、DLSSG provider/实际present queue及UI reader不能混作一个owner。`hooks/FG_Hooks.cpp::FGPresent`（约L1700）按精确StarRail + Upscaler/DLSSG/wDX12路线调用PrepareStarRailPresent（L1820），在真实source上FinishedPicture NR（约L1900），同source的generated Present不重复运行NR。

`framegen/dlssg/DLSSG_Dx12.cpp::{CanRetainForInputGap,IsStarRailContinuousOwner,IsStarRailF5DsOwner,PresentStarRailInputGap,SuppressStarRailGap}`（约L387/545/588/603）保独立F5 DS实际行为：输入空窗可Off并retain provider，不把游戏SR handle释放映射成FG池重建；`WaitForStarRailF5DsUIAllocator`（L793）及真实slot fence保护旧UI/reader。partial Execute/Signal失败持有实际owner/quarantine。

`shaders/dlssnr/DlssNr_Late.inl::Capture/ApplyToFinishedPicture`（约L695/1200起）管理StarRail guide snapshot/source identity/当前backbuffer，slot含实际producer list、queue、ready/done fence/source epoch。合法resize/device/config/用户关闭推进独立重建，旧in-flight尾部须完成。卡顿根因不能从SDK500ms等待、池常驻或Present间隔直接推断。本计划不再改变SL版本、PTX或200fps变量；崩铁本次新F6实际性能/持续FG验收UNKNOWN。

## 8. 绝区零：所选SR、R30 Late与F5-owned FG

历史实测2026-11-04：session20261104-072622_ZenlessZoneZero_20832/t0北京07:26:21.495/退出0，用户报告NR无深度USER_FAIL。完整Opt见 `audit/r1030-zzz-depth-20261104/incoming/pid20832-OptiScaler-full.log`：L37370/L65443准备hasDepth=true，L37421/L65480实际picture-flow变false；L80325/81149/81956非raw深度预览source/modelDepth=0（后两未复用），10条cadence producerDepthGuides=false，FG仍持续Depth tag eOk。实际SR1280×800→2560×1600。R1030已确认Late原L1699在ZZZ flow分支无条件清modelDepth和depth base，直接到实际Dispatch/preview；不是原神mapping同因。Root有限审4文件9hunk后准确before局部集成，当前模型保本次SR scene-depth及其低分辨率subrect/reversed-Z，flow-only temporal Frame仍为空depth；10标量模型depth契约参与原HistoryContract精确相等。新标志仅ZZZ写入，不授picture-depth复用资格；R30无guide fallback及真实queue/fence/retire不变。末段interval1缓存slotBusy/feedback关闭仍独立未修。唯一正常根cell23已实际compile/link+正式原位部署exit0，mtimeUTC=2026-11-03T23:48:03.7327766Z；后PID27300用户确认输入深度可见，方向UNKNOWN、细节闪烁仍USER_FAIL，R1032续修。遮挡/HUD/净成本仍待验收；不授已映射picture-depth能力。Plato已关闭归档。

本机nap_cn选择Dx12Upscaler=dlss，FG Upscaler→DLSSG；实际运行API、source及队列须由同PID日志确认，不能仅凭配置把全部ZZZ当native DX12。游戏实际SR输入经现有selected feature和Upscaler inputs交FG；F6继续持有F5-owned provider，不静默回退游戏FG，不从StarRail gap逻辑借资格。

`DlssNr_Late.inl::IsZzz/ApplyToFinishedPicture`（L163/约L1337邻近）独立判断游戏；Ready时优先真实DLSS captured guides。R30 `AcquireFreshColor`（L509）在NotReady/资源移除时只取 **当前backbuffer新鲜color**，poll-only取合法slot，无guide也可NR；没有新鲜slot就安全跳过，绝不重放旧DLSS纹理/guide。该fallback不适用于Genshin也不应被“NR必须depth”共享改动阻断。

成功source Begin/End receipt（Late约L836起）绑定真实output/source attempt/list，决定新source及history资格。R1013/R1016的source interval逐跳flow/trust、latent/confidence及支持域失效处理已接源码/正式部署；generated child对应仍UNKNOWN。不能以模型完成、请求interval8或计数就声称多帧反馈成功。重建分别处理source epoch、backbuffer尺寸格式、Late capture slot、NR history及provider slot，不把source失败当已完成retire。

## 9. 共享NR模型、历史、真实调试、滤镜和界面

### 9.1 真正的模型入口和历史

`native/NativeDriverDx11.cpp::OnPresent`（L109起）→TakePresentNr→GenshinPresentNr::Scope→`NativeDriver::RunFrame`（L121）。`NativeProducer::Run`（L227）等待实际ready fence，核设备/queue domain与allocator completion，录制flow/trust/model/copyback，Execute+Signal；失败/未知完成不释放实际reader。`DlssNr_OrderedLifetime.h`与recording/submission lifetimes保有实际fence token；CPU返回不是retire。

`NativeDriver.cpp`约L456调用`DlssNr::ApplyNativeInput`，随后 `shaders/dlssnr/DlssNr_Dx12.cpp::Dispatch` 处理input/guide region、working scale、model尺寸、compression与每pass资源。`DlssNr_GuideCoordinates::Resolve`分开depth和MV subrect，不重复Y flip：picture depth不套FG取样行反射，picture-flow不再负MV Y。

真正模型函数是初始化解析的 `dlssnr_call_evaluate_v2`（Dx12.cpp约L785），实际 `g_nr.evaluate` 调用约L5528–5536，传`passInput, modelDepth, spatial?modelMotion:reusePlan.motion`及真实offset/extent、inverted/reset。ApplyNativeInput的非null不等于最后modelDepth非null，需看compression/可读region及该callsite。

历史/cache按真实source、extent、epoch、interval、reset、flow/trust/latents资格；R1025 interval1/spread1双链互不反馈混合结果，最终显示blend，stable full-grid dispatch由3到5，净GPU成本UNKNOWN；512MiB硬寿命预算保留。ZZZ source receipt、原神picture clock、StarRailsource与generated clock不能共用一份连续性判断。

### 9.1.1 真正AfterSR与FinishedPicture不是同一入口

`IFeature_Dx11wDx12.cpp`约L1032计算bridgeFinishedNr=FinishedPicture且非BeforeSR/非Deferred。普通w/DX12路线若finished则先`CaptureFinishedPictureDx11Bridge`保存实际guide/producer收据，再在Present由Late消费最终picture；非finished才走`EvaluateBeforeUpscale/EvaluateAfterUpscale`的实际inline位置。

Genshin另有确实接线的AfterSR：约L1047 `GenshinCaller::BridgeAfterUpscale::Observe`环绕真实selected provider，约L1112–1133在成功Evaluate且非FinishedPicture/非BeforeSR/非Deferred时`OfferNr`→`DlssNr::EvaluateAfterUpscale`。若只完成private recording-slot wait，进入共享NR helper前必须升级原shared recording wait。这是另一个authenticated inline NR生命周期，不能以“AfterSR”标签替代它的真实资源/返回证明。

`DlssNr_Dx12.cpp::EvaluateAfterUpscale/EvaluateBeforeUpscale`（L7003/L7010）与Late.inl的`ApplyToFinishedPicture`（L1894）、`CaptureFinishedPictureDx11Bridge`（L1901）、`ApplyNativeInput`（L1983）是不同callsite。当前正式原神profile为FinishedPicture=true，候选局实际走Present NativeInput；本次修复不得改profile让它绕到inline、删除`!finished`限制或把inline成功冒最终画面支持。HoYoShade的BeforeSr滤镜也不是NR的BeforeSR placement，两个开关/consumer不能混为一谈。

### 9.2 NR/FG调试究竟看哪个resource

Dx12.cpp约L5349在Evaluate前准备GuidePreview私有snapshot：mode1=目标/编码前color，mode2=modelInput，mode3=`raw?rawDepthIn:modelDepth`，mode4=`raw?rawMotion:实际reusePlan.motion`；非raw按本次Evaluate guide region，raw明示源基准。无guide显示明确unavailable/checkerboard，不偷用上一帧；fresh-color fallback的guide模式不伪造。`DlssNr_GuidePreview.h`与独立DebugView0..3不是一个功能。

所以NR非raw depth预览应与同次modelDepth及region来自同一receipt，允许私有copy导致texture指针不同但copy来源/serial必须一致。FG `GenshinFgPreview`显示provider使用的NormalizeNative guides，不能用它证明NR modelDepth。ZZZ历史已有“非线性Z看似全黑”的显示问题，但候选原神三条模型call确为空，不是仅调试对比度。渲染预览正确也不证明最终模型像素或性能正确。C的 `NativeDriver.cpp` L453在现有同步NativeInput scope发布SourceReceipt；Dx12.cpp L5434既有preview记录增加generation/sourceFrame/picture/mapping_serial/content_version/selected_source/model_depth_resource及preview_is_model_depth，L5551既有R849实际Evaluate记录增加selected→origin→prepared→mapped→imported身份与coverage。非raw仍按本次 `modelDepth` 复制，缺失保持unavailable；重用frame仍标would-evaluate，不冒真实模型执行。原g_nr.evaluate算法、region选择、压缩和preview shader未改。

### 9.3 HoYoShade实际阶段、消费者和逐层Look

`integration/HoYoShadePolicy.h::Stage`仅三种：BeforeSr=1、BeforeNr=2、FinalOutput=3。没有独立通用AfterSR枚举；UI“SR后/NR前”必须落到真实BeforeNr endpoint，不以标签冒新阶段。

|阶段/入口|资源及实际消费|寿命/未完成边界|
|---|---|---|
|BeforeSR：`IFeature_Dx11wDx12.cpp` L976–1028→`HoYoShade::RenderStage`|真实SR input color经既有owned runtime处理，随后同次selected SR Evaluate消费；Genshin `BeforeSrNativeColor`和typed producer/transport，独立selection donor|`HoYoShadeOwnedStage/OwnedRuntime`保实际device/list/queue/submission；EffectsExecuted/Submitted与最终被SR消费分别记录。当前候选latched false，不能称本局滤镜成功。|
|BeforeNR：`NativeDriver.cpp` L294–348；`native/NrConsumerFilter.h`；Late约L1506|实际FinishedPicture color/depth lease/ui mask，经filter后真实NR输入；`BeforeNrNativeColor`独立于BeforeSR四资源资格|缺合法depth可按现有明确color-only能力拒绝或降级，不能凭普通runtime存在授typed深度。|
|FinalOutput：`framegen/FinalOutputEndpoint.cpp::Endpoint::Present` L442，RenderStage L538；`wrapped/wrapped_swapchain.cpp`约L1459|F5-owned provider物理输出创建scope/真实DIRECT queue/当前backbuffer token；Source/Output runtime分域。Genshin CopiedSourceScope必须真实copy+Wait+FinishedPicture后才成立|prepare/capture/binding、Present/resize/tail completion各有owner；原始HRESULT/flags保持，部分提交后不能再假装untouched。未知厂商/外来hook输出不冒支持。|

`HoYoShade.h::ColorEndpoint/RenderStage`（L176/220）与`HoYoShade.cpp::RenderStage`（L4167）是统一接口；只用已存在HoYoShade core/明确接口，无第二核心或side DLL。自然runtime选择、stage generation、typed owner、color domain、真实techniques提交不同事实不能互授。BeforeSR是用户主路线，BeforeNR是独立后置选项；HoYoShade默认不强开。

逐层Look实际为 `dlssnr/LayerLookProfile.h` / `LayerLookConfig.h::{Load,Save}` 的每Pass配置；DX12 Dx12.cpp约L5600–5610每个启用pass后真实Mode_LayerLook dispatch，FillFinal（L5830）避免重复global。`menu/NextgenNrLayers.h`按VisiblePassCount和启用层显示；未逐层模式第一层仍控制global，不能新增假独立全局入口。Vulkan保旧global，不宣称depth/多帧/逐层支持。逐层像素验收UNKNOWN。

### 9.4 真实mask、GIMI、UID

`ui_mask/DrawUiMask.cpp::{Configure,Present,Acquire,Selected,Begin,End,Observe}`（L287/301/339/345/349/358/379）观察真实draw并为FG/NR/filter单独订阅/消费，绑定真实坐标/retain；不是用深度或假图冒UI mask。真实遮罩视觉/全部游戏覆盖UNKNOWN。

GIMI `mods/gimi/GimiRuntime.cpp::{Enabled,Present,NativePresentScope}`（L2096/2950/3023/3178）沿Load→parse/include/required graph→CompilePlan→PreloadResources→CreateDx11Resources→immutable执行→Present fence/retire。默认关闭，bounded single-part/exact-match，实际resource/shader分类与真实draw匹配，不per-draw IO/hash/readback/upload。CPU runtime约128MiB、texture约256MiB、16 flights/8 retired等原有预算与GPU完成门保留；这些是具体子池限制，不互相替代。

当前R1072已随08:30:49联合正式部署：GimiCommands/Resources/IniParams/FormatText/Runtime/Executor同完整immutable页和native tape闭合有限PRE/POST参数交易，selected callback按同original binding/reached PRE授权。完整所需图与连续unknown输入仍严格证明，原proof budget保留；页容量4096，每页4096float4/64KiB，最坏payload256MiB，实际按proved states预加载并计原512MiB总预算，非预创建最大页数。旧followup8的512页/32MiB是历史状态，原证据保留，不回滚R1072。完整MOD Load、实际页数/VRAM/像素/净成本仍UNKNOWN；详见R1072 RESULT及HANDOFF。

`GimiCommands.cpp`真实committed page→有序action，`GimiDx11Executor`整tape预核/VS-PS t120绑定及恢复；`GimiNativeOverlay.cpp`真实z=0/w=1/x70=1或2与Draw(3,0) reader。Session成功提交才commit，保Dx11StaticResources完整COM集合到真实Present Signal/fence；Signal失败/device loss/UINT64_MAX仍quarantine。证据 `audit/r1022-f6-gimi-resume-20261103/followup8/RESULT.md`；该报告私有未集成状态已由Root本次集成回执更新，完整Load、页数及像素仍UNKNOWN。骨骼/LOD/shadow/FSR共存不能由静态required闭包证明。

`uid/DrawUid.h`是独立默认关闭的显式请求、精确draw/有界内存采集（最多4096 bytes、120 Presents/10秒）。**尚无被证明的glyph decoder，Snapshot.hasUid并未成为真实解码结果。** 识别draw不等于取得UID或授权。无registry/cache/network/OCR/明文日志/落盘fallback，付费gate无真实verifier应拒绝；本文件不记录任何UID、authKey或带凭据地址。

### 9.5 监控、采样与UI未完边界

`menu/menu_common.cpp`约L2420图表及L2545 tooltip：FSR从左端起画，NR从当前Present条右端起画，长度用真实异步GPU样本，横向锚点是示意，不是同帧GPU起止；不能把不同帧/嵌套inclusive相加。无样本显示暂无数据，监控独立于FG/NR开启。HookCpuTiming页约L8328读取既有采样，区分总调用/original/附加CPU、锁/等待/Flush/GPU；Present间隔不等于CPU忙时。

菜单保稳定ID/INI：调试在设置二级、Pro含HoYoShade/GIMI/UID既有面板、NR只启用层/多帧子标签、FG HUD/alpha高级与帧率归位。派蒙文本建议仅本次启动前120秒且不因切页重计时。当前SOURCE已含Kepler followup1：实际可见1–4项走多按钮单选、5+保combo，逐项tooltip/本地化；保原##UpscalerCombo ID及hidden旧选择，不把隐藏第3项clamp成已选第2项。Root局部集成三文件14hunk，与首批15处/2文件均随R1028 retry4/cell2正式构建部署actual exit0。Kepler已关闭，不待机；本次不改UI，用户布局验收UNKNOWN。

## 10. 历史同局证据、失败链与未知

官方sessions根：`D:/APPS/test/TEST/HoYoShadeHub_Portable_1.4.5.0_x64_Full/log/sessions`。本次未扫进程、hash或新建采集器。

|候选|已核身份/配置|不能推断|
|---|---|---|
|`20261104-043653_YuanShen.exe_90152`|session t0=2026-11-04 04:36:44.362+08；实际Opt首行04:36:44.643256，v0.1.29-final(a4d7a345)；启动trace L254为pid90152/creation FILETIME134382118043622083；actual INI在runtime 2E415BFEBE7E11B2。Hub同局L2835/2838注入该runtime成功；L2809 profile activated、L2882退出fallback stored。LogFileName指official/F6；启动true NR/FinishedPicture、FG Upscaler/DLSSG、dlss_12。|runtime选择/头版本/源码路径相符不证明加载内存与depth6磁盘逐字节相同；用户这条失败反馈对应本PID仍UNKNOWN。|
|版本差异核查|当前S/resource.h=0.1.29，resource_build_commit.h=a4d7a345，build date=20261104_042534；0.1.29不是天然“旧错包”。实际factory日志L284指work/f6-source的D3D11_Hooks.cpp/native seed hooks。|不能以相邻0.1.36.1版本更高就判它才是F6新局。|
|`20261104-043117_YuanShen_170196`|t0=04:31:15.238+08，runtime9CC5F395A4EB1FF0、session模块0.1.36.1；session +86.1s exit0。|session没有实际Opt日志，NR/深度/加载depth6字节UNKNOWN，不能和90152拼证据。|

PID90152 session +354.7s定时发现进程已退出，准确退出码UNKNOWN；实际Opt尾至04:39:00.182513。没有活游戏局认定或自动操作。该session列过外部Bridge模块条目，Hub却明确原Bridge未找到、独立Opt注入；只以实际internal native FSR执行链证明producer，不用模块清单猜双桥。

实际Opt：`20261104-043653_YuanShen.exe_90152/opti-configured-2E415BFEBE7E11B2-OptiScaler.log`，9,309,676 bytes。关键同源链：

- L18722–18723：selected original canonical resource=2524136165600，1536×960，generation1/sourceFrame1，owner context2518764390088/thread140484，deviceEpoch1/configEpoch2。
- L19938：mapping reason7 DEVICE_Z_PROJECTION_UNPROVED，mapped=false、producer_ready=true、producer_cpu_complete=false、producer_read_ordered=true，semantic_failure=NO_SELECTED_RETURNED_RASTER。已排依赖不是GPU已完成，也不是模型有深度。
- L19960–19967：真实CopyResource从canonical 2525208864224到selected2524136165600；源track已不存在。这里是内容证明未随实际copy到达，不是“没有深度纹理”。
- L20066–20070：同一源下一clear返回（clear2192756），随后writers1256时`before-snapshot-refused`并丢track；L20071/20077下一raster/copy拒绝，后续L20233–20246、L20340–20345重复。源码1024硬界是独立确定限制，但本段具体firstUnprovedReason和Snapshot拒绝分支未保住，不能断言本段最早原因就是writer cap。
- L17598等first8全局样本确有gs=false/hs=true/ds=true，但不与L20070所选源的具体draw绑定；曲面细分是有据候选，**不是已证实的该次根因**。早期login另一个source的语义成功/clear-cache eviction不能代世界场景。
- L20205/L20279/L20381：generation4/5/6真实`dlssnr_call_evaluate_v2`采样，input_depth/region_depth/readable_depth/model_depth全false，guide=producer-depth-without-picture-uv-mapping，null_stage=native-input-depth-missing，model2096×1312。样本中0条model_depth=true；不是对全局每帧的穷尽计数，但足以证明这些真实模型调用未收到depth。
- L30595：另有reason16 QUERY_SCOPE_ACTIVE_UNKNOWN_OR_UNHOOKED，仍NO_SELECTED_RETURNED_RASTER。不能删除query保护或把它与reason7混为同一根因。retention样本refused=0，不支持宣称byte budget耗尽。

旧PID186216独立证据：`audit/r1024-f5-local-depth-20261103/followup6/incoming/pid186216-OptiScaler-full.log`，18,915,009 bytes/68,311行。L20003仅一次model_depth四true；L20071同CB mappingKnown=false且payload无效。depth6修真实Unmap返回恢复，最初loss触发仍UNKNOWN。该旧局不能证明当前90152同一失败原因，更不能称用户新局已通过。

## 11. 后续维护与交接

完整接手顺序与私有产物见 [HANDOFF](HANDOFF.md) 第3/9节和 [R1074效果重评](audit/r1074-f6-effectiveness-review-20261104/DESIGN.md)。优先按持续真实输入→合法相邻历史发布→实际消费→真实退休闭合；原神FG异步显示、ZZZ source/SDK clock与星铁NR候选分别处理。后续编码Sol high、物理隔离、Root有限审局部集成；不继续逐拒绝点补丁或全游戏统一FG协议。

Root后续自行沿现有有界CLI，保准确PID/creation/源日志/实际配置；dispatch/Ready/短深度样本不冒world/像素或性能。无新tests/hash/manifest/fixture/验证器/诊断构建门禁。旧文末R1031–R1046连续追加的部署/worker快照已保在[原字节前像](cleanup/r1077-document-before-20261104/F6-TECHNICAL-PATHS.md)，完整逐轮事实仍在canonical RECORD；删除重复状态不删这里的三游戏技术主体。

回归保护与原桥维护分别见 [REGRESSION](docs/REGRESSION-HISTORY.md)、[BRIDGE参考](docs/BRIDGE-SOURCE-REFERENCE.md)，两者已移docs并修相对链接；历史裸路径按项目根解释。当前项目路径、未完成/配置/运行/资源所有权结论只以HANDOFF/CURRENT及实际源码为准。
