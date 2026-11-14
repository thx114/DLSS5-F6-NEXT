# F6 与 ReShade / GIMI 兼容性

更新：2026-11-14（UTC+08）。本文以本机固定核心、当前 F6 源码、真实 Windows / WARP 测试和已锁定的用户游戏日志为依据。新候选的游戏画面、Rocket 退出与性能仍待用户新局验收；不更新 `F6-HANDOFF.md`。

最终源码 `44380f86f4611f51e9c08de77e8668eab0f4f3f4` 的 Release x64 已构建并原位部署到既有 `local/F6-StarRail-GapPacing`、`local/F6-ASI`：SHA256 `082361809BA0256E1AD898CFE280AC21610F9601E59AB680B11191E8380A5F49`，32,540,672 B。两目标 FileId 和 2 / 6 个硬链接不变，核对的 INI / ReShade core 哈希不变，本轮没有 `.next`。磁盘回执为 `f6-installed-after-final.json`；完整验证与实际加载边界见 [当前状态](CURRENT-STATE.md)。

## 当前实现

原神继续使用外部 `genshin_fsr_brigde` → 普通 NGX → F6，内部原神 FSR 捕获链不恢复。`Enabled=auto, Stage=BeforeSR` 现在启用阶段调度；显式 `Enabled=false` 优先。本轮没有改用户 INI、preset、启动器选项、profiles 或 ASI。

BeforeSR 在当前生产输入的 handoff 后、SR Evaluate 前执行。共享颜色和深度使用同一源帧、源代次及真实 producer fence；独立 reader admission 与实际同队列 Signal / DX11 GPU Wait 保护后续写入。稳定的私有深度 copy 由对应 owned runtime / color bundle 持有，源 A/B 纹理轮换不会重建绑定或混用帧。CPU 返回不能归还未知 GPU reader。

BeforeSR placement 对已登记的源链和物理输出链抑制自然 Present 效果，包括 worker 回调；不依赖某次成功的颜色 receipt，也不永久关闭用户 effects 或修改 preset。阶段编译或输入尚未就绪时等待当前路径，不转到 Present 补画。

| F6 路径 | BeforeSR | BeforeNR / FinalOutput |
|---|---|---|
| DX11-w-DX12 外部 NGX 桥 | 已有生产入口，本轮加入同帧深度、LP/MXAO 契约和自然效果去重 | 普通 FinishedPicture / 物理输出保留；原神默认 Present NR 仍缺独立 BeforeNR 入口 |
| 普通原生 DX12 NGX | 无独立 recording slot 时继续明确拒绝阶段执行 | 既有入口保留 |
| NativeProducer / 虚拟超分 | 受实际提交与 depth reader 权限约束的独立入口保留 | 取决于实际 native 路由 |
| Vulkan 原生 / Vulkan-w-DX12 | F6 阶段调度未支持 | 全部 Vulkan 后端保留；普通 ReShade Vulkan 自然效果另行游戏验收 |

现有阶段颜色支持 RGBA8/BGRA8 UNORM 及 sRGB、SDR；HDR、FP16、10-bit 和 typeless 不属于当前 owned stage 执行契约。保留 SR/NR/FG 和所有真实 queue/fence、allocator/list Reset 与资源退休。

FG 物理端点仍要求已观察 provider 创建域、同线程输入 scope、SDR 和 flip-discard；TEST、DO_NOT_WAIT、dirty rect / scroll Present 会跳过。`UnknownPhysical` 不区分原图与生成帧，不能从提交次数推导每张生成帧都已处理。

## LAUNCHPAD、MXAO 与光流

本机 `Mod OFF.ini` 快照选择 LAUNCHPAD → MXAO，`NORMALS_MODE=1`、`HOYO_NORMAL_RADIUS=1.19`、`MXAO_USE_LAUNCHPAD_NORMALS=1`、`HOYO_NORMAL_BLEND=1`。这些是已锁定的磁盘设置，不代表旧游戏局整局都使用它们。

MXAO 需要当前深度。LAUNCHPAD 为它生成基础/平滑法线；`NORMALS_MODE=0` 仍生成基础法线，只关闭相应平滑/细节分支。本轮要求真实深度和正确效果顺序；启用共享法线时，首批 LP→MXAO 在私有 scratch 上预热 IPC，保留真实 completion fence，下一批才允许写回 SR 输入。

已修正一个真实核心初始化问题：首次 preset 加载前设置深度方向宏，会把尚未初始化的宏表整体写回私有 preset，丢掉 `MXAO_USE_LAUNCHPAD_NORMALS=1`。现在等待真实首次 reload 回执，再投影方向宏并等待其 reload；选中效果的 owned 有效 MXAO 宏也须与 natural selection 一致。深度定义缓存不再把每次反射 graph 计数当成编译输入，避免同一定义反复 reload。原 shader 和用户 preset 保持原样。

