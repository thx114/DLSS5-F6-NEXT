# F6 使用的外部 FSR 桥补丁

本目录保存外部 `AizawaHikaru233/genshin_fsr_brigde` 的可重建源码差异，避免修复只留在被忽略的本机构建目录。

- 上游基底：`c37d3a54ee736595bd9d461e11ab475dd137a00e`，是固定参考，不表示远端最新版本。
- 本地完整源码保存点：`5ce2efbf4651a4df7d5e3bd1b0ccfe6a1f45fd5b`；前一保存点为 `586f7f0fd77be27f9ed9884537fff3180b88bbb9`。
- 按顺序应用两个补丁：`0001-preserve-provider-and-interface-routing.patch` 包含接管范围配置控制和提供者 / COM 转发修复；`0002-observe-completed-device-wrappers.patch` 统一两个 D3D11 API 的外层创建观察边界。

在上游仓库的干净基底上应用：

```powershell
git switch -c codex/f6-bridge c37d3a54ee736595bd9d461e11ab475dd137a00e
git apply --check '<F6>/patches/genshin-fsr-bridge/0001-preserve-provider-and-interface-routing.patch'
git apply --index '<F6>/patches/genshin-fsr-bridge/0001-preserve-provider-and-interface-routing.patch'
git apply --check '<F6>/patches/genshin-fsr-bridge/0002-observe-completed-device-wrappers.patch'
git apply --index '<F6>/patches/genshin-fsr-bridge/0002-observe-completed-device-wrappers.patch'
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

当前 18 项测试覆盖 NGX 参数、GPA / IAT 多提供者、嵌套转发、成功设备与工厂、精确接口表边界、代理绘制链、SwapChain3 / 4 方法配对和真实 WARP 资源字节。`ResizeTarget` 专项让不同对象布局的两条交换链交错调用，并反转创建顺序；旧桥在两个顺序下均检出两次错误转发。旧桥的提供者负控及修正前的 HDR 槽位负控也均检出错误。

三个创建边界专项在包装器尚未返回时检查内层 device / context / factory 的方法身份，覆盖 Device → Device、Combined → Device 及失败重试。前一候选三个用例均检出三项提前改写，新候选均为零；返回后再检查桥接入、准确 receiver 和每层 Draw 次数。Combined 用例没有创建真实 swapchain，代理 fixture 不等于真实 GIMI 的 BeforeDraw / AfterDraw。测试不启动游戏，不能代替 Rocket / GIMI、新局画面或性能验收。

嵌套创建隔离只参考用户旧项目 `thx114/genshin_fsr_brigde` 的设计概念（远端固定 `2c8a91e98d8d93a7ea4a9375c1402c75b9aa4848`，本地历史保存点 `366e778221f5b0521ba9c95838f1e731d9db26db`）。未移植其 DLL 黑名单、全局 original、固定长度接口复制或同步读回；创建 scope 不进入每帧路径。该修改尚未建立游戏崩溃根因或性能改善证据。

补丁保留了一个已记录的继承问题：COM 对象同址重建的安装身份及克隆表退休尚未闭合，见应用补丁后的 `docs/OPEN-ISSUES.md`。当前测试不覆盖地址复用。

本机部署身份及日志以 `docs/CURRENT-STATE.md` 和对应构建回执为准。
