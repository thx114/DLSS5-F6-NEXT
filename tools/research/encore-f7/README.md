# Encore 对 F7 有用的数学、FG/NR证据与离线工具

2026-11-07（北京时间）。定向补查与离线参考，不重做前两轮扫描；本包独立于F7产品，未更改产品src、tests、配置、部署或游戏。

## 当前接续与未完成项

F7主会话`01a1aed8-27e6-7c81-b113-e12c93a4d6c1`连续续接仍返回上游HTTP413，
最后失败turn为`01a1b543-69a4-7112-b110-12215d707c95`。先在该会话执行
`/compact`并确认恢复，再继续产品构建/运行。不要反复追加大段研究正文；本文及
专题文件是接续入口。研究侧没有修改provider、模型或接管主线程的游戏操作。

1. **FSR与移动性能：** 新setter候选实际运行目录为F7根下
   `build/performance-genshin/fg2-setter-motion-20261107T0835`。
   `session/result.json`记录未确认大世界就绪，`observationCompleted=false`，
   08:47:52Z原runner收尾退出；不能沿用当前状态页里“尚未启动”的旧文字，
   也不能把这局当新setter移动验证。先核冻结源码/构建身份和原生回归，再由唯一
   owner安排新局；比较时合计setter getter与draw getter，保留性能和画质验收边界。
2. **FG2再到6×：** 既有成功移动局仍为NativeOnly1×；解决当前writer/source/
   最终颜色消费资格后，在所有既有GetState查询处保留同tuple原始max、返回码、
   配置与查询区间。只有真实max允许五生成帧，才有依据安排6×实验；请求值、
   hint、菜单或CPU缓存命中都不替代实际生成/显示证据。查询离线合同见FG-RUNTIME.md。
3. **NR：** 当前`NrDx12.cpp`仍是core feature18 scratch查询失败即拒绝的路由。
   先明确实际NR载荷SHA、合法调用ABI/路由及同DeviceOwner的会话合同，再由唯一
   GPU owner安排受控执行。参数候选的341个原生断言和17槽静态对照已经可用；
   不再重复请求编译它们。Create直接五key在F7当前已存在，盲加字段不是已证修复。

研究交付的616项Python回归、原字节及CPU数值参考已完成独立复核；它们只证明
所述有限合同，不证明全量Encore逆向、三游戏验收、真实NR或FG×6。下一次优先
处理上述运行缺口；同样的静态验证不重复执行，除非输入或实现实际变化。
本接续段在最近完整验证后更新；原validation.json保留当次README快照哈希，
本次没有将文档更新写成一次新的测试执行。

## 代码与运行

