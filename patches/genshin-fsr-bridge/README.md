# F6 使用的外部 FSR 桥补丁

本目录保存外部 `AizawaHikaru233/genshin_fsr_brigde` 的可重建源码差异，避免修复只留在被忽略的本机构建目录。

- 上游基底：`c37d3a54ee736595bd9d461e11ab475dd137a00e`，是固定参考，不表示远端最新版本。
- 本地源码保存点：`f53873a7943b502e7739f5361c8b8f65c714b602`。
- 补丁：`0001-preserve-provider-and-interface-routing.patch`，包含接管范围配置控制和提供者 / COM 转发修复。

在上游仓库的干净基底上应用：

```powershell
git switch -c codex/f6-bridge c37d3a54ee736595bd9d461e11ab475dd137a00e
git apply --check '<F6>/patches/genshin-fsr-bridge/0001-preserve-provider-and-interface-routing.patch'
git apply --index '<F6>/patches/genshin-fsr-bridge/0001-preserve-provider-and-interface-routing.patch'
```

`<F6>` 替换为 F6 工作区路径。补丁不包含安装器、第三方 DLL、用户配置或原神内部捕获链。

在 x64 MSVC 开发终端中构建，`<SDK>` 指已有 FidelityFX SDK 的根目录：

```powershell
cmake -S Dx11FsrBridge -B build-f6-bridge -G Ninja `
  -DCMAKE_BUILD_TYPE=Release -DFFX_SDK_KIT_ROOT='<SDK>/Kits/FidelityFX' `
  -DDX11FSRBRIDGE_ENABLE_NGX=ON -DDX11FSRBRIDGE_RELEASE_RUNTIME=ON `
  -DDX11FSRBRIDGE_DIAGNOSTICS=OFF -DDX11FSRBRIDGE_BUILD_TESTS=ON `
  -DDX11FSRBRIDGE_BUILD_FAMILY_TEST=OFF
cmake --build build-f6-bridge --parallel 6
ctest --test-dir build-f6-bridge --output-on-failure
```

当前 13 项测试覆盖 NGX 参数、GPA / IAT 多提供者、嵌套转发、成功设备与工厂、精确接口表边界、代理绘制前后处理、SwapChain3 / 4 方法配对和真实 WARP 资源字节。旧桥负控及修正前的 HDR 槽位负控均检出错误。测试不启动游戏，不能代替 Rocket / GIMI、新局画面或性能验收。

补丁保留了一个已记录的继承问题：COM 对象同址重建的安装身份及克隆表退休尚未闭合，见应用补丁后的 `docs/OPEN-ISSUES.md`。当前测试不覆盖地址复用。

本机部署身份及日志以 `docs/CURRENT-STATE.md` 和对应构建回执为准。
