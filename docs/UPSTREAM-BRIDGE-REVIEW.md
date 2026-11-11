# FSR 桥与 GIMI 集成的应用评估

审阅日期：2026-11-08，北京时间；对象为下列固定提交与活动树快照。结论：优先借鉴 **Aizawa 的按需资源签名识别**修复 FSR 变种漏接，借鉴 **CXP 的原生互操作、参数 ABI 与 Present 所有权**支持 GIMI。两者都不能直接替代本项目的异步 FG 输出与 GPU 退休链。

本次只读审计产品；仅新增本文。没有构建、部署、修改 profiles 或操作游戏。两个 DS 审计任务均因 429 失败，以下结论由 Lead 直接读取源码、补丁和本地日志核实，不冒称独立复核通过。

## 版本与证据范围

| 对象 | 本次实际审阅版本 |
|---|---|
| AizawaHikaru233/genshin_fsr_brigde | 实时 HEAD `26b0b7b7d7a0f30f05f213237c5449cc7d6b43a6`；提交时间 2026-10-29 11:55:18 UTC |
| CXP-2024/GIMI_reshade_dlss_integration | 实时 HEAD `cc4697187cb57bacc7c932b7f53f16be24af152a`；审阅其当前补丁、架构、来源清单和 Bridge 配置 |
| 当前产品开发树 | `D:/CODE/DLSS5_NEXT-workers/f6-rewrite`，`db0e646` + 未提交改动；不是原根 `master` 的旧产品快照 |
| 最新掉线采集 | PID105592，2026-11-08 06:47—06:49；主会话保存的 `build/genshin-105592-OptiScaler.log`；部署收据 SHA256 前缀 `916B5977`，不宣称已核 loaded-memory SHA |

Aizawa 六份、CXP 十份关键文本的本地 `git hash-object` 均与指定提交 Git tree 中的 blob SHA 相同。下载副本分别在 `D:/CODE/DLSS5_NEXT-workers/research-{fsr,gimi}-lead-20261108`。本机 `D:/CODE/genshin_fsr_brigde` 是 thx114 fork，未拿它代替 Aizawa 当前上游。

CXP 的来源清单固定 Bridge 为 **`620f47ca3f6959bc27b7866e4f8db813df8bbcc4` / 1.2.3.0，DLL 未修改**；因此其 GIMI 兼容不是另外实现了一套 FSR shader 识别器。外置 `nr-before-sr`/`nrchain` 有二进制与来源说明，本文没有审计其不可见源码。[C1]

## 掉 DLSS：已确认的缺陷与尚未证明的部分

PID105592 的同一日志中：

- 06:48:19.772710：固定七输入签名命中 `78057A29AF6C2D99`，15,388 B。
- 06:48:56.315820：最后一次 D3D11 SR Evaluate，全局共 468 次。
- 06:48:56.845810：同类资源签名命中 **`FEAC669E48FD931E`，15,656 B**；仍是 DrawIndexed(3)，CB0=496 B，输入 2304×1440，输出 2560×1600。
- 06:49:01.791924：Render generation 已到 645，目标 production generation 停在 473；提交与 copyback 均停在 468。

这是“新变体确实存在，而旧哈希生产门拒绝它”的直接机制证据，足以把修复优先级放到入口识别；尚未通过修复后的游戏运行证明它是所有场景掉线的唯一原因。两个签名观察不表示中间所有帧连续匹配，也不授权把观察布局直接当作数值合同。

活动代码的具体阻断点：

| 位置（审阅时行号） | 固定约束及修复意义 |
|---|---|
| `GenshinLegacyCapture.cpp:1504` | `BeforeDraw` 在读取生产 bindings 之前仅承认旧 accumulate hash；新 shader 会走 1508—1511 的拒绝路径 |
| `GenshinLegacyCapture.cpp:2924`、`GenshinDrawAdmission.h:20` | SR-only 快速拒绝把“有身份记录但未列入家族”的 shader 当非 FSR；新变体不能先被此处吞掉 |
| `GenshinNativeMrtContract.h:116,144` | 源合同与 schema 生成继续要求旧 hash；只改第一处仍不能闭合输入链 |
| `GenshinNrProfile.h:27`、`GenshinFamilySkip.cpp:280,590` | NR profile 固定旧长度/FNV/SHA256，需要按数值语义独立版本化 |
| `GenshinTypedGuides.cpp:29,162`、`GenshinFgGameMotion.h:80` | 深度/MV 与 FG 坐标适配仍约束旧 shader，不能默认继承新变体资格 |
| `GenshinEngineProducer.cpp:24`、`GenshinRawObservation.h:150` | 相关生产/源观察路径同样要检查；是否订阅该运行路线另行确认 |

