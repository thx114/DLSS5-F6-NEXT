# Encore 直接 NR payload 与 F7 core 路由的差异

2026-11-07，北京时间。目标 Encore DLL 的固定身份见 README。以下均为静态 RVA、
原文件字节和 F7 当前源码观察，不加载 DLL、不初始化 SDK、不执行 GPU。

## 已闭合的直接调用表

Encore 的 NR 初始化不是仅把 feature18 交给 SR 的 driver core：

|阶段|原始位置与行为|
|---|---|
|选择文件行|0x35731d 取两行表0x71b7a0，逐行跨度0x68，按文件SHA文本选择；表中版本只是历史标签|
|加载|0x3574ae 调0x35a650，以LoadLibraryExW取得模块，返回值保存为r15|
|已加载身份|0x35755b检查已加载文件与此前验证文件；失败分支FreeLibrary并拒绝|
|解析导出|0x357584..0x3575e4 对同一模块GetProcAddress，取得五个下列入口|
|提交表|0x357f56..0x357f80 将五个指针依次写全局state+0x70/0x78/0x80/0x88/0x90|
|访问表|0x3553a0 调全局state访问器，返回state+0x70|
|Init|0x151ed4取上述表，0x151ef7调用首项；r8为device、r9d字面值0x15、第五参数为0|
|传入NR|0x15225a取同一表，0x15226a置stack+0x60，0x1522c5传入NR入口|
|Create/Evaluate|0x157f13调用表+8，feature编号18；0x158c3c调用表+0x10|

五个精确导出名为 D3D12 的 Init_Ext、CreateFeature、EvaluateFeature、ReleaseFeature
与 Shutdown1，均以 NVSDK_NGX_ 前缀导出。这个对应关系来自导出字符串、临时寄存器/
栈槽、state写入和table getter，不以函数名字相似或DLL文件名猜测。

Init调用的应用ID属于Encore，不能移用于F7；0x15也是该固定调用点的值，
不是对当前所有payload有效的SDK版本建议。Init_Ext的driver/public顺序差异、
Shutdown1参数数目、跨模块参数对象与异常清理仍须对实际选定payload单独核证。
本批没有给出可直接执行的私有接口声明，也不建议在同device上盲开第二套NGX。

## 五个导出之前还有模块路径查询替代层

定向补查已闭合一层此前遗漏的加载工作。本节地址仍为固定Encore DLL的RVA，
不是下文历史CDF NR参考的RVA；只读取原字节，没有实施任何模块改写。

| 阶段 | 已核原始位置与数据流 |
|---|---|
| 固定载荷基址 | 0x3574b3保存LoadLibrary返回值r15；已加载身份检查通过后，0x35757f存至rsp+0x68 |
| 同一全局state | 0x356fac调用与API表访问器相同的0x35a450，0x356fb1将结果保存在rsi；替代函数的fallback也调用同一访问器 |
| 查找导入槽 | 0x3575e8..0x3575ff以该载荷模块及GetModuleFileNameW名字调用0x355750，将返回槽地址存rbp-0x20；随后同helper查GetProcAddress |
| 槽查找器 | 0x355750..0x3557f1按PE Import Directory遍历导入名/对应IAT，名字匹配时返回对应槽地址，不是直接返回导出函数地址 |
| 保存原函数 | 0x357c9f..0x357ca6从上述槽读取原函数指针，保存到同一全局state+0x148 |
| 安装替代函数 | 0x357cad取得0x356c30，准备8-byte写入，经0x357cc4调用0x35ab30；该次返回失败不进入其成功分支 |
| 提交顺序 | 0x357ccb成功后至0x357ed0；之后可跳过或执行另一GetProcAddress导入处理，成功路径才到0x357f2f并提交五个payload API指针 |

0x35ab30..0x35abcf先请求写保护变更、调用字节复制helper，再恢复保护并
FlushInstructionCache；返回值还检查恢复/flush是否成功。报告只证明这个
静态顺序，不证明运行中改写已执行、失败后完全回滚或该状态可安全卸载。

替代函数0x356c30..0x356d03的行为有明确作用域：

- 以flags=6和本Encore模块内地址0x35a680调用GetModuleHandleExW，随后比较
  传入HMODULE是否等于所查到的自身模块；不是把任意模块名都改成同一个名字。
- 对匹配模块，只有输出buffer非空且容量至少10个WCHAR时，才以五次32-bit写
  形成NUL终止的宽字串nvngx.dll并返回9。它没有加载或查找一个新的nvngx.dll文件。
- 匹配模块但非零容量小于10时，写空首字符、SetLastError(0x7a)、返回传入容量。
  这个受限实现不代表已认证完整Win32 API等价行为。
- 其它情况尾调用state+0x148保存的原GetModuleFileNameW；原函数生命周期、
  并发卸载以及另一GetProcAddress替代路径仍不能由这个窗口单独证明。

这层解释了为什么不能把Encore的NR接入简化成LoadLibrary加五个GetProcAddress。
与CDF参考的caller-path子串检查结合时，可推断这种模块路径查询替代会影响该类
检查读取到的路径；但CDF SHA不等于Encore表内历史行，本包没有执行任意一方，
不能将这个机制关系写成“已证明Encore实际调用CDF载荷成功”。

F7仍需选择实际载荷、路由、合法ABI与生命周期合同。这里不提供可移植的导入
改写实现，不把该层当作F7已具备的能力，也不将它视为NR硬件支持、GPU完成或画质。
可重生成证据为runtime/nr-payload-import-*.json、nr-payload-module-name-*.json、
nr-payload-protected-write.json及nr-payload-import-contract.json；新增八窗口225条
指令、50个锚点、两个精确ASCII导入名和五个IAT断言。完整加载器CFG、其它兼容
处理和异常清理仍未恢复。

## 参数对象在本模块内构造

0x14260b..0x142642 将vtable0x599750放到runtime+0x1f8，并初始化自身容器。
MSVC RTTI的locator0xcf8248、type descriptor0xd81188及名字
Parameters@nr_runtime把这张表与自带参数对象绑定。它不是本批观察到的
core AllocateParameters返回值；也没有因此证明两者不可互操作。

