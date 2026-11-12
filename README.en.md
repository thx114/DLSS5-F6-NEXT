# DLSS5 F6 NEXT

[简体中文](README.md)

An experimental image enhancement project based on OptiScaler, primarily for Genshin Impact, Honkai: Star Rail, and Zenless Zone Zero. It provides configuration and debugging tools for super resolution (SR), neural rendering (NR), frame generation (FG), and NR detail retention.

The project is still under development, with image quality, compatibility, and latency still needing improvement. Read the [known issues](#known-issues) before use, and adjust settings for each game and GPU.

## Features

| Feature | Purpose |
|---|---|
| SR: Super resolution | Reconstructs a lower-resolution rendered image at the output resolution |
| NR: Neural rendering | Applies neural rendering, with controls for the model, strength, and other settings |
| NR detail retention | Retains details produced by NR across frames, with controls for history duration, strength, and debug previews |
| FG: Frame generation | Integrates frame generation, with separate settings for the input source, output method, and requested generated frames |
| Debugging interface | Shows input resources, NR history contributions, runtime status, and timing information; timing currently has known issues |

SR, NR, and FG are configured separately. Enabling one does not mean the others are active. The requested number of generated frames does not establish the actual displayed frame rate.

**6× frame generation on RTX 40 series GPUs is provided by ASI. This project contains no code for that functionality and does not distribute ASI.**

## Before you start

- Windows x64, with a GPU, drivers, and runtime libraries compatible with your chosen features.
- A way to load OptiScaler into the game, with its complete runtime directory.
- Compatible runtime libraries for each NVIDIA SR, NR, or FG feature you use. NR currently targets NVIDIA RTX; support for specific models depends on the supplied model and runtime. AMD NR, Intel NR, and automatic GPU configuration remain on the roadmap.
- If you use an external FSR bridge, prepare the bridge and game settings according to its documentation. Integration methods are not interchangeable between games.

Back up your existing configuration before installing. Keep separate settings and runtime library selections for Genshin Impact, Honkai: Star Rail, and Zenless Zone Zero.

## Setup and usage

### 1. Prepare the runtime directory

Use this project's DLL in an existing OptiScaler runtime package that is compatible with the target game. Keep the libraries, configuration files, and supporting subdirectories required by that package. Copying only the main DLL does not provide all dependencies for SR, NR, and FG.

| Component | Required supporting files |
|---|---|
| NR | Matching `nvngx_dlssnr.dll` and `nvngx.dll_dlssnr.dll` |
| Streamline FG | A complete, version-matched `streamline/` directory under the dependency root, including `sl.*` and `nvngx_dlssg.dll` |
| Configuration and other dependencies | Each game's configuration, the original package's other runtime libraries, and licenses; `[Libraries] OptiDllPath` must point to the actual dependency root |

If your launcher supports custom OptiScaler packages, import the runtime directory using the launcher's procedure and select it for the target game. When using a loader directly, follow that loader's directory and DLL naming rules.

- **Genshin Impact:** Integration currently requires an [external FSR-to-DLSS/NGX bridge](https://github.com/AizawaHikaru233/genshin_fsr_brigde). Select FSR anti-aliasing in the game and follow the bridge's instructions. Adding this project's DLL alone does not connect Genshin Impact's inputs.
- **Honkai: Star Rail and Zenless Zone Zero:** Use an integration method and separate configuration compatible with each game, including the required in-game DLSS option or external bridge. Opening the menu or enabling in-game DLSS alone does not prove that SR, NR, or FG inputs are connected.

Restart the game after changing configuration files or runtime libraries.

### 2. Open the interface and enable features one at a time

Open the interface using the menu hotkey in your active configuration. The source default is **Insert**; runtime packages or personal presets may change this key.

Use **CN / EN** at the bottom of the menu to switch languages. Click **Save Settings** to save your choice.

1. Confirm that SR receives the game's image, then adjust the upscaling method and image quality.
2. Enable NR on its own. Once you have confirmed the effect, adjust its model and strength.
3. Enable NR detail retention if needed. Check moving objects, changes in occlusion, and backgrounds for ghosting.
4. Enable FG last. Confirm its input source and output method, then check smoothness and latency.

Change one group of settings at a time so you can identify which feature produces each effect. NR models and detail retention add processing cost; the actual cost depends on your GPU, resolution, and settings.

### 3. Use your own ASI plugins

Get the release package from [dashdogy/MFG-Unlocker Releases](https://github.com/dashdogy/MFG-Unlocker/releases). See the [upstream OptiScaler guide](https://github.com/dashdogy/MFG-Unlocker/blob/main/docs/optiscaler.md#menus) for integration and menu instructions.

Extract the package, rename `MFGUnlocker.dll` to `MFGUnlocker.asi` (change only the file extension), and place it in your own separate plugin directory. Configure the `OptiScaler.ini` actually used by that game:

```ini
[Plugins]
Path = <absolute path to your plugin directory>
LoadAsiPlugins = true
```

Replace the placeholder with a real path, then restart the game. The project provides a general plugin loader and does not download, package, or copy ASI. Plugin initialization, SDK support for the requested features, and the final image must each be checked separately.

When using this project's frame generation pipeline, set `[FrameGen] External = false`. ASI plugin loading is a separate setting.

## Configurations for the three games

These configuration snapshots were exported on **2026-11-13** from each game's current runtime directory. They preserve the separate SR, NR, detail retention, FG, and interface settings for reference and adjustment.

| Game | Configuration | NR `WorkingScale` | Companion NR library |
|---|---|---|---|
| Genshin Impact | [OptiScaler.ini](configs/genshin/OptiScaler.ini) | `0.84` | Custom version; supply your own |
| Honkai: Star Rail | [OptiScaler.ini](configs/star-rail/OptiScaler.ini) | `0.86` | `310.9.16-SF` |
| Zenless Zone Zero | [OptiScaler.ini](configs/zenless-zone-zero/OptiScaler.ini) | `0.81` | `310.9.16-SF` |

All three retain `FGInput=Upscaler`, `FGOutput=DLSSG`, `External=false`, `LoadAsiPlugins=true`, and a request for 5 generated frames (a target total multiplier of 6×). Actual support and the displayed multiplier depend on the GPU, runtime libraries, and your own ASI.

1. Back up the target game's existing configuration, then save the corresponding file as the `OptiScaler.ini` actually loaded by that game. With a launcher, apply it through the launcher's configuration import or persistence mechanism so an old preset does not overwrite it at startup.
2. Replace `[Libraries] OptiDllPath = <DEPENDENCY_ROOT>` with the absolute path to your dependency root directory, not a DLL file, and `[Plugins] Path = <USER_PLUGIN_DIRECTORY>` with the absolute path to your plugin directory. Replace each entire placeholder, including the angle brackets, before use.
3. Prepare the required runtime libraries and plugins, then manually restart the game. If you do not use ASI, adjust plugin loading and the FG request accordingly.

The public copies remove machine-specific paths, reset the log filename to `auto`, and omit legacy frame generation settings that the current F6 no longer reads. They are snapshots of the current settings, subject to the known issues below, and are not universal recommendations or evidence of successful testing in the games. The INI files do not include DLLs, models, or ASI.

## Known issues

The following issues were confirmed by the user and remain unresolved as of **2026-11-13**:

- Frame generation produces ghosting.
- NR detail retention produces ghosting.
- HUDless does not work.
- Frame generation causes slight GPU queue buildup.
- NR details can persist on objects at different depths, with poor results on backgrounds.
- Super resolution takes too much processing time.
- NR detail retention has high processing cost and latency.
- Frame timing does not include FG and does not show frame counts.
- Reflex timing incorrectly attributes simulation and queueing time to the first timing item.

The interface's timing information alone cannot establish the full cost of FG, the actual displayed frame rate, or latency at each stage.

## Roadmap

- [ ] GIMI integration and compatibility improvements.
- [ ] Run ReShade before super resolution.
- [ ] XeFG integration and validation.
- [ ] AMD NR.
- [ ] Intel NR.
- [ ] Automatic configuration for different GPUs.
- [ ] UI redesign.

These items remain unfinished. Existing code or options in the repository do not mean that the complete feature is available.

## Build from source

The product source is on the `codex/f6-rewrite` branch. Install the Visual Studio 2022 C++ build tools, MSVC v143, and the Windows SDK, then run the following in its Developer PowerShell:

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

Keep `OptiScaler/` and `external/` in the same parent directory. The example uses up to 8 compiler workers. The output is `build/out/OptiScaler.dll`, with a UTF-8 build log at `build/msbuild.log`. Prepare the runtime libraries and game integration as described above.

## Report an issue

Include the game and its version, GPU and driver, project build, graphics API, resolution, and the settings for SR, NR, detail retention, and FG. Describe reproduction steps where possible, and provide the relevant logs or images. Distinguish ghosting, frame drops, latency, and incorrect timing displays.

For input previews and NR history debugging, see the [diagnostics guide on the product branch](https://github.com/thx114/DLSS5-F6-NEXT/blob/codex/f6-rewrite/docs/F6-GUIDE-DIAGNOSTICS.md) (Chinese). Development collaboration rules are in [AGENTS.md](AGENTS.md) (Chinese).

## License

See [LICENSE](LICENSE) for the project license, and [Licenses](Licenses/) and the dependency directories for third-party notices.
