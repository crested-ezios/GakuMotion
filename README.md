<div align="center">
<picture>
  <source media="(prefers-color-scheme: dark)" srcset="assets/branding/gakumotion-wordmark-dark.svg">
  <img alt="GakuMotion" src="assets/branding/gakumotion-wordmark-light.svg" width="480">
</picture>
<p>A Native Performance Extension for Gakumas</p>
</div>


GakuMotion 是运行在 GakuLoader 插件宿主之上的游戏原生演出扩展。

它利用《学园偶像大师》原生角色、服装、材质、动画与渲染环境，为玩家提供外部动作回放、面部表现、摄像机运镜及舞台控制能力。

[安装指南](docs/INSTALLATION.md) · [使用指南](docs/USAGE.md) · [常见问题排查](docs/TROUBLESHOOTING.md) · [源码构建](BUILD_NOTES.md) · [全部文档索引](docs/README.md)

## 支持范围

当前支持：

| 内容 | 说明 |
| --- | --- |
| **Motion** | MMD/VMD 舞蹈动作导入与回放 |
| **Facial** | 面部表情、眼动及相关表现 |
| **Camera** | VMD 原生运镜与角色跟随运镜 |
| **Audio** | 同步播放 16-bit PCM WAV 音频 |
| **Stage** | 部分场景，以及自定义舞台环境与场景表现 |

GakuMotion 目前处于 Developer Preview 阶段。

长期目标是提供可编排的原生演出能力，让独立创作工具能够通过受控接口调用游戏内的角色、动作、镜头与场景。

**让偶像以游戏原本的方式，呈现更多可能。**

## 准备工作与依赖

