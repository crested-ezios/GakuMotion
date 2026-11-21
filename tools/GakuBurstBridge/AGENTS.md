# GakuBurstBridge staging 工作规约

在 `git-work/GakuBurstBridge` 中，`main` 是稳定发行工作树，`dev` 是开发工作树。

- 保留旧仓库、历史、真实素材和工作区中已有的其他改动。
- 远端写入、发布与部署按当前任务授权执行。
- 不得部署到游戏、启动/终止/重启游戏、连接游戏进程或加载游戏 DLL。
- 只允许 MSVC Windows x64 构建与合成测试；ABI 测试只加载本次构建的 Bridge DLL。
- 合成测试前设置 TEMP/TMP 到本次新建的隔离 build 子目录。ABI 测试会写入再删除固定名临时 JSON。
- 构建产物、缓存、测试输入/输出、日志只放 ignored build；不进入源码 allowlist。
- 保持 C ABI v2 兼容、v3 载荷布局、默认关闭实验及 fallback 契约。
- MinHook vendor 的固定版本、逐文件 hash 和许可必须保留。不要把自己的修改冒充未经修改的上游源码。
- 修改完成并实际验证后本地提交，精确暂存本轮文件；不得夹带无关改动。
- 验证报告区分构建/合成检查与游戏内验收，不要求重做用户已通过的 GakuMotion 验收。