LAUNCHPAD 也能估计图像光流。`OPTICAL_FLOW_Q=0` 表示 Low，并非关闭；实际生成取决于 IPC 请求。当前 MXAO 法线平滑不要求 F6 先接入光流；本机 MXAO 未启用 TAAU 宏，不能据此宣称旧局持续产出 flow。F6 当前没有读取 LAUNCHPAD flow，也没有向 ReShade 提供游戏 motion semantic。API20 可反射该纹理，但它是图像估计流，不能直接当成 SR/FG 的游戏几何运动矢量。

## Rocket / GIMI 与加载器

已锁定 Rocket 会话 `20261114-042258_YuanShen_228728`：约 38.3 秒后退出 `0xC0000135`，SR Create/Evaluate、NR cadence、FG dispatch 均为 0。XXMI 1.2.2 的主动 `DoubleBeepExit()` 也使用这个退出码，不能仅据此归因为缺 DLL。没有对应 dump/WER；最后只证实 DX12 设备创建，尚未证明进入哪条主动退出分支。

本轮统一真实 loader 参数和观察入口，保留 Win32 hFile/flags、原 ANSI 字节、native PathToFile/NTSTATUS 及 UNICODE_STRING 长度、last-error；非执行加载不参与重定向/初始化。静态依赖只观察已完成初始化的真实模块，Hook target 进程期 pin。成功安装才记录 applied；失败保留并由单个延时 worker 推进，不依赖下一次偶然加载。修正 FSR3 启动重复安装、OTA Streamline 路径漏观察及若干失败状态误报成功。

内置 GIMI 独立实现来自 `a768969...`，主树保存点 `06b0724`。公共入口现在区分早期 DeviceCreation 候选与显式选定的源 swapchain/native pair/代次；辅助链只退休，不能由 depth watcher 冒充游戏 owner。构造时 HWND 尚不可见可在 game-facing Present 按保存的游戏 HWND 重试。source 失效和 capture 确认使用同一 owner 提交锁，过期 Acquire 不能再清掉新设备历史。PRE/POST 共用一次解析的 native pair 和可撤销源代次；shader 元数据按实际 native device 登记，允许 proxy→native 内层回调，仍由资源身份去重。嵌套 native factory 返回后独立观察实际设备，外层包装策略仍只执行一次。

这些是源码和离线验证结果，不是 Rocket 崩溃的实机修复结论。新增 queue-create、interop prepare、hidden DX11 swapchain 的进入/返回日志，为下局定位保留边界；不删 GPU 同步或扩池掩盖断链。

## 固定核心与验证

本机核心：`HoYoShade/ReShade64.dll`，FileVersion `6.8.0.2155`、API20、5,592,064 B；SHA256 `0CEE63F9C9F13F3AC909C5B4903F4DBB4B719A7AB3B4F13B0DEAF83C814B94F7`。F6 连接还核对路径、API 和唯一导出核心；仅版本相同不等于接受。没有修改 ReShade core。

连接时机限制仍在：核心缺失时最多尝试 30 次、间隔一秒，`connectStarted` 单向置位；本进程首轮失败后切换开关不会重连。延迟注入超过约 29 秒可能错过窗口。本轮没有将这一已知限制当作 Rocket 退出根因。

证据统一位于 `build/genshin-reshade-20261114/`：

| 验证 | 当前结果 |
|---|---|
| 真实固定核心颜色 BeforeSR | `real-core-color-final.log`，270,405 检查；含自然链/worker 抑制和重新启用 |
| 真实固定核心同帧深度 | `real-core-depth-final.log`，221,337 检查；双源轮换、当前帧/方向与 reader completion |
| 原样 LAUNCHPAD + MXAO | `real-core-launchpad7.log`，75,588 检查；首批不写回、MXAO 改色、真实法线随深度变化、缺深度/错帧拒绝 |
| Windows loader | `loader-final5.log`，20 个实际参数/嵌套场景，以及生产 observer 和自动失败重试/OTA 测试 |
| GIMI 公共入口 | `gimi-present-final2.log`，真实双 WARP 设备/三条 chain、晚 HWND 登记、选源/确认锁交错和生产嵌套 VS/PS 回调；`gimi-routing-final2.log`，93 内容检查/100,000 并发发布 |

以上不包含 NGX SDK / 游戏显示或性能验收。完整 CommonCore、NGX、UI 和最终 Release / 部署回执见当前状态文档；不能用旧产物替代最后候选。

## FSR Bridge PR15 与上游

重新核验的 main 为 `c37d3a54ee736595bd9d461e11ab475dd137a00e`，PR15 head 为 `97d2b06af415a6e8b4d4660b3dd6b5c1ae23db95`，仍未合并。PR head 缺 main 分叉后 10 个提交。预测合并候选真实 `NgxParamMapTest` 为 3,657 检查、18 失败，因此没有原样合入。

