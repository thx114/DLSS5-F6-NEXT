# Encore generic SR 与时域输入诊断

2026-11-07。固定DLL身份同README，以下为RVA。只读静态证据和独立离线数学，
没有修改F7产品、调用SDK/GPU或操作游戏。F7当前画质反馈不能由此宣称修复。

## 原生generic SR实际准备的参数

0x35dc87调Create wrapper；0x35e34a明确写featureId=1，0x35e352经传入函数指针调用。
0x35e1e4调Evaluate wrapper，0x35e38e经函数指针调用，回调参数r9在0x35e382置0。
调用者在0x35e1cd把本地参数容器rsp+0x20作为Evaluate参数传入。
动态API名的既有解析证据仍由F7 REFERENCE-RESEARCH索引，本次不加载目标验证。

|key|静态准备值/来源|关键RVA|
|---|---|---|
|Jitter.Offset.X|容器标量0；前面的movsd是在复制key字符串，不是读jitter值|0x35dfe1..0x35e024建key，0x35e068..0x35e06e写0/tag1|
|Jitter.Offset.Y|xmm2清零后写入|0x35e072..0x35e081|
|Sharpness|xmm2清零后写入|0x35e086..0x35e095|
|Reset|输入对象r14+0x3a非零转double 1，否则0；上游producer仍未知|0x35e09c..0x35e0b7|
|MV.Scale.X / Y|xmm6中的double 1|0x35e0bc..0x35e0df|
|DLSS.Pre.Exposure|同一double 1|0x35e11e..0x35e12d|
|DLSS.Exposure.Scale|同一double 1；不是preExposure同名字段|0x35e132..0x35e141|

1来自0x35da01加载的0x53fba0原始double。共同numeric helper 0x14f1d0
在0x14f1f7保留传入xmm2，在0x14f27c写容器qword数值并设置tag1。
本包runtime profile直接重解码准备窗口/两个wrapper/helper，断言56个指令锚点、
8个ASCII key（含NUL）及1个double常量。它们证明静态准备值，不证明NGX实际接受、
具体版本逐字段使用、资源正确或游戏画质。不能把Jitter.Offset.X地址的ASCII字节当浮点常量。

这条generic链处理已准备的颜色/guide；不能因为它用jitter=0、MVScale=1就让F7的
原生FSR输入照用这些常数。判断F7应以该次真实Evaluate、同源guide和实际projection
为准；也不能用Sharpness常数给本次用户主动选择0.4的对照值归因。

## 离线代码及需要的观测

sr_temporal_probe.py是独立诊断数学，不是恢复的Encore实现。可直接输入两组raw token
值及调用方记录的实际SDK像素jitter，也可读取两个完整CB中显式指定的projection候选。
不新增SDK调用或产品日志；全部JSON、哈希与精确有理数计算在离线执行。

    & D:/APPS/conda/python.exe -B tools/research/encore-f7/sr_temporal_probe.py --raster-size 1000 500 --raw-previous 0 0.002 --raw-current 0.001 0 --sdk-previous 0.5 -0.5 --sdk-current -0.5 0.5

上例全部是合成值，说明0.5-raw*extent的绝对残差为0；-raw*extent只有差值相同，
绝对残差为-0.5。工具逐轴列出raw、-raw、±raw*extent、0.5-raw*extent、
±raw*extent/2七个假设，不排名或自动选择。

**差值不能辨固定bias；同两时点raw/SDK绝对值则能比较传参公式的bias。** 后者仍不能
证明物理上应采用哪个bias/符号、真实producer单位或source/frame关系。
只有raw预览而未提供SDK值时，相应SDK comparison为空，不伪造“实测匹配”。

投影按声明layout转换为row-vector，要求精确稀疏形式：

    [sx,0,0,0; 0,sy,0,0; ox,oy,A,s; 0,0,B,0]