|槽偏移|实际目标RVA|已读行为|
|---|---|---|
|0x00/08/10|0x15a690|共同跳到资源/指针存储helper0x14f550|
|0x18|0x15a650|低32位有符号数转double，再到0x14f1d0|
|0x20|0x15a660|低32位无符号数转double，再到同helper|
|0x28|0x15a680|double原样到同helper|
|0x30|0x15a670|float扩为double，再到同helper|
|0x38|0x15a6a0|uint64转double，再到同helper|
|0x40/48/50|0x14cc70|指针读取路径|
|0x58/60/68/70/78|0x14c920/14c9f0/14cba0/14cac0/14cd40|不同类型的数值读取路径|
|0x80|0x1566c0|清空本地容器|

表中槽顺序是这份二进制的实际字节，不是公开C++头里的声明顺序。
不能按声明文字顺序手工造vtable、把double当指针、或把三个资源重载合并成
任意ABI。同一编译器编译公共接口可能生成对应布局，但仍须核实际二进制及签名。
本篇仅将setter的可见转换用于解释参数存储，没有提供完整容器复刻或所有越界语义。

进一步核对表明，同 key 的 numeric 与 pointer 是独立双槽：0x14f27c/14f281 写
node+0x40/+0x50，0x14f5fc/14f600 写node+0x48/+0x51。共享helper对已有key
返回原节点，更新不清另一flag；pointer getter在0x14ccfc检查+0x51。独立原生
候选、精度差异与CPU验证边界见[NR-PARAMETER-STORE.md](NR-PARAMETER-STORE.md)。
不能把单一variant的最后写入类型视为原件合同。

## Feature 释放回调与持有关系

另已闭合直接表的 ReleaseFeature 路径，不将相似的 COM call 当作 Shutdown1：

| 阶段 | 原始位置与行为 |
|---|---|
| 建立 holder | 0x157c77..0x157cb9 分配0x28 bytes，写入vtable0x59c2f8，初始化计数与成员 |
| 持有另一接口 | 0x157ccf/157cd9 从runtime+0x90复制到holder+0x10；helper0xcb70对新非空接口调用slot+8，对旧接口调用slot+0x10 |
| 绑定释放函数 | 0x157cde..0x157ce6 从同一payload API表+0x18取指针，存holder+0x20；该表项对应已解析的ReleaseFeature |
| 保存创建结果 | Create未进入BAD失败族且handle非空后，0x157f3f将handle存holder+0x18，0x157f4f..0x157f62提交handle/holder对 |
| 销毁内容 | vtable首项指向0x15dad0；0x15dad9..0x15dae2以holder+0x18的非空handle准备RCX，再调用holder+0x20 |
| 随后释放持有接口 | 0x15dae6..0x15dafa读取holder+0x10、清零该成员并调用接口slot+0x10 |

前两项vtable原始指针为0x15dad0、0x115df0；证据只断言这个前缀，不声称
这是完整vtable。对应限定窗口与指针在runtime manifest及nr-payload-api-tables.json
可重生成。下节补查将runtime+0x90及holder+0x10关联到实际取得的ID3D12Device；
单看这段复制/释放行为本身仍不足以命名接口类型。

这支持F7将feature、其实际释放函数以及所需接口生命周期一起管理，而不是在
每帧借用临时参数或随意从另一NGX表取Release。所有feature回调结束前仍须保持
该payload模块与函数表有效；这不定义何时可安全Shutdown或卸载。这个CPU holder及其引用计数
不证明GPU已完成：捕获的销毁回调没有queue/fence完成检查，不能将其照搬成F7
资源退休条件，也不能由这些caller准备的寄存器推导所有payload版本的完整ABI。

### 补查：holder 持有的实际 device 与 CPU 析构顺序

新增五个限定窗口共237条指令，21个锚点；不是新的SDK或GPU调用。原字节显示：

| 阶段 | 已核数据流 |
|---|---|
| 取得device | 0x15140f从输入+0x20取child；0x151424调用0x92d0取得输出槽地址，0x15142c装入ID3D12Device GUID，0x151436调用child slot+0x38，结果在rsp+0x70 |
| helper与身份门禁 | 0x92d0..0x92f8先清/释放旧槽，返回同一槽地址；GetDevice结果的HRESULT负值分支拒绝，0x15144a又与输入+0x10的预期device作指针比较，不等则拒绝 |
| runtime持有 | 0x1518f0重新取得runtime，0x151918把同一rsp+0x70值写runtime+0x90；变更时先AddRef新值，再Release旧值 |
| feature持有 | 上文0x157cd9把runtime+0x90复制到holder+0x10；Init调用0x151ee4也从runtime+0x90取device。现有字段身份链支持将holder所持接口识别为ID3D12Device |
| holder最后引用销毁 | 0x15dae2先调用它绑定的ReleaseFeature；随后0x15dafa对所持device调用Release；两个调用分别有非空条件 |
| runtime析构 | 完整函数0x143430..0x14369e先在0x1434c8取runtime+0x168的holder，最后强引用才在0x1434eb调用其首项；之后0x143645才Release runtime+0x90的device |

公开Windows SDK d3d12.h的ID3D12DeviceChild C-vtable将GetDevice、AddRef、Release
分别绑定Win64 byte offset0x38/0x08/0x10。原件GUID的16字节、上述三个slot、五组
.pdata begin/end/unwind均在nr-device-holder-contract.json核对；SDK头哈希仍由runtime
manifest绑定。这里依靠真实输出槽、GUID、成功分支、成员写入及后续使用关联身份，
不是由COM Release的外观猜测device。

runtime析构窗口包含参数容器和其它资源释放，只将其中feature/device的相对次序
列为事实；可能仍有其它holder引用，因此条件回调不等于此刻必然执行ReleaseFeature。
该函数与holder回调均没有在所示直接指令中读取所属queue/fence；这不证明整个调用链
从未等待，也不认证析构可在GPU未退休时调用。F7仍须先取得真实完成证明，保留device、
feature、对应函数表与模块。**device COM引用不等于payload模块租约，不替代Shutdown。**