上述路径相对目录为活动树的 `OptiScaler/genshin/`。主会话在并行开发，行号和 WIP 会变化。`Shader::Known()` 只证明对象/创建记录身份，不等于已确认它不是任何 FSR 变种。

两份已提取 DXBC 的 SHA256：旧版 `E2CFA165B0EBF36F655C7E9EF7823B6AFED1113E18D6FCB7D82FE5877DF5ADBF`，新版 `F75FB726E8661C5EB5F639B47FA99145A7D47E07B3D2AC0F43570D42D93A9AFE`。新版确实增加 `t7`、`s1` 及相关采样，不是只有文件摘要变化。具体新增资源意义、颜色/运动/透明度/历史语义须审查；不能自动给它旧 NR profile。

## Aizawa：应该借鉴什么

**1. 把资源布局作为识别入口，哈希作为可验证的快捷索引。** `inspect_target_upscaler_draw_on_demand` 先读取双 RTV，再检查 SRV/CB/viewport；固定布局是 t0 色、t1 16×16 LUT、t2 深度、t3 MV、t4 flags、t5/t6 历史，RTV0 metadata、RTV1 色。固定布局失败后，`identify_target_upscaler_resources` 按格式/尺寸/槽位评分寻找另一条路径。该按需路径不要求指定 PS hash，正好覆盖当前漏接类型。[A1]

移植时，结构匹配仅产生候选；必须由已审 schema 明确槽位、视图 mip/format、CB 字段、jitter、MV 解码和输出角色。动态评分用于发现/诊断，不能仅凭“像深度/MV”授权 NR/FG。未知 shader 不应立即被永久记作非 FSR。

**2. 普通 setter 直通，候选处按需取状态。** 上游 Mode2 OnDemand 下，PS SRV/CB、PS shader 等 setter 可直接调用原函数；目标 draw 只识别一次，把结果传入 `try_fsr2_translation_draw`，避免再次查询。[A1]

本项目应复用已证明覆盖完整的 PS binding cache；未覆盖的状态走当次真实 getter。depth writer、GIMI、BeforeSR 等实际消费者仍需自己的订阅，不能为 SR-only 优化全局关闭观察。现有 `ReadBindings` 已先筛双 RTV，并只查询 t0/t2/t3，已复用强引用视图的不可变描述；这些不是待从零引入的新功能。

**3. 正缓存多种布局，变化后能重新发现。** 上游最多八项成功布局，按槽位/尺寸/格式形成指纹，命中时读取所需视图并核验；不把第一次遇到的 shader 当成永久唯一目标。[A1]

在本项目只缓存不可变布局/字节码事实，每次生产仍绑定真实 context/device/epoch、当前 view/subresource、Render token 和 generation。上游缓存快路径省略部分 CB/viewport 条件，不能原样当作本项目源授权。其旧 `FastStateTracking` 学习单个 hash 的路径仍在；应借鉴默认 OnDemand 路径，不启用单目标锁定。CXP 发行配置也明确采用 `Fsr2FastStateTracking=0`、`Fsr2Mode2OnDemandState=1`。[C2]

**4. 间断恢复重置 temporal history。** 上游记录 takeover gap、路径改变和当前 generation，恢复时 reset；本项目可保留相同意图，但以确切源连续性/场景/尺寸变化为主，500ms 时间阈值不能证明帧身份，更不能证明 GPU 完成。[A1]

**5. 明确 FSR2 公共接口边界。** Bridge 通过六个标准 FSR2 导出发现协议对接 OptiScaler，由后者负责 backend；若将来拆独立桥，这是维护接口的参考。当前产品已有内部入口，不需要再叠加一层扫描/注入。[A2]

**FG 的真实边界：** Aizawa 有 `DlssgDxgiWorkaround`，会处理 fullscreen/ResizeTarget/无变化 ResizeBuffers；并不是完全没有 FG 兼容代码。但它没有提供本项目所需的完整异步 SDK physical stream、生成帧 owner、最终输出完成证明。不能用它的 RTV 捕获替代这些机制；部分 DXGI workaround 直接返回 S_OK，也不应全局照搬。[A1]

## CXP：应该借鉴什么

**1. 一个游戏 wrapper，显式原生互操作接口。** GIMI 的私有 Context IID `91ACFD68-5A6F-45EA-B8D0-71ACC32151B7` 返回保存原始函数的 context trampoline；Device IID `DB17DC9A-5A5A-4AC7-A4CE-EF41F7C51D5C` 返回原生 device。普通游戏/MOD 绘制继续走 GIMI，私有 NGX 初始化与工作走同一原生设备域，避免递归和设备身份混用。[C3]

