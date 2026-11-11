# NR 参数对象：双槽证据与原生 CPU 候选

2026-11-07，北京时间。代码在 native/，独立于 F7 产品。固定 Encore 原件身份
见 README；下列地址均为 RVA。候选使用现有公开 NGX 头，让编译器生成虚函数表，
没有手工构造槽位、加载 payload、初始化 SDK 或提交 GPU 工作。

## 从原件确认的语义

Encore 的每个 key 节点有两个独立的 present flag：

| 路径 | 原始指令 | 节点数据 | present |
|---|---|---|---|
| numeric setter | 0x14f27c / 0x14f281 | +0x40，double | +0x50 = 1 |
| pointer setter | 0x14f5fc / 0x14f600 | +0x48，opaque pointer | +0x51 = 1 |
| pointer getter | 0x14ccfc / 0x14cd02 | 检查 +0x51 后读取 +0x48 | 与 numeric 独立 |

两个 setter 都经共享查找/插入 helper 0x1419a0，已有 key 返回原节点；更新不清除
另一槽的 flag。新节点才初始化两 flag。因此将每个 key 实现为最后写入类型的
单一 variant 会丢失原件允许的值。空 pointer 仍是已写入值，不能等同 missing。
evidence/runtime/ 中的限定指令、断言和 manifest 绑定原件字节；静态证据不证明
任何实际 payload 接受该对象。

Encore 数值 setter 最终全部写 double；其 ULL setter 0x15a6a0 也先作此转换。
候选有意改用 typed numeric union，精确保留 2^53+1、ULL_MAX 等整数原值。
这是一项明确的安全设计差异，不是 Encore 全数值语义或容器布局的逐位复刻。

## 原生候选合同

encore_research::NrParameterStore<Capacity=64, MaxName=127> 是继承公开
NVSDK_NGX_Parameter 的 final C++20 类型，实现 8 Set、8 const Get 和 Reset：

- 固定数组、线性查找、大小写敏感且复制的有界 key；无堆分配。满容量仍可更新
  已有 key 或填该 key 的另一槽。默认容量是候选限制，不声称所有 payload 足够。
- 每个 key 独立保存 numeric 和 pointer；数值类型之间替换 numeric，三类资源/
  指针重载共用 opaque pointer 槽。对象不解引用、AddRef 或 Release 资源。
- Set 的非法/空名称、超长名称、容量不足、非有限数值锁存首错；之后写入冻结，
  所有 Get 返回 Fail，且不改输出，即使该次 Get 参数本身无效。
- 健康时非法 Get 参数返回 InvalidParameter；缺 key、缺对应槽、不可安全转换
  返回 MissingInput。Get 失败不污染健康状态，失败时不改输出。
- 整数间采用精确范围检查；浮点到整数先向零截断，再检查精确的幂二半开区间，
  避免 MSVC 的 long double 与 double 同精度时误将 2^64 转 ULL。浮点输出允许
  正常舍入、下溢和有符号零，但拒绝溢出。Set 保留所写浮点类型。
- 禁止复制/移动。调用者提供可读名称、正确类型的可写输出与外部串行化。
  公开基类没有虚析构，不得经基类指针 delete。Reset 只清 CPU 数据和首错。

调用者应在进入 SDK 前检查 Healthy()，不能把被拒绝的输入送入 SDK。若未来
SDK 自身通过 Set 修改参数并造成首错，调用后的错误也不表示没有录制 GPU
工作：已提交或可能录制的资源仍由原 DeviceOwner、queue/fence 合同保留，
不能因 Reset、CPU 返回或 getter 失败提前退休。

## CPU fixture 与复现

NrParameterStoreConsumer.cpp 只包含公开接口与 consumer 声明，不包含 store
实现；它通过普通虚调用覆盖所有重载与 Reset。关闭 IPO，避免把跨 TU 验证变成
同一实现内的直接调用。测试覆盖整数/浮点边界、双槽、空 pointer、失败输出、
首错冻结、Reset 与容量/名称边界；指针仅为 CPU token，不是真实 COM 对象。

CMake 要求显式 NGX_INCLUDE_DIR、64 位 C++20 工具链，启用严格警告；不链接
NGX/D3D 库。生成头绑定六个候选文件及两个公开头共八项 SHA256，源文件变化
触发正常 CMake 重新配置。测试输出 JSON 包含 checks/failures、编译器、pointerBits
和八项输入哈希，以及 payload_called/sdk_initialized/gpu_tested=false。
这是编译输入回执，不是运行时模块身份认证。

由 F7 唯一编译 owner 串行运行；需要重编译时先协调独立 CPU 编译窗口：

    cmake -S D:/CODE/DLSS5_NEXT/tools/research/encore-f7/native -B <独立CPU构建目录> -DNGX_INCLUDE_DIR=D:/CODE/DLSS5_NEXT-workers/f7-clean-rewrite/external/nvngx_dlss_sdk
    cmake --build <独立CPU构建目录> --config Release
    ctest --test-dir <独立CPU构建目录> -C Release --output-on-failure