### Shutdown 定向补查的范围与未确定项

本次仍未找到上述直接payload API表+0x20的实际Shutdown caller；没有认证其参数
合同或成功会话的ReleaseFeature→Shutdown1→FreeLibrary顺序。本节14窗口220指令、
96锚点保留下列直接字节证据，不把限定检索未命中当作不存在动态caller：

- 0x357640检查解析得到的第五项非空，0x357645为空跳0x358122；0x358125调用
  已核IAT0x53b3f0的FreeLibrary。该窗口是必需导出缺失的失败卸载，不是已建立会话的关闭。
- 同一state访问器0x35a450的0x35a4aa取0x539bc0，0x35a4b1交给注册helper。
  退出清理必须连读[0x539bc0,0x539bea)、[0x539bea,0x539c0a)、
  [0x539c0a,0x539c43)三个.pdata段；首段结束不等于函数全部工作结束。
  这些直接指令清理容器和调用其它清理helper，没有直接展示Shutdown槽调用；
  下文补查其局部内存释放链，异常边和并发卸载仍未认证。
- 第三处已定位API getter caller0x1549a7取得的表，被0x15502b保存到上下文。
  末端0x145c8b读取context+0x20的是API表指针，0x145caa实际调用table+0x10，
  因而属于Evaluate。context字段偏移0x20不能误读为table的Shutdown槽0x20。

nr-shutdown-scope-contract.json绑定FreeLibrary、HeapFree、GetLastError三项IAT与
14组.pdata begin/end/unwind；对应窗口的完整原字节在runtime中。报告显式保持shutdown_call_identified、
complete_dynamic_call_coverage_proven、transitive_cleanup_authenticated、
successful_session_unload_proven及resource_retirement_proven=false。
全局payload state为0x1663e60，其表为0x1663ed0、第五槽为0x1663ef0；
它与前节runtime+0x90的device字段属于不同对象，不可混用偏移。

另一解析路径0x35d3ee使用另一state accessor0x35d6b0、保存偏移+0x80；
尚未认证它与本表的生命周期关系。0x350145的call [rdi+0x90]属于另一COM路径，
也不是此payload的Shutdown1证据。Release/device链闭合不授权盲开第二会话、
提前卸载payload或忽略Shutdown未知项。

退出清理helper的原字节补查进一步区分了容器存储与载荷会话：

- 0x539bcb进入0xc7b0..0xc818，按首地址与容量端地址计算所释放的存储，
  清零三个CPU指针字段。0x539c22与尾跳0x539c3e进入0x18200..0x1826f；
  后者处理16位缓冲，短容量阈值为7，堆路径字节数为容量乘2加2。
- 0x539c2e进入0x35ae70..0x35af26，必须连读三组.pdata分段。它按0x48字节
  步长遍历记录，仅将每项+0x18交0xc6c0..0xc725处理8位缓冲；随后释放整个
  记录存储并清零三个CPU指针字段。这不是逐项调用NR API表或退休GPU资源的证据。
- 所示释放调用经三个5字节跳板0x4e76f0→0x4e80a0→0x4f7de0，到达
  0x50b6c0..0x50b6fc。非空路径将原指针传R8、RDX置零、RCX取全局heap值，
  经IAT0x53b668调用HeapFree。失败后读取GetLastError并调用两个尚未完整
  恢复的错误helper；不能把正常释放路径扩大为完整异常/失败清理合同。

这些限定helper直接展示CPU缓冲及记录存储的清理，没有展示直接NR表第五槽的调用。
bounded_cleanup直接绑定13条call/jump边、三个叶跳板、两处IAT调用及68项字段/
步长/控制指令；叶跳板的五字节完整性及未被.pdata覆盖也被核对。
它们不证明全局不存在其它Shutdown caller，也不证明成功会话已关闭或模块可卸载。
F7仍须分别实现实际会话关闭、borrow准入、feature释放与所属queue/fence退休；
不能把该退出回调或HeapFree的返回当作这些条件的替代证明。

### 同名 NGX Shutdown wrapper 与直接 NR 表保持分开

继续核0x093356的同名引用后，已闭合一个不同的名字/指针槽/wrapper关联：

| 步骤 | 固定原件的可见数据流 |
|---|---|
| 构造描述项 | 0x93356/9335d将名字0x55b208写栈+0x60，0x93362/93369将槽地址0x14795a8写栈+0x68，0x9336e/93375将wrapper0xffa00写栈+0x70 |
| 解析并写槽 | 0x93421按描述项+0x18的动态条件分支；0x9342d调用GetProcAddress；0x93442把结果或条件未满足时的空值写入描述项+8指向的槽，每项步长0x20 |
| 注册项 | 0x936d2取得槽地址；条件保留时0x93742写wrapper、0x93746写槽地址，形成16B的{slot-address, wrapper}项；另一个条件分支0x936e9会清槽 |
| wrapper转交 | 0xffa00..0xffa4f先保存原RCX；0xffa25调用内部helper0x56590，0xffa2a读槽0x14795a8，恢复RCX后0xffa34调用该指针 |
| 保留结果 | 0xffa39/ffa3b将原EAX作为后处理输入并存EBX，0xffa3d调用0xff8f0，0xffa42从EBX还原EAX返回 |
| 有界后处理 | 0xff8f0..0xff9bd先看flag0x1478838；result等于字面1才进入带SRW独占锁的0xc00块准备/helper调用/serial递增分支，随后有0x400次诊断限流 |

诊断UTF-16原字串位于0x559c10，包含shutdown api=D3D12、device、result与
registryInvalidated字段。它支持命名这条局部CPU观察路径；不把诊断文字当GPU
完成、真实NR payload被Shutdown或完整注册表语义的证明。块的所有传递作用、
锁外参与者和异常后果尚未认证，不直接复刻该后处理到F7。

注册调用者另闭合到0x10afa8。它的诊断循环中，0x10afe6先取槽地址，
0x10afea再解引用出入口值，0x10aff8把该值作为参数保存到栈；不能把这两层
都称为槽指针。0x10b002的R9用于格式中的export名称，不能误当wrapper。
0x55b560的UTF-16字串明确为[NR NGX target]及index/export/entry/core-rva字段；
0x10b017与0x10b023这段记录路径不是安装生效证明。

