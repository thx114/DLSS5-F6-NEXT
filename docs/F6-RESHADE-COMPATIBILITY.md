# F6 与当前 ReShade 的兼容性评估

检查日期：2026-11-14（UTC+08）。范围为当前 `codex/f6-rewrite` 阶段 1 源码、现有安装文件及已有日志。没有启动、停止或操作游戏，没有修改 ReShade、启动器或用户配置。源码能力、磁盘版本与实机验收分别记录。

## 结论

本机指定的 HoYoShade ReShade 6.8.0 核心满足 F6 的固定接入身份条件；实际连接和效果执行仍待新局验证。阶段调度有明确边界，不能据此宣称与所有 ReShade 6.8.0 构建、插件和后端完全兼容。原神默认 Present NR 路径缺少 BeforeNR 调用；普通 DX12 NGX 的 BeforeSR 缺少独立提交入口；Vulkan 没有 F6 的 HoYoShade 阶段调度闭包。普通 ReShade 自然效果能否与当前 SR/NR/FG 稳定共存，仍需用户手动验证。

前两项在阶段 0 基线中已存在，不能归因于本次移除内部 FSR 捕获。当前阶段 1 没有新增对这些入口的支持。

## 版本与身份

| 对象 | 本次证据 |
|---|---|
| 官方下载版本 | ReShade 官方首页当前列出 6.8.0，发布日期 2026-08-02 |
| 官方 SDK | `crosire/reshade` 的 `main/include/reshade.hpp` 当前定义 `RESHADE_API_VERSION 20` |
| F6 内置 SDK／握手 | `integration/hoyoshade/sdk/reshade/reshade.hpp` 与 `integration/HoYoShadeApi.h` 均使用 API 20 |
| 本机核心 | `D:/APPS/test/TEST/HoYoShadeHub_Portable_1.4.5.0_x64_Full/HoYoShade/ReShade64.dll`，5,592,064 字节，ProductVersion `6.8.0`，FileVersion `6.8.0.2155` |
| 本机核心 SHA256 | `0CEE63F9C9F13F3AC909C5B4903F4DBB4B719A7AB3B4F13B0DEAF83C814B94F7`，与 F6 固定允许值一致 |

F6 的 `HoYoShade::Connect` 还要求进程中只有一个导出 `ReShadeRegisterAddon` 的核心，路径以 `HoYoShade/ReShade64.dll` 结尾。仅版本或 API 相同不构成接受条件；改名、升级、重编译及替换为普通发行版都可能被拒绝。磁盘 SHA 不是本局加载内存的 SHA，也不证明这个定制核心与官方原版字节相同。

官方来源：

- ReShade 首页与下载说明：`https://reshade.me/`
- API 定义：`https://raw.githubusercontent.com/crosire/reshade/main/include/reshade.hpp`

两项已于本次收尾时重新在线核验，原文保存为 `build/fg-runtime-followup-20261113/phase1-reshade-home-current.html` 和 `phase1-reshade-sdk-current.hpp`；本机核心字节身份也已重新计算。

官方支持 DX11、DX12 和 Vulkan，是 ReShade 自身的后端能力；F6 的阶段执行支持见下表。官方普通版与 full add-on support 版也有区别，不能只按版本号判断 addon 可用性。

## 按实际执行路径划分

| 路径 | BeforeSR | BeforeNR | FinalOutput |
|---|---|---|---|
| DX11-w-DX12 的外部 NGX 桥 | 有入口，真实 producer handoff 后、所选 SR 之前 | 普通 FinishedPicture 有入口；原神默认 Present NR 路径缺少入口 | DX12 原图或 FG 物理端点有条件入口 |
| 普通原生 DX12 NGX | 没有独立 recording slot 时明确拒绝阶段执行，继续 SR | 普通 FinishedPicture 有入口 | 无 FG 时在 FinishedPicture NR 后、Opti overlay 前；FG 走物理端点 |
| NativeProducer／虚拟超分 | 有受实际提交权限约束的独立入口 | 取决于实际 native NR 路由 | 取决于当前输出端点 |
| Vulkan 原生、Vulkan-w-DX12 | F6 阶段调度不支持 | F6 阶段调度不支持 | F6 阶段调度不支持；普通 ReShade Vulkan 效果单独验收 |

调用依据：

