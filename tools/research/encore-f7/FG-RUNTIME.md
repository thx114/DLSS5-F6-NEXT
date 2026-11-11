# Encore FG：配置、能力和呈现计数

2026-11-07，北京时间。目标为README中的固定DLL，SHA256
06a7fd63ee0f0e95574f3869d70c3c9ae4aaf326ba53ff8c864064410a5f1217；
ImageBase 0x180000000。下文地址均为RVA，仅限定静态逆向。

## 离线代码

fg_state.py 仅用标准库，严格解析 DLSSGState v4 / Win64 / pack8 / 88B快照。
错误GUID、版本或长度均拒绝，不猜布局；不加载DLL、调用SDK或解引用next/fence。

    & D:/APPS/conda/python.exe -B tools/research/encore-f7/fg_state.py --state 'snapshot.bin' --enabled true --requested-total 6 --get-state-result 0 --configured-generated 1 --set-options-result 0

snapshot.bin 必须由调用方另行提供。本包没有实际SDK状态样本，测试全部合成。
例中的请求6与实际配置generated1刻意不同，不表示F7已发生回退。只有调用方独立确认
精确查询窗口与其中成功base Present数后，才添加 --base-presents N --exclusive-query-window。
所得sdk_interval_ratio也不证明逐source输出、不同画面、外部显示、持续6×或资源退休。
配置数、调用结果和窗口独占性均由调用方提供，工具不认证其来源。

fg_query_ledger.py 补充多次查询的离线JSONL检查，不再只靠单次调用的
exclusive_query_window。它接收所有既有GetState调用的边界，保留当次raw count，
用本次SDK计数除以同窗成功base Presents的增量；不对两次raw count作差。

    & D:/APPS/conda/python.exe -B tools/research/encore-f7/fg_query_ledger.py --input queries.jsonl --output query-report.json

目前只有test_fg_query_ledger.py中的合成记录，没有真实查询ledger。工具不调用SDK、
写入产品或认证输入。CLI拒绝重复JSON键、未知/缺失字段和覆盖输入文件。

### schema 1：每个实际GetState尝试一行

|字段|记录者合同|
|---|---|
|schema|整数1|
|session_id / pid / process_creation_filetime|唯一SDK session及真实PID/创建FILETIME；SDK重启或进程重启更换session_id|
|viewport_id / owner_id / display_epoch|实际viewport、display owner、显示生命周期递增epoch；不可复用旧owner身份|
|runtime_tuple_sha256|固定规范顺序的实际加载DLL名/版本/SHA256集合清单之SHA256，小写64位hex；本字段不认证清单，外部保留清单|
|query_ordinal / query_site / query_qpc|同session所有viewport、所有调用点共用严格递增ordinal；每次尝试先取号，失败也记录；site为用途，QPC在串行调用边界读取|
|base_present_total|同viewport/owner/display epoch已成功S_OK的真实base Present累计数，uint64，不是FG生成数、attempt数或显示帧数|
|dropped_total|同session累计丢记录数，永不回退；缓冲溢出也要增加ordinal与loss计数|
|all_queries_recorded / query_boundary_serialized|bool，记录者明确保证所有读者纳管、查询与相应Present累计边界串行；不确定就false|
|options_epoch|同session递增的配置连续性epoch；有效mode/generated/queue/flags变化、失败/异常/未知或离开后恢复都必须递增；完全相同且成功的重复SetOptions可保持|
|options_mode / options_generated / set_options_result|当前已知配置为unknown/off/on；uint32生成数或null；支持当前配置的最近SetOptions结果或null。成功时off必须0、on必须正数；失败/未知不沿用上次accepted值|
|requested_total|用户请求1..6，与实际accepted配置独立|
|get_state_result / status / sdk_presented / max_generated|当次原始uint32；失败输出仅归档，不认证。options传nullptr；若以GetState(options)改变配置，也必须更新options_epoch|

