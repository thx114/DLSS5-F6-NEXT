# Encore 的完成查询与 CPU 等待：给 F7 的排查边界

2026-11-07，北京时间。只读静态分析，固定 Encore v2.0.1 的 SHA256 与路径见
README；本篇地址全部是该 Encore DLL 的 RVA，不是历史 NR 参考的 RVA。
没有执行 Encore、SDK、GPU 或游戏，不以静态代码判断 F7 本局卡顿原因。

## D11Completion 的两种 context 与三个操作

实际 MSVC RTTI 名为 .?AUD11Completion@ngx_live_preset@@：vtable位于0x559908，
COL为0xcf74f8，type descriptor为0xd80468。这里只断言四项表前缀，不手造
私有接口声明。factory窗口0xec820..0xecb25，返回的interface指针为控制块+0x10。

factory对输入context调用GetDevice，通过device创建一个EVENT query，再取得
同device的immediate context。它分别持有输入context、immediate context和query，
并保存创建时GetCurrentThreadId及两个初始为零的byte。输入context是否deferred、
调用方何时提交命令及本对象的所有调用者，仍须沿实际链路核证。

| 操作 | 表槽/实际目标 | 已核字节行为 |
|---|---|---|
| End/issued | +0x18 → 0xa9a00..0xa9a24 | 用输入context调用End(query)，然后写issued=1 |
| poll | +0x10 → 0xdd5b0..0xdd61f | 先核当前thread等于创建thread，且issued非零、complete为零，再用immediate context调用GetData |
| cached complete | +8 → 0x9c1e0..0x9c1e5 | 只读complete byte，不进行GetData，不等待GPU |

以上成员偏移均相对于interface this：输入context+8、immediate context+0x10、
query+0x18、thread ID+0x20、issued+0x24、complete+0x25。factory中的控制块
基址与interface this相差0x10，不能混用偏移。

GetData的output为初始化为零的4-byte BOOL，DataSize=4，flags=1，即本地SDK
定义的D3D11_ASYNC_GETDATA_DONOTFLUSH。只有HRESULT=0且BOOL非零，poll才写
complete=1。S_FALSE、失败、不同thread、尚未issued，都不会在此方法置完成位。
该方法窗口没有主动Flush、Sleep或轮询等待循环；这不承诺驱动调用成本为零。

cached complete是粘滞缓存；End本身不清complete。不能将已经完成的对象简单
End第二次就当作新的帧完成证据。现有窗口也没有把GetData错误分类或锁存：
错误与尚未完成都可能表现为complete仍为零。F7须保留独立的设备故障、错误及
owner隔离，不能把这个简化缓存照搬成完整生命周期状态机。

destructor窗口0x851d0..0x85248依次Release它持有的query、immediate context、
输入context。Release引用是CPU生命周期行为；它没有在这里核对应GPU完成。
因此单凭这个析构函数不能制定F7的退休或故障后释放规则。

## 同一二进制也存在显式的 5000 ms CPU 等待

本包既有fg-post-present窗口0x140d0..0x14b04中，0x14255之后取得一个fence指针
和目标值。这里尚未把该fence的生产queue、提交者与F7任何对象等同起来。

- 0x14275调用fence的GetCompletedValue，随后与目标值比较；未达目标时进入事件路径。
- 0x14287创建event，0x142a1调用SetEventOnCompletion。
- 0x142a9置timeout=0x1388，0x142b1调用WaitForSingleObject：这是5000 ms上限的
  CPU等待，不是GPU queue上的Wait，也不是D11Completion的GetData轮询。
- 注册/创建失败或Wait返回非零走失败路径；0x142db选择inputs-fence-timeout诊断，
  0x142ea返回0x887a0006。这个返回值是该代码分支的行为，不证明真实设备在此处
  首次发生hang，也不能反推F7旧局设备故障的根因。

这条路径有上游条件，不是每一帧必经；更不能由它推导纯SR且FG关闭的F7本局
进入了同一等待。它只说明“Encore所有完成检查都不阻塞CPU”这一笼统结论不成立。
实际卡顿时长、频率、执行与否和收益，都要用同PID/同source-frame的运行证据验证。

## 可直接用于 F7 的判断顺序