- `upscalers/IFeature_Dx11wDx12.cpp::Evaluate`：已提交共享输入 → BeforeSR → provider Evaluate。
- `upscalers/IFeature_Dx12.cpp`：普通 DX12 Evaluate 的 BeforeSR unsupported 分支。
- `native/NativeDriver.cpp`：NativeProducer 的 BeforeSR endpoint。
- `shaders/dlssnr/DlssNr_Late.inl`：普通 FinishedPicture 的 `nrFilter.BeforeNr`。
- `shaders/dlssnr/DlssNr_GenshinPresentation.inl`：当前专用路径直接 `g_compose->Dispatch`，没有 `RenderStage`／`nrFilter.BeforeNr`。
- `wrapped/wrapped_swapchain.cpp`：无 FG FinalOutput。
- `framegen/FinalOutputEndpoint.cpp`：FG 物理端点的 snapshot、滤镜、原 Present、restore、tail fence。
- `menu/HoYoShadeControls.h`：Vulkan 路由禁用阶段位置控制。

## 效果与生命周期边界

1. **颜色格式**：阶段调度支持 RGBA8/BGRA8 UNORM 及其 sRGB 变体，并要求 SDR。FP16、10-bit、typeless、HDR 不在当前执行支持范围。普通 ReShade 自然效果的能力另算。
2. **深度效果**：现有 BeforeSR 调用点没有传入 `endpoint.depth`，owned runtime 清除缺失的 `DEPTH` 绑定；颜色效果成功不能证明深度效果成功。普通 BeforeNR 可传入真实深度租约，仍需核对当帧资源和约定。
3. **FG 输出**：物理端点要求已观察 provider 创建域、同线程输入 scope、SDR、flip-discard。TEST、DO_NOT_WAIT、dirty rect/scroll Present 会跳过。`UnknownPhysical` 不区分原图和生成帧，不能从提交次数推导每张生成帧均已处理。
4. **自然效果去重**：仅在 owned 技术实际录制、提交成功后，才用空 RTV `render_effects` 消耗相应 natural epoch。不是永久关闭自然效果；必须用实测确认无漏执行或双重应用。
5. **GPU 退休**：保留颜色、工作纹理、深度 reader 与实际 queue/fence。执行 `flush_immediate_command_list` 后由同队列 Signal，退休读取真实 `GetCompletedValue`。CPU 返回或开关切换不替代完成；异常保留资源。
6. **连接时机**：缺核心时最多重试 30 次、间隔一秒；`connectStarted` 单向置位。本进程首轮失败后，切换开关不能重新连接。延迟注入超过约 29 秒也可能错过连接窗口。

## Hook 共存与阶段 1 变化

`LoadReShade` 从 EXE 目录加载 `ReShade64.dll`；HoYoShade 集成则观察已加载的指定核心。Luma 延迟加载及 SpecialK 的 ReShade graphics hook 环境处理仍在。DXGI factory 先调用原函数再包装；D3D11 factory 仍经原 trampoline 创建设备和 swapchain。

`Util::CheckForRealObject` 只识别现有 Streamline GUID，不能据此宣称会剥离任意 ReShade proxy。F6、ReShade、GIMI 哪层处于实际 Hook 最外侧，取决于当局加载和安装顺序。

阶段 1 移除了内部原神 typed-color 与私有父子链 copied-source 票据。现在物理输出使用普通实际 runtime／backbuffer 关联，不恢复旧捕获授权。此能力收缩符合删除旧捕获链的范围；尚无新局证据证明它导致外部桥崩溃、黑屏或重复滤镜。重构审查发现的 Hook trampoline 发布、公共 shader 身份和 GIMI 后启用补装回归另行修复，不应与 ReShade 已有阶段限制混为一谈。

## 已有运行证据及待验收项

证据目录中的 `OptiScaler-44888.log` 记录 HoYoShade `latchedEnabled=false`，不能作为阶段执行通过证明。磁盘可见三游戏 Addons 中有 RenoDX DLSS5 13.3.0、MFGUnlock 1.9.0.0；本次没有确认它们当前实际加载或启用。

| 用户手动场景 | 检查重点 |
|---|---|
| 原神外部桥、FG 关 | 三个阶段分别选择；BeforeNR 当前记为缺入口，观察其余阶段是否实际提交 |
| 原神外部桥、FG 开 | 原图／生成画面是否漏滤镜或重复，HUD 与菜单是否正确 |
| 崩铁、绝区零的现有接入 | FG 关逐阶段；FG 开进出菜单、切场景，分别记录画面、稳定性、帧间隔 |
| 原生 DX12 | 普通 NGX BeforeSR 明确跳过；BeforeNR、FinalOutput 与虚拟超分入口分别记录 |
| Vulkan 两种路径 | 普通 ReShade 自然效果与 SR/NR/FG 共存；F6 阶段调度记为不支持 |
| 已支持入口 | 一个明显颜色效果、一个深度效果；开关、preset reload、resize 各一次 |

每次绑定新 PID、加载 DLL／插件、当前设置、对应日志及用户画面反馈。构建、离线测试、磁盘身份不代替这些实机项目。
