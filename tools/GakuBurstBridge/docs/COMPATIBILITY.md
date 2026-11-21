# 版本与组件关系

| 项目 | 当前关系 |
| --- | --- |
| 平台 | Windows x64；原生实现显式要求 64 位指针。 |
| GakuMotion | 加载库并提供角色姿态、生命周期及后端选项，须使用其发行说明配套的 Bridge。 |
| GakuLoader | 是 GakuMotion 的宿主；Bridge 不直接依赖 Loader SDK，也不作为插件扫描目标。 |
| Bridge C ABI | 支持 v2 单演员和 v3 多人上下文函数表。两种接口共用 v2 载荷，调用方校验各表及载荷的 size/version。 |
| 当前参考 profile | `gakumas-burst-20261008-windows-x64`，含 SSE/AVX 参考入口和 ABI 偏移；该日期不是游戏发行版本号。 |
| 工具依赖 | Python 仅维护工具需要，MSVC/SDK 仅源码构建需要；普通用户运行不需要它们。 |