真正的另一调用阶段在0x10b072向helper0x196a0传注册项列表；0x10b077读取
返回对象+8，值为0时0x10b07e跳0x10b1d7。空列表或所示失败路径可进入
0x10b0c0清槽块，其中0x10b0e6用R15写0x14795a8；所示调用链0x10af75
先把R15清零。这里保存的是caller的静态路径，不认证helper全部实现、回写
trampoline、运行中清零时刻或完整回滚；清槽更不是GPU退休或可卸载模块的证书。

0x14795a8位于.data的虚拟尾部，超出该节文件原始bytes；get_data返回空不等于
原件里有八个已观察的零字节，更不证明运行中已绑定哪个原函数或trampoline。
这条槽与直接NR的0x1663ef0不同；wrapper间接调用的具体模块、安装生效、并发
卸载、私有payload ABI仍UNKNOWN。描述项中的函数地址也不等于PE按此名导出。

新增八窗口177条指令、51个锚点，ngx-shutdown-interposition-contract.json另绑定
三个原始字串、GetProcAddress与两项SRW锁IAT、四组.pdata及.data原始/虚拟布局。
报告显式不认证原指针运行目标、安装trampoline、直接NR Shutdown caller、GPU
退休或transitive postprocess效果。F7可借鉴的是保留实际调用结果、分开会话关闭
与CPU观察失效的合同；不能以清空CPU登记、递增serial或函数返回替代所属fence。


### Hook 事务：begin 失败并非都没有 owner 变更

新增ngx-hook-transaction-contract.json与限定窗口，覆盖调用者0x196a0..0x19ac6、
begin/保护准备、abort/commit的owner前缀、线程准入及所用IAT/thunk。backend函数
按RVA指称；公开Detours头的相似声明不被当作此私有二进制的符号或版本认证。

| 步骤 | 原件可见合同 |
|---|---|
| begin | 0x19749调用0x425320；非零在0x19753跳0x1998a复制结果返回，绕过外层abort调用 |
| CPU owner | begin先检查0x1665150，已占用或CAS失败返回0x10dd；0x42534a的CAS成功时将当前TID写入该字，不是GPU fence或某次提交的nonce |
| 目标检查 | 对每个slot中的入口调用0x424f50，空目标与先前helper结果相等分别给target/duplicate-target状态，后者raw错误0xd；这种地址比较不认证COM context或frame身份 |
| wrapper模块 | 0x1981a取pair+8的wrapper，0x19831以flags5调用GetModuleHandleExW；libloaderapi.h明确5=PIN1加FROM_ADDRESS4，不含UNCHANGED_REFCOUNT2。失败拒绝，不能用来证明NR payload模块已pin |
| 线程准入 | 0x1b700用snapshot flags4，过滤本process、排除当前TID；OpenThread mask0x10001a，错误0x57被跳过，其它错误保留。句柄/TID以16B项交caller，snapshot在0x1b850关闭 |
| update失败 | 0x1b889调用0x425830，非零时对该线程WaitForSingleObject(handle,0)，只写exit观察byte、不清原错误；外层仍会失败并abort。Wait0是CPU线程信号，不是GPU完成 |
| attach/commit | 0x19a2b把slot地址及wrapper交给0x4248d0；失败走abort。0x19a6c提交后返回的failed-slot地址用于寻找对应pair索引；0x198b8逐项关闭caller拥有的线程句柄。current-thread伪句柄不入该vector |

关键条件路径已经独立原字节复核：0x42534a取得CPU owner后，0x42537a再调用
0x424870对已有region做VirtualProtect，大小0x10000、保护值0x40；若该调用失败
且GetLastError为非零，该值经0x42537f写0x1665154、0x425385返回。完整begin
窗口中没有随后清0x1665150的指令；外层非零分支又直接返回。**因此此固定版本
的begin失败，不能普遍推出没有状态变更或可以干净重试。** 这不是实际失败复现，
也不证明运行中必然遗留owner；所需region/保护失败/非零错误均是条件。

另一方面，owner未取得的拒绝路径也真实存在，不能反向建议对所有begin失败
无条件abort：abort/commit前缀都比较同一CPU TID，非owner返回0x10dd；即使
TID相同，也不区分嵌套调用的历史事务归属。F7应先核它实际选用的backend、
本次事务所有权和失败路径，不照搬这份实现的“retry allowed”诊断。

报告绑定13个stage字串、12项IAT、6个jump thunk、8组.pdata与官方loader三项
flag宏。线程snapshot不覆盖未来创建的线程，OpenThread错误0x57不单独认证退出；
超时和等待失败都可能使exit byte为0。0x425830、异常逃逸、完整回滚、运行期安装
及安全卸载仍未认证。wrapper pin尝试、CPU线程句柄和状态返回都不替代DeviceOwner
的资源租约与真实所属queue/fence退休，也不证明直接NR Shutdown已接通。

## 对 F7 下一步的实际影响

2026-11-07本轮再读生产接口，NrSessionBorrow持有session、ownerLease与completionLease，
但NrDx12.h:91的注释明确把禁止Close/reinit/module unload交给外部调用者；NgxSession
仍没有borrower计数或可执行的Close准入门。NrDx12的Idle/ShutdownChecked与失败隔离
属于它自己的feature资源合同，不自动阻止另一调用者关闭借用的会话。具体实现应由F7
主线在真实owner的串行域接入，不能用device AddRef、API值快照或不相关shared_ptr代替。
本轮未改产品；这与Encore新核CPU device hold形成对照，不将Encore当安全卸载范本。

当前F7的 backends/NgxApi.cpp 从 adapter-bound core取得Init、AllocateParameters、
scratch/Create/Evaluate等函数；tests/neural-hardware/NrHardwareProbe.cpp 则借同一
SR NgxSession调用feature18。payload是显式冻结输入，GetModuleHandle与SameFile只
观测是否出现该模块，没有将该payload的五个入口替换成独立会话。

