# F6 项目协作规则

本仓库用于 OptiScaler F6（Windows x64 / C++ / MSVC）。实现、分析和文档工作均以 F6 为范围；旧根或其他分支资料只在明确需要时作为历史参考，不把其中的路径、状态或测试结论当作当前事实。

## 架构与工程原则（用户最新授权，2026-11-13）

- **高可维护性优先**：评估与实现需求时优先消除历史债、收敛状态机并保持清晰职责边界，不做继续堆叠分支复杂度或掩盖问题的最小胶水修复。
- **授权精简与重写**：允许直接删除已废弃或无实际价值的历史功能、重写高耦合巨型模块、合并精简跨层调用链路，并允许联网检索相似公开仓库吸纳成熟设计。
- **后端保留边界**：已废弃的内部原神 FSR 捕获链应整体移除（原神统一走外部 `genshin_fsr_brigde` 直连 NGX/DLSS 输入）；**Vulkan 后端（含 Vulkan 原生及 Vulkan-w-Dx12）保持完整可用**，修改公共接口时须同步兼顾 DX11 / DX12 / DX11-w-Dx12 / Vulkan 编译与语义闭合。
- **高效批量执行**：合并相关检索、分析与验证操作，减少无意义的多次零散调用与重复循环。

## 部署约定（用户最新要求，2026-11-13）

- 部署只在既有目标位置原位覆盖 DLL（如 `tools/runtime/Deploy-F6.ps1`），保留硬链接；不为每次构建创建新的 Opti 包、版本或 runtime。
- OptiScaler 构建选择与所有启动器选项由用户设置；未经用户明确要求，不调用构建选择或配置修改 CLI，不修改相关数据库、配置或 profiles。
- 游戏测试由用户手动完成，包括启动、进入场景、操作、调整选项和退出；AI 负责构建、原位覆盖 DLL、读取日志和分析采样，不控制桌面或通过 CLI 启停游戏。确需读取管理员进程且普通权限不足时，仅使用经过系统授权的提权只读工具。
- 部署不查询或判断游戏是否运行，只按目标 DLL 的实际文件占用处理：可写则原位部署并保留硬链接；被占用则写同目录 `OptiScaler.dll.next`，记录为待替换，不冒充已部署。不因游戏进程存在阻止部署，不为部署强制终止游戏进程。需要重启加载时由用户手动退出及重开，再执行同一原位部署入口。
- “可回退”指代码层面的可回退，保留源码提交/保存点；不据此创建部署版本、切换包或恢复启动器选项。

## F6 路径

- 接手先读 `README.md`、`docs/CURRENT-STATE.md`、`docs/F6-HANDOFF.md`；来源统一见 `D:/CODE/DLSS5_NEXT/docs/REFERENCE-REPOSITORIES.md`，历史进度与验收边界见 `docs/F6-STATUS.md`。
- SR/NR/FG 公共链路：`OptiScaler/hooks/`、`OptiScaler/inputs/`、`OptiScaler/upscalers/`、`OptiScaler/native/`、`OptiScaler/with_dx12/`、`OptiScaler/framegen/`、`OptiScaler/dlssnr/`、`OptiScaler/shaders/dlssnr/`。
- 构建入口：`build.ps1`；产物和日志：`build/out/`、`build/msbuild.log`；原位部署：`tools/runtime/Deploy-F6.ps1`；运行证据工具：`tools/runtime/Get-F6RunEvidence.ps1`；公共回归：`tools/tests/Run-CommonCoreTests.ps1`。

## 外部参考

- 原神 FSR Bridge：`https://github.com/AizawaHikaru233/genshin_fsr_brigde`。原神通过该外部桥接入 NGX/DLSS 输入。
- GIMI ReShade DLSS integration：`https://github.com/CXP-2024/GIMI_reshade_dlss_integration`。参考其与 GIMI/ReShade 的兼容实现；不要据此假定 F6 的 SR/NR/FG 资源生命周期相同。
- 允许联网检索并参考 OptiScaler 官方上游及同类公开仓库（NR 历史、XeFG、低延迟/计时、多后端 NR 等），记录参考来源与边界，不盲目整枝照搬。

## 子智能体协作

- 允许频繁调用只读 `gpt-6.1-sol`（`high`）子智能体并行执行代码审计、依赖闭包梳理、日志分析、外部仓库检索与独立复核；不再使用 DS。核心设计与代码修改由主会话核实并统一集成。
- 子智能体必须由当前主会话创建和管理；不得到之前的会话创建、调用、恢复或派发子智能体任务；任务完成后及时关闭回收。
- 创建已超过 30 分钟的子智能体不得再调用或复用；默认每次独立子任务新建实例，仅当当前会话内未满 30 分钟且上下文直接有用时才复用。

## 产品边界

- 40系6倍帧生成由asi提供，本项目无相关代码，也不分发asi。
- 崩铁 SR 对象缓存交给用户自备的 `NgxFeatureCache.asi`，产品不再维护 `PersistentSrDx11` / `PersistentSrPool`。不要重新叠加内置对象池；普通 NGX 生命周期、NR 历史与实际 GPU 退休同步仍须保留。2026-11-13 回退入口见 `build/starrail-asi-20261113/README.md`。

## 当前已知问题与开发计划（2026-11-13）

以下为用户确认的当前未解决事项，后续 AI 接手时须保留，不以已有代码、菜单选项、构建或离线测试通过代替实际验收。

### 已知问题

- 帧生成有残影。
- NR 细节保留有残影。
- HUDless 无法工作。
- 帧生成会导致 GPU 略微排队。
- NR 细节会在深度不同的物体上保留，对背景效果不佳。
- 超分耗时较高。
- NR 细节保留耗时较高，延迟较高。
- 帧计时没有计入 FG，也不显示帧数。
- Reflex 计时将模拟和排队错误地计入第一个计时项。

### TODO

- [ ] GIMI 接入与兼容完善。
- [ ] ReShade 在超分前执行。
- [ ] XeFG 接入与验证。
- [ ] AMD NR。
- [ ] Intel NR。
- [ ] 不同显卡设备的自动配置。
- [ ] UI 重构。

以上清单记录问题与目标，不预设根因或修复方案；修复结论需按游戏、设备与功能分别验收。README 面向使用者维护介绍、教程和上述清单，本机路径、部署身份及交接证据留在维护文档中。

## 实施与验证

- 先沿 F6 的实际调用链确认所有权、资源身份、队列和退休条件，再调整抽象；优先使用统一公共接口和测试，不为单个游戏复制整套项目线路。
- 资源只有在实际所属 queue/fence 完成后才能退休；CPU 返回、Ready、窗口 ACK 或其他对象的 fence 不能替代。
- 保留 SR、NR、FG 各自语义及用户配置。不得通过删拒绝条件、复用旧帧或扩大资源池掩盖断链。
- 构建、部署、实际加载、画面正确性和性能是不同验收项。当前加载内容须绑定当局 PID、加载身份与证据，旧 PID、旧日志或旧部署快照不能证明本次加载或性能；用户体感与显示采样分别记录。