### 前置要求
- 操作系统：Windows 10 / 11 64 位（x64）
- 宿主程序：[GakuLoader](https://github.com/crested-ezios/GakuLoader)（游戏根目录下的 `version.dll`）
- 核心文件：配套版本的 `GakuMotion.dll` 与 `GakuMotion.VmdImporter.Native.dll`

### 文件放置路径

退出游戏后，将相关文件放置在游戏根目录下对应的文件夹中：

```text
<游戏根目录>/
├── gakumas.exe
├── version.dll                                  # GakuLoader 宿主
└── GakuLoader/
    ├── plugins/
    │   ├── GakuMotion.dll                       # 动作播放插件
    │   ├── GakuMotion.VmdImporter.Native.dll     # VMD 原生导入器
    │   └── GakuBurstBridge.dll                  # 可选物理伴生库
    ├── Configs/gakumotion/                      # Motion 配置
    ├── Logs/
    │   ├── gakuloader.log                       # 宿主日志
    │   └── gakumotion/                          # 导入和诊断日志
    └── PluginData/gakumotion/
        ├── playlist.json                        # 播放列表
        └── motions/MyDance/                     # 每支曲目使用独立子目录
            ├── motion.gkclip                    # 导入后的身体动作
            ├── camera.gkcamera                  # 运镜，可选
            └── song.wav                         # PCM WAV 音频，可选
```

可以直接解压候选发布包中的完整安装包，也可以按 [构建指南](BUILD_NOTES.md) 自行编译。升级时覆盖这两个 DLL 即可，不会影响已有曲库素材或个人配置。

## 使用方法与快捷键

启动游戏并进入“偶像摄影”页面，通过以下快捷键与面板进行操作：

| 快捷键 | 功能 | 说明 |
| --- | --- | --- |
| **Ctrl+M** | 唤出或唤回控制面板 | 仅在支持的偶像摄影或更衣室页面中响应 |
| **F9** | 播放 / 暂停 | 切换当前动作的播放与暂停状态 |
| **F10** | 停止播放并释放接管 | 停止动作，角色恢复游戏原生姿态 |

### 常用操作流程
1. **呼出面板**：进入偶像摄影棚后，按 **Ctrl+M** 唤出 GakuMotion 控制面板。
2. **导入资源**：切换到“曲库”页签，可以直接将 VMD 动作文件、表情文件、镜头文件以及 WAV 音频拖入窗口，也可点击“导入”按钮选取文件。
3. **播放与预览**：在列表中选中曲目后，点击“加载预览”可查看动作第一帧；点击“播放选中”开始正式表演。
4. **调整控制**：
   - 播放进度：支持时间轴拖动、帧定位与逐帧查看。
   - 速度与时差：支持全局播放倍速调节，以及单独微调音频和动作的秒级时间偏移。
   - 运镜模式：镜头页与播放器统一提供“游戏 / VMD / 跟随 / 注视”。选择“游戏”释放 Motion 镜头接管并保留参数，恢复游戏手动取景；选择 VMD 播放曲目运镜，无可用轨道时保持机位。旧固定模式配置会在载入时迁移为 VMD。

详细的功能说明与进阶设置请查阅 [使用指南](docs/USAGE.md)。

可选的 `chara-adapter` v1 服务可为实际载入的脸部提供角色身份、显示名与表情拟合表。GakuMotion 也兼容现有的 `rinha.facial-fit` v1。眼眉拟合依据整段曲线判断实际使用的键，零权重的未验收曲线不会阻止已验收曲线。其他插件可通过 `gakumotion.actor/v1` 读取当前选中角色的身份与脸部快照；接口声明在 [GakuMotionActor.h](include/GakuMotionActor.h)。

## 常见问题与排查

GakuMotion 当前仍在积极开发中。已知问题目录请参考 [已知问题](docs/KNOWN_ISSUES.md)。

1. **按 Ctrl+M 无法打开面板**：
   - GakuMotion 仅在偶像摄影及指定更衣室页面开放。游戏主界面、Live 演出、养成培育和未适配页面默认不开放接管，按键不会响应。
   - 检查 `GakuLoader/Logs/gakuloader.log`，确认 `GakuMotion.dll` 是否已成功被加载。
2. **接管期间官方拍照按钮无法点击**：
   - 在 GakuMotion 接管角色骨骼期间，为了避免相机和姿态产生冲突，模组会自动屏蔽游戏官方的手动拍照入口。停止播放（按 **F10**）或收起接管后即可恢复。详情见 [页面准入与拍照边界](docs/USAGE.md#打开面板)。
3. **VR 模式下镜头控制不生效**：
   - 与 [GakuVRify](https://github.com/crested-ezios/gakumas-VRify) 同时使用时，Motion 运镜仍有已知兼容问题，不能假定镜头会自动让位。遇到镜头冲突时，先选择“游戏”释放 Motion 镜头接管，并在反馈中注明 VR 状态。VR 限制见 [兼容性说明](docs/USAGE.md#已知兼容性)。
4. **播放倍速不为 1.0 时没有声音**：
   - 当前音频后端不支持拉伸。音乐的有效速度是“全局播放倍速 × 音乐文件速度”，乘积不为 1.0 时静音，恢复为 1.0 后从当前位置起播。例如，全局 0.5 倍、音乐文件 2 倍时仍可有声播放；只把全局倍速调回 1.0，不一定能恢复音乐。详见 [播放时序](docs/PLAYBACK_TIMING.md)。
5. **更多排错指南**：
   - 若遇到动作导入报错或模型变形，请参考 [常见问题与排查](docs/TROUBLESHOOTING.md)。GakuMotion 会优先处理播放角色为`花海咲季`的相关问题。

## 源码构建

源码构建需要 Windows x64 系统，并安装 Visual Studio 2022（或更高版本，包含 MSVC C++20 工具集）以及 .NET 8 SDK。

发布源码和开发源码的构建入口、参数及产物目录不同。请按所在仓库使用 [构建指南](BUILD_NOTES.md) 中对应的命令。编译完成后，将配套的两个 DLL 放入游戏的 `GakuLoader/plugins/`，并重启游戏。

## 相关项目
### 依赖项目
- [GakuLoader](https://github.com/Gaku-Mod-Tools/GakuLoader)：提供插件宿主、按键分发与控制窗口支持。
- [KineTexture](https://github.com/crested-ezios/KineTexture)：提供底层正向运动学、数学计算与动作采样能力。
- [GakuBurstBridge](https://github.com/crested-ezios/GakuBurstBridge)：可选的原生物理伴生库，用于让官方 Burst 物理读取动作输入。
- 模组源码遵循 [MIT 许可证](LICENSE)，第三方组件遵循各自许可证，详见 [第三方通知](THIRD_PARTY_NOTICES.md)。
### 参考项目与致谢
GakuMotion 的开发过程中参考了 MMD 动作生态、游戏 Mod 工具及相关研究成果。感谢以下为动作语义验证、兼容性研究和技术设计提供的帮助。
 - [Chinosk - Gakumas Localify](https://git.chinosk6.cn/chinosk/gkms-localify-dmm) — 原生插件兼容生态。
 - [KagaminTheMirror - Gakumas VRify](https://github.com/KagaminTheMirror/gakumas-VRify) — VR 兼容生态。
 - [三目YYB - YYB Hatsune Miku_Default](https://www.bilibili.com/video/BV1Bg411D7Me/) — GakuMotion VMD Source Evaluator 研究中的重要 MMD/PMX 参考模型。
 - [樋口優 - MikuMikuDance](https://www.nicovideo.jp/watch/sm2420025) — VMD 的原始执行环境。
 - [Mogg - MikuMikuMoving](https://sites.google.com/site/mikumikumovingeng/) — 另一个独立的 MMD 动作执行环境，作为动作表现验证。
 - [powroupi / 后续维护者 - blender_mmd_tools](https://github.com/powroupi/blender_mmd_tools) — MMD 数据与行为研究参考。
 - [Countd360 - MMD Director](https://www.patreon.com/posts/mmd-director-pv-55604378) — HS2/AI/KK/KKS CharaStudio的 VMD 播放与编排工具先例。
 - [ifui小灰灰 / ArinLee](https://arin3000.github.io/) — 学马仕动画与原生表现相关研究。
 - [nijinekoyo - Gakuen-idolmaster-ab-decrypt](https://github.com/nijinekoyo/Gakuen-idolmaster-ab-decrypt) — 提供了版本研究思路。
 - [Akisaira](https://github.com/Akisaira) — 援助了早期兼容性研究的智能体调用额度。