1. 分开记录CPU进入/离开等待、GPU queue Wait/Signal与GetData的单次查询；
   不能把三者都记成一个waitMs，也不把Present间隔直接解释成某个GPU pass耗时。
2. 每次完成证明绑定其实际query/fence、目标值、device/queue和source-frame。
   issued、缓存标志、CPU返回与提交成功都不能替代实际完成。
3. 对CPU等待先区分正常未完成、超时、注册失败、Signal失败、device removed与
   UINT64_MAX；保留F7已有隔离与故障门禁，不通过放松准入或复用旧帧改善计数。
4. 若要缩短等待，先证明当前帧的输入/输出合同允许异步消费、降级或丢弃。
   把阻塞改成DONOTFLUSH查询不会自动提供可用输出；未提交、未消费或未知在途
   的资源也不能因此释放、清quarantine或扩大池子继续写。

上述方向不要求增加每帧文件IO、深度扫描、额外SDK读者或第二个D3D12 owner。
这里只提供定点证据和诊断顺序，不提出盲改timeout、强制Flush或跳过真实等待。

## F7 源码对照：已有计时不覆盖获取锁的等待

DS定点搜索后，Lead直接重读SharedTransport.cpp的同一份字节；2026-11-07快照
SHA256为55d7a0142c7726d8baa8e0816ab966441c0a0ab63077cc8c58e0fc176f7ae10b，
路径为D:/CODE/DLSS5_NEXT-workers/f7-clean-rewrite/src/gapi/SharedTransport.cpp。
以下行号属于这个源码观察，不是当前加载DLL或本局执行频率的证明。

- Submit在1091行先拿mutex_，进入SubmitLocked后1110–1111行才启动QPC。
  因此RecordSubmitTiming的submit/prepare/queue CPU区间不包括等待该mutex；
  Bridge::OnNativeDraw开头的claimMutex_等待也不在transport的区间内。
- 普通SubmitLocked在1114行调用DrainLocked(false)，不是250ms循环等待；
  PollCompletion的902–905行观察提交时保留的四个fence，Pending即返回。
- 1303–1306行是DX11 input Signal/Flush到DX12 queue Wait；1310–1328行是
  DX12 Execute与两个Signal、DX11 output Wait/copyback/retire Signal/Flush。
  两个Wait是GPU队列依赖；API调用耗时仍可测，但不能将其直接标为CPU等待GPU完成。
- 显式的CPU轮询预算在DrainLocked(true)：949–960行以Sleep(1)重复观察，
  kDrainBudgetMs=250。Configure、Drain、Shutdown调用该等待形式；其实际调用
  频率未知，不能按源码上限直接估算每帧成本。
- WitnessCopybackPixel的1482/1489行有两次D3D11_MAP_READ、flags=0的staging Map。
  入口1324行受copybackPixelWitness且!pixelWitnessDone_控制，1428行在检查
  资源前就设置一次性标记。该读回可阻塞CPU，但启用、重置及实际耗时未知，
  不能仅由其存在解释周期性卡顿。

下一轮若测锁等待，应在有界诊断窗口内分别采拿锁前、拿锁后及SDK调用边界，
与同source-frame的reset原因和真实queue/fence对齐。现在只能确认计时覆盖缺口，
不能据此宣布锁竞争、配置drain、Map或SDK内部同步是F7长尾停顿的原因。

## 连续诊断的离线 CPU 时间分析

dlss_trace_probe.py读取F7的F7_DLSS_RESET_TRACE_BEGIN/END version=1文本，
与旧sr_log_probe.py的每slot前8次/每600次稀疏输入日志分开。新格式的actual_call
是实际BeginEvaluate序号，不是source帧序号、显示帧数或SDK历史长度。
分析器不修改jitter/reset数学，不增加产品日志或SDK读者。

每个BEGIN到对应END单独分析，不把不同configuration、进程或捕获窗口拼接。
32列顺序、整型范围、completed布尔值和尾部计数须与v1合同一致；坏event不会
被默默跳过。缺尾的合法记录前缀保持incomplete；截断/损坏的event行则拒绝解析。
Capacity、EarlyStop、StopFallback及未End事件都不能写成完整捕获。
已结束的完整窗口还须核实sealed达到配置到期点、flush不早于seal或已记录End、
零延迟的首调用与anchor一致。非连续序号或缺少可证首边界不会被标记为完整；
尾actual_calls只容许最后已捕获调用，或为触发封口而额外进入的一次未捕获Begin。