- encore_math.py：标准库矩阵、透视候选、互逆/乘积残差、假定历史 clip 关系；独立实现 Encore guide depth、九项屏幕运动模型和 S10.5 编解码。
- cb_probe.py：读取完整 little-endian float4 CB。所有 --offset 均为**整个 resource 的绝对字节偏移**；metadata 的 Context1 绑定范围独立保存，不裁剪或修正。四种 storage/vector 解释分别列出，两两数学等价，不能凭数值识别真实约定。
- SR-INPUTS.md / sr_temporal_probe.py / test_sr_temporal_probe.py：generic SR准备参数的原始锚点；独立投影/raster位移及raw/SDK jitter绝对/差值比较，26项回归，不自动选择公式或认证相机。
- SR-SYNC.md：D11Completion的End/同thread DONOTFLUSH查询/缓存读取、引用析构，以及另一FG路径的5000 ms CPU等待；分开静态机制与F7卡顿因果。
- dlss_trace_probe.py / test_dlss_trace_probe.py：严格解析F7连续DLSS trace v1的32列、计数和窗口边界，49项CPU/CLI回归；分别汇总带observer开销的调用括号与连续调用间隔，QPC整数精确相减，不测GPU或推定因果。
- hook_cpu_probe.py / test_hook_cpu_probe.py：严格解析Hook CPU累计snapshot，57项CPU/CLI回归；严格legacy/采样两格式，旧8行歧义和新格式仅11行完整分开，保留每阶段max的整数QPC，按nearest-rank/阈值计数拒绝不可能的分位区间；不跨snapshot拼接长调用，不对inclusive阶段求和或由Present推Render次数。源码SHA按格式分别绑定所读参考文件，不认证旧局编译源码。新格式逐stage核stride1/128，只保留accepted分布和observed max，绝不乘stride外推总体；独立绑定缓存指标未在本报告解析。
- sr_log_probe.py / test_sr_log_probe.py：现有F7稀疏日志的逐行七公式区间比较，40项回归；考虑打印量化、opened段、有限float32域与sidecar关联，不认证物理语义或GPU完成。
- test_encore_math.py：CPU 回归，包含有限/无限远、正反 Z、RH/LH、clip range 歧义、轴翻转、非零中心、错误输入、history、完整资源和 flow 符号/舍入。
- fg_state.py / test_fg_state.py：严格解析DLSSGState v4 Win64 88B离线快照，分开请求、配置、能力、API结果及区间计数；9个合成测试，没有真实SDK样本。
- fg_query_ledger.py / test_fg_query_ledger.py：离线JSONL查询边界检查，30项合成测试；所有现有查询共用ordinal，遇失败/漏号/配置或身份变化拒绝跨越比率；合同见FG-RUNTIME.md，没有原生采集器或真实ledger。
- FG-RUNTIME.md：请求与应用倍率、失败锁存/2×回退、GetState区间、旧UI高值保留与F7查询窗口缺口。
- FG输入fence合同另核97项原字节锚点、四导入、两组函数/pdata/unwind、两个公开槽及12条路径；区分Encore条件5000ms CPU等待、sentinel缺口和F7已有SDK输入/真实queue尾双完成保护，不据此添加阻塞等待或认证6×。
- FG-RUNTIME.md另补FFX configure/prepare到Streamline资源tag的九窗口闭合链：完整frame key、设备/extent准入、三tag与两种生命周期；不把显式FFX输入当D3D11消费证书或绑定缓存。
- NR-CONTRACT.md：feature18参数、guide/subrect/scale分支、颜色复制与codec调用、失败保留SR颜色、性能测量位置。
- NR-CODEC.md / nr_codec.py / test_nr_codec.py：四个NR PSO与字节表对应、颜色编码/残差重建，14项数值回归。
- nr_codec_rect_reference.hlsl：矩形无warp的两个独立compute参考入口，host合同见NR-CODEC.md；FXC编译不是GPU验证。
- NR-WARP.md / nr_warp.py / test_nr_warp.py：每轴warp、颜色取样坐标、深度/MV端点变换与给定量化参数的CPU Gmax，24项回归；包括原生可发射的零falloff与Center负零。
- nr_grid.py / test_nr_grid.py：有限int32半整数舍入、整数grid score、98%-100%候选搜索与两阶段work尺寸，24项回归；score不等于GPU时间或内存。
- nr_axis_config.py / test_nr_axis_config.py：有限每轴24B writer、显式百分数与三个已核preset、48B pair门槛，24项回归；不是实际配置来源认证或Guides CB。
- NR-WORK-POLICY.md / nr_work_policy.py / test_nr_work_policy.py：限定工作尺寸稳定选择块与reset写入，38项整数状态回归；保留opaque key语义未知，不借旧帧、不提交feature或认证GPU性能。
- NR-PAYLOAD-ABI.md / nr_payload_inspect.py / test_nr_payload_inspect.py：闭合Encore直接payload API表与自带参数对象；71项真实合成PE/CLI回归。检查器分开direct/forwarder/missing/invalid，记录显式pefile依赖身份，不认证ABI或硬件支持。
- Encore NR加载器另有模块路径查询替代层：先找到并改写payload的GetModuleFileNameW导入槽，再提交五API表；八个新增限定窗口与两名/五IAT断言记录在runtime。只恢复机制，不提供移植实现或执行支持证明。
- nr_reference_evidence.py / test_nr_reference_evidence.py：对SHA固定的历史CDF NR参考重解码25个入口/内部窗口；1705条指令、110个原有锚点；对象token/Shutdown合同另绑定1个GUID、9组函数/pdata/unwind、127个锚点及15条路径。Stats另绑定3个key、79条完整指令、1组函数/pdata/unwind及11条路径；内部Create另核379条代码与145B尾部选择表数据；rect helper另核72条指令、59个锚点和七条路径。73项回归覆盖解码、保存报告重解码、字节/字段/边界、scope与输入保护；显式输入可迁移到同SHA副本，不绑定本机绝对路径。该参考不是F7选定payload；caller检查、对象token、Populate、两个callback和内部Create见NR-PAYLOAD-ABI.md。
- nr_rect.py / test_nr_rect.py：历史CDF rect helper的独立CPU数值参考与24项测试；零size采用完整texture尺寸，无符号减法边界检查，不裁剪；保留false时已写出的部分字段。不查询资源、执行backend或认证GPU完成，与Encore外层裁剪分开。
- NR-PARAMETER-STORE.md / native/：公开NGX接口的独立C++20双槽参数候选与跨TU CPU fixture；固定容量、无堆分配、首错冻结，整数精确保留是相对Encore全double存储的有意差异。F7 owner已于11月7日13:32–13:33完成MSVC x64编译/执行，341个断言、0失败，CTest 1/1通过；当前八项输入SHA与回执一致。原生结果独立于Python计数，不代表payload/GPU验收。
- nr_native_receipt.py / test_nr_native_receipt.py：11项回归核对已有owner回执、唯一CTest stdout与当前八项输入，覆盖严格JSON类型、冲突/重复测试行和摘要，以及解析后文件被替换时的原始字节哈希；不重编译/执行，保留compiler exit未知和跨模块ABI未认证的边界。通过verify.py的三个显式--nr-native-*选项接入，见NR-PARAMETER-STORE.md。
- nr_warp_reference.hlsli / nr_guides_reference.hlsl：独立映射函数与80B Guides参考入口；没有改变既有矩形codec的CB。
- rebuild_evidence.py：核对 DLL SHA256 后重新解码限定本机指令和十个 DXBC，不加载目标 DLL。Capstone使用既有参考目录，pefile由当前Python环境提供，shader profile另需本地SDK FXC；执行版本记录在manifest。
- test_rebuild_evidence.py：50项显式C-vtable SDK头解析及FG/NR静态合同回归，覆盖Win64 slot、缺失/重复方法、不完整声明、GUID单字节改变和生命周期/descriptor枚举漂移；另逐字节拒绝NR清理和FG输入fence控制流/导入/timeout/边界漂移；不替代COM/GPU执行。
- verify.py / test_verify.py：统一运行CPU回归、核对静态证据和原DLL哈希，重做可选CB、主点、SR日志及连续trace报告；33项输入保护/CLI回归覆盖真实文件、hardlink、目录联接、历史NR主/可选exports报告、保存profile的外部SDK/原件/静态产物、参考编译器/shader/include/DXBC和新trace输入。显式重建所用SDK头在无manifest或旧manifest时也先保护；NR原生回执选项必须成组，十项输入逐一验证hardlink输出别名，非法回执在测试和输出创建前拒绝。保留报告可在evidence/自身路径存在；其它输入或报告的输出别名均在首次写入前拒绝。