以有符号s归一化，主点NDC为(ox/s,oy/s)。D3D左上原点、x右/y下时，同一个
viewport的主点变化对应dx=W/2*dNDCx、dy=-H/2*dNDCy。前提是共同view basis、
相同viewport尺寸/原点/方向及其它系数稳定；都由调用方提供，不因数值匹配获得认证。
斜投影、skew及仿射项即使极小也拒绝，不用近似稀疏形状抹掉它们。

非offset系数以原始binary64精确比值比较，默认tolerance=0；可显式给非负容差，但
原始残差、exact ratio与精确相等状态仍保存。显示float残差若下溢为0另设标志。
中心差先对精确比值相减，再转为float，避免归一化商舍入碰撞造成假零。
不可表示的归一化、中心/像素差溢出或下溢明确拒绝。其它系数变化超过容差时，
不生成raw/projection比较。只保留主点差不能识别jitter、lens shift或crop的真正来源。

完整CB模式使用--previous-cb/--current-cb、--projection-offset、--storage/--vectors；
offset是完整资源绝对字节，不重解释Context1 range。未给--raster-size不计算像素值，
未给raw/SDK值不猜它们。CLI拒绝输出覆盖任一CB，包括hardlink别名。

## 既有标题样本及边界

verify带既有--capture-directory时，另输出evidence/f7-title-projection-shift.json，
输入仍是README固定目录的capture-1/2.vs-cb2.bin，整个资源560B、offset80。
row-major/row为假设，归一化非offset差精确为0，主点NDC变化：

    [+0.0001627849997021258, -0.00046292098704725504]

metadata无对应viewport尺寸、raw jitter和SDK jitter；本报告不补造这些值，不沿用
新游戏局的1536x960，也不把两capture当已认证相邻帧。原输入只有observed-only身份，
不是当前世界/相机证据。该变化可能来自jitter或其它投影偏移，原因仍unknown。

新26项CPU回归覆盖实际齐次点投影、LH/RH、D3D Y、全局比例、布局/资源offset/hash、
公式绝对/差值残差、固定bias歧义、输入拒绝、溢出/下溢与商舍入碰撞。
无真实SDK调用、GPU或SR画质验收；当前F7稀疏日志字段由主线程维护。

## 当前F7日志接点与读取限制

只读核到F7专用worktree src/backends/DlssDx12.cpp的[dlss-input] logger，
它在真实evaluate返回后保存该次输入与result：token_jitter、token_jitter_encoding、
sdk_pixel_jitter、sdk_mv_scale、requested_reset、sdk_reset、render/display，
并携带history_slot、instance/context/generation/token_frame、sdk/frame/owner、
queue/epoch/fence_target、flags/formats/frame_time_ms。记录不证明GPU完成。
采样条件为每slot前8次或每600次，不天然是相邻真实帧，不拿相邻日志行当相邻source。
后续字段可能由主线程改动，以当前源码/该局DLL为准；工具不内置旧F6日志parser。

原始raw与SDK绝对值已同时存在于这一格式，离线可分别传给上文--raw-*和--sdk-*。
比较前须核同一局PID/FILETIME/DLL/config、实际同source关系与共同render尺寸，
不能跨history slot、generation、epoch、配置或resize直接做差。
最终参数值可以核转换公式，但仍不证明原始producer单位、projection身份或正确物理符号。
日志当前由f7/Log.h的默认ostringstream输出，未设置float往返精度；残差有打印量化误差，
不能以机器epsilon或精确非零残差就断言产品转换错误。需要逐位诊断时由主线程另存
实际float位型/足够有效位，离线工具不补造精度，也不自行修改产品日志或增加采样。

Sol独立关闭初版归一化商碰撞P2：最终使用精确比值而非浮点商判等，原40px反例
与正常数商碰撞均已加入回归；独立26项测试通过，另核48组齐次点、16组布局/offset/SHA、
14项有理数模型。上述额外内存检查未交付为文件、不计统一测试。
新增538条native窗口及runtime2188条/56锚点/8key/1常量已由Sol直接复核。