- evaluate_cpu来自每个已完成记录的end_qpc减begin_qpc。源码在取begin QPC后
  才执行BeginEvaluate记账，再调用SDK，所以这是带观察开销的CPU调用括号墙钟
  区间，不是纯SDK内部CPU cycles或GPU耗时；失败的SDK返回同样计入调用时间。
- between_evaluates_cpu只对连续actual_call、上一调用已End且无重叠的相邻记录
  计算后begin减前end。它可能包括准备、锁等待、游戏工作和线程调度，不能直接
  归因任何一项；漏号、未End和重叠必须显式计数，不能把缺失边界当作空闲时间。
- InvalidClock、InvalidTicket等使时间关联失去可信度时，保留结构统计并禁用
  时间汇总；跨调用QPC回退或串行调用括号重叠也禁用计时，不只检查单行差值。
  未完成事件即使sdk_result为零也没有可用的调用时长或SDK结果。
- 有效tick差先按整数精确计算，再按报告的QPC频率换算ms；64位QPC/身份以
  十进制字符串输出。分位使用nearest-rank，不先把大QPC转float再相减。
- 记录的SDK参数中的nan/inf保留为观察值/警告，不重写参数、不凭空抹掉独立的QPC
  区间。饱和的生命周期serial也不能证明身份从未变化。

分析器只读明确输入，绑定所读字节的SHA256/长度；读取间文件变化即拒绝。
输出树、hardlink等别名在写入前保护。真实样本须先取得owner冻结的日志/回执，
并可用expected SHA锁定；不会自行操作运行中的游戏或把落盘日志时间当作QPC。
CLI及统一verify的trace报告共用独占临时文件与原子替换，写前复核别名。
即使最后一刻输出被换成输入的hardlink，替换也不会按输出链接截断输入；
平台拒绝替换则报告失败并清临时文件，原输入字节须保持不变。

    & D:/APPS/conda/python.exe -B tools/research/encore-f7/dlss_trace_probe.py --input 'owner冻结的F7.log' --expected-sha256 'owner确认的64字符SHA256' --output tools/research/encore-f7/evidence/f7-dlss-trace.json

capture_complete只描述该文本窗口的完整性。输出始终保持performance_baseline、
gpu_completion_proven和causality_proven为false；这些统计不替代同PID/FILETIME、
DLL/config、source-frame及实际queue/fence证据，也不单独证明性能恢复。

### 已结束诊断局的固定副本

研究输入位于inputs/sr-m-reset-continuity-20261107T024736/，含F7.log、
result.json、stage.json与snapshot.json。该旧PID21516在复制前已不存在；
读前后文件身份/size/mtime和复制后字节核对一致。日志SHA256为
8dc6730fab8f080ce13e04ecc0aff5888b8d887a9e03858302b2d10c5f70ee74，
496,388 bytes。保存回执声明creation FILETIME为134384934332617839，
runtimeInputsStable与performanceInputsStable均false、session completed=false，
原因包括隔离INI改变。副本固定不等于原局配置稳定，也不认证加载内存身份。

一组90秒delay/30秒window文本具有完整BEGIN/END，actual_call3825..5531连续，
1707条均completed、flags=0、sdk_result均为1；sdkReset非零24条，reset_reasons=16。
这些是诊断记录字段，不是GPU成功或reset因果验收。离线CLI与独立整数CSV核算一致：

| CPU墙钟区间 | 数量 | P50 ms | P95 ms | P99 ms | 最大 ms |
|---|---:|---:|---:|---:|---:|
| Evaluate调用括号，含Begin观察开销 | 1707 | 0.2582 | 0.4045 | 0.4708 | 1.1218 |
| 相邻actual_call之间 | 1706 | 16.4446 | 21.5347 | 36.4269 | 267.0422 |

最大gap的call5382.end=1195012491802，到call5383.begin=1195015162224，
频率10,000,000 ticks/s。两条记录同slot1、source/instance7697263131264、
context7697370762624、session/feature/bind serial均1，generation5445→5446、
frame1953→1954、sdkReset均0；两侧调用括号分别0.322/0.4211 ms。
这支持补测调用括号之外的CPU时间，不能由此认定锁、游戏线程、资源准备或GPU
依赖是哪一个根因，也不能断言DLSS的GPU成本小。用户已验收抖动解决，本报告
不把旧记录中的reset次数变成继续修改jitter/reset数学的依据。

