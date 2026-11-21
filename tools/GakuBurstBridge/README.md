# 本仓库配套源码说明

本目录包含独立构建的 GakuBurstBridge 源码，基于上游 dev 提交 `8536b659fa569812273fc8d037917111b3ca04b1`，并整合本地动态上下文修改（准备提交 `c83756fc6281b5f41ae3a7a8eb845fd5bc7982ac`）。它是独立伴生库，并非 GakuMotion 主插件。

`SOURCE_PROVENANCE.json` 和 `SOURCE_REVIEW.md` 是早期来源与审阅记录，不是本目录当前版本的逐文件校验清单。MinHook 已包含许可和来源说明；其精确 fork/修改来源尚未确认，详见 `deps/minhook/UPSTREAM.md` 和既有来源记录。

# GakuBurstBridge

[安装与卸载指南](docs/INSTALLATION.md) · [使用排障](docs/TROUBLESHOOTING.md) · [版本兼容说明](docs/COMPATIBILITY.md)

## 这是什么

GakuBurstBridge 是 [GakuMotion](https://github.com/crested-ezios/GakuMotion) 的可选 Windows x64 原生物理伴生库。它将动作播放时捕获的角色身体姿态传递给已确认的 Unity Burst Swing 动画流，让官方头发和裙摆解算读取动作输入。

当前 GakuMotion 默认优先尝试原生物理；本伴生库缺失或未就绪时使用内置弹簧物理。用户也可显式选择内置后端，常规动作、表情与镜头播放不要求安装 Bridge。

## 准备工作与依赖

### 前置要求
- 已安装好 [GakuLoader](https://github.com/crested-ezios/GakuLoader) 插件加载器（游戏根目录下的 `version.dll`）。
- 已安装好 [GakuMotion](https://github.com/crested-ezios/GakuMotion) 动作播放插件（`GakuLoader/plugins/GakuMotion.dll`）。
- 配套版本的 `GakuBurstBridge.dll`。

### 文件放置路径

退出游戏后，将 `GakuBurstBridge.dll` 放置在如下特定目录中：

```text
<游戏根目录>/
├── gakumas.exe
├── version.dll                              # GakuLoader 核心
└── GakuLoader/
    └── plugins/
        ├── GakuMotion.dll                   # 动作播放插件
        ├── GakuMotion.VmdImporter.Native.dll # VMD 导入器
        └── GakuBurstBridge.dll              # 按需加载的物理伴生库
```

## 使用方法与快捷键

安装完成后，在游戏内按以下步骤启用：

1. 启动游戏并进入“偶像摄影”。
2. 按 **Ctrl+M** 唤出 GakuMotion 控制面板。
3. 切换至“设置”页，在“物理引擎”中选择“原生物理（默认，实验性）”。

## 常见问题与排查

1. **设置中选择原生物理后为什么不生效**：
   - 确认配套 DLL 是否放在 `GakuLoader/plugins/GakuBurstBridge.dll`。
   - 检查 `GakuLoader/Logs/gakuloader.log` 日志文件，搜索 `BurstBridge` 查看加载日志与初始化状态。
2. **角色头发或服装出现异常拉伸或抖动**：
   - 当动作位移幅度过大或帧率发生剧烈波动时，官方物理求解器可能产生瞬间抖动。
   - 可以在 GakuMotion 设置中随时将次级物理后端切回“内建弹簧物理”。
3. **如何彻底卸载**：
   - 完全退出游戏后，移走 `GakuLoader/plugins/GakuBurstBridge.dll`。原生后端不可用时会回退到内建物理；若原先已关闭物理，则保持关闭。模组不支持运行中热卸载。
4. **更多排障细节**：
   - 详细的诊断方法与故障排查请参阅 [使用排障指南](docs/TROUBLESHOOTING.md)。

## 源码构建

### 编译环境
- 操作系统：Windows 10 / 11 64 位
- 编译器：Visual Studio 2022（或更高版本，包含 MSVC C++20 与 Windows 10/11 SDK）
- 发布源码快照包含所需 MinHook 源码；开发检出如配置为子模块，请按仓库配置初始化并确认依赖完整。

### 编译命令
在 `GakuBurstBridge` 根目录下运行 PowerShell：

```powershell
# 编译 Release（产物输出为 build/GakuBurstBridge.dll）
powershell -NoProfile -File build.ps1

# 编译并运行合成测试套件（覆盖 ABI、Core、Quartz 与 Sleeve 测试）
$bridgeTestTemp = Join-Path (Get-Location) ('build/test-temp-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $bridgeTestTemp -Force | Out-Null
$env:TEMP = $bridgeTestTemp
$env:TMP = $bridgeTestTemp
powershell -NoProfile -File build.ps1 -RunTests
```

编译完成后，手动将 `build/GakuBurstBridge.dll` 复制到游戏目录的 `GakuLoader/plugins/` 文件夹中即可。

### 开发者文档

> 本仓库的部分开发者文档、接口契约和版本适配 SOP 由 AI Coding Agent 编写，并不代表开发者已逐项人工审校或能够独立复现全部研究过程。
> 这些文档主要用于保存开发过程中的技术发现、维护方法与验证依据。由于 GakuBurstBridge 涉及较多底层实现细节，后续维护建议结合源码、测试记录和 AI Coding Agent 进行。
> 项目开发过程中主要使用 Codex 与 ChatGPT 辅助分析、实现和验证。文档中的技术结论仍可能存在错误或过时之处。

- [文档索引导航](docs/README.md)
- [C ABI v2/v3 接口定义](include/GakuBurstBridge.h) 与 [集成契约](docs/INTEGRATION.md)
- [客户端更新维护指南](docs/MAINTENANCE_SOP.md)
- [构建验证记录](docs/VALIDATION.md)
- 源码遵循 [MIT 许可证](LICENSE)，第三方通知见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)，第三方依赖许可与声明见上述通知及 `deps/minhook/LICENSE.txt`。