其它uint字段按脚本限定uint32/uint64；session/owner/site为非空字符串（至多256字符）。
base Present累计数在其scope内不得回退；ordinal在session内不得重排/重复。
QPC、loss、options/display epoch回退使整个输入失败，不能靠排序“修复”原件。
多session可交错，单session跨viewport也不能先分组；中间另一个viewport查询会拒绝跨越窗口，
这是在未证明provider计数作用域前的保守处理，不宣称SDK计数必然全局。

首个查询只建基线。缺号、loss增加、配置epoch/tuple/owner/viewport改变使跨越窗口未知；
失败或非零status使两侧窗口未知，后续两次相邻干净查询才重建候选窗口。返回字段无效、
max变化、accepted大于max、配置未知、无base Present或独占性未证均不算比率。
输出exact sdk_interval_fraction保留整数分子/分母，sdk_interval_ratio仅便于展示。
即便候选窗口比率为6，也不产生持续6×、逐source生成、scanout或退休证明。

记录建议：在现有两处SDK调用周围记录固定大小POD至有界single-writer缓冲，ordinal覆盖
每次尝试，溢出显式loss；session/tuple字符串在冷路径建表。离线或受控冷路径导出JSONL，
不要在Present路径分配JSON、哈希DLL、flush文件、等待consumer或新增GetState。
本交付只提供离线分析器，尚未实现/编译原生采集器，也未测其CPU开销。

## 请求与实际应用闭合链

- 0xe97de/0xe97e5 将0xdc130安装到0xd88398，0x14fcc调用回调。
  0xdc27f..0xdc287返回packed uint64：低32位请求总倍率，高32位策略最大总倍率；
  消费者先将最大值限制2..6，再限制请求。策略最大值含UI、架构/资格分支，
  尚未闭合为GetState.maxGenerated+1，不能当实测能力。
- 0x1504a保存desired total到0xd66054。0x15050..0x15077对比失败锁存0xda1158，
  同一失败倍率改试2×；选择2×的分支清除锁存，0x15182记录失败倍率。
- 0x150a3计算appliedTotal-1，0x150b0写生成数，0x15115调SetOptions。
  多帧失败时0x151e7先Off，0x15200将generated置1，0x15207重试；
  只有成功分支0x15238才向0xd66050提交实际应用总倍率。

可借鉴：分开 requestTotal / attemptedGenerated / SetOptionsResult / acceptedTotal /
GetStateResult / runtimeStatus / reportedMaxGenerated；回退2×不能显示为请求6×成功。
完整恢复时机、会话状态及实际provider行为仍未闭合。

## 计数陷阱与原始值

本地SDK头明确 numFramesActuallyPresented 是自上次slDLSSGGetState以来的区间计数，
不是累计总计，也不天然等于逐源倍率。0x1421e调GetState，0x14643起取maxGenerated并
限制callback参数最多5；0x1465d/0x14661/0x14664传presented/status/调用结果，
0x14667经0xd88390调用0xf5eb0，再转交0xf5cc0。回调注册见0xe97d0/0xe97d7。

0xf5dbd将新actual读到esi；0xf5dcd比较它是否小于2。若新值0或1且enabled标志
0x13d3879成立，0xf5dd6取旧0x13d4d68，0xf5ddf跳过更新；其它分支才在0xf5de9
写新值并更新时间。因此UI/遥测可能保留过去的较高值。0x14777 present trial及
0x148e3 receipt日志均不证明外部显示，后者明确scanout=unmeasured。

本工具保留当次raw count，包括0；不沿用旧6。若另做UI平滑，须与原始计数、
窗口和过期状态分开，不能将平滑值用于生成或退休判断。额外GetState读者会扰动区间。

## v4 Win64 ABI

