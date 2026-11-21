# 开发者构建与第一次接入

## 获取和构建

取得完整源码和依赖后再构建。发布源码快照已包含 MinHook；开发检出若使用 Git 子模块，需按其 `.gitmodules` 初始化。只复制 `src/` 会遗漏公开头文件和依赖。

需要 Windows x64、Visual Studio 2022 或更新版本的 MSVC C++ x64/x86 工具链和 Windows 10/11 SDK。脚本用 Visual Studio Installer 的 `vswhere` 自动设置环境，不要求 Developer PowerShell。

```powershell
powershell -NoProfile -File build.ps1
```

在仓库根目录运行，得到 `build/GakuBurstBridge.dll`；`.lib`、`.exp` 等为同目录构建产物。脚本不安装到游戏。运行时 DLL 使用 C++20、静态 CRT `/MT` 和 MSVC，不改用 MinGW。

## 合成测试

ABI 测试会在 TEMP 创建再删除固定名 JSON，请给本次测试一个新目录：

```powershell
$bridgeTestTemp = Join-Path (Get-Location) ('build/test-temp-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $bridgeTestTemp -Force | Out-Null
$env:TEMP = $bridgeTestTemp
$env:TMP = $bridgeTestTemp
powershell -NoProfile -File build.ps1 -RunTests
```

此命令构建 DLL 并运行 ABI、Core、Quartz、Sleeve 和多人上下文用例；ABI 程序仅加载本次 DLL，其余测试使用受控内存和回调，不需要游戏输入。

可选 Python 工具与测试只依赖标准库，Windows CPython 3.12 已实测：

```powershell
python -m unittest discover -s tools/tests
python tools/inspect_burst.py --help
python tools/inspect_metadata.py --help
```

这些入口使用合成输入，无需另装测试框架。

## 第一次 C ABI 接入

1. 包含 [GakuBurstBridge.h](../include/GakuBurstBridge.h)，按固定文件路径加载 Windows x64 DLL。
2. 解析 `GakuBurstBridge_GetApi` 并单演员请求 ABI v2，多人请求 ABI v3。设置 size/version、清零 reserved，校验必需函数指针。
3. 在 Unity 主线程配置、发布和清理，v3 每位演员持有独立上下文。线程与所有权要求见[集成契约](INTEGRATION.md)。
4. 区分 OK、pending、unavailable、faulted、shutdown；失败交回 fallback，避免两个求解器写同一批骨骼。

本库没有脱离 GakuMotion/Unity 的动作播放程序。[abi_tests.cpp](../tests/abi_tests.cpp)演示函数表协商，并确认没有 GameAssembly 时返回 unavailable；它不是完整游戏接入示例。

## 构建故障

| 错误 | 处理 |
| --- | --- |
| `Visual Studio C++ Build Tools and Windows SDK are required` | 安装 VS C++ x64/x86 工作负载、Windows SDK 与 Visual Studio Installer。 |
| `No MSVC x64 installation found` | 确认安装了能被 vswhere 发现的 C++ 工具。 |
| `MSVC or Windows SDK headers are missing` | 补齐 SDK 头文件和 x64 库。 |
| `Vendored MinHook sources are missing` | 重新取得完整源码；开发检出若使用子模块，按仓库配置初始化后重试。 |
| 构建成功但游戏未变化 | 构建不负责安装，新 DLL 也不会在现有进程生效；见[安装指南](INSTALLATION.md)。 |

MinHook 的许可见 `deps/minhook/LICENSE.txt` 和[第三方通知](../THIRD_PARTY_NOTICES.md)。文档维护不改变该依赖。

## 开发分支与来源

从 `main` 创建发行标签，日常功能合入 `dev`。将发行修复合回 `dev`，按成熟功能向 `main` 交付。

`SOURCE_PROVENANCE.json` 与 `SOURCE_REVIEW.md` 记录 v0.1.0 发行基线的来源与评审。多人上下文增量来自旧开发线提交 `b973445`，可在 `dev` 的 Git 历史中审查。
