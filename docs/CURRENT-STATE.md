# F6 当前状态

更新：2026-11-14。BeforeSR 深度效果、LAUNCHPAD / MXAO、内置 GIMI 公共入口与 loader 修复已完成源码集成、离线验证和 DLL 原位部署。用户新局已确认加载 `44380f8`，但启动仍有退出；外部 FSR 桥的提供者与 COM 转发修复现已原位部署。**新桥的 Rocket / GIMI 游戏稳定性、画面和性能仍待用户新局验收。** 本轮没有修改 `F6-HANDOFF.md`。

产品树：`D:/CODE/DLSS5_NEXT-workers/f6-rewrite`，分支 `codex/f6-rewrite`。最终构建源码保存点为 `44380f86f4611f51e9c08de77e8668eab0f4f3f4`（`Fix BeforeSR depth effects and native hook coexistence`）；后续维护文档提交不改变该构建身份。游戏由用户手动操作；未修改启动器选择、用户 INI / preset / profiles 或 ASI，未推送远端。

## 本轮实现

- `Enabled=auto, Stage=BeforeSR` 正确启用，显式 `false` 优先。外部原神桥继续直连普通 NGX；没有恢复内部原神 FSR 捕获链。
- BeforeSR 接入同源帧 / 代次的颜色与深度，保留真实 producer / reader queue-fence 交接。私有深度 copy / SRV 按尺寸和格式复用，源 A/B 轮换不重建绑定。自然 Present 在登记的源链和物理输出链持续去重，覆盖 worker，不永久修改用户 effects。
- 修复私有 runtime 首次 preset reload 前写深度宏导致 `MXAO_USE_LAUNCHPAD_NORMALS=1` 丢失的问题；等待真实 reload、核对有效定义，避免反射 graph 计数触发重复编译。LAUNCHPAD → MXAO 首批在私有 scratch 预热 IPC，保留实际 completion fence，随后才允许写回 SR 输入。缺深度 / 错帧不写回。
- 内置 GIMI 独立开发提交 `a76896977d3e28a4e58bd71f45dcd261d5bebc8b` 已集成为 `06b0724`。主树补齐 native device / context / swapchain 源代次、晚 HWND 登记、选源与确认共用 owner 锁、可撤销 token，以及嵌套 proxy → native 的 shader / factory 观察。辅助链只退休，不能冒充游戏源；interop global PRE / POST 仍未支持。
- Loader 保留 Win32 / native 原参数、实际初始化状态、last-error / NTSTATUS；仅成功 Hook 才记 applied，目标模块 pin，失败由单个延时 worker 重试。修复 FSR3 重复安装和 OTA 角色漏观察；新增 queue-create / interop / hidden DX11 创建边界日志，为 Rocket 新局定位提供证据。
- 保留 SR / NR / FG、普通 FSR、DX11 / DX12 / DX11-w-DX12 / Vulkan / Vulkan-w-DX12，以及真实 queue / fence、Reset、资源退休和 quarantine。崩铁 SR 对象缓存仍由用户自备 `NgxFeatureCache.asi` 负责。

LAUNCHPAD 为本机 MXAO 提供基础 / 平滑法线，需要当前深度。它也能按请求估计图像光流；F6 当前没有读取其 flow，也没有向 ReShade 发布游戏 motion semantic。API20 可反射该纹理，但图像光流不能直接当成 SR / FG 的游戏几何运动矢量。完整范围与路径限制见 [兼容性评估](F6-RESHADE-COMPATIBILITY.md)。

## 最终构建与部署

本轮证据：`build/genshin-reshade-20261114/`。

| 项目 | 已确认结果 |
|---|---|
| Release x64 | `f6-build-release-final.log`，MSBuild exit 0，3 分 35 秒；已有 C4744 / LNK4098 / u8path 警告仍在 |
| 产物 | `build/out/OptiScaler.dll`，32,540,672 B |
| SHA256 | `082361809BA0256E1AD898CFE280AC21610F9601E59AB680B11191E8380A5F49` |
| mtime UTC | `2026-11-13T23:57:44.4550690Z` |
| `local/F6-StarRail-GapPacing` | `DEPLOYED_IN_PLACE`，FileId `0x000000000000000000070000001df2d3`，2 硬链接保留 |
| `local/F6-ASI` | `DEPLOYED_IN_PLACE`，FileId `0x000000000000000000020000001b42ff`，6 硬链接保留 |