在 D:/CODE/DLSS5_NEXT 运行：

    & D:/APPS/conda/python.exe -B tools/research/encore-f7/test_encore_math.py
    & D:/APPS/conda/python.exe -B tools/research/encore-f7/cb_probe.py --cb "完整路径/capture-1.vs-cb2.bin" --metadata "完整路径/capture-1.json" --offset 80 --offset 144 --offset 208 --offset 272 --inverse-pair 144 208 --product 144 80 272 --output "结果.json"
    & D:/APPS/conda/python.exe -B tools/research/encore-f7/rebuild_evidence.py --reference-root D:/CODE/DLSS5_NEXT-workers/f7-clean-rewrite/build/research --fxc "C:/Program Files (x86)/Windows Kits/10/bin/10.0.22621.0/x64/fxc.exe"
    & D:/APPS/conda/python.exe -B tools/research/encore-f7/verify.py --capture-directory "上文只读副本目录"

新增FG与完整复核入口：

    & D:/APPS/conda/python.exe -B tools/research/encore-f7/fg_state.py --state 'snapshot.bin' --enabled true --requested-total 6 --get-state-result 0 --configured-generated 1 --set-options-result 0
    & D:/APPS/conda/python.exe -B tools/research/encore-f7/verify.py --reference-root D:/CODE/DLSS5_NEXT-workers/f7-clean-rewrite/build/research --fxc 'C:/Program Files (x86)/Windows Kits/10/bin/10.0.22621.0/x64/fxc.exe' --windows-sdk-include 'C:/Program Files (x86)/Windows Kits/10/Include/10.0.22621.0/um' --capture-directory 'C:/Users/thx11/AppData/Local/Temp/F7-isolated-tests/scene-cb-motion-reviewed-20261106T193038/raw-input/camera-input-40288-929910133393-0'
    & D:/APPS/conda/python.exe -B tools/research/encore-f7/rebuild_evidence.py --reference-root D:/CODE/DLSS5_NEXT-workers/f7-clean-rewrite/build/research --profile runtime --windows-sdk-include 'C:/Program Files (x86)/Windows Kits/10/Include/10.0.22621.0/um'

