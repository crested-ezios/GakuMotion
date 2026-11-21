# 安装、升级与卸载

Bridge 只在使用实验性原生物理时需要。GakuMotion 默认优先尝试原生后端；缺少 Bridge 或原生后端未就绪时会使用内建弹簧物理。普通动作播放无需本库，预编译版本也无需 Python 或编译工具。

## 获取匹配组件

先按 GakuMotion 对应版本的说明安装 GakuLoader 和 GakuMotion。

请使用发行说明标明的配套组件，或按[开发者指南](DEVELOPMENT.md)构建。构建通过、文件名相同或 ABI v2 相同都不能证明任意 DLL 可以互换，详见[兼容说明](COMPATIBILITY.md)。

## 安装

1. 正常退出游戏。
2. 将配套 `GakuBurstBridge.dll` 放入游戏目录的 `GakuLoader/plugins/`；没有此目录时可创建。
3. 重新启动游戏，在 GakuMotion“设置”页选择实验性原生物理。

```text
游戏目录/
└─ GakuLoader/
   └─ plugins/
      ├─ GakuMotion.dll        动作插件
      └─ GakuBurstBridge.dll   可选伴生库
```

## 升级与回退

正常退出游戏，保存原 DLL 和对应版本说明，再换成新的配套 DLL。回退时同样在退出后恢复保存版本。发行说明要求配套更新 GakuMotion 时应一起更新。

## 卸载

在 GakuMotion 中切回内建物理或关闭物理，正常退出游戏，然后移走 `GakuLoader/plugins/GakuBurstBridge.dll`。重新启动后，原生后端不可用时会回退到内建物理；若选择关闭物理，则保持关闭。无需删除动作、音乐、曲库或 GakuMotion 配置，也不需要移除 Loader 和其它插件。

本仓构建/测试脚本不执行这些安装、替换或卸载操作。