|偏移|字段|类型|
|---|---|---|
|0x00|next|pointer64，不解引用|
|0x08|GUID|16B|
|0x18|structVersion|size_t64，严格为4|
|0x20|estimatedVRAMUsageInBytes|uint64|
|0x28|status|uint32|
|0x2c|minWidthOrHeight|uint32|
|0x30|numFramesActuallyPresented|uint32，区间计数|
|0x34|numFramesToGenerateMax|uint32，生成数上限|
|0x38|bReserved4|Boolean8，保留不解释|
|0x39|bIsVsyncSupportAvailable|Boolean8|
|0x40|inputsProcessingCompletionFence|pointer64，不解引用|
|0x48|lastPresentInputsProcessingCompletionFenceValue|uint64|
|0x50|bIsDynamicMFGSupported|Boolean8|

GUID字节 e1c88acc79a1f54497fae74112f9bc61；Boolean只有0/1解释为假/真，其余未知。
固定6×需要generated5及reportedMax>=5，dynamic MFG位是另一能力。返回指针/value
或API成功本身都不证明实际输入使用已完成。

## F7实际交集与缺口

### FFX输入到Streamline tag的限定桥接链

新增九个原件重解码窗口共456条指令、54个指令锚点，见runtime中的
fg-ffx-*、fg-frame-slot-*、fg-sl-*与fg-resource-tag-contract.json。
这条链处理显式FFX descriptor；它不是D3D11 Draw输出消费证明，也不是绑定缓存。
以下结构名/枚举对照本地固定的FidelityFX与Streamline公开头；头文件SHA写入manifest。
0x90/0xb8是二进制中的读取长度检查，没有原生编译证明跨版本ABI相容。

- 0x827a..0x840c读取0x90字节configure描述符，须ReadProcessMemory成功、
  完整长度、type=0x20002并启用FG，再按context与frame ID选择slot。
  非空HUDLessColor经state转换0xef30，调用0x15ad0，tag type=2；
  command list参数为null，最后的volatile参数为false。
- 0x84b1..0x86dd读取0xb8字节prepare描述符，须完整长度与type=0x20004。
  通过各上下文门禁及slot查找后，保存本次descriptor；另有两个功能开关调用
  0x85ea/0x8627，其完整配置语义未闭合，不据此宣布所有prepare都准入。
- 0x87d4..0x88ac取prepare的depth和motionVectors原生资源/state，分别以
  tag type=0/1调用0x15ad0，两个extent均为本次renderSize，command list
  来自本次prepare，volatile参数为true；任一步失败走明确诊断分支。
  不把只保存了prepare或已设置bit当作SL已成功使用资源。
- slot helper 0xffa0的限定入口0x1000c..0x1005a，用低三位选八槽、stride=0x2b0，
  已占用槽还要比较完整frame ID；相等才在0x10364返回对应槽。
  不同ID且特定未闭合状态组合会拒绝复用。这里只闭合key检查，不证明全部slot
  初始化/清理、帧连续性、GPU完成或可直接给F7增加八槽。

0x15ad0先调0x16050。后者以D3D12 Resource::GetDesc检查texture2D、
sample count=1、非零extent及范围，再GetDevice并和当前owner设备比较。
GUID原件为ID3D12Device；两个公开vtable slot由本地d3d12.h显式C声明重核。
不能把这些检查套在尚未证明类型的D3D11接口上。

volatile=true时，0x15b51..0x15b75要求command list非空且QueryInterface取得
ID3D12GraphicsCommandList；失败则不提交tag。随后构造sl::Resource与ResourceTag，
两个GUID原件也已核对。Resource包含输入state，extent含调用者的base/width/height。
tag lifecycle由volatile xor 1写入：prepare的true对应0=eOnlyValidNow；
configure的false对应1=eValidUntilPresent。二者不能互换，不能以生命周期枚举
替代真实资源所属queue/fence或准许提前回收。

