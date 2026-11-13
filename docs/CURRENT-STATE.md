# F6 当前状态

更新：2026-11-14（UTC+08）。阶段 1 重构已完成源码与离线验证，并已原位部署；本候选尚无新局画面、稳定性或性能验收。用户已撤销暂停和编写交接的要求，本轮没有更新 `F6-HANDOFF.md`。

当前产品树为 `D:/CODE/DLSS5_NEXT-workers/f6-rewrite`，分支 `codex/f6-rewrite`。阶段 0 保存点为 `5d0f0aebd5cfe690703d89a5012c6c52a5226079`；阶段 1 源码保存点见包含本文件的提交及本地 `phase1-result.json`，未推送。游戏由用户手动测试；AI 未启停或操作游戏，未修改启动器选择、配置、profiles 或 ASI。

## 阶段 1 实现

- 整体删除废弃的内部原神 FSR 捕获、私有 transport／receipt／父子输出票据及其专用菜单、配置与测试。原神继续经外部 `genshin_fsr_brigde` 接入普通 NGX；保留实际使用的 `DlssNr_GenshinPresentation.inl`、公共 native producer、普通 FSR 输入和 SR／NR／FG 功能。
- 将共享 Hook 所有权／事务、CPU 计时、游戏识别、COM 身份和深度 shader 能力迁到公共模块。Hook 计时移除旧捕获链维度，收敛为 5 个 Hook、3 条路径、18 个阶段，共 33 桶；保留真实调用、发布及退出时序。
- GIMI context hooks 由统一入口验证和补装，覆盖初始及晚启用；原神 interop Present NR 路径恢复恰好一次 `GenericDepthDx11::OnPresent`，继续关闭观察并准备 MOD 接入。仍需按游戏与加载顺序实测兼容性。
- DLSSG 拆成主流程、`DLSSG_Dx12_Inputs.cpp` 和 `DLSSG_Dx12_Lifetime.cpp`；多个编译单元共享同一个 teardown quarantine 状态。
- NR 新增 `NrGpuExecutionLifetime`，统一持有 recording registry 与待退休 feature／parameters／resource。保持原 mutex 锁域、实际每条 queue／fence、Reset 门和 feature 先于 parameters 的释放顺序；未知完成状态继续保留资源。
- 保留 DX11-w-DX12 共享输入的早归还、实际 reader retirement 与 unfenced quarantine。不恢复内置 PersistentSR；崩铁 SR 对象复用仍由用户自备 `NgxFeatureCache.asi` 负责。
- 工程清单核对：774 个编译／头文件条目无缺失、无重复；相对阶段 0 的 89 个 Vulkan 相关条目全部保留。Release 编译覆盖 Vulkan 原生及 Vulkan-w-DX12；没有本候选 Vulkan GPU 实测。

## 构建与部署

证据统一位于 `build/fg-runtime-followup-20261113/`。

| 项目 | 本次结果 |
|---|---|
| 最终构建 | `phase1-build11-console.log`，Release x64，exit 0，76 秒；已有编译／链接警告仍在 |
| 产物 | `build/out/OptiScaler.dll`，32,426,496 字节 |
| SHA256 | `E454F55C1A8E185F0B6703BBA6AF1E10F6AED310B2D4931DF91BEECD8E051418` |
| mtime UTC | `2026-11-13T18:26:25.8379165Z` |
| `local/F6-StarRail-GapPacing` | `DEPLOYED_IN_PLACE`，FileId `0x000000000000000000070000001df2d3`，原 2 个硬链接保留 |
| `local/F6-ASI` | `DEPLOYED_IN_PLACE`，FileId `0x000000000000000000020000001b42ff`，原 6 个硬链接保留 |

两目标及完整硬链接集合中的内容均已核对为上述 SHA256；本轮没有 `.next` 待替换候选。部署仅依据目标实际可写状态，没有查询游戏进程或修改构建选择。部署前身份为旧 `AB0B37E0…`，前后 FileId 与硬链接集合一致。回执为 `phase1-predeploy-current.json`、`phase1-deployment.json`、`phase1-installed-after.json`。