生成的evidence/f7-dlss-trace.json保留每对调用的边界、身份关系和ticks，
便于主线按精确QPC对齐其它诊断。它仍是带观察负载、配置不稳定的局部样本，
不是正式性能baseline或FG/NR画面证据。

## 可重现证据与 API 对照

### Hook CPU累计snapshot与最新冻结样本

hook_cpu_probe.py接收hook-cpu header及其stage行，保留样本、打印值、
分位区间和每个max调用自己的QPC。旧格式的11阶段为完整组，8行只能标为
legacy-compatible，因为原格式没有version/stage count/tail，无法排除11行被截断。
不足8/11或被无关日志打断的prefix不冒充完整；重复/未知stage、缺字段、
非法类型、阈值计数矛盾拒绝。未来扩展字段原样标unknown，不认证语义。

输入、实际读取的writer/types/scopes源码均绑定SHA和前后文件身份。源码参考
按日志格式分别核对；已漂移时报告source_contract_certified=false，即使完全相同，也不认证这局加载
DLL实际由该源码编译。输入/源码别名输出在写前拒绝，报告原子发布并复核绑定。
既有42项CPU/CLI回归包含大整数QPC、完整/截断组、打印精度、丢样、重复身份、
hardlink/symlink/junction与发布前变更。没有增加产品采集点或SDK读者。
分位候选bin还须与nearest-rank、精确max和未溢出的33/50/100ms阈值计数
存在整数tick交集；rank等于样本数时必须含max，不反演缺失histogram。

#### 新采样格式与旧格式分别校验

F7当前writer新增三个header合同字段：sampled_stages_do_not_extrapolate=true、
observed_max_only=true、periodic_aliasing=unknown，并把binding_cache改为
separately-reported。每行stage另有sampling_stride；这是已知协议变化，不能只把
原not-implemented放宽为任意字串。缺少必需字段、已知字段冲突、格式混用或
stage对应stride漂移均须拒绝；其它未来扩展字段仍标unknown，不认证其语义。
新采样格式以11行才完整，8行不享有旧格式的legacy-compatible完整组标记。

| stage | 声明的adapter采样stride |
|---|---:|
| TokenWork、NativeRender、DrawBindings、SrDispatch | 1 |
| DrawSelect、CompositionPrepare、NativeDraw、CompositionCommit、SurfaceLockWait、CompositionLockWait、ShaderGetter | 128 |

HookCpuSamplingStride与HookCpuScope定义的是线程+stage最外层scope的采样策略：
初始phase为0时选首个，之后每128个选一个；collector Configure/Reset不重置其它
线程的TLS phase，日志也没有总体尝试次数或可信reset边界。sampling_stride是
writer声明的adapter策略，不是全部实际调用次数或完整覆盖证明；collector直接
Record与显式counter仍可全频率写入，丢样、overflow和未观察scope继续独立处理。

因此保留accepted样本自己的sum、mean、阈值、分位区间和observed max，绝不
乘128外推总体CPU耗时、全部调用数或总体p99，不用Present index作Render分母。
固定间隔还可能与工作周期混叠，不能把observed max称为未采样调用的完整尾部。
各inclusive阶段仍不能相加、相减或跨snapshot拼接max端点。

新binding_cache=separately-reported指向独立的[fsr-binding-cache]记录；本CPU
snapshot解析器不冒充已经解析了那条记录。ShaderHit/Miss仍不能代替绑定缓存
hit/miss，BindingCapture也不是命中率。本次没有添加产品采集点或额外SDK读者。
源码参考按legacy/new profile分开，当前源码匹配不能认证旧局的编译来源；混合
格式也不能借一套新hash把旧格式认证通过。实际加载DLL的编译源码身份仍UNKNOWN。
原42项回归保留，新增15项采样协议与来源隔离测试，共57项Python CPU/CLI测试通过；
新格式当前只有合成回归，尚无已结束新局的真实采样日志作为执行证明。

    & D:/APPS/conda/python.exe -B tools/research/encore-f7/hook_cpu_probe.py --input tools/research/encore-f7/inputs/fg2-ownedmap-hookcpu-20261107T0427/F7.log --expected-sha256 a8a7cce88305bdee1e45bb14c4c4603d7a553d6b4f4b18536f898516b4f66d58 --output tools/research/encore-f7/evidence/f7-hook-cpu.json

