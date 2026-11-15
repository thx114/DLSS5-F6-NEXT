# Neo AO／非 AO 无 DLSS 调查（2026-11-16）

用户澄清“新版和旧版”为两个 Neo 桥：带 AO 和不带 AO。**Neo 已复现并修复 Present 后丢失 Draw 入口的缺陷，游戏接入仍待新局验收。** 下列时间沿用原日志的 UTC+08:00；宿主通过不能代替游戏通过。

## 最新：Neo 已修复 Present 后的入口丢失

Neo 会话同步后，本会话只读核对候选身份、源码接线和原始回执，并将回执复制至 `build/bridge-reconnect-20261116/neo-present-repair/`；来源与 SHA256 见该目录 `sources.json`。没有重跑测试、修改用户设置或再次部署。

故障在 Neo 执行入口：原来只 patch context 虚表，真实 DXGI Present 后 Draw／DrawIndexed 槽会恢复，后续绘制失去桥接管。此前本会话的混合8例及首绘4例没有覆盖 Present；它们通过仅证明各自创建与首绘路径，不能排除此故障。该验证缺口现由新负控覆盖，不用 F6 的 SR／NR 算法解释入口丢失。

新桥在创建时用交换链 private-data interface 持有对应 execution context；最外层 Present／Present1 调用返回后、返回游戏前，发现 Draw 入口丢失才重装配置要求的钩子。当前接线为 `runtime/ContextDispatch.cpp::bind_present_execution`、`GraphicsIngress.cpp::publish_creation` 和 `Dx11FsrBridge.cpp::repair_present_execution/route_present`。不新增逐 Draw 检查；诊断 Present 工作与入口修复分别受控。

| 原始回执 | 核对结果 |
|---|---|
| Neo `restore-dlss-baseline/providers-20261116-014702-302/result.json` | 旧候选 `50BA73C1…`，`ngx-v1-present` PID271836、`ngx-nvidia-present` PID296844 均 exit1；`draw=0 indexed=0`，只读回原生红／绿像素，预期 provider 像素失败。 |
| Neo `providers-20261116-014849-922/result.json` | 新桥 `682FB302…`，F6 V1 PID294964、NVIDIA PID292308 两项均 exit0；每项 Present 后连续两帧像素正确。F6 两次回执均 `status=2 result=1`，桥两次 `DISPATCH_OK/f6_receipt_v1`。 |
| Neo `mixed-neo-20261116-015703-940/mixed-result.json` | 8/8 exit0；均为真实 F6 DX11-w-DX12 ForHwnd 包装，覆盖 Present／Present1 及两加载顺序，候选与像素通过。 |
| Neo `providers-20261116-020038-638/result.json` | AO 硬件四组4/4，分别覆盖 F6／NVIDIA provider 和 AO 开／关。 |

新候选位于既有 `D:/CODE/DLSS5_NEXT-workers/Neo-FSR-Bridge/build-neo-release/candidate-before-sr-ao/FSR-neo-AO.dll`，**755200 B，SHA256 `682FB302C5FDDF1CAE2DB37586D0B011B39EB127FFB9D6E92598F98E5C52D413`**。同目录 INI 已为 `NgxProvider=external`，旧 DLL／INI 的 `.bak-20261116-020350` 均存在。这里只记录 Neo 已更新的候选，未覆盖 temp3 或 F6。候选清单的三套 CTest 为75/75、64/64、78/78，本会话读取该清单，未独立复跑这三套测试。

当前安装 F6 仍为 `3de4270`／`B81F992EB024EFCA91729CDA6CBFFC24F2E5E194D61937D8E1CA7235229BA3EF`；新 provider 回执也使用该 SHA256。`7f15810` 的 BeforeSR 候选仍未部署，不能用本次入口修复宣称 BeforeSR／MXAO、NR 残影或性能已验收。用户下一局手动测试的实际 PID、桥新哈希与 F6 标记仍需配对。

## 修复前游戏证据

已将两局原始桥日志、完整 F6 日志与 session／Hub 记录保存在 `build/bridge-reconnect-20261116/`，避免同名桥日志被下一局覆盖。汇总为该目录的 `result.json`。

| 项目 | AO | 非 AO |
|---|---|---|
| 游戏 PID | 210300 | 268808 |
| session | `20261116-003128_YuanShen_210300` | `20261116-003448_YuanShen_268808` |
| 桥日志首时间 | 00:31:27.524 | 00:34:49.026 |
| 桥日志末时间 | 00:32:16.771 | 00:36:01.421 |
| 桥加载路径 | Neo `build-neo-release/candidate-before-sr-ao/FSR-neo-AO.dll` | Hub `temp3/Dx11FsrBridge (2).dll` |
| 桥测试后磁盘 SHA256 | `50BA73C1835B5D4457308AB62D115F83F6CDA447992DDCFCC7E9CF70DBEA7F81` | `13617E0FAAF7DDCAD227A52EA5127844EE9F92D93FC26D0C9345A21FB9C554E9` |
| 同 PID F6 标记 | `3de4270` | `3de4270` |
| 创建流程 | D3D11 系统入口、CreateDevice、ForHwnd 有回执 | 同左 |
| 普通 Draw 激活／FSR 候选／provider 派发回执 | 均未记录 | 均未记录 |
| F6 SR Create／Evaluate 回执 | 均未记录 | 均未记录 |

