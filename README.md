# DLSS5 F6 NEXT

基于 OptiScaler 开发的实验性图像增强项目，主要面向原神、崩坏：星穹铁道和绝区零，提供超分辨率（SR）、神经渲染（NR）、帧生成（FG）以及 NR 细节保留的配置与调试界面。

项目仍在开发中，画面质量、兼容性和延迟均有待改善。使用前请先阅读下方的[已知问题](#已知问题)，按游戏和显卡分别调整设置。

## 功能说明

| 功能 | 用途 |
|---|---|
| SR：超分辨率 | 将较低渲染分辨率的画面重建到输出分辨率 |
| NR：神经渲染 | 对画面进行神经渲染处理，提供模型、强度等调节项 |
| NR 细节保留 | 在帧间保留 NR 产生的细节，提供历史时长、强度和调试预览 |
| FG：帧生成 | 接入帧生成，并分别配置输入来源、输出方式和生成帧请求 |
| 调试界面 | 查看输入资源、NR 历史贡献、运行状态和计时信息；计时目前有已知缺陷 |

SR、NR 和 FG 分别配置。开启其中一项，不代表另外两项已经生效；请求的生成帧数量也不等于实际显示帧率。

**40系6倍帧生成由asi提供，本项目无相关代码，也不分发asi**

## 使用前准备

- Windows x64，以及与所选功能匹配的显卡、驱动和运行库。
- 能加载 OptiScaler 的游戏接入方式，及其对应的完整运行目录。
- 使用 NVIDIA SR／NR／FG 时，分别准备兼容的运行库。目前 NR 面向 NVIDIA RTX，具体型号兼容取决于配套模型和运行时；AMD NR、Intel NR 和不同显卡的自动配置仍在开发计划中。
- 采用外部 FSR 桥接入时，按桥项目说明准备桥及游戏设置。不同游戏的接入方式不能直接互换。

首次安装前备份原有配置。原神、星铁、绝区零分别保存自己的设置和运行库选择。

## 接入与使用

### 1. 准备运行目录

在已有的、与目标游戏兼容的 OptiScaler 运行包中使用本项目 DLL，保留原包所需的运行库、配置及配套子目录。不要只复制主 DLL 后就认为 SR、NR 和 FG 的依赖已经齐全。

| 组件 | 需要保留的配套文件 |
|---|---|
| NR | 匹配的 `nvngx_dlssnr.dll` 与 `nvngx.dll_dlssnr.dll` |
| Streamline FG | 依赖根下完整且版本匹配的 `streamline/`，包括 `sl.*` 与 `nvngx_dlssg.dll` |
| 配置与其他依赖 | 各游戏配置、原包的其他运行库及许可证；`[Libraries] OptiDllPath` 应指向实际依赖根 |

如果使用支持自定义 OptiScaler 包的启动器，按启动器的导入方式选择该运行目录，再为目标游戏选择它。直接使用加载器时，遵循该加载器的目录和 DLL 命名规则。

- **原神：** 当前接入依赖[外部 FSR→DLSS／NGX 桥](https://github.com/AizawaHikaru233/genshin_fsr_brigde)，游戏中选择 FSR 抗锯齿，并按桥说明配置；单独放入本项目 DLL 不会自动接通原神输入。
- **星铁、绝区零：** 沿用各自兼容的接入方式及独立配置，使用所需的游戏 DLSS 选项或外部桥。菜单能打开、游戏已开启 DLSS，都不能单独证明 SR／NR／FG 输入已经接通。

更换配置或运行库后，重新启动游戏加载。

### 2. 打开界面，逐项启用

使用当前配置指定的菜单快捷键打开界面。源码默认快捷键是 **Insert**，运行包或个人预设可能修改此键。

菜单底部的 **CN / EN** 可以切换中英文，点击“保存设置”保留选择。

1. 先确认 SR 能接收到游戏画面，再调整超分方式与画质。
2. 单独开启 NR，确认效果后再调节模型和强度。
3. 需要时开启 NR 细节保留，观察移动物体、遮挡变化和背景是否出现残影。
4. 最后启用 FG，确认输入来源与输出方式，并观察流畅度及延迟。

每次只改一类设置，便于判断效果来自哪一项。NR 模型和细节保留都会增加开销；具体耗时取决于显卡、分辨率和参数。

### 3. 使用自备 ASI 插件

需要 ASI 时，将插件保留在用户自己的独立目录，并在该游戏实际使用的 `OptiScaler.ini` 中配置：

```ini
[Plugins]
Path = <用户插件目录的绝对路径>
LoadAsiPlugins = true
```

把占位符替换为真实路径，再重新启动游戏。项目提供通用插件加载入口，不下载、打包或复制 ASI。插件是否初始化、SDK 是否支持请求的功能、最终画面是否正确，需要分别确认。

使用本项目的帧生成链路时，`[FrameGen] External = false`；ASI 插件与这一设置是不同的配置项。

## 已知问题

以下为截至 **2026-11-13** 用户确认的未解决事项：

- 帧生成有残影。
- NR 细节保留有残影。
- HUDless 无法工作。
- 帧生成会导致 GPU 略微排队。
- NR 细节会在深度不同的物体上保留，对背景效果不佳。
- 超分耗时较高。
- NR 细节保留耗时较高，延迟较高。
- 帧计时没有计入 FG，也不显示帧数。
- Reflex 计时将模拟和排队错误地计入第一个计时项。

目前不能只凭界面计时判断 FG 的完整开销、实际显示帧率或各段延迟。

## 开发计划

- [ ] GIMI 接入与兼容完善。
- [ ] ReShade 在超分前执行。
- [ ] XeFG 接入与验证。
- [ ] AMD NR。
- [ ] Intel NR。
- [ ] 不同显卡设备的自动配置。
- [ ] UI 重构。

这些项目仍待完成；仓库中已有相关代码或选项，不代表完整功能已可用。

## 从源码构建

产品源码位于 `codex/f6-rewrite` 分支。准备 Visual Studio 2022 C++ 构建工具、MSVC v143 和 Windows SDK，在其开发者 PowerShell 中执行：

```powershell
git clone --branch codex/f6-rewrite https://github.com/thx114/DLSS5-F6-NEXT.git
cd DLSS5-F6-NEXT
$repoRoot = (Get-Location).Path
$null = [IO.Directory]::CreateDirectory("$repoRoot/build")
$compilerWorkers = [Math]::Min(8, [Environment]::ProcessorCount)
$savedCompilerFlags = $env:_CL_
try {
  $env:_CL_ = "/MP$compilerWorkers /FS"
  MSBuild .\OptiScaler\OptiScaler.vcxproj /t:Build /m:1 `
    /p:Configuration=Release /p:Platform=x64 `
    "/p:SolutionDir=$repoRoot/" "/p:IntDir=$repoRoot/build/obj/" `
    "/p:OutDir=$repoRoot/build/out/" /p:PostBuildEventUseInBuild=false `
    /fileLogger "/fileLoggerParameters:LogFile=$repoRoot/build/msbuild.log;Verbosity=normal;Encoding=UTF-8"
  if ($LASTEXITCODE -ne 0) { throw "MSBuild failed: $LASTEXITCODE" }
} finally {
  $env:_CL_ = $savedCompilerFlags
}
```

`OptiScaler/` 与 `external/` 保持同级。默认最多使用 8 个编译工作进程。产物为 `build/out/OptiScaler.dll`，UTF-8 构建日志为 `build/msbuild.log`；运行库准备和游戏接入按上文进行。

## 反馈问题

提交反馈时，请附上游戏及版本、显卡与驱动、所用构建、渲染接口、分辨率，以及 SR／NR／细节保留／FG 的开关与参数。尽量说明复现步骤，并提供对应日志或画面，区分残影、掉帧、延迟和计时显示异常。

输入预览与 NR 历史调试方法见[产品分支的诊断说明](https://github.com/thx114/DLSS5-F6-NEXT/blob/codex/f6-rewrite/docs/F6-GUIDE-DIAGNOSTICS.md)。开发协作规则见 [AGENTS.md](AGENTS.md)。

## 许可证

项目许可证见 [LICENSE](LICENSE)，第三方署名见 [Licenses](Licenses/) 及各依赖目录。