这是定制 GIMI 的 ABI，不是任意 GIMI 都有的标准接口。当前产品源码未检出这两个 IID；接入时应做可选能力协商、持有接口和生命周期，不用指针偏移猜 unwrap，也不取消当前游戏原生 device/epoch 的验证。

**2. typed/untyped NGX 参数槽必须一致且完整恢复。** 后续 delta 明确同时保存、设置、恢复 D3D12 typed 与 legacy `void*` 槽，避免表面换了 FP16 carrier，而观察者仍读到旧 packed 资源。[C4]

当前树已有 `BridgeResourceParameterGuard` 和生产前实际 getter 回读；应检查真实 native NGX 参数表是否分槽，再扩展现有 guard，覆盖 null/失败/早退，避免另写第二套参数管理。

**3. 只在格式合同需要时使用 FP16 carrier。** 补丁为 R8/R10/R11 packed/typeless 输入/输出提供可用 typed carrier，通过实际 shader transfer 转换；NR→SR 或 SR→NR 录入既有 D3D12 list 后，再转换写回游戏输出。[C4]

这是格式兼容方案，不是让所有路径额外复制一遍。先证明模型输入格式和颜色域不被当前路径接受，再选择转换。格式同为 FP16 不代表色域/曝光已正确；同一 list 的顺序也不能代替跨 API 的 fence/query 完成。

**4. 最终 Present 只设一个 owner，后处理走显式阶段。** GIMI 使用 ReShade hosting API 在最终 Present 前跑 effects，禁止 ReShade 再接管 graphics wrapper；补丁补发 hosted add-on 每帧事件，并传 HDR10/PQ 色彩空间、处理 resize 生命周期。[C5]

可借鉴“单 owner + 显式 stage + frame 事件”的边界。默认不搬其 ReShade core 补丁，也不把所有 effects 搬去 BeforeSR：需要低分辨率输入的 effects 和最终图像 effects 应分别接现有阶段；采用标准 hosting 时必须核实实际 add-on 事件是否完整。

**5. 载入模块与 API 类型准确识别。** CXP 排除 `nrchain_nvngx.dll` 被 `_nvngx.dll` 后缀拦截的冲突，并防止把 D3D11 context 交给 D3D12 feature 的计时接口。[C6]

当前树已有 `pureDx12Feature` 计时分流（`wrapped_swapchain.cpp:1062`），不能再报该项为当前已证漏洞；只有采用 CXP 外置 nrchain 时才需处理该文件名碰撞。一般化价值是缩窄 loader 匹配与使用有类型的 API 入口。

**明确不移植：** CXP 主补丁含 `framesLeft=32` / `TickNrRetired`，按 Evaluate 次数释放旧资源；其发行路线说明关闭这套内置 NR。无论它是否在特定包中执行，这种退休策略都不能导入本项目：使用实际所属 queue/fence、query 和最后使用票据。queue-affinity GUID 只标识 queue，不证明该 queue 已完成。[C4][C6]

## 实施优先级与验收

| 优先级 | 应做的完整改动 | 验证目标 |
|---|---|---|
| P0 掉线 | 在现有 Family/NativeMrt 中统一 schema 与 profile 查询，消除各层散落旧 hash；审阅旧/新 shader；补齐 production、NR guide、FG reference 的相应适配 | 登录→大世界→技能/切场景，新旧 shader 都持续生产；不因旧 hash 停 SR；分别确认 NR 和 FG 当前源 |
| P1 CPU | 保留有效缓存，候选一次取状态并沿链传递；普通路径减少无订阅观察、重复 getter、锁与字符串构造；重型诊断按开关/有限采集启用 | 同场景同设置比较 hook 非重叠耗时、实际帧间隔和卡顿，不把缓存命中率当功能成功 |
| P1 连续性 | 源变种/尺寸/owner 变化时恰当 reset；保留仍在途资源，SR/NR/FG 独立回退 | 没有新源时保原生画面；恢复使用新源，没有旧帧闪回和多推进 history |
| P2 GIMI | 可选原生 Device/Context ABI、单 Present owner、参数槽与格式 carrier 适配 | 同局完整模型 MOD 与纹理 MOD、重载、resize、效果阶段均正确，确认游戏调用仍过 GIMI |
| P2 NR 性能 | 利用已有 BeforeSR 选项比较 NR→SR 与 SR→NR；必要转换留在既有 queue/list | 同画质目标对比总 GPU 时间、画面细节与延迟；不能以更小 NR 工作尺寸单独冒充净性能收益 |