该副本PID39384/creation FILETIME134384992027619583；复制前OS Wait0与原
result的Exited/Wait0已确认，三份原件读前后size/mtime/identity及副本字节一致。
日志224,794B、SHA为上述值；result声明runtime/performanceInputsStable=true，
但sceneReadyObserved=false、observationCompleted=false、completed=false，
并以CLI停止退出1。用户在主会话的入场反馈与runner未验场景分别保留，
这里不以任一方替另一方生成大世界baseline或正常退出结论。

报告包含19组完整11阶段；最后一组header/stage位于F7.log行585..596：
reportQpc1254150842380、frequency10,000,000；drop samples4202、counter4。
记录是累计accepted scope，完整分布和未记录调用均UNKNOWN。

| 阶段 | accepted样本数 | 最大单次CPU墙钟ms | 对应线程 |
|---|---:|---:|---:|
| TokenWork | 7,607 | 79.0439 | 69280 |
| CompositionPrepare | 117,198,592 | 26.1405 | 98700 |
| CompositionCommit | 234,397,099 | 0.2121 | 98700 |
| SrDispatch | 7,894 | 186.7367 | 98700 |
| SurfaceLockWait | 12,736,046 | 0.0951 | 98700 |
| CompositionLockWait | 477,758,180 | 0.1457 | 98700 |

SrDispatch最大调用自己的边界1251738953264..1251740820631，整数差
1,867,367 ticks；打印max_ms=186.737是六位有效数值，不覆盖精确端点差。
不能把不同阶段的最早max_begin与最晚max_end拼成一次数百秒长调用。
各阶段inclusive且可能嵌套，sum/max均不能相加或相减当作纯hook开销。
采集器Record、部分时钟/线程查询在scope外，报告也没有线程busy/GPU时间。

海量Composition/lock调用计数给出优化观察频率及对照timing off/on的明确方向，
不直接证明丢帧根因。锁max只是已接受样本，不能由其短就排除漏测或其它锁。
ShaderHit91662/Miss0是shader身份缓存，BindingCapture7897没有绑定缓存命中证明；
Present index10800也不是实际NativeRender样本7441的替代分母。
F7主会话统一处理F6绑定缓存对照、产品修改与真实性能验收。

同局qualification首条记录仍为postpass raster subset拒绝，随后有reservation
revoked及非三index拒绝；这些是first-only诊断，不能把三条计作失败发生总数。
本分析不调整raster门禁、writer版本、jitter/reset或资源退休规则。

rebuild_evidence.py的runtime profile将本节限定窗口、四项vtable前缀、RTTI、
调用锚点和相关IAT观察保存为证据；计数与哈希以runtime/manifest.json为准。
0x9c1e0的5-byte cached getter是leaf，不因没有.pdata而否认其存在。

公开接口对照来自本地Windows SDK 10.0.22621.0的um/d3d11.h和d3d12.h，
不是从私有vtable名字猜方法。Win64偏移为：Device::CreateQuery=0xc0、
GetImmediateContext=0x140；Context::GetDevice=0x18、End=0xe0、GetData=0xe8；
Fence::GetCompletedValue=0x40、SetEventOnCompletion=0x48。
这些公开接口偏移不认证未知对象身份；仍需上述构造、成员与原字节数据流配合。

只刷新runtime、不调用FXC的命令：

    & D:/APPS/conda/python.exe -B tools/research/encore-f7/rebuild_evidence.py --reference-root D:/CODE/DLSS5_NEXT-workers/f7-clean-rewrite/build/research --profile runtime --windows-sdk-include 'C:/Program Files (x86)/Windows Kits/10/Include/10.0.22621.0/um'

当前runtime总窗口/指令/锚点计数以runtime/manifest.json为准；其中D11Completion
五窗口共283条，FG的CPU等待位于既有窗口，不重复加计。
新合同在evidence/runtime/dx11-completion-contract.json；四个原profile的shader
及既有参考编译产物保持独立，未把这次CPU解码写成shader重编译或GPU验证。