0x10bbe..0x10bd8按字符串slSetTagForFrame解析，写全局0xda1088；
resolver 0x12d60..0x12dbd还检查解析地址所属模块。0x15c38从该全局调用，
RCX为slot+0x20中的frame token，R8为单个tag地址，R9D=1，栈参数为上述
command list接口；只有返回0才进入相应tag bit更新。不因bit置位便推定逐源
生成、最终画面消费、scanout或退休。

对F7可用的方向是：现有真实源身份准入之后，分别保留frame key/token、
tag type/extent/state、生命周期和实际SetTag结果；不要将FFX回调输入直接升级为
最终显示颜色证书。这也不能据以放宽当前postpass raster/版本/写者门禁。
固定DLL中这段FFX→SL桥没有为F7的D3D11 shader绑定缓存或SR→最终backbuffer
消费提供现成证据。没有执行此链、加载Encore或改F7产品。

只读核查 StreamlineFg.cpp：SelectStreamlineFg约177..189已按generated=total-1
对比maximum，无产品上限1硬编码；当前RefreshCapabilities约677..685与AfterPresent
约1465..1475均调用GetState（行号会随主线程编辑漂移）。
后者正确保存sdkPresentsSinceLastQuery；前者只发布max/min/status，
未保存该次计数/窗口。需要解释区间比率时，应由同一query owner记录所有查询的
ordinal/时间/用途及对应base Present，不在每帧增加第二次SDK调用或阻塞IO。

此前文档记录曾声明固定tuple maxGenerated1；本次所读CURRENT-STATE.md已更新，
没有保留该原始能力字段。本研究不据此改变固定tuple策略或推导新能力。
但DS找到的旧x2-srgb回执（2026-11-06T12:51Z，PID72536）未保存原始max字段，
其numFramesToGenerate=1是请求数，不能证明上限1或5；它还含interposer2.13/plugin2.14.16
混合身份，不能冒充当前固定tuple。当前max1原始观测来源交主线程确认，不因缺证开放6×。

追加定向搜索：固定目录及两个清单引用的stage目录未找到晚于2026-11-06T12:51Z、
同tuple的raw max运行回执；这不是全build穷尽检索。直接读到staged-identities.json
utc=2026-11-06T12:48:50.3404402Z，profile为interposer2.13/plugins2.14.16，
runtimeVersionKnown=false、gameValidated=false，无PID/GetState原始max；stage身份不能
升级为能力证据，也不能据此撤回主线程可能另有来源的max1结论。

优先在已有查询处落盘原始cap/结构版本/返回码与DLL tuple身份，再判断provider还是配置
选择问题。Encore顶层包未包含实际provider DLL；兼容枚举、白名单和hint不是已实现6×。

## 输入消费 fence 与 CPU 等待是不同合同

固定Encore的post-Present函数0x140d0..0x14b04已完整解码；本节只恢复其中
插件输入fence分支，不执行它。局部条件要求owner byte+0x18为0、当前slot的
byte+0x586非零、状态查询结果为0且返回fence非空，才可能进入以下等待。
这些原始字段的完整配置含义仍未恢复，不能将它描述成每帧必然等待。

状态对象在栈上以v4构造；相对对象+0x40的fence和+0x48的目标值分别被取到
RDI和R15。目标为0直接跳过等待。非零时0x14275调用fence的GetCompletedValue，
0x14278/1427b以无符号`completed >= target`跳过事件等待，**该窗口没有先拒绝
UINT64_MAX**，因此该位型会通过比较。这个字节事实不表示运行中已发生设备丢失。

pending时分配事件、调用SetEventOnCompletion，再以5000ms调用单对象等待：
事件创建失败、SetEventOnCompletion负HRESULT、或等待结果非0均走局部失败；
等待结果0则继续。不能把所有非0等待结果都标为timeout。事件非空时调用关闭，
但关闭返回值未被检查；等待成功后、跳到0x142f4之前没有重新查询fence。
局部失败调用fallback helper后返回0x887a0006；内部fallback和所有更上层失败传播
并不由该窗口完整认证。CPU等待返回、GetState状态和资源安全退休各自保持独立。