snapshot.bin由调用方提供；请求6、配置generated1示例仅说明字段分离。rebuild_evidence.py
默认camera profile；--profile runtime重解码FG/NR/SR同步，不需要FXC，但须显式指定
--windows-sdk-include绑定d3d11.h/d3d12.h/libloaderapi.h；--profile nr-codec
重解码NR颜色shader/PSO链；--profile nr-warp重解码Guides与参数绑定，两者需要FXC。
verify.py始终核对四组manifest，带--reference-root/--fxc/--windows-sdk-include
时先重解码四组并编译三个参考入口（包含hlsli哈希）；未带这组参数时静态部分仅检查已有
文件与哈希，CPU测试仍会执行。

比较两个 CB 时显式添加 --previous-cb 和 --clip-offset。工具计算 inverse(currentWorldToClip) * previousWorldToClip，不去 jitter，不认证捕获次序、相邻 source、共同 world、history 或 camera owner。

库使用 float64，是数值参考，不保证 GPU float32/FMA 逐位结果。pack_flow 拒绝 NaN 和 signed32 ftoi 溢出输入，不另造溢出饱和策略。guide_depth 对有 depth 的 [0,1] 输入提供公式；缺失 depth 返回1，不代表 F7 可接受该深度。

## F7 实际样本结果

只读输入副本：C:/Users/thx11/AppData/Local/Temp/F7-isolated-tests/scene-cb-motion-reviewed-20261106T193038/raw-input/camera-input-40288-929910133393-0/。

本包读取 capture-1/2.json 与各自完整560-byte VS CB2。主线程将它们标记为标题界面样本，本线程不重读图片。metadata 明确 observed-only，camera/writer/frame/source 均未认证；绑定范围 first=0/count=4096 超出资源的部分只照实记录，不补数据。报告保存 CB/metadata SHA256。

下表以 row-major / row-vector 为**假设**：

| 核查 | capture-1 | capture-2 |
|---|---:|---:|
| c9..12 × c5..8 与 c17..20 的 maxabs | 5.3442564e-8 | 1.3920489e-7 |
| 同上相对最大误差 | 8.5794538e-10 | 2.2347392e-9 |
| c9..12 × c13..16 与 I 的 maxabs | 4.9015058e-7 | 4.9015058e-7 |
| c13..16 × c9..12 与 I 的 maxabs | 3.8815648e-6 | 3.8815648e-6 |