两个既有目标已原位覆盖，实际安装哈希均与候选一致；本轮没有产生 `.next` 待替换。部署只判断目标实际文件可写状态，没有查询游戏进程。前后 FileId、完整硬链接集合、核对的用户 INI 和 ReShade 核心哈希均未变。回执：`f6-build-identity-final.json`、`f6-installed-before-final.json`、`f6-deployment-final.json`、`f6-installed-after-final.json`。

这是磁盘部署证据。需用户手动重开游戏后，用新 PID / 时间 / 加载身份和本局日志验证实际运行。

## 最终验证

| 范围 | 日志与结果 |
|---|---|
| 完整 CommonCore | `common-final3.log`，exit 0；CPU / WARP 主套件及最后的 FG / Reflex / interop / 设置共享回归通过 |
| 真实固定 ReShade 核心颜色 BeforeSR | `real-core-color-final.log`，270,405 检查；含自然链与 worker 去重 / 恢复 |
| 同帧深度与 reader 退休 | `real-core-depth-final.log`，221,337 检查；双源轮换、方向、错帧拒绝与 reader completion |
| 原样 LAUNCHPAD + MXAO | `real-core-launchpad7.log`，75,588 检查；首批不写回、MXAO 改色、真实法线随深度变化、缺深度 / 错帧拒绝 |
| GIMI 公共入口 | `gimi-present-final2.log`，真实双 WARP 设备 / 三条 chain、晚 HWND、源确认锁交错和嵌套 VS / PS；`gimi-routing-final2.log`，93 内容检查、100,000 次并发发布 |
| 真实 Windows loader | `loader-final5.log`，20 个参数 / 嵌套场景、生产 observer、自动失败重试与 OTA |
| MethodHooks | `method-hooks-final.log`，实际 original、native / proxy、嵌套 factory 返回观察、rollback / reinstall；本专项显式跳过 shared，完整 shared 由 CommonCore 覆盖 |
| NGX 生命周期 | `ngx-final.log`，15 router suites、7 release suites，通过 |
| UI / Menu input | `ui-final.log`：6,019 检查；`menu-final2.log`：51,959 检查、8 个执行负控被检出 |
| 工程闭包 | 784 个编译 / 头文件条目，无缺失或重复；最终 Release 编译覆盖全部保留后端，未执行 Vulkan GPU 验收 |

完整 CommonCore 开始后补充的 GIMI token 与嵌套 factory 修改，另由表中最终 GIMI / MethodHooks 专项覆盖；没有将开始时间不同的套件伪称为同一瞬时源码的全量重跑。日志中预期失败的 mutation / negative control 用于检出不安全改动，不是未处理的测试失败。早期 `menu-final.log` 的 vcvars 超时已由 `menu-final2.log` 实际重跑通过覆盖。

以上固定核心、Windows / WARP、CPU fixture 与源码检查各自说明覆盖范围，不能证明实际 NGX SDK / 游戏显示、Rocket 稳定性或 FPS / 延迟改善。

## FSR Bridge PR15 与上游

最终 GitHub API 复核时间为 `2026-11-14T00:09:01.7895314Z`，回执 `upstream-final-recheck.json`：main `c37d3a54ee736595bd9d461e11ab475dd137a00e`；PR15 head `97d2b06af415a6e8b4d4660b3dd6b5c1ae23db95`，仍 open / 未合并。PR head 缺 main 分叉后 10 个提交。

没有整包合入 PR15：其清除低分辨率 MV 标志但继续提交低分辨率纹理，违反 F6 FG 输入契约，新增字段默认值 / 测试也未闭合；预测合并候选的真实 NgxParamMap 测试为 3,657 检查、18 失败。安装器改配置、复制 ReShade、自签和信任根行为不在集成范围。

实际采用 main 加独立 `Ffx12SingleInstance` 接管配置控制，隔离源码保存点 `3ec47e3de096fd7f64872f819d108a0535938884`，归档 `bridge-source-3ec47e3.zip`。Release x64、NGX ON、release runtime ON、diagnostics OFF；4/4 CTest 及 NgxParamMap 的全部 3,657 检查通过。