最终复核通过 GitHub API 于 `2026-11-14T00:09:01.7895314Z` 完成，上述 main / head 未变化、PR 为 open / merged=false，回执 `upstream-final-recheck.json`。

阻断点：PR 清掉低分辨率 MV 标志，却仍交低分辨率纹理，违反 F6 FG 输入契约；新增字段默认值/测试未闭合。安装器改配置、复制 ReShade 与信任根等行为不纳入本次集成。

已采用最新 main 加独立的 `Ffx12SingleInstance` 配置接管控制。源码保存为隔离仓库提交 `3ec47e3de096fd7f64872f819d108a0535938884` 和 `bridge-source-3ec47e3.zip`。Release x64、NGX ON、release runtime ON、diagnostics OFF，4/4 CTest 与 NgxParamMap 全部 3,657 检查通过。

已原位部署既有 `temp3/Dx11FsrBridge (2).dll`：SHA256 `B5966CFF7A7A54D9B8471FDC7F956A0D7AA409F01D16BD37DCAAE6F31D22B240`，662,528 B；FileId `0x000000000000000000040000001d8630` 和 2 个硬链接保持，桥 INI/ReShade core hash 不变。回执为 `bridge-installed-after.json`。尚无新局实际加载/画面验收。

参考：`https://github.com/AizawaHikaru233/genshin_fsr_brigde/pull/15`、`https://github.com/AizawaHikaru233/genshin_fsr_brigde`、`https://github.com/CXP-2024/GIMI_reshade_dlss_integration`、本机固定核心 API20 SDK 与 iMMERSE 原 shader。

## 2026-11-14 原神实测证据

证据锁定于 `build/genshin-reshade-20261114/`，复制前后的源文件与副本 SHA256 一致，清单为 `locked-files.json`。时间按会话记录的 UTC+08，不再追加八小时。两局正常退出，退出码均为 0；未读取加载内存 SHA。会话记录的 Opti runtime 路径与部署的 E454F55C 候选对应；编译版本文字仍为当时的 `5d0f0ae`，不能单凭该文字区分本次重构 DLL。

| 项目 | 普通局 | ReShade 局 |
|---|---|---|
| 会话 | `20261114-024410_YuanShen_57172` | `20261114-024706_YuanShen_230352` |
| PID | 57172 | 230352 |
| 启动至退出 | 02:44:09–02:46:22 | 02:47:04–02:50:08 |
| NGX Evaluate 入口数 | 1446 | 1571 |
| 前台且 Opti 菜单关闭的源间隔数 | 816 | 341 |
| 源间隔 P50 / P95 | 47.408 / 58.934 ms | 59.198 / 72.326 ms |
| 源间隔中位数的倒数 | 约 21.1 源 FPS | 约 16.9 源 FPS |
| 有效唯一前台 Reflex 样本数 | 68 | 33 |
| GPU 开始前队列 P50 / P95 | 26.7895 / 63.724 ms | 48.136 / 191.133 ms |
| NR GPU partition 样本数 | 12 | 5 |
| NR model 中位耗时 | 28.7458 ms | 27.2046 ms |

统计脚本为 `analyze.py`，完整结果见各局 `analysis.json`。源间隔混有未完整标记的游戏菜单、场景切换和生命周期空档，不能用整局平均值表示稳态。没有对应 Display／PresentMon 采样，FG Present 次数和请求倍率均不能补成显示 FPS。两局场景、前台区间和 NR 历史设置不同，上表差值不能单独归因于 ReShade。

第二局两份 ReShade 配置都指定同一 preset，但 runtime 分别为 DX11 源链和 DX12 FG 链；必须验证选择与去重。Opti 日志的启动快照为 `parsedEnabled=null, latchedEnabled=false, latchedStage=1`，随后仍为 `enabled=false stage=before-sr`，没有 API 注册接受或阶段执行证据。

既有 INI 中的 `Dx11ReShadeOnly=true` 当前没有 F6 读取入口，不能据此认为 DX12 自然 runtime 已被排除；本局实际观察到两个 runtime。

当前磁盘 preset 已另外保存为 `preset-current-Mod-OFF.ini` 与 `preset-current.json`，选中 Launchpad、MXAO，SHA256 为 `4F78042CD69EF64835A6BC1B23B396DE50E32CAF99B3F04CD5090FE3E826EE3D`。这是读取时的磁盘快照，两局的会话归档没有保存 preset 正文，不能据此证明整局的技术选择。

两局均有一次 shader observation Hook 拒绝，日志 `32` 为十六进制 `ERROR_NOT_SUPPORTED`；ReShade 局另有一次 bridge recording 清退 `E_INVALIDARG`。这些是旧候选证据；本轮重整了 Hook / loader 入口，尚无新局证明它们不再出现。真实 GPU 完成与 Reset 条件继续保留。