c5..8（byte80）满足稀疏透视；两个样本的对角焦距/深度系数相同，Y焦距负号。可解释为 FOVy约45°、aspect约1.6；唯一正 near/far 候选为 [0,1] reverseZ，near约0.250000015、far约600.036108。far 由已量化 float32 系数反推，不是精确配置。中心偏移变化不证明 jitter，lens shift/组合等含义仍需实证。

这支持继续核查 projection / view-like inverse pair / combined-like 关系，**不证明真实 gameplay worldToView**。c9 的3×3 determinant约-1，有反射/基变换，不能直接作为纯旋转。c17..20 自身不是稀疏 projection，不能直接用作 cameraViewToClip。

第二报告另算两个 capture 的假定 combined clip 关系：非零平移，重构残差约7.11e-15；这是同一组数的代数自洽，不是跨帧身份或去 jitter 已验证。

## 固定目标与新补机制

RTX Encore v2.0.1 DLL：42,682,368 bytes，SHA256 06a7fd63ee0f0e95574f3869d70c3c9ae4aaf326ba53ff8c864064410a5f1217。文件在 D:/CODE/DLSS5_NEXT-workers/f7-clean-rewrite/build/research/rtx-encore-v2.0.1/extracted/rtx-encore.dll。ImageBase=0x180000000，下文地址均为模块 **RVA**。shader 名沿用上一轮分析标签，不是 PDB 符号，不把后缀当结构偏移。manifest.json 保存工具、blob SHA、范围与计数；tokens.json 对应 FXC /No code-chunk offset、实际RVA及字节。

### SDK 合成相机与 guide depth 属于不同域

- 0x13050..0x13447 构造合成投影。0x1316e 读取半角0.523598790；0x131c3 调数学helper；0x131d9..0x131fe 由 width/height 生成焦距。0x13210 写 reversed A bits=0xb727c61a，0x13224 写 forward A bits=0x3f800054；B 来自0x53fb88/0x53fbb8，约 +/-0.100000992。均为合成常量，不来自游戏 CB。
- 0x13301 调 constants builder 0xe010；0xe4cb..0xe53c 组合已有历史矩阵，0xe543起保存 clipToPrevClip 并求逆。不能断言历史映射永久 identity，尺寸/投影变化也会改变它。
- jitterOffset 在 constants +0x160/+0x164：0xe355/0xe3b6 读参数，0xe5b0/0xe5b9 写出；合成调用0x1325f/0x13270传零。MV scale 位于+0x168/+0x16c（0xe5cd/0xe5ec除尺寸）。**0xe5e5清零的+0x170是 cameraPinholeOffset，不是 jitterOffset**。成员对照本地 external/streamline/sl_consts.h。
- feed-preprocess RVA0x713d80，指令33..39（offset0x448..0x4f0）：可选 z=1-z，随后 z/(1000-999z)，无depth输出1。数学上等于 (viewDistance-near)/(far-near)，但对应 far/near=1000 的正向[0,1]透视。SDK合成投影的约0.1/10000是另一比例。不能把两域当游戏相机合同，差异本身也不证明bug。

### motion camera model 是屏幕运动拟合

三个 blob：pso-38 RVA0x781570、pso-40 RVA0x798ce0、pso-48 RVA0x7b1f20。基函数为：

    x = ((px+0.5)*Scale - W/2) / (W/2)
    y = ((py+0.5)*Scale - H/2) / (W/2)
    q = depthFlag ? min(0.01/max(depth,0.0001), 1) : 0
    b = [1,x,y,x*x,x*y,y*y,q,q*x,q*y]
    flowX = dot(coeffX[0..8], b)
    flowY = dot(coeffY[0..8], b)

x/y 均除半宽。depth 指该 shader 输入数值，不认证为原始游戏深度/世界距离。

