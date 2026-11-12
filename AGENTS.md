# F6 项目协作规则

本仓库用于 OptiScaler F6（Windows x64 / C++ / MSVC）。实现、分析和文档工作均以 F6 为范围；旧根或其他分支资料只在明确需要时作为历史参考，不把其中的路径、状态或测试结论当作当前事实。

## F6 路径

- 接手先读 `README.md`、`docs/CURRENT-STATE.md`、`docs/F6-HANDOFF.md`；来源统一见 `D:/CODE/DLSS5_NEXT/docs/REFERENCE-REPOSITORIES.md`，历史进度与验收边界见 `docs/F6-STATUS.md`。
- SR/NR/FG 公共链路：`OptiScaler/hooks/`、`OptiScaler/inputs/`、`OptiScaler/upscalers/`、`OptiScaler/native/`、`OptiScaler/with_dx12/`、`OptiScaler/framegen/`、`OptiScaler/dlssnr/`、`OptiScaler/shaders/dlssnr/`。
- 原神 FSR/NR/FG：`OptiScaler/genshin/`、`OptiScaler/inputs/GenshinFsrDx11.cpp`、`OptiScaler/inputs/GenshinFsrContract.h`、`OptiScaler/inputs/FG/`；相关离线测试在 `OptiScaler/genshin/tests/`。
- 构建入口：`build.ps1`；产物和日志：`build/out/`、`build/msbuild.log`；F6 运行证据工具：`tools/runtime/Get-F6RunEvidence.ps1`。

## 外部参考

- 原神 FSR Bridge：`https://github.com/AizawaHikaru233/genshin_fsr_brigde`。参考其 FSR pass 识别与桥接流程；移植前核对当前 F6 的 hook、资源身份和状态约束。
- GIMI ReShade DLSS integration：`https://github.com/CXP-2024/GIMI_reshade_dlss_integration`。参考其与 GIMI/ReShade 的兼容实现；不要据此假定 F6 的 SR/NR/FG 资源生命周期相同。

## 子智能体协作

- 用户最新要求问题检索和独立复核使用 `gpt-6.1-sol` / `high`，不再使用 DS；核心由主会话核实并统一集成。

- 子智能体必须由当前主会话创建和管理；不得到之前的会话创建、调用、恢复或派发子智能体任务。
- 创建已超过 30 分钟的子智能体不得再调用或复用；需要继续工作时，在当前会话新建实例并传递必要交接。
- 默认每次任务都新建子智能体；仅当实例属于当前会话、创建未超过 30 分钟，且已有上下文对当前任务直接有用时才复用。

## 实施与验证

- 先沿 F6 的实际调用链确认所有权、资源身份、队列和退休条件，再调整抽象；优先使用已有接口和测试，不为单个游戏复制整套项目线路。
- 资源只有在实际所属 queue/fence 完成后才能退休；CPU 返回、Ready、窗口 ACK 或其他对象的 fence 不能替代。
- 保留 SR、NR、FG 各自语义及用户配置。不得通过删拒绝条件、复用旧帧或扩大资源池掩盖断链。
- 构建、部署、实际加载、画面正确性和性能是不同验收项。特定历史会话用户确认仅加载 OptiScaler；当前加载内容须绑定当局，旧 PID、旧日志或旧部署快照不能证明本次加载或性能。
- 不操控提权游戏窗口。游戏启动、进入场景和视觉验收由用户手动完成。部署不查询或判断游戏是否运行，只按目标 DLL 的实际文件占用处理：可写则原位部署并保留硬链接；被占用则写同目录 `OptiScaler.dll.next`，记录为待替换，不冒充已部署。不因游戏进程存在阻止部署，不终止游戏进程。
