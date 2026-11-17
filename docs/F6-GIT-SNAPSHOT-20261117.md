# F6 已部署源码的 Git 保存点（2026-11-17）

本保存点将已经构建、定向验证并部署的 `4E8D796F3C503A80578ECAB01BC447C5A535AB8D1E48CCD013CA96BB8380C408` 源码写入 Git。它是后续精简提交的依赖基线，避免旧 HEAD 仍调用已被后续合版替换的呈现链。

来源为 `build/f6-monitor-reflex-en-20261117/source-final/manifest.json` 的冻结源码，SHA256 为 `589AEC7F90BBA1F1248C7A10380A568F6EC1D3767CF0EFBCD1DD0E9E1B0AD2ED`。Git 保存生产源码、相应测试、工程项和 runner；SDK、静态库、构建目录及自动生成的构建时间/commit 头不随本提交分发。原有不参与构建的测试素材不因快照未列出而删除。

该基线包含已完成的 TAA 输入、NR/HDR 数值域与连续性、SR 前滤镜私有 DXGI 隔离、输出 FPS/FG 状态及 Reflex/英文合版；保留前两批精简。删除已被当前路线替代的 `DlssNr_GenshinPresentation.inl`。不采集共享工作区随后发生的 MI 模型替换及其他未完成修改。

历史交付回执记录 Release x64 exit 0、10/10 实际 DLL 宿主通过、原位部署成功。游戏实际加载身份、画面与性能仍需用户手动验收；本次 Git 保存操作没有再次部署或操作游戏。