- pso-38 指令125..148（0xe40..0x10d4）构造基函数；45个对称Gram项、X/Y各9个RHS共63项。index63/64是加权flowX²/flowY²，index65是权重（222..225）。它输出66float=264B的partial，不是4×4 view。线程内展开2×2不同位置，不把线程坐标乘2误读为仅隔2像素采样。
- 初始weight=confidence²。bit32分支（76..121）用当前模型2D残差再乘 1/(1+residual²/Param0²)。确定的是shader分支，不宣称CPU鲁棒pass次序已闭环。
- pso-40归约/解算partial。末尾3008..3017（0x18c14..0x18d34）以 support≥Param0 且 RMS≤Param1判有效。bytes0..35为X9系数、36..71为Y9系数、72为valid float、76 RMS、80 support；末次从68写4float。不是 camera pose。
- pso-48 指令611..650（0x43b4..0x4850）在相应flag且valid>0.5时求模型，向局部运动加0.05权重先验；不是把它当SDK camera矩阵提交。
- pso-48 指令686..694（0x4bd0..0x4cdc）将flow×32、round-nearest-even、ftoi、clamp signed16、打包两个半字。feed-guides 指令49..53等作 signed16/32解码，其history坐标中显式 pixelCenter+flow，确定的是**该shader局部** current→previous查找解释。最终Motion与另一路RawMotion有不同过滤/拒绝处理，RawMotion不是完全未加工输入。

CPU既有证据定位model分配128B、partials=count×264B，但本轮未重核完PSO/descriptor/constant/schedule链。不武断地将0x38/0x40普通偏移标为对应PSO，也不声称完整恢复了多尺度/鲁棒调度及Param实际值。

## 新增FG/NR直接交付

- FG的总倍率请求与成功应用值是两条状态；失败后可回退2×。Encore某UI分支会在当次
  count为0/1时保留旧高值，不能用菜单显示6×证明持续生成。具体RVA与v4 ABI见FG-RUNTIME.md。
- F7已有GetState能力刷新与AfterPresent两个查询位置；SDK计数从上次查询起算。建议记录
  所有现有查询的ordinal/用途/区间及同窗base Presents，避免新增SDK读者或每帧阻塞IO。
- 旧2×回执中的numFramesToGenerate=1是请求，不是maxGenerated=1；缺少该原始字段
  不能证明上限，亦不推翻当前固定tuple的2×实验限制。实际provider能力仍须同tuple原始回执。
- NR对SR结果作颜色复制、编码、Evaluate及条件解码；已识别BAD失败分支跳过解码，
  保留SR颜色。既有F7已设置大部分同名参数；不把其已映射guide的base0/scale合同误判为缺项。

## F7 实施方向与性能

1. **先缩小已有capture语义核查范围。** 数值分解将projection候选从c17..20移到c5..8；下一轮gameplay验证writer、source/frame、jitter/clip约定、history。保留 CameraFrameBinding 的真实准入门禁。
2. **优先证明原生MV合同。** Encore屏幕拟合可作离线符号/尺度/残差诊断，不能认证相机。F7已有原生深度/MV时，不默认引入另一套稠密光流、拟合或CUDA同步替代它。
3. **分别核depth/MV数据域。** 本库可作数值对照；不将固定depth变换、float1、zero-motion、零jitter塞进真实游戏合同，也不因指针非空就认为有效。
4. **按实际queue/pass同帧时间优化。** pso-38统计、pso-40解算、pso-48邻域/先验、feed-guides条件history均有潜在成本，静态指令数不是毫秒或瓶颈证据。先测真实 SR+NR+FG+ReShade、跨API复制/等待和消费路径。本包不增加产品每帧扫描/拟合/IO，不扩池，也不替换资源所属fence。

## 验证边界