早期部署的 `B5966CFF…` 桥已被下述提供者转发修正版覆盖；`bridge-deployment.json`、`bridge-installed-after.json` 只保留该次历史部署。

### 续接：提供者与 COM 转发修复

用户补充“只注入 FSR 也会崩溃”。实际旧桥 DLL 的独立宿主复现了两个同名 `d3d11.dll` 共用 original：第二提供者的 GPA / IAT 调用进入第一提供者。成功返回设备后，COM 方法也存在同类错配；同址 Factory2 / Factory 的重复安装还可能保存自身 hook。

运行时修复保存点 `f53873a7943b502e7739f5361c8b8f65c714b602` 按实际 callable 统一创建入口与 COM original，逐线程恢复嵌套调用；用标准 QI 定位实际 context，保留代理的绘制前后处理，内部 bypass 不再全局切换 vptr。接口表按实际 SDK 能力保留完整长度，并纠正 SwapChain4 的 HDR 方法槽 40，保留槽 39 的 `ResizeBuffers1`。最终完整源码为 `586f7f0fd77be27f9ed9884537fff3180b88bbb9`，只追加本局 `ResizeTarget` 回归与 fixture 的 UTF-8 构建设置，生产源码与前一提交一致。未改 Rocket / ReShade 核心或用户配置。

最终 `bridge-routing-final-verified.log` 为 Release x64 / NGX ON / release runtime ON / diagnostics OFF 的构建与 15/15 CTest 通过证据；包含真实 DLL 多提供者、嵌套成功路径、页边界接口表、SwapChain3 / 4、不同布局交换链的 `ResizeTarget` 正反创建顺序和 WARP 字节读回。旧桥负控 `bridge-route-negative-old.log` 检出 5 项错误；SwapChain4 修正前的 `bridge-routing-hdr-negative.log` 检出两个提供者的槽位错配；`bridge-resize-routing-negative-old.log` 与 `bridge-resize-routing-reversed-negative-old.log` 各检出 2 次错误转发。`DllMain` 归属 `Dx11FsrBridge.cpp.obj`，产物含新增提供者绑定日志。首次新增 ResizeTarget fixture 因 provider 未开 `/utf-8` 失败的日志保留，使用既有统一测试配置后实跑通过，不归因于生产路由。

2026-11-14 10:18（UTC+08）已原位覆盖既有 `temp3/Dx11FsrBridge (2).dll`：SHA256 `94D3B633195B55EE8F398FDD837CAB4A732FD718962FD2BEA81FD7FC56C275C0`，818,176 B。原 FileId `0x000000000000000000040000001d8630` 与两条硬链接保留，两个别名哈希一致；桥 INI、ReShade 核心和 F6 DLL 哈希未变。最终回执 `bridge-routing-final-build-identity.json`、`bridge-routing-final-deployment.json`、`bridge-routing-final-installed-before.json`、`bridge-routing-final-installed-after.json`；未生成待替换 `.next`。较早 `bridge-routing-*` 的 A49014B2 是本轮中间部署记录。

完整源码归档 `bridge-source-586f7f0.zip` 留在本轮证据目录；可重建差异随 F6 保存在 [外部桥补丁](../patches/genshin-fsr-bridge/README.md)。独立复核记录了 COM 同址重建与克隆表退休的继承缺口，已列入桥的 `docs/OPEN-ISSUES.md`，不据此解释首次启动退出。

08:54 局的真实 dump 已把该次访问冲突绑定到旧桥原方法 / receiver 错配，具体见下节；更早的 `0xC0000135` 仍无同等调用栈证据，不能归为相同根因。新桥加载及启动稳定性仍需用户手动新局。

## 已锁定游戏证据与待验收

下述会话时间为记录中的 UTC+08，不再追加八小时；均为本轮部署之前的旧候选，不能代替新版本验收。完整统计和 SHA 清单见兼容性文档及 `locked-files.json`。