## 逐行日志consumer与真实故障局重放

sr_log_probe.py只解析F7的[dlss-input]完整字段；支持已核早版缺encoding字段和当前版
有encoding字段两种，不给缺失encoding补默认，不采用旧F6 SR32格式。未知、缺项、
重复字段、非有限值或截断dlss行使输入失败。它不比较两行，不要求稀疏日志相邻。
每个[log] F7 rewrite log opened边界切独立log_segment；相同identity也不跨段合并。
无header的首段标未观察到opened边界，segment不是认证的进程session。

    & D:/APPS/conda/python.exe -B tools/research/encore-f7/sr_log_probe.py --input '该局/session/F7.log' --session-receipt '该局/session/result.json' --output '离线报告.json'

每行逐轴比较七公式的绝对值。默认声明6有效位defaultfloat，保守采用一个完整decimal
quantum（不是半bin）及宽松float32终值舍入余量；--significant-digits可声明1..9。
打印中心可能略超FLT_MAX，故以打印区间与有限float32域相交来验证；不直接拒绝中心。
MVScale两轴与frame_time也按同一significant-digits验证；负frame_time仍拒绝，原始
打印字符串保持不变。低精度的完整quantum区间可能很宽，例如1有效位的4e38仍与
有限域相交；这是声明模型的过近似，不证明该字符串来自真实float32打印。
零在该打印模型下假设为真实零。格式/locale/舍入模式不由工具认证；变更后须另声明合同。
结果名intervals_overlap_under_declared_print_model表示区间相交，**不是某个真实
float32执行的存在性witness**，更不是物理jitter正确。模型刻意overapproximate，
不排名、不选择公式；打印中心的小残差不当产品bug。

可选session-receipt绑定sidecar原始SHA与其中报告的PID/FILETIME、磁盘DLL、配置哈希，
仅为调用方选定的关联；dlss-input行自身不证明该进程身份或映射内存哈希。输出保护
同时覆盖日志与receipt，含hardlink别名。原始行、数值字符串、identity与opened段均保留。

实际只读重放目录：F7 worktree/build/performance-genshin/sr-M-sdk-input-20261106T225633。
session/F7.log与runtime/F7.log同SHA256：

    a43874b29ea9f0ad216d6860127445be4cbe4db58067cf898db25271a7971ee5

22条记录，UTC2026-11-06T22:58:32.467Z至23:00:05.712Z，一次opened；均为早版无encoding。
render1536x960、display2560x1600，记录的SDK MVScale均1536/960；result字面码均1，
sdk_reset有7条1和15条0，不按这些稀疏样本判定世界历史链正确。
在默认声明模型下，raw*extent两轴各22/22区间相交；0.5-raw*extent在X为1/22、
Y为0/22，其它列出的公式均0/22。这确认这些已打印值与旧传参形式相容，不说明
producer物理语义、应选符号/bias、SR显示/画质或设备故障原因。

sidecar报告PID23412、FILETIME134384794461024050，磁盘DLL A872E044…、
INI a49f19da…；完整值和receipt SHA保存在evidence/f7-sdk-input-log.json。映射内存
完整SHA仍unknown。本局USER_FAIL和设备故障事实以主线程failure-summary为准，
本工具不授予GPU完成、不归因TDR、不建议恢复已暂停的GPU实验或操作旧PID。

verify添加--sr-log与可选--sr-session-receipt可重新生成此报告并绑定输入/输出哈希。
它在首次mkdir、测试日志、静态重解码或FXC输出之前保护所有显式输入；evidence是
生成目录，拒绝其中的输入以及现有生成件的hardlink/目录联接别名，不让cpu-tests或
其它先写的文件覆盖尚未读取的日志/receipt。CLI与统一验证均保留只读原件。
40项SR日志测试覆盖格式、interval、log段、precision边界、非jitter有限域、sidecar
及拒绝路径；另12项统一验证测试使用真实文件、hardlink和Windows目录联接验证输入保护。