没有执行/加载/注入Encore，没有操作游戏、正式DLL或profiles。当前共616项Python CPU测试
（19项数学/CB、26项SR时域、40项SR日志、9项FG状态、30项FG查询ledger、14项NR codec、24项warp/guide、24项grid、24项axis config、38项work选择、71项payload静态检查、73项历史NR证据、24项rect数值参考、50项SDK头/FG及NR生命周期合同、49项连续trace分析、57项Hook CPU分析、33项统一验证输入保护、11项原生回执核验），另有owner归档的341项原生断言/1个CTest，camera窗口650条本机指令、
runtime窗口5466条、nr-codec窗口300条、nr-warp窗口1038条，按profile累计7454条限定指令
（窗口可重叠，不代表同等数量的独立位置）与11012个shader token；
runtime/nr-warp另有573/42个固定指令断言；runtime另有8个参数key/1个常量断言，nr-warp有13个常量与3项preset表断言，
nr-codec直接核对四个bytecode表，Guides核直接PSO绑定。
runtime另直接核五个payload导出名、两行历史hash/版本、17个参数vtable槽和3项RTTI。
feature holder的两个vtable前缀项及ReleaseFeature绑定/handle保存/回调链亦已核；
其CPU引用生命周期不等于GPU退休证明，Shutdown1的实际调用合同仍未闭合。
新增五窗口/237指令/21锚点将runtime+0x90与holder+0x10关联到实际取得的ID3D12Device，
并核完整runtime析构中feature条件销毁先于device Release；device COM引用不等于模块租约。
nr-device-holder-contract.json另绑定一个GUID、三个公开SDK slot与五组.pdata边界。
Shutdown补查共14窗口/220指令/96锚点及14组.pdata，区分缺导出的失败FreeLibrary、
三段退出清理与上下文中的Evaluate；容器释放链另绑定13条direct edge、三个叶thunk、
两处IAT调用及68项字段/步长/控制指令，正常路径到HeapFree；两个错误helper仍UNKNOWN。
这些CPU内存清理不认证直接表第五项caller、成功会话卸载或GPU退休。
同名NGX Shutdown wrapper另有八窗口/177指令/51锚点，分开0x14795a8转发槽与直接
NR槽0x1663ef0；保留调用结果不等于会话/模块/所属fence已退休。
Hook事务另补14窗口/528指令/62锚点：保留begin取得CPU owner后仍可能失败的条件路径，
以及wrapper模块pin、线程准入与exit标志不消除update错误的边界；不认证F7实际backend或安全回滚。
runtime新增D11Completion五窗口、四项vtable前缀/三项RTTI、四个IAT、公开SDK七slot
及DONOTFLUSH常量断言；公开头与原件哈希记录在manifest，仍不认证实际调度或F7性能。
NR导入替代层另有八窗口/225条指令/50锚点、两个导入名与五个IAT断言；
模块路径文本变化不认证真实模块名、调用成功或F7接入完成。
NR创建/复用门禁另有五窗口/119条指令/40锚点、11个key与两个数值常量断言；
Encore本地ratio1回调与CDF载荷发布的quality回调保持不同身份，不当作同一默认策略。
三个独立HLSL参考入口有既有FXC编译回执；本次仅核源头、编译器与产物哈希，未重新编译或执行GPU。
历史CDF NR参考另有1705条指令/110个原有锚点，独立于上述Encore四profile计数；输入SHA、
五个目标导出与额外Populate导出、25组.pdata边界、caller检查IAT/字串、
四个缩放key/三个float常量与Capstone源/实际native库身份在
nr-reference-entrypoints.json中记录。shutdown_identity另绑定1个GUID、9组新增函数/pdata/unwind、
127个合同锚点与15条路径（锚点集合可重叠），保留写结果未核、null对象选择范围及正常literal1的边界。
published_stats另绑定三个原始key、79条完整指令、一组函数/pdata/unwind及11条路径，
保持被调对象/方法/聚合语义未知；Stats成功不作当前NR会话、硬件支持或GPU退休证书。
internal_create另外绑定五个key、132个锚点、一个含尾部数据的父函数记录、
两张145B数据表、五条读取合同和十条路径。379条实际指令与表数据分开计数，
默认值与局部选择准入不认证完整Create ABI、嵌套参数读取或实际硬件支持。
descriptor_rect另外绑定59个指令锚点、一组函数/pdata/unwind及七条路径，
独立CPU参考保持无裁剪、非零origin加零size与false输出非事务性等边界。
统一验证绑定同一显式Capstone工具身份，重新解码
原文件并逐行核保存报告的指令边界、字节、助记符和操作数；计数来自新解码结果。
validation.json保存四组manifest哈希、独立historical_nr_reference、源码/include哈希、输入报告哈希与执行模式。
它们只证明限定数值与字节可重现，不证明全量反编译、GPU逐位一致、F7 FG可用、
性能改善、画质或6× provider。前两轮原件由F7 docs/REFERENCE-RESEARCH.md索引。