| 会话 | 已确认事实 |
|---|---|
| 普通局 `20261114-024410_YuanShen_57172` | 1,446 个 Evaluate 入口；816 个前台且 Opti 菜单关闭的源间隔，P50 / P95 47.408 / 58.934 ms，中位源间隔倒数约 21.1 FPS |
| ReShade 局 `20261114-024706_YuanShen_230352` | 1,571 个 Evaluate 入口；341 个对应源间隔，P50 / P95 59.198 / 72.326 ms，约 16.9 源 FPS；旧 F6 `auto + BeforeSR` 实际关闭 |
| Rocket 局 `20261114-042258_YuanShen_228728` | 约 38.3 秒退出 `0xC0000135`，SR / NR / FG 入口全 0；XXMI 的主动 `DoubleBeepExit()` 也使用此码，未定位实际退出分支，无对应 dump / WER |
| Rocket 局 `20261114-082043_YuanShen_227916` | 确认加载 `44380f8`，约 31.1 秒后 `0xC0000135` 退出；隐藏 DX11 swapchain 创建尚未返回，未进入 SR / NR / FG |
| 仅桥对照 `20261114-084512_YuanShen_238544` | 约 8.6 秒后 `0xC0000135` 退出，桥日志停在创建设备接管后；该局不能用旧 F6 日志证明加载了 F6 |
| 新局 `20261114-085448_YuanShen.exe_240612` | 加载 `44380f8` 和旧 B596 桥；隐藏 DX11 链和 DX11-w-DX12 wrapper 创建成功后，旧桥把 F6 wrapper 交给 ReShade 的 `ResizeTarget` 原方法，在 `ReShade64.dll+1804B7` 发生 `0xC0000005`；UEF dump 146,284,432 B，已逐文件复制并 SHA 锁定 |

本局 dump 的真实展开栈为 `ReShade64+1804B7 ← Dx11FsrBridge (2)+16B7C ← YuanShen+AAB844`，活动栈没有 F6 帧，不能把栈扫描的残留地址冒充活动调用。旧桥基址 `7FF871EC0000`，timestamp `6AF7870F` / image size `AA000` 与保留的 `bridge-only-lock-20261114/bridge-baseline-B596.dll` 匹配。主会话重读 dump，核验了全局原方法位置 `7FF871F5EA10` 指向 `ReShade+180480`，receiver 为 F6 wrapper `2A033225A50`；ReShade 把其 `+8` 的 Operations `7FFFF0039FB8` 解释成自己的原对象，再把 F6 函数地址当虚表，在 `call [rax+70h]` 访问冲突。寄存器、模块、指令和匹配 PE 身份见 `dump-resize-routing-analysis.json`；完整加载内存 SHA 未核，wrapper 虚表槽页未保存在 dump 中。本次无需修改 F6 wrapper 布局。新增异构布局专项通过、旧桥负控失败，说明修正版覆盖了这个调用错配；仍不替代新桥游戏验收。

没有这些会话的 Display / PresentMon 数据，不能报 FG 后显示 FPS。两局场景和设置不同，不能把差值直接归因 ReShade。新源码修复和测试不等于 Rocket 崩溃已实机修好。

下一轮由用户手动验证 Rocket / GIMI 进入场景、BeforeSR 的 LAUNCHPAD + MXAO 画面、SR / NR / FG 与实际性能；AI 读取同局新证据。[AGENTS.md](../AGENTS.md) 和 [README.md](../README.md) 的 9 项已知问题、7 项 TODO 继续保留，不据本轮源码或离线测试划掉。

## 历史保存点

- 阶段 0：`5d0f0aebd5cfe690703d89a5012c6c52a5226079`。
- 阶段 1：`2052553`，整体移除内部原神捕获链，将 Hook / 身份 / 计时移至公共模块，拆分 DLSSG Inputs / Lifetime 和 NR GPU execution lifetime。实际 queue / fence 退休保护保留，Vulkan 两条后端完整保留。
- 阶段 1 DLL `E454F55C1A8E185F0B6703BBA6AF1E10F6AED310B2D4931DF91BEECD8E051418` 已被上述 `08236180…` 候选原位覆盖。历史构建 / 回归 / 部署证据保留在 `build/fg-runtime-followup-20261113/phase1-*.json` 与日志中，不代表当前安装内容。