2026-11-07定点复查找到的是build/tests/neural-hardware/Release/下的EXE与
build receipt，后者executed=false；默认build/temp/neural-hardware/运行目录
不存在，限定搜索未找到本探针的实际运行回执。脚本的NrPayloadPath与SHA256为
强制外部输入，没有默认选定路径。因此不能把已构建探针写成已执行，也不能将
FG日志中的payload搜索失败当作本探针的feature18硬件结果。

若未来观察到core路径的FeatureNotSupported/NotImplemented，它只能说明该具体路由失败，
不能据此认定直接payload也不支持。反过来，五个导出存在、哈希命中历史表或文件
版本相同，也都不证明Init/Create/Evaluate或硬件可用。下一步应明确选择哪条路由，
以实际文件和ABI证据实现隔离候选，由F7唯一GPU owner安排硬件验证。
独立NR状态若最终需要存在，仍必须归属同DeviceOwner和真实queue/fence，
不能把本篇静态差异当作绕过borrow与资源退休合同的依据。

## 离线检查器

nr_payload_inspect.py只使用现有pefile解析原始PE字节，核AMD64/PE32+/DLL、
精确导出名、ordinal/RVA、文件映射和forwarder，输出输入SHA256与字节数。
forwarder只记录，不跟随；缺少可选AllocateParameters等入口不被解释为功能不支持。
ABI、设备支持、完成与画质始终未认证。
报告字段required_exports只指本次要观察的五个名字，不是NR功能支持的充分条件。

    & D:/APPS/conda/python.exe -B tools/research/encore-f7/nr_payload_inspect.py --input '明确的nvngx_dlssnr.dll路径' --pefile-file 'D:/APPS/conda/Lib/site-packages/pefile.py' --expected-sha256 '调用方已确认的完整SHA256' --output 'payload静态报告.json'

上述pefile源路径由本次解释器实际定位，版本2024.8.26；报告绑定其源哈希。
另一显式依赖模式是--reference-root，其指定子目录须真实包含pefile.py；本次该目录
不含此文件，不能照搬旧命令或在生产CLI中默默退回全局安装。

输出与DLL输入、显式pefile源或其hardlink/目录联接别名相同时会被拒绝；
格式或expected hash错误不写报告。
工具不复制payload、不加载模块、不修改导出/权重/架构门槛、不做网络访问或GPU探测。

本次实跑两个已知文件作为对照，并保持support=UNKNOWN：

- 旧F5包的nvngx.dll_dlssnr.dll，152,064 bytes，SHA256
  201eb85a47d481da6965abd309685788a70d35b2a6ce21a7fa9fff744e544658，
  五项观察名全部missing。它是已知forwarder文件，不能冒充nvngx_dlssnr.dll载荷。
- 当前SR参考nvngx_dlss.dll，58,956,912 bytes，SHA256
  3975567b8943c53acce397f2b72380092f84f162d00b0d2c7d08a1025c563983，
  五项均direct-present；AllocateParameters/Capability/Destroy三项missing。
  这直接说明“有五项导出”不能识别NR，也不能推导可用。

两份报告为evidence/nr-forwarder-exports.json和sr-feature-exports.json，包含明确输入
路径/SHA、analyzer源SHA与原始导出观察。这两份是对照，不是下述历史NR参考的
检查结果；也不能据SR对照推断NR参数容器可互换。F7仍未明确选定真实NR payload。

可重生成原件在evidence/runtime/nr-payload-*.json、nr-parameter-*.json与
nr-payload-api-tables.json，manifest绑定固定Encore DLL及实际指令字节。
完整加载器CFG、其它import处理/架构修改、payload各版本ABI与动态效果不在已证范围；
上文已单独闭合GetModuleFileNameW替代层，不将这一局部机制说成全部加载器已恢复。

## 已定位的历史 NR 参考与 caller 检查

限定搜索定位到现存文件：

    D:/CODE/HoyoDLSS5/artifacts/f5-final-validation-fba91098/OptiScaler/official/DLSS F5/nvngx_dlssnr.dll

实际重算SHA256为cdfa6c1ec784867182164c7836f324f087b5c9f374d0c9fe46f54999feabf3ed，
276,162,560 bytes，PE FileVersion为310,9,16,0。以下称CDF参考。历史目录中相同
大小/版本的094D2B0E28EEB65A4250F99EEE8230B1BC9BEFCB459331E569DDB76B26BCC6CB
条目不等于该文件，不能将其标记成原神V4、Encore表内版本或F7选定载荷。
文件仅作只读静态研究输入，未复制、加载或执行。

evidence/nr-historical-reference-exports.json观察到55项named exports，其中五个
目标名字均direct-present。nr-reference-entrypoints.json独立重解码如下窗口。
**本节所有地址均为CDF NR DLL的RVA，不是Encore的RVA**；ImageBase同为0x180000000。

| D3D12入口/内部块 | RVA起止，右端不含 |
|---|---|
| Init_Ext | 0x156b0..0x157e0 |
| CreateFeature | 0x15010..0x150e1 |
| CreateFeature主体/epilogue/lock失败 | 0x150e1..0x1525a、0x1525a..0x15275、0x15275..0x1527d |
| EvaluateFeature | 0x15280..0x1548b |
| ReleaseFeature | 0x15900..0x159e4 |
| Shutdown1 | 0x15b00..0x15be6 |
| Init共同内部路径 | 0x13d40..0x14015 |
| Shutdown共同内部路径 | 0x14210..0x143fc |
| Shutdown对象token | 0xe8b0..0xe93c |
| Shutdown清理helper（三段） | 0x23dc0..0x23e57 |
| Shutdown下层helper（五段） | 0x1b090..0x1b20b |
| 16位子串搜索helper | 0xa8010..0xa8222 |
| PopulateParameters_Impl | 0x157e0..0x15900 |
| 载荷发布的scaling callback | 0x12740..0x12841 |
| 载荷发布的stats callback | 0x14920..0x14a72 |
| 内部Create代码区（父.pdata含尾部选择表） | 0x17b20..0x18140，父记录终点0x181d1 |
| 资源子矩形helper | 0x17a20..0x17b12 |