两局 F6 均有日志头、同 PID 记录及持续 Present。Streamline 初始化、模型加载和 NGX 缓存查找不是 SR 求值证据；不能从其中的成功或缓存失败推断这次桥的 DLSS 已执行或失败。上述桥哈希是测试后磁盘身份，不冒充运行中映像哈希。

## provider 选择与后续同步

调查开始时，两份桥 INI 均为 `NgxProvider=nvidia`。Neo 的 `ngx/ProviderDiscovery.cpp::discover_provider` 在该模式只选择 NVIDIA NGX，不会把 SR 交给 F6；这可以解释 F6 无 SR 输入，但不能解释更早的无 Draw 激活回执。

Neo 会话随后同步：已按用户先前“只调 INI”的授权，把 **temp3 非 AO 桥** 的该键改为 `external`，Win32 读回一致；本会话只读复核确认。当时 AO 候选目录仍为 `nvidia`，其后续变化以顶部 Present 修复记录为准。不把后改配置回填为上述旧局配置，也不在本会话重复修改。

已复核 Neo 的 `build-neo-release/f6-input-route-20261116-005328/result.json` 及原始 provider 回执，并复制到本证据目录的 `neo-configuration-change.json`／`neo-external-provider-host.json`：

- 当前磁盘旧 Neo `13617E0F…` 配安装 F6 `3de4270`／`B81F992E…`；真 RTX 隔离宿主 PID261096，`ngx-v1` exit 0。
- 两帧 F6 均 `status=2 result=1`，桥均 `DISPATCH_OK`／`f6_receipt_v1`；输出像素分别 `8,24,224,255` 和 `175,88,32,255`。
- 夹具关闭 NR／FG／HoYoShade，不加载 ReShade。这证明该组合能通过显式外部 NGX 路由交付本帧输出，不证明游戏绘制已被捕获。

## 本会话补充的实际二进制控制

旧混合宿主使用的 F6 为 `BF3B4EF8…`，不能直接代表当前安装的 `B81F992E…`。本次分别复制两个实际 Neo DLL，配安装 F6 `3de4270`，使用同一份当前游戏 INI 的隔离副本，仅将测试日志改为相对路径、关闭夹具 ASI 加载。

既有 `Test-NeoF6Interop.ps1` 共 **8/8** 通过：每个 Neo 各四例，覆盖桥先加载／设备先创建、原 context／返回链回取 context、多线程保护开关及真实像素。均确实创建 F6 DX11-w-DX12 ForHwnd 包装并进入桥的 FSR 候选。原始回执：

- 非 AO：`non-ao-control/mixed-neo-20261116-005541-253/mixed-result.json`，PID290876／270276／249740／293800。
- AO：`ao-control/mixed-neo-20261116-005832-652/mixed-result.json`，PID269640／226168／283492／278196。

既有宿主在首绘前会回取设备／context 并查询扩展接口。另在 `before-getter/` 生成隔离诊断变体：ForHwnd 成功后立即在原 context 绘制，绘制前不调用链的 GetDevice、GetImmediateContext 或 QI；观察函数只读取实际槽地址，避免观察本身通过 getter 重装 hook。MSVC x64 `/O2` 编译 exit 0；**4/4** 通过，两个 Neo 各覆盖桥先加载与设备先创建，PID268700／210244／271976／253080。首绘前槽归属、首次 DrawIndexed 进入 Neo 与像素均成立；原始回执 `before-getter/result.json`。

这些是合成绘制、候选识别与像素控制，不执行游戏 IL2CPP／实际场景。没有复现就不修改生产工厂／Draw 路由，不添加全局 Draw hook，不恢复 F6 内部 FSR 捕获。

## 修复前停点与独立审查项

修复前游戏只定位到“成功创建之后、未观察到普通 Draw 激活之前”。当时 Neo `bind_created_graphics` 忽略 `install_context_hooks` 返回值，ForHwnd 的成功日志也不证明绑定成功；仅凭这些旧局日志不能区别绑定失败、对象不一致、槽被覆盖或实际调用方式不同。MT／READ Map 重装代码均存在，不能简单归因漏迁移或 AO。其后 Present 槽恢复的独立复现与修复见顶部，旧局本身没有逐槽采样，不回填成已取得游戏内根因证明。

Neo 同步的 F6 只读审查已在本地代码确认，作为独立后续问题记录，**不作为这次无 Draw 的根因**：

1. `IFeature_Dx11wDx12.cpp:912-914` 仍会从最终输出 G22＋非 HDR 设置输入 `sdr`；`HoYoShadeBeforeSrInput.h` 的 Legacy 分支保留该值。缺生产者声明的受支持 typed 输入可能误获准入；typeless 仍另有格式拒绝条件。本轮未做该问题的像素负控，不扩大为游戏触发结论。
2. `HoYoShadeOwnedRuntime.cpp` 的 owners 固定表仅登记，Retire 标退休而 TryCollect 清 Entry 后未清 Owner；连续不同 owner 的容量问题需独立回归及退休设计，不能通过扩大槽数处理。

本会话未改生产源码、安装 DLL、启动器、profiles、ASI 或 ReShade core，未构建或部署新产品。`7f15810` 的 typeless BeforeSR 候选仍为 BUILT_NOT_DEPLOYED；安装仍为 `3de4270`。用户独立控制游戏；新局按实际 PID／加载身份验收，不要求为离线控制反复重测。长期性能任务继续暂停。