建议结构是一个可查询的 schema/profile 注册表，外加现有对象上的当前调用事实；不复制两套 FSR 生产框架。分别表达：`候选布局`、`已审 SR 源语义`、`NR 数值 profile`、`FG 坐标/连续性资格`。允许不同摘要共享经证明相同的数值 profile，但不能只因布局相同共享全部能力。新增变种应集中登记/适配，所有相关调用点查询同一结果。

本局登录卡顿单独处理：PID105592 在 06:48:02 起确有 `R448 Hook CPU ... enabled=true` 的重型采样日志，尽管此次用户无需主动开启它；来源可能是已有配置或进程 override，本文未改配置，也未证明来源。三次已输出的签名扫描实际仅 606/899/1297 draws，不是每窗耗尽 20,000，也不是整个登录期持续扫描；没有开关 A/B 不能把登录卡顿唯一归因给扫描。应先核实际采样/日志状态，再比较关闭诊断后的登录帧间隔，继续排查登录期普通 hooks 的成本。

实机最终验收还须确认选定 RTV1 真被游戏后续消费、生成输出落到正确 display owner，以及各自 queue/fence 完成。CPU/WARP fixture、SR Evaluate 或 copyback 次数都不能单独证明这些结论。原神专用槽位/CB/shader 数值不套给星铁与 ZZZ；只共享生命周期、参数表、可选互操作和采样机制。

## 固定来源

- [A1：Aizawa 当前 Bridge 源码](https://github.com/AizawaHikaru233/genshin_fsr_brigde/blob/26b0b7b7d7a0f30f05f213237c5449cc7d6b43a6/Dx11FsrBridge/Dx11FsrBridge.cpp)：关键位置 7759、7811、8080、8145、8242、8344、8495、8554、8612、8651、10436、12091、12284、7136、7176、7225。
- [A2：OptiScaler compatibility contract](https://github.com/AizawaHikaru233/genshin_fsr_brigde/blob/26b0b7b7d7a0f30f05f213237c5449cc7d6b43a6/Dx11FsrBridge/OPTISCALER_COMPATIBILITY.md)。
- [C1：CXP 来源与固定版本](https://github.com/CXP-2024/GIMI_reshade_dlss_integration/blob/cc4697187cb57bacc7c932b7f53f16be24af152a/third_party/UPSTREAM-COMMITS.md)：Bridge 20—27 行、二进制归属与许可 49 行起。
- [C2：CXP Bridge 配置](https://github.com/CXP-2024/GIMI_reshade_dlss_integration/blob/cc4697187cb57bacc7c932b7f53f16be24af152a/components/Bridge/Dx11FsrBridge.ini)：34—46、101—114 行。
- [C3：GIMI Context/hosting 补丁](https://github.com/CXP-2024/GIMI_reshade_dlss_integration/blob/cc4697187cb57bacc7c932b7f53f16be24af152a/src/patches/3Dmigoto-GIMI-hosted-reshade.patch)，[原生 Device delta](https://github.com/CXP-2024/GIMI_reshade_dlss_integration/blob/cc4697187cb57bacc7c932b7f53f16be24af152a/src/patches/3Dmigoto-GIMI-native-device-interop.delta.patch)。
- [C4：OptiScaler 后续 delta](https://github.com/CXP-2024/GIMI_reshade_dlss_integration/blob/cc4697187cb57bacc7c932b7f53f16be24af152a/src/patches/OptiScaler-GIMI-RTX30-dual-mode.delta.patch)：307—369 行 affinity，387 行起参数槽，574—611 行写回/恢复。
- [C5：ReShade hosted events 补丁](https://github.com/CXP-2024/GIMI_reshade_dlss_integration/blob/cc4697187cb57bacc7c932b7f53f16be24af152a/src/patches/ReShade-hosted-addon-present-events.patch)，[CXP 架构说明](https://github.com/CXP-2024/GIMI_reshade_dlss_integration/blob/cc4697187cb57bacc7c932b7f53f16be24af152a/docs/ARCHITECTURE.md)。
- [C6：OptiScaler 主补丁](https://github.com/CXP-2024/GIMI_reshade_dlss_integration/blob/cc4697187cb57bacc7c932b7f53f16be24af152a/src/patches/OptiScaler-DLSSOn12-GIMI-pre-NR.patch)：487—539 行退休策略，1986 行起 hook/loader，2032 行起 context 解析，2370 行起格式转换，2485 行起 API guard。

引用代码需保留各组件原有许可/署名：Aizawa Bridge GPL-3.0-or-later；CXP 根目录 MIT 不覆盖其第三方 GPL 补丁与二进制声明。本次未复制上游产品实现。