这五个入口均先从自身入口栈读取原始返回地址，以flags=6调用IAT
0xd7118的GetModuleHandleExA，再调用IAT0xd7080的GetModuleFileNameW。
之后以调用模块路径和RVA0xd9380的宽字串nvngx.dll调用helper0xa8010。
helper返回空时在进入后续工作路径前返回0xbad00002；旁边诊断字串为
Error: Not called from NGX runtime - %S。独立原始字节复核确认helper为区分大小写、
NUL终止的16位子串搜索，具有wcsstr式语义；其154条指令已纳入限定报告。
这是模块路径中的字面子串检查，不是basename精确匹配或模块身份认证，
也不据此命名具体CRT符号归属。

这表明CDF参考有调用来源条件。仅解析五个入口不能证明从F7模块直接调用可行；
Encore的模块路径查询替代层已由上文原字节闭合，但与这份不同SHA的CDF参考
之间没有实际运行证明。这里不提供绕过、改名或修改门槛的方案，
也不将该条件泛化到所有NR版本。

Init_Ext可见参数流如下，不升级为通用私有ABI声明：原R8保存至RBP，原R9D
保存至EBX；调用0x13d40时RCX/RDX保留原值，R8D取原R9D，R9取原第五参数
（入口RSP+0x28；wrapper局部RSP+0x2c0），内部第五参数取原R8，其后参数置零。
结合Encore调用点，原R8为其device，原R9D为该调用点字面值0x15，原第五参数为0。
Shutdown1把原RCX保存至RBX；通过caller检查后以RCX=RBX、DL=1调用0x14210。
不能由这些wrapper推导所有内部参数身份、资源所有权或安全Shutdown时机。
CreateFeature的四个相邻.pdata分段均已纳入；首段只到caller检查后的分支，
不能把该首段单独当作完整Create实现。实际内部Create调用位于0x15212，
内部Evaluate调用位于0x15450；入口检查失败不会到达这些call。

### CDF Shutdown 的对象 token 与清理结果

以下仍是CDF参考的RVA。Init共同路径0x13dbf与Shutdown共同路径0x14253都调用
0xe8b0..0xe93c；后者不是原始对象指针比较，也不是GPU完成查询：

- 空对象直接返回0。非空对象先调用vtable+0x18，传入GUID原字节
  `11d7e043ea7b684ea077e9d11a7625f3`、初始值为8的32位size槽和8字节输出槽；结果非负时
  返回槽内值。所示代码未再核对返回的size是否仍为8。
- 读取结果为负时，0xe8fe用lock xadd将全局计数加1，再将取回的旧值在局部加1，放回输出槽，
  经vtable+0x20、同GUID与size8写入对象。写入调用0xe92b之后直接返回该token，
  没有检查写入结果。这只恢复读写形状，不认证接口ABI、GUID用途或实际调用成功。
- Shutdown保留原输入为R12。非空时，0x14289比较登记节点+0x10与上述token；
  不匹配则增加跳过计数并继续。原输入为空时直接进入所示登记项的清理路径，
  不能据此建议F7用空参数关闭共享会话。函数返回值不是已关闭目标数的证明。

由所示数据流可作条件推论：若token写入未生效，后续读取又失败，返回token
未必保持稳定；没有运行故障复现，不能把这种可能性写成已发生的Shutdown漏清理。
F7也不能借该进程内token代替实际DeviceOwner、会话借用或所属queue/fence身份。

0x23dc0..0x23e57的清理helper有三组.pdata分段，内部0x1b090..0x1b20b又有
五组分段；完整正常返回路径在0x1b1f9固定写EAX=1。因此0x142a2/0x142aa
虽然保留非1结果的汇总分支，本次并未识别所示helper的非1正常返回，不能称为
已证明可达的失败路径。深层间接调用、异常、真实清理成功和GPU退休仍未认证。

可重生成命令：

    & D:/APPS/conda/python.exe -B tools/research/encore-f7/nr_reference_evidence.py --input 'D:/CODE/HoyoDLSS5/artifacts/f5-final-validation-fba91098/OptiScaler/official/DLSS F5/nvngx_dlssnr.dll' --pefile-file 'D:/APPS/conda/Lib/site-packages/pefile.py' --capstone-root 'D:/CODE/DLSS5_NEXT-workers/f7-clean-rewrite/build/research/rtx-encore-v2.0.1/python-deps' --output tools/research/encore-f7/evidence/nr-reference-entrypoints.json

生成器只接受固定CDF SHA，核五个目标导出及额外Populate导出RVA、25组.pdata边界、
完整连续指令字节、110个原有指令锚点、两个IAT入口和宽字串；shutdown_identity另绑定
一个GUID、九组新增函数/pdata/unwind、127个合同锚点及15条路径（锚点集合可重叠）。
记录显式pefile源及Capstone包/实际native库
哈希。CLI在写输出前保护输入、工具源和依赖的hardlink/目录联接别名。
1705条解码指令独立于Encore四profile计数，内部Create尾部145B选择表单列为数据；CPU测试覆盖解码拒绝、保存报告
语义/指令切分篡改、decoder身份、token字节/字段/边界与过度scope及输入保护。统一verify用同一显式Capstone重新
解码原DLL的每个窗口，逐行核对完整指令记录并由新解码结果计数；不会只依赖
保存报告的bytes拼接或锚点；对象token合同也从原件与新解码逐项重建，拒绝把bool当整数或把静态观察升级为运行证明。
保留主报告及可选exports报告的输出别名也在CPU日志写入前拒绝。
单独生成或刷新报告仍使用上面命令；显式输入可指向相同SHA的副本，身份以内容校验，
不绑定本机绝对路径。同内容副本的保存报告也须完整重解码。所有报告保持f7_selected_payload=false、
abi_authenticated=false与support=UNKNOWN。

### 载荷发布的缩放回调不等于 Encore 本地回调

