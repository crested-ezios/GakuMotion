# 构建与验证

GakuMotion 由 C++ 插件 `GakuMotion.dll` 和 C# NativeAOT 导入器 `GakuMotion.VmdImporter.Native.dll` 组成。日常使用预编译文件不需要开发工具；安装位置见[安装指南](docs/INSTALLATION.md)。本文基于 v0.1.0 发布线，可能存在过时之处，具体功能实现以实际代码为准。

## 环境与依赖

- Windows x64、PowerShell，以及安装了桌面 C++ 工作负载和 Windows SDK 的 Visual Studio 2022 或更新版本。
- NativeAOT 导入器构建需要 .NET 8 SDK、MSVC 和 Windows SDK；首次恢复依赖可能需要访问配置的 NuGet 源。运行预编译导入器不需要安装 .NET 运行时。
- 构建前确认仓库所需的 Dear ImGui、MinHook、KineTexture 与 Candidate B 来源文件均已就位。开发仓库的子模块需要按其仓库配置准备；发布源码快照包含构建所需的选择文件。
- 发布仓库根目录的 `global.json` 选择稳定的 .NET 8.0.427 feature band，允许该 band 中的更新补丁；开发仓库应按自身工具链选择兼容的 SDK。

## 构建

在所使用的 GakuMotion 仓库根目录执行相应命令。两套仓库的 `build.ps1` 参数与输出位置不同，请使用对应行：

| 仓库 | 命令 | 输出 |
| --- | --- | --- |
| 发布源码 | `pwsh -NoProfile -File build.ps1 -Target All` | `build/native/GakuMotion.dll` 与 `build/nativeaot/GakuMotion.VmdImporter.Native.dll` |
| 开发源码：运行时 | `powershell -NoProfile -File build.ps1 -Configuration Release` | `build/GakuMotion.dll` |
| 开发源码：导入器 | `powershell -NoProfile -File interop/publish-native.ps1 -OutputDir build/test-vmd-csharp` | `build/test-vmd-csharp/GakuMotion.VmdImporter.Native.dll` |

发布源码还支持 `-Target Runtime`、`-Target NativeAOT`、`-Target Preflight` 和 `-Target Contracts`；`-OutputRoot` 可指定输出目录，`-DotNet` 可指定 SDK 可执行文件。开发源码的运行时脚本提供 `-Configuration`、`-NoImGui`、`-Preview`、`-DiagnosticCapture` 和依赖目录选项，不支持发布脚本的 `-Target` 参数。两个脚本不会自动安装运行时插件。

将匹配的两个 DLL 放入 `GakuLoader/plugins/`，并正常退出、重新启动游戏才能加载新版。

## 离线验证

| 仓库 | 命令 | 检查范围 |
| --- | --- | --- |
| 发布源码 | `pwsh -NoProfile -File tests/run_release_tests.ps1` | 合成核心、舞台加载边界与界面语法 |
| 开发源码 | `powershell -NoProfile -File tests/run_tests.ps1` | 开发仓库的离线核心、相关边界与界面语法 |

两套仓库均提供 `tests/run_layout_tests.ps1`、`tests/test_layout_migration.ps1` 和 `tests/run_unicode_path_tests.ps1`，用于各自源码范围的合成路径与迁移检查。发布源码的 `pwsh -NoProfile -File build.ps1 -Target Contracts` 运行舞台加载的合成契约检查。

## 常见问题

| 现象 | 检查 |
| --- | --- |
| 找不到 Visual Studio、MSVC 或 SDK | 检查桌面 C++ 工作负载、x64 工具链与 Windows SDK |
| 找不到依赖源码 | 按所使用仓库的文件及子模块配置补全，不以空文件替代 |
| .NET SDK 不匹配或 NuGet 恢复失败 | 核对 SDK 版本、发布仓库的 `global.json` 与恢复日志 |
| 导入器加载或 ABI 失败 | 检查 DLL 是否来自同一配套构建，重新启动游戏并查看[故障排查](docs/TROUBLESHOOTING.md) |

架构与格式分别见[架构说明](docs/ARCHITECTURE.md)和[动作与镜头格式](docs/formats/ACTION_FORMATS.md)。