既有数学、FG与NR组件已分别接受独立Sol复核；原始指令、shader、有限支持域和已修
反例留在对应专题说明。额外内存解释器检查不计入交付测试，也不替代GPU等价证明。
当前统一计数、源码/include及输入报告哈希以validation.json为准，不累计旧轮次数字。
连续trace可通过--dlss-trace显式加入统一验证，--dlss-trace-sha256锁定所读原件；
不会自动读取主线新游戏局。当前真实诊断副本1707条、24次reset及其不稳定配置
边界见SR-SYNC.md；文本capture_complete不表示该局性能baseline或游戏验收。
本次先单独运行runtime profile重解码，再运行完整verify核四profile哈希；后一步未带
--reference-root，因此validation根级static_decode_refreshed=false只描述Encore四profile。
历史NR参考另存static_decode_refreshed=true，其逐窗口重解码在统一验证中实际执行。
未将其改写成四profile全部fresh decode，shader仍使用既有编译产物。

SR日志收尾修复了两项P2：统一验证器在任何输出前保护日志、receipt及其它显式输入；
MVScale和frame_time按同一声明精度检查有限float32打印区间。保留合法FLT_MAX打印、
零和subnormal；低精度的宽区间仍可能包含不合理的打印中心，不升级为执行存在性证明。
真实故障局22条记录、sidecar关联与打印精度限制见SR-INPUTS.md。此批仅离线分析，
不会改变F7的物理jitter合同、USER_FAIL或设备故障结论。

直接NR payload与SR core函数表的差异见NR-PAYLOAD-ABI.md；检查器已用真实旧F5
forwarder和SR DLL作对照，并定位SHA为cdfa6c1e…的真实历史NR参考。其内容不等于
历史目录中标注的094D2B0E…版本，也不是F7选定payload；五个入口的caller检查
与Encore模块路径查询替代层已有静态机制关系，但两份不同内容身份间没有实际
执行证明，不能据导出或静态报告宣称直接调用可用、硬件支持。
正式CLI须显式选--pefile-file或--reference-root，保护DLL和选定依赖源不被报告覆盖；
本次使用conda已有pefile.py 2024.8.26，其源SHA由每份检查报告绑定。
独立Sol复核发现的两项P2已补修及反例：拒绝非锚点语义篡改/伪造指令切分计数，
并在CPU日志写入前保护保留历史主报告及可选exports报告，避免其hardlink输出别名被覆盖。

## GitHub 工具补查

按用户建议检索了 NationalSecurityAgency/ghidra、avast/retdec、capstone-engine/capstone。本轮实际采用已有 Capstone 5.0.7 + pefile 2024.8.26 定向解码本机指令、SDK FXC 解码 DXBC，并复用只读静态索引。未安装或运行 Ghidra/RetDec；Ghidra 的 HeadlessAnalyzer 文档已定位，适合后续复杂 CPU 调度器控制流补查。RetDec 当前README声明有限维护且安装约需5–6GB，不引入本轮小型数学工具的依赖。工具仓库状态仅代表本次读取，不作下载/安装/执行成功声明。
