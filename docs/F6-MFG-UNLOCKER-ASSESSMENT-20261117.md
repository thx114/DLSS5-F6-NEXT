# MFG-Unlocker 对 F6 的适用性评估

2026-11-17。固定参考 [dashdogy/MFG-Unlocker](https://github.com/dashdogy/MFG-Unlocker/tree/c8d9bd1a5b1376e1f327aa2fcc389db5aa8fdc2d)，提交 `c8d9bd1a5b1376e1f327aa2fcc389db5aa8fdc2d`，README／OptiScaler 文档标注 V1.7.1。本地只读参考位于 `build/reference/mfg-unlocker-20261117`，没有将其源码或二进制加入 F6 产品。固定身份不代表以后仍为上游最新版本。

## 结论

不能靠直接引入该库修复“原／铁／绝选择 FSR／XeSS 后失效或退回 DLSS”。本次已定位并修复 F6 自身的 SR 输出契约和 DX11 选择生命周期，见 [修复报告](F6-SR-BACKEND-DELIVERY-20261117.md)。该项目的相关 OptiScaler 集成主要提供 FG／MFG；其 DLSS 预设功能也不是 F6 的 FSR／XeSS 选择、创建和输出链。

用户自备 ASI 的既有产品边界继续保留，F6 不维护或分发 40 系 MFG ASI。若以后引入它的完整菜单／provider 能力，应独立实现并验证插件宿主接口，不能仅凭 ASI 已加载便宣布兼容。

## 接口差异

上游 [OptiScaler 文档](https://github.com/dashdogy/MFG-Unlocker/blob/c8d9bd1a5b1376e1f327aa2fcc389db5aa8fdc2d/docs/optiscaler.md) 要求当前 OptiScaler 10，明确不支持 0.9.x，并区分上游和其 fork 的能力。V1.7.1 的部分 XeMFG、Vulkan、菜单和 DLSSG 替代能力要求其配套构建。

`source/native/optiscaler_plugin.h` 定义带 size／version 的 C ABI，按字段大小探测能力；`single_module.cpp` 的 OptiPluginInit 在宿主接受后用 RegisterProvider 注册 OptiProvider_Dlssg。F6 当前 `runtime/AsiPluginStartup.cpp` 调用 InitializeASI 和可选 PatchResult，未实现这套 OptiPluginInit／RegisterProvider 宿主契约。因此：

- 通用加载／ASI 自己安装的 hook，与插件菜单、provider 注册是不同验收项。
- 普通 DLSS MFG 的自带 hook 能否工作应按插件、显卡和实际运行库验证；本轮没有运行或验收该 ASI。
- 不将 fork 的 README 支持列表写成 F6 已支持 XeMFG／Vulkan FG／共享菜单。

## 可参考的设计与采用边界

| 设计 | F6 后续可采用方向 |
|---|---|
| 追加字段的 size／version C ABI | 统一可选插件能力探测，避免宿主和插件直接依赖内部 C++／ImGui 布局 |
| 待请求路由与已确认运行路由分离 | UI 明确显示请求、实际 provider 和失败原因；本轮独立修复 DX11 已提交偏好，没有复制其路由代码 |
| 运行库配对与失败诊断 | 将模块存在、初始化、功能支持、真实输出分别记录，保留 backend 和文件身份 |

该项目旧 SR handle 的“四次 Evaluate 且 250ms”延迟释放不是实际 GPU 完成证据，不替换 F6 的所属 queue／fence／recording 退休条件。没有整枝照搬、加入模型或 vendor 程序数据，也没有修改 F6 GPU 生命周期以迁就插件。

原作者代码的 [LICENSE](https://github.com/dashdogy/MFG-Unlocker/blob/c8d9bd1a5b1376e1f327aa2fcc389db5aa8fdc2d/LICENSE) 为 MIT；[NOTICE.md](https://github.com/dashdogy/MFG-Unlocker/blob/c8d9bd1a5b1376e1f327aa2fcc389db5aa8fdc2d/NOTICE.md) 明确 Intel 派生 DXIL／SPIR-V 程序数据不在 MIT 范围。本轮仅评估公开设计，没有复制这些数据。以后若选取源码，应按具体文件保留原署名和适用许可证，不能将整个发布二进制视为单一 MIT 代码。