这些结果确认磁盘部署，不确认当前进程加载；需要用户手动重开游戏后，绑定新 PID、加载身份和本局日志再验收。

## 验证证据

| 范围 | 结果及日志 |
|---|---|
| 公共 CPU／WARP 套件 | 初轮各执行套件通过，`phase1-common-tests.log`；随后停于旧私有链静态断言，该断言已迁移并由最终共享回归覆盖 |
| 最终共享回归 | `Run-CommonCoreTests.ps1 -SuiteName NrGpuExecutionLifetime` exit 0，`phase1-confirmed-shared.log`；包含新 owner CPU 测试及共享 FG／调度／Reflex／互操作回归 |
| 实际 NR owner／recording 退休 | `phase1-module-lifetime-tests.log`：18 个隔离 DetailHistory WARP runtime 场景；NR 重配置 30 场景／191 检查；GPU 未完成、pending 和 Reset 失败负控均按预期拒绝 |
| FG／共享输入 | WARP handoff 30 场景／1186 检查、bridge retirement 15 场景／494 检查及对应负控通过；模块拆分后定向回归见 `phase1-final-regression.log`，该轮后续过期 scheduling 提取锚点已修复并在最终共享回归通过 |
| 计时与真实 Hook | `phase1-final-hook-tests.log`；SDK 计时 49 条连接与 57 条 CPU 检查、帧延迟适配 780 条与调度 1065 条检查在最终共享回归再次通过 |
| NGX 生命周期 | `phase1-final-ngx.log`：15 个 router suites、7 个 release suites 通过 |
| UI／输入 | `phase1-ui-localization.log`：6019 检查；`phase1-final-menu-input.log`：51959 检查；对应负控通过 |
| 工程闭包 | 公共 FSR／Vulkan、direct DLSS、设置读取与清退入口检查通过；最终生产输入不晚于 DLL，差异空白检查通过 |

WARP、CPU fixture、源码连接检查及构建分别说明其覆盖范围；它们没有执行实际游戏 SDK／显示链，也不能证明残影或排队已改善。独立只读审查提出的 Hook、GIMI 晚启用和 Present 漏入口问题均已修复；最终生命周期复核未发现 P0／P1 回退。

## ReShade 兼容性

完整评估见 [F6-RESHADE-COMPATIBILITY.md](F6-RESHADE-COMPATIBILITY.md)。本机指定 HoYoShade 核心满足 F6 的固定路径、哈希与 API 条件；这不代表任意同版本 ReShade 可以替换，也不证明当前游戏已连接并执行效果。

- 原神默认 Present NR 缺少 BeforeNR 入口。
- 普通 DX12 NGX 在无独立 recording 提交权时不支持 BeforeSR。
- Vulkan 两条路线没有 F6 的 HoYoShade 阶段调度闭包；普通 ReShade 自然效果共存另行验收。
- 当前阶段执行仅支持指定 SDR 8-bit 颜色，BeforeSR 没有深度输入；FG 物理端点存在格式、创建域、线程和 Present 参数条件。

本轮完成评估，没有把这些已有阶段限制标记为修复。后续按文档中的手动矩阵逐路径验证。

## 实机待验收与后续

本轮不据重构、DLL 减小或离线检查推导性能提升。继续优先复测关闭预览时的崩铁 FG 卡顿和 GPU 排队；原神与绝区零分开记录。阶段 0 及更早 PID 的性能与画面反馈属于历史证据，不能代表当前 `E454F55C…` 候选。

[AGENTS.md](../AGENTS.md) 与 [README.md](../README.md) 中的 9 项已知问题、7 项 TODO 均继续保留，包括 FG／NR 残影、HUDless、GPU 排队、深度分层细节保留、SR／NR 耗时和计时归属错误。GIMI、ReShade BeforeSR、XeFG、AMD／Intel NR、设备自动配置和 UI 重构仍须继续实施或验收。

后续读取本局新日志、加载身份与用户画面反馈，分别区分 SR 输入空档、复制槽实际退休等待、SDK Present 和 Reflex 排队。不得用旧帧、假 guides、删同步或扩池掩盖问题。