F7 编译 owner 已于 **2026-11-07 13:32–13:33（北京时间）**完成编译与执行，
MSVC **19.44.35229.0 / x64**，**341 checks、0 failures，CTest 1/1 passed**。
主根 `build/nr-parameter-store-result.json` 与 `build/temp/nr-parameter-tests.log`
保存同一 stdout JSON；本次逐项核对六个候选文件、两个公开头与生成头，八项输入
SHA256 均与当前文件一致。`build/nr-parameter-store-input-recheck.json` 的
**15:04**记录只是再次核对输入，明确未重新编译或执行。本线程也没有执行 EXE。
341 是原生 fixture 的断言数，独立于 Python 测试数。

本次读取的 owner receipt SHA256 为
`5b3e4686f03030f1a4df2acc0523f1ce8948037825fc057cdd2e11c0195427d9`，
verbose CTest 日志 SHA256 为
`b17b80f5239b0637dac110f6ff9f8c24c7d2c6cfe5d02de9d0be554eede065b6`。
现存 `build/tests/nr-parameter-store/Release/NrParameterStoreTests.exe`
为 101,888 bytes，SHA256
`1acee4f812f8de91a07eb614fedc2b79cbb0e4fdfd33e9186a0448dda1fbe15f`；
这个哈希只标识本次读取的文件，owner 的原始执行回执未记录 EXE 哈希。
配置/编译日志存在，但没有独立保存 compiler exit 字段，不从 CTest 结果补造。

`nr_native_receipt.py` 只读核对 owner receipt、唯一 CTest stdout、单测通过摘要
及当前八项输入；`verify.py` 可显式接入，结果单独写入 validation 的
`native_parameter_store`，不会执行 CMake、CTest、EXE 或 SDK：

    $f7 = 'D:/CODE/DLSS5_NEXT-workers/f7-clean-rewrite'
    & D:/APPS/conda/python.exe -B tools/research/encore-f7/verify.py --nr-native-receipt "$f7/build/nr-parameter-store-result.json" --nr-native-ctest-log "$f7/build/temp/nr-parameter-tests.log" --nr-native-ngx-include "$f7/external/nvngx_dlss_sdk"

三个选项必须一起提供；输出前保护 receipt、日志、六个候选文件和两个公开头，
包括 hardlink/目录联接别名。输入位置由调用者提供，同内容副本允许换路径；
receipt 内的 buildDirectory 只作为历史文字，不自动读取或运行其 EXE。
这是归档一致性与当前输入哈希核对，不是执行认证或本次原生测试重跑。

同编译器跨 TU 通过只能证明此公开头与候选之间的 CPU 调用；真正 payload 的
签名、跨模块 ABI、生命周期、Init/Shutdown 清理和设备支持仍须单独验证。

## 现存 consumer OBJ 的实际槽位

另只读反汇编 owner 构建树中的
`NrParameterStoreTests.dir/Release/NrParameterStoreConsumer.obj`，没有重新编译。
该COFF对象3,984 bytes，SHA256
`440775fcf9c400ef78367bbd1d93970ca0137798f86cc6d4c4d793d391e9dc9d`。
`dumpbin /nologo /disasm`退出0，完整输出与工具身份保存在
`evidence/nr-native-consumer-disassembly.txt`，该观察文件SHA256为
`8b1ae0d2e6341d4a3d6187684101801d4d7f2c6b4a958a86ed78369a140906bf`。
下表是consumer实际间接调用的vtable字节偏移，不按头文件文字顺序推测：

| 公开重载类型 | Set | const Get |
|---|---|---|
| unsigned long long | +0x38 | +0x78 |
| float | +0x30 | +0x70 |
| double | +0x28 | +0x68 |
| unsigned int | +0x20 | +0x60 |
| int | +0x18 | +0x58 |
| ID3D11Resource pointer | +0x10 | +0x50 |
| ID3D12Resource pointer | +0x08 | +0x48 |
| void pointer | +0x00 | +0x40 |

`ConsumerReset`尾调用+0x80。共17个槽位与已核Encore参数表的偏移一致，
也给CDF Stats中的uint64 setter+0x38与int setter+0x18提供本机编译器对照。
这只是该现存OBJ和公开头的布局观察；原owner执行回执没有记录OBJ哈希，
不能据此补造当时编译输入到此OBJ的认证链。OBJ反汇编记录也独立于
`nr_native_receipt.py`的回执一致性核验，不增加原生或Python测试计数。
它没有验证实际payload的跨模块调用、Init_Ext签名或设备支持。

键名也须保持独立：候选按原字符串精确存储，不把CDF Stats写出的`#D`/`#E`
自动映射成公开头中的`Snippet.OptLevel`/`Snippet.IsDevBranch`长名。
读取方不能仅因两组宏名有关联就假定它们是同一key；这也不代表真实NGX core
参数对象没有自己的别名处理。实际接入须核所选对象与载荷的合同。

## 给 F7 的使用顺序

先静态确认实际选择的 NR payload 文件和五个导出，再核其公开/私有签名与参数
ABI；本对象只作为隔离候选，不替换 SR core 的参数或会话表。由唯一 GPU owner
决定是否执行受控 Init/Create/Evaluate/Release/Shutdown，并记录设备/会话和实际
所属 queue/fence。feature18 成功、颜色 codec/guide 合同、真实 NR 输出与性能
分别验收，FG2/6× 的 provider 能力也保持独立。