### 当前F7对照与已有保护

本次只读的`src/framegen/StreamlineFg.cpp` SHA256为
`ee14f85a1f3355aed269d3577b26ce594bea2c11e5bf178874a2e979589af008`。
观察到的源码已经实现两条完成条件，不能把“添加Encore式5000ms CPU等待”
当作填补其缺口的默认方案：

- AfterPresent要求匹配ticket、proxy、Present线程和S_OK，再使用一次既有GetState
  查询；保存返回的SDK fence、target与实际完成值。非零SDK target要求有效fence；
  target和完成值的UINT64_MAX都会隔离，不能当作已完成。
- SDK输入fence与client presenting queue的post-Present尾target分开保存；
  尾Signal失败会隔离。TryRetire必须实际观察自身execute/tail完成以及适用的
  SDK输入target完成，再清tag和释放本source的输入lease。
- 零SDK target不等于没有生成。只在该source成功SetOptions记录了明确block
  presenting queue请求、真实producer与client queue相同且相关资源/recording/
  producer lease仍完整时，才使用受限的blocked-presenting输入退休路径。
  若SDK仍返回了零target fence，它也会被pin并检查移除sentinel。
- 这条受限规则来自当前公开头的presenting-queue合同，不是从Encore跳过零值
  分支推导的许可。foreign queue、未知/未成功配置或上个source的模式不得借用。

所读`external/streamline/sl_dlss_g.h` SHA256为
`075bc65a5d30a59e158d3c1ea4dff34f8335bc16486c31b33394e9270ee59e8c`；
其说明明确区分presenting和non-presenting队列以及eBlockNoClientQueues。
这份头和源码观察不认证当前实际加载provider、它是否遵循合同、生成倍率或画面。
`TelemetryContractTests.cpp`已有对应零target、尾target pending、错误模式与sentinel
反例；本轮仅阅读其源码，没有执行这些原生测试或借用其历史通过数。

F7的未完成项仍是当前source/颜色消费资格、同tuple真实能力、SDK区间记录与
实际显示验收。逐source输入退休不代表SDK全局/pacing/generated work已经终结，
也不授权Shutdown或卸载；CPU墙钟等待次数/时长不证明GPU瓶颈或输入消费因果。

`evidence/runtime/fg-input-fence-contract.json`将本节绑定到97个独立指令字节锚点、
四个Kernel导入、两组函数/pdata/unwind、两个公开Fence槽与12条路径。
其中resolver的0x14d1d..0x14d68是额外的有界原字节绑定，不作为新增解码窗口
重复计入runtime指令数；post-Present正文仍使用既有完整窗口。report/manifest同时
保存这些独立计数，反例测试覆盖字节、timeout、查询guard、导入、边界与SDK槽漂移。

## 验证

evidence/runtime/manifest.json绑定DLL与SDK头哈希；runtime-anchors.json保存已核指令断言
的实际字节，当前数量以manifest为准，不沿用最初30项的旧计数。它们不证明完整调度、
异常清理或运行成功。fg_state.py的9个合成CPU测试覆盖ABI、
错误结果、非零status、配置/能力分离、区间与0计数；Sol独立复核未发现确定P1/P2，
另核ctypes布局、Boolean、64位版本高位和padding。无真实SDK/GPU/性能/显示验收。

查询ledger新增30项合成CPU测试，覆盖能力查询截窗、零计数、失败两侧、漏号、记录丢失、
options变化/回退、跨viewport/session/owner/tuple、精确64位累计差值及输入拒绝。
Sol独立复核最新代码未发现确定P1/P2，独立实跑30项测试及200条内存断言；内存断言
未交付为测试文件、不计入当前统一测试数。写者另实跑CLI，验证成功/零计数报告、输入SHA256、
同路径与hardlink别名输入保留、重复键失败不写报告；临时合成文件已移除。