CDF参考的PopulateParameters_Impl也先执行caller-path检查；通过后调用参数对象
vtable首槽的pointer setter，按两个key分别写入callback指针，不改写vtable：
DLSSNRGetStatsCallback对应0x14920，
DLSSNRComputeScalingRatioCallback对应0x12740。本批只收入后者的函数正文；
Stats的限定函数正文及边界见下节；两种callback不能互相代用。

CDF scaling callback读取PerfQualityValue，Get unsigned int使用参数表+0x60。
null参数或BAD族getter结果走0xbad00005；未进入BAD分支后，读取值按以下有限分支处理：

| PerfQualityValue数值 | 写DLSSNR.ScalingRatio | 返回值 |
|---|---|---|
| 2或5 | float 1.0 | 1 |
| 4 | float32原始bits 0x3f59999a，即约0.8500000238 | 1 |
| 其它uint32 | 不进入ratio写入分支 | 0xbad00010 |

写入采用参数表+0x30的float setter；原生代码还以0.5与1.0做边界选择，
本固定常量的有效分支恰为表中结果。四个ASCII key和三个float原字节均在
nr-reference-entrypoints.json的published_scaling字段中绑定，统一验证重新读取。
这张表只描述固定CDF字节的callback策略，不是完整模型能力、实际可运行quality
范围或公开SDK通用合同。

相对地，Encore的本地0x15a240回调不读取PerfQualityValue，直接写ratio1；
创建common helper还将PerfQualityValue设为0。由此只能确认它主动提供了不同
回调与准备策略，不能说它曾用CDF callback成功处理0。两份DLL的内容身份不同，
本包没有执行Populate、callback或Create；F7也不应据此盲填回调、改quality或推定
默认缩放比。具体创建参数和复用门禁见NR-WORK-POLICY.md。

### CDF Stats 是首登记对象的遥测路径

以下仍为固定CDF参考的RVA。`DLSSNRGetStatsCallback`发布到0x14920，
本次完整重解码其0x14920..0x14a72的单个.pdata窗口，338 bytes、79条指令；不执行该回调。
它接收参数对象，并没有从该参数对象读取device、feature handle或frame key：

| 路径 | 原字节恢复的行为 |
|---|---|
| 空参数 | 0x1492d检查RCX；空值返回0xbad00005，不写输出 |
| 登记项选择 | 调同步helper0xa3d30后，0x14989读取全局表头，再取`[head]`；与head相等或首节点+0x88为空，走失败路径。所示正文没有遍历或按调用者device筛选 |
| 统计调用 | 0x149b9从该对象vtable+0x1f8取函数，0x149c0经间接调用调度槽调用；RDX/R8分别指向两个已初始化为0的64位输出槽。接口和方法身份保持UNKNOWN |
| 返回处理 | 统计调用后没有检查其返回值；0x149ca调用同步释放helper，随后只把第一个输出槽写入参数对象，第二个槽未用于下列回写 |
| 参数回写 | `SizeInBytes`使用参数vtable+0x38的uint64 setter；原始key `#D`写int32值40，`#E`写int32值0，后二者均使用setter+0x18 |
| 正常尾部 | 三次setter之后0x14a22固定写EAX=1；不另外核验参数对象是否接受这些写入 |
| 首登记项不可用 | 0x14a52释放同步helper后返回0xbad00007；不会继续查更晚登记项，也不证明进程中其它对象不可用 |
| 同步helper非零结果 | 0x14983跳到0x14a6a，再调用0xa3e44；不把这个未展开helper路径当作普通BAD返回或已证可恢复错误 |

原始key的ASCII字节分别在0xd9740、0xd974c和0xd9750。当前公开头把
`#\x44`和`#\x45`命名为OptLevel和IsDevSnippetBranch；本合同保留原始key，
不由字段名称推断某种优化已启用、开发模型、NR硬件支持或实际执行。
原始短key与`Snippet.OptLevel`/`Snippet.IsDevBranch`长名不是相同字符串；
自有参数候选按原key精确存储，不能假定它会实现NGX core可能具有的别名处理。

这段正文至多恢复“从首登记对象取得两个输出、回写其中一个及两个固定值”的
策略，不证明回调及其间接调用只有统计副作用。没有证明它汇总所有feature、对应本次选择的DeviceOwner、统计值新鲜或
等于某帧显存峰值；也没有在所示正文查询queue/fence。即便返回1或SizeInBytes
非零，都不能替代Create/Evaluate结果、源帧身份、实际所属queue/fence完成或
画面验收。统计方法内部是否同步、如何计数及异常路径仍未知。

`nr-reference-entrypoints.json`的`published_stats`单独绑定三个key、79项完整指令、
一组函数/pdata/unwind与11条限定路径，不合并进原有110个锚点或Shutdown合同。
统一验证重新解码原件后逐项核对，并拒绝将未知方法、聚合范围、ABI或GPU完成
改写成已证明；静态报告的一致性不等于实际调用成功。

### CDF 内部 Create 的代码区、选择表与参数读取

导出wrapper在0x15212调用内部0x17b20。其`.pdata`记录为
`0x17b20..0x181d1`，但这不是全段都可当作指令的范围：

| 区域 | 固定范围与内容 |
|---|---|
| 指令区 | 0x17b20..0x18140，1568 bytes、379条连续指令，正常返回位于0x1813c |
| 目标表 | 0x18140..0x18160，8个little-endian uint32目标RVA |
| 选择表 | 0x18160..0x181d1，113个uint8索引 |

目标表实际为`17bc5,17c2b,17c2b,17c2b,17c2b,17be8,17c2b,17bef`。
因此不能只看附近仍保留的0x17bcc/17bd3/17bda/17be1块，就断言它们被此表选中。
输入值来自内部对象虚表+0x38调用的返回值，其接口和数值含义未认证，不将它们
命名成GPU代际、真实架构或硬件支持。这里不提供表项修改或门槛绕过实现。

所示选择规则先按32位计算`value-0x140`，无符号比较是否在0..0x70内；
在范围内再经两表跳转。0x140/0x150选到0x17bc5，0x1a0选到0x17be8，
随后进入返回0xbad00001的路径；0x160/170/180/190/1b0直接选到参数准备块
0x17c2b。其它表内值选到0x17bef；表外值也走该处的signed EAX比较，
只有signed值至少0x140才进参数准备。这个局部准入不证明后续创建成功。

