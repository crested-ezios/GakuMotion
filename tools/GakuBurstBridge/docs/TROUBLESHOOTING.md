# 使用排障

| 现象 | 先检查 |
| --- | --- |
| F1 Manager 或 GakuMotion 页面没有出现 | 先检查 Loader/GakuMotion 安装；Bridge 不提供独立窗口。 |
| 伴生 DLL 找不到 | 路径应为 `GakuLoader/plugins/GakuBurstBridge.dll`，使用 Windows x64 配套版本。 |
| 一直 pending，随后回退 | 查看 detail；可能没有活跃的支持入口或客户端已改变。请保留内建物理，前往issue报告版本与状态。 |
| ABI/version/size、metadata/profile 拒绝 | 检查组件组合和支持范围。 |
| 换场景/角色后 faulted | 停止动作并保存状态，再按 GakuMotion 常规流程重新加载场景；记录可复现步骤。 |
| 已换 DLL 但仍像旧版本 | 正常退出并重新启动；库不会同进程热卸载。 |
| 需要恢复动作播放 | 切回内建物理；完整移除见[卸载](INSTALLATION.md#卸载)。 |

Bridge 的状态和少量初始化日志通过 GakuMotion 转交宿主。使用 GakuLoader 时，收集游戏目录的 `GakuLoader/Logs/gakuloader.log` 及 GakuMotion 页面 state/result/detail。

报告时请附游戏客户端、GakuMotion/Loader/Bridge 版本或来源提交，Windows 版本、实际后端、重启后是否仍出现、场景/角色/服装和复现步骤，以及简短相关日志。维护者要求时再提供只读报告，不直接上传游戏 DLL 或大量录制。

开发者的 ABI/profile、时间窗和 CPU 排查见[维护指南](MAINTENANCE_SOP.md)。
