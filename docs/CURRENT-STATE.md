# F6 当前状态

更新：2026-11-12。**FG 排队锁环修复、退出入口保护、全界面英文已完成源码和离线验证；Release x64 候选 ADFF3486 已构建，未部署。HDR 按用户要求暂缓。**

唯一产品写树 `D:/CODE/DLSS5_NEXT-workers/f6-rewrite`，分支 `codex/f6-rewrite`；产品源码提交 `b1ee21020a58`。导航树为 `D:/CODE/DLSS5_NEXT`，分支 `codex/navigation-20261112`。GitHub 目标 `thx114/DLSS5-F6-NEXT`，本轮修复单独提交，远端身份以本轮 Git 回执核验。

| 项目 | 当前事实 |
|---|---|
| 最新用户实测 | ZZZ 关闭 FG 后周期性卡顿消失，仍观察到 GPU 等待。此反馈针对旧部署，不代表新候选通过 |
| DLSS 接入 | 固定日志 `build/repair-20261112/zzz-186740.log` 已出现 `owner=selected`；不再沿用旧 PID180352 的 backend-only 结论 |
| FG 修复 | 无关 native list 的 Reset/Execute 通知先以独立短锁核对身份，避开 Present 持 continuity 等待 SDK worker 的锁环；相关／未知 list 仍走原检查，保留真实 queue/fence/Reset 退休门 |
| 退出保护 | 进程寿命 pin、attach 缓存 loader probe、退出／loader context 的 owner/NGX/DLSSG/重配／child 退休入口拒绝；拒绝保留完整对象，并释放当前线程 CPU lease。不支持热卸载，不取消已在途 SDK 调用 |
| 界面英文 | 新增354词条，共2013；覆盖监视、NR持续调试、弹窗、通知与动态状态。已显示通知随语言切换，用户路径、格式参数和 ImGui ID 保留；视觉布局未实测 |
| HDR | 暂缓；本轮 HDR helper、测试及色彩状态改动已撤出。留存 `build/repair-20261112/hdr-deferred-20261112.zip`，不进入候选 |
| HUDless/UI | 桥固定提交1865c585没有独立HUD-less/UI发布ABI；不能用SR颜色或reactive/transparency mask冒充；[真实输入契约](FG-BRIDGE-HUD-CONTRACT.md)保持 |
| 新候选 | `build/out/OptiScaler.dll`；SHA256 `ADFF3486F21C52FCD0FE72D73A14BB3AFA93E599A2A73AD72DCC7EAD1F790372`；33,985,536 B，内置FG，无新增ASI，Release x64构建通过；编译／链接警告保留在日志，不称零警告 |
| 身份验证 | 2425项输入（含FgDepthContract.h与vendored依赖）构建前后不变；生成版本头另存；53必要NGX导出及全部948导出名称／序号核验一致 |
| 定向验证 | 16组通过，含105项清理接线、真实Windows生命周期21项、wrapper Release28项、FG Hook55场景／1873项、NGX生命周期50项、英文6061项／6负控、native guides29项及旧缺陷负控、ZZZ16套／107接线、12套核心CPU/WARP；详见本轮tests.json |
| 正式部署 | 保持5265915DF9DC34951F10E3D60CB11819EADB5FAAABD427F7F4DDA846B1360238；本轮不写正式DLL或profiles，不查询／操控游戏 |
| FSR桥 | 保持已部署 `CC301B42EC7AA31059B9FCD10B29B3915B9EB21DBE363D5F431ED9201CB904A1`，本轮未改 |

本轮回执平铺在 `build/repair-20261112/repair-current-{source,tests,review,artifact}.json` 及对应日志；当前构建指针 `build/current-{artifact,build,tests}.json` 指新候选，`build/current-deployment.json` 仍指旧安装。旧5265915D候选另存 `build/rollback/repair-pre-5265915D.dll`，旧回执保留。

游戏加载身份、FG实际显示率、GPU等待／周期卡顿改善、退出正常性、英文布局仍需用户在新版本中分别验证。离线检查和构建成功不代替这些验收。FG使用内置实现，跳过残留MFGUnlocker.asi；不修改用户配置或跨游戏借用结论。