更早的0x17b5f检查内部状态byte+0x18，未就绪时直接返回0xbad00004。
就绪时先通过另一虚表+0x1c8方法传入参数对象，其内部行为未知；之后才进行
上述值查询和局部选择。因此以下表只覆盖正文中五次直接Get，不是完整的输入合同：

| 原始key | 参数虚表槽 | Get结果必须是 | 其它返回值时的采用值 |
|---|---|---|---|
| CreationNodeMask | +0x60，unsigned int输出 | 精确1 | 1 |
| VisibilityNodeMask | +0x60，unsigned int输出 | 精确1 | 1 |
| DLSSNR.Width | +0x60，unsigned int输出 | 精确1 | 0 |
| DLSSNR.Height | +0x60，unsigned int输出 | 精确1 | 0 |
| DLSSNR.Hint.Render.Preset | +0x58，int输出 | 精确1 | 0 |

尺寸和preset的零fallback来自能到达该块时的EDI=0；不是把所有失败Getter
结果泛化为零。preset随后在0x17d1a清bit31。回调/后端可能在间接调用里读取、
改写其它参数，不能由这五次Get推导Width/OutWidth、quality、ratio或scratch均无用。
Get成功读到的零值仍保留；这些fallback也不证明零尺寸或默认配置能安全运行。

0x17d71的另一间接调用按非零结果走0xbad00002路径；与五个Get的精确1判断
以及后续BAD族判断各不相同。0x17f29调用分辨率helper0x21000，0x17f7b另有
条件调用初始化helper0x1dba0，0x1800e还调用0x173b0；后三处都按BAD族结果
分支。更晚0x18070才向输出槽写handle，最终正常成功路径
在0x180cc固定返回1。这些分阶段结果仍不证明GPU完成、异常安全、所选实际
payload可运行或F7已经接入，不得只凭前面的参数准备或选择表准入报告成功。

本次只读的F7 `src/neural/NrDx12.cpp`（SHA256
`d32bb39a4b3392593d210bdcf9227561a73144379b972dc02efaac974bd370a3`）
已设置这五个key，包括两个node mask=1；其当前路由先调用core的feature18
scratch查询，失败会明确拒绝且不fallback到私有payload。该静态差异不支持
“再盲加一组字段即可修好”的结论，仍需选定真实载荷和合法ABI/会话路由。

`internal_create`单独绑定五个原始key、132个指令锚点、一组父函数/pdata/unwind、
两张数据表、五条读取合同和十条限定路径。统一验证重新解码379条实际指令，
表数据按原字节独立校验；其它窗口仍要求原来的精确.pdata边界，不改为宽泛的
包含关系。异常handler的语言专用数据及完整清理语义没有因此得到认证。

### CDF 子矩形 helper 严格拒绝越界，不裁剪

内部Evaluate调用的helper为0x17a20..0x17b12，单个.pdata记录、242 bytes、
72条指令。它的输入描述符含一个64位资源指针和四个32位原始字段；本节只
恢复该helper，不把这些字段本身当作真实资源、source或SDK身份的证明。

| 输入偏移 | 已读字段 | 输出偏移 |
|---|---|---|
| +0x00 | opaque资源指针 | 不复制到输出 |
| +0x08/+0x0c | origin X/Y，uint32解释 | +0x00/+0x04 |
| +0x10/+0x14 | 请求width/height，uint32解释 | +0x08/+0x0c，有零值默认处理 |
| 由backend查询输出+0/+4 | texture width/height，32位 | +0x10/+0x14 |

helper立即清零输出24 bytes。输入描述符和输出存储本身没有空指针检查；本包
数值参考要求它们有效且不重叠。RCX backend或资源指针为空返回false；非空时
调用backend虚表+0x190，返回EAX必须精确等于0。接口、方法身份、查询描述符
其余字段及间接调用内部行为保持未知，不据此直接调用任何D3D公开接口。

查询成功后，先将texture尺寸写入输出，再检查两维非零。请求width/height为0
时采用**整张texture的尺寸**，不减origin；非零请求保持原值。随后以无符号
比较检查`origin <= textureExtent`，再检查`requestedExtent <= textureExtent-origin`。
它使用先检查再减法，没有以可能溢出的`origin+size`判断，也没有裁剪origin或size。

例如texture为100×80、origin为(10,0)、请求size为(0,80)，width默认成100，
大于剩余90，因此返回false；请求(90,80)才满足该helper的边界。origin恰好在
texture末端也不能承载正size。uint32负数位型按大整数比较，不能当作合法负origin。

**false不保证输出仍为零。** texture尺寸已在检查非零之前写入；更晚越界时
origin和默认后的size也已写入。读取方必须先检查AL，不得仅以输出尺寸非零
或若干字段看起来合理便消费。该helper未在正文直接核format、resource类型或
所属queue/fence；boolean成功仍不提供GPU完成或完整Evaluate准入。

这与NR-CONTRACT.md中Encore外层的`min`裁剪不是同一层合同。F7当前SetFrame
明确传base0和已映射finished raster尺寸，本节不将它判成缺项，也不建议把外层
裁剪或默认值重复施加到已认证guides。独立CPU参考见`nr_rect.py`；它只接受
调用方提供的uint32数值和查询结果，不执行backend或认证资源。

```python
from nr_rect import DescriptorRect, resolve_rect

result = resolve_rect(DescriptorRect(10, 0, 0, 80), 100, 80)
assert not result.accepted
assert result.output.width == 100  # false仍保留已写出的默认值
```

数值参考的host参数须为明确uint32，presence参数须为bool；类型/范围错误在
模拟分支前拒绝。它不模拟无效指针、输入输出别名、backend异常或未写查询结果。

历史报告的`descriptor_rect`绑定完整函数字节、59个指令锚点、一组pdata/unwind、
七条限定路径与输入/输出布局。保存报告会由原件重新解码并严格比较，拒绝将
裁剪、失败全清零、已知接口或GPU完成等额外结论加入合同。
