# F6 项目交接入口

更新：2026-11-12。**绝区零已按最新用户指令独立改用 ASI，原神、星铁保持内置。** 新绝专属 `local/F6-ZZZ-ASI`／DLL283B6B07／源码97a2cad已构建、定向验证和隔离部署；runtime为`A5009284E9948E8D`，真实加载与卡顿改善待重启启动器及绝区零验收，HDR暂缓。详见[产品当前状态](../../DLSS5_NEXT-workers/f6-rewrite/docs/CURRENT-STATE.md)和[完整交接](../../DLSS5_NEXT-workers/f6-rewrite/docs/F6-HANDOFF.md)。

唯一产品写树 `D:/CODE/DLSS5_NEXT-workers/f6-rewrite`，`codex/f6-rewrite`；本根保留导航／迁移基线。启动器仅改nap_cn一个构建选择键，其他两游戏仍`official/F6`；共享1ABD9AFD四DLL原硬链接、233项保护文件、全局state和旧profiles未改。绝专属配置`External=false`，两个内置解锁关闭，既有ASI171AC18D从独立插件目录加载；SR／NR、原6×及NR版本保持。原FSR桥CC301B42未动。

新生产ASI在post-attach worker初始化，FG有界等待保留SR／NR独立；Gate28、loader context5、options1132、builtin41、Reflex19、7种无害Windows loader与16种有效SL请求组合通过，2431输入和948导出核验，独立gpt-6.1-sol/high终审无确定blocker。未操控或查询游戏，不称卡顿已修好。单游戏回执`current-game-deployment-nap_cn.json`与共享`current-deployment.json`分开；不把build/out新文件冒充共享已部署。

旧recording修复8df406c及未部署候选0C7D6709、旧日志和回退保留。GitHub目标仍`thx114/DLSS5-F6-NEXT`。新证据为产品`build/repair-20261112/zzz-asi-*`，详细约束以产品当前状态为准。
