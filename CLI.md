# F5 启动器 CLI 要点

详细手册见 [启动器历史手册](docs/history/LAUNCHER-CLI.md)（历史文档，路径指向旧根，仅作参考）。

- 启动原神前必须核实逐游戏 Opt 开关；全局启用、选择 DLSS F5 和 `launch_game` 成功均不能代替它。
- Opt 与 HoYoShade 通过启动器既有流程载入，Opt 内部功能随 Opt 主 DLL 构建；禁止使用 `LoadDllMain` 或额外旁路载入。
- 游戏结束方式以手册为准；含强制结束回退的命令不能当作"仅正常关闭"。
- 未通过真实游戏验收的能力会明确标注。
