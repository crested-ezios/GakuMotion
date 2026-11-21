# 版本更新维护 SOP：从客户端更新到可审查候选

> **关于本文档与维护方式**
>
> 本文档由 AI Coding Agent 生成，尚未经开发者逐项人工审校。
>
> GakuBurstBridge 涉及游戏原生代码、Burst 内部结构及版本相关的二进制适配。当前开发者无法在不借助 AI Coding Agent 的情况下独立完成全部维护工作。
>
> 建议后续维护者将本文档、当前源码及相关测试记录一并提供给具备代码检索、编译和调试能力的 AI Coding Agent，辅助分析游戏更新后的兼容性变化。
>
> 本仓库主要使用 **Codex + ChatGPT 6.1 Sol** 辅助开发。AI 生成的适配结果仍须经过实际编译、运行和必要的人工验收。

适用：Windows x64 的 GakuBurstBridge 维护者。参考 profile 为 `gakumas-burst-20261008-windows-x64`；其中日期是参考标识，不是当前游戏版本号。读者应能编译 C++ DLL、看懂指针和结构体、使>用 Git；定位改变的入口还需要辨认基本 x64 指令。不了解某条指令时，先保持 fallback，再查证，不以“地址好像接近”代替确认。

这份 SOP 的产物是一份能审查的适配证据和候选 DLL。流程不会自动修复未来的 Unity ABI。常规 GakuMotion 播放与内建弹簧物理不需要维护者处理 Burst；当前默认优先尝试原生后端，缺少或不能使用 Bridge 时会回退。

## 1. 更新后先做什么

1. 保持 GakuMotion 的 fallback 模式，让动作和基本物理继续可用。
2. 保存更新前的伴生 DLL、配置、源代码 commit、客户端文件 hash、上一版报告。不要把商业游戏 DLL 提交进 Git。
3. 从新客户端生成 PE 报告与 Recorder PROBE。先完成只读检查，再决定是否需要改 profile。
4. 如果只有文件 hash 改变且关键 ABI / 入口特征全部相同，仍执行短实机验收，不能只修改 hash。
5. 任一关键布局、调用约定、Actor 身份验证、stream 句柄校验无法确认，保持 fallback 并记录阻断点。

不要先删掉 ABI / 字节门禁试一试。一次没有崩溃不能证明指针正确，SEH 也不能逆转已写坏的内存。

## 2. 最小工具箱

| 工具 | 做什么 | 必须安装吗 |
| --- | --- | --- |
| Python 3 标准库 + 本仓库 `tools/inspect_burst.py` | SHA-256、PE section、导出名、异常目录 / unwind 入口、有限字节与 FF 15 间接调用槽检查 | 需要 Python；不需要 pip 安装 |
| `tools/inspect_metadata.py` | 整理 Recorder probe / metadata index / 手工记录的实测 ABI JSON，并区分 match / mismatch / unknown | 同上 |
| GakuRecorder PROBE 与 RIG | 主线程反射元数据；录制实际服装、发骨 local / world 姿态 | 已有 Recorder 即可 |
| Visual Studio C++ + Windows SDK | 构建 DLL、查看源码、调试自己编译的桥 | 构建需要 |
| Ghidra | 反汇编、函数交叉引用、查看 wrapper 与 Swing 参数来源 | 入口变化时建议；只做只读分析 |
| Visual Studio 的 dumpbin 或 LLVM llvm-objdump | 独立复核 PE 导出 / unwind / 反汇编 | 可选；命令取决于本机安装路径 |
| 文本编辑器 / Git diff | 检查新旧报告、profile 修改范围和文档一致性 | 任意熟悉的工具 |

第一次维护可以先用内置 Python 工具，不必为了列出 PE 入口安装反编译器。工具不加载 DLL，不连接游戏进程，不执行从 JSON 读出的地址。

当前 GakuRecorder 的 `il2cpp_index.py` 以已测元数据版本和布局为前提。它的旧 header / stride 不能直接应用到新的 global-metadata.dat。更新后先用反射 probe 和当前桥的运行时尺寸检查；确实需要重建离线 index 时，先重新验证元数据 header / version / table stride，再使用索引脚本。旧 index 只用于名字检索，不是新客户端字段偏移的证明。

## 3. 先认识三个地址

| 名称 | 含义 | 当前 AVX 例子 |
| --- | --- | --- |
| VA | 某次进程内的实际地址，受加载基址影响 | 日志中 `caller=00007FFB...` |
| RVA | VA 减去模块 image base，适合记录某版 DLL 的位置 | `0xcc9c8` |
| file offset | DLL 文件中的字节位置，由 section 映射求出 | 不能直接把 RVA 当作文件偏移 |

PE 规范明确区分 RVA、VA 和磁盘文件位置。[Microsoft PE 格式说明](https://learn.microsoft.com/en-us/windows/win32/debug/pe-format)

本桥记录的是 `GetDeltaTime` 返回地址：CPU 执行完 CALL 后将返回的位置。当前 AVX 的 CALL 指令在 `0xcc9c2`，长 6 字节，返回地址 `0xcc9c8`。把 CALL RVA 当返回 RVA，后续 `caller-6` 校验会错。

当前 `FF 15 disp32` 使用 RIP 相对间接调用，目标指针槽 RVA 为 `return_rva + signed_disp32`。槽位当前在 `0x1585c0`，位于未映射到磁盘数据的区域，因此只读磁盘报告显示 `slot_file_value=null` 正常。游戏初始化后该槽才指向实际 native icall；磁盘值不能用来证明运行时目标。

## 4. 保存输入指纹和只读 PE 报告

以下命令在仓库根目录运行。先将 `$burstDll` 设为你已保存的待检查 Burst DLL 文件路径，将 `$traceJson`、`$probeJson`、`$metadataIndex` 分别设为同一客户端的 trace、probe 和 metadata index 文件路径。它们是显式输入，不由本仓自动查找或采集。不要修改 profile 地址来迎合报告。

```powershell
python tools/inspect_burst.py --dll $burstDll --caller-rva 0xcc9c8 --scan-slot-rva 0x1585c0 --output build/pe-report.json
```

报告包含：

- 文件 SHA-256、文件大小、image base / size、section 表。
- 导出名与 RVA。当前 DLL 的导出大量是 hash 字符串，不能期待导出名里直接出现 Swing。
- AMD64 `RUNTIME_FUNCTION` 记录数量与已查询 caller 的 begin / end / unwind RVA。
- 已知 SSE / AVX entry prefix、caller 所属 unwind 入口、间接调用槽是否与参考 profile 一致。
- 旧槽位的 FF 15 调用候选。候选还可能是其它 job，工具不会自动把它们命名为 Swing。

退出码 `0`：参考检查匹配；`2`：生成了报告，但至少一项 reference profile 不匹配；`1`：文件 / PE / 参数检查失败。无论退出码为何，都不能据此直接写动画流。工具只做磁盘校验。

导出和 runtime function 数量用于比较版本变化，不是固定门槛；新 job 增减会改变计数。示例中的 caller/slot 来自参考 profile，检查其它版本时使用实际采集的 RVA。

## 5. 收集主线程元数据报告

如需新增客户端运行证据，在目标角色与服装已加载时，按配套 GakuRecorder 的协议使用 `PROBE`；probe 由 Recorder 在主线程执行。本仓不发送采集指令，不把旧端口或旧报告当作新版本证据。

不要从 Burst 回调中自己扫描对象。采集属于需要游戏运行的独立维护步骤，日常源码构建和合成测试不需要执行。



将生成的 `actor_pipeline_report.json`、`official_pipeline_methods.json` 复制到本地工作报告目录。先打开报告检查角色、assembly、生成时间，避免对旧文件做出新客户端的结论。

```powershell
python tools/inspect_metadata.py --probe $probeJson --index $metadataIndex --output build/metadata-report.json
```

当前已知实际类型：

| 类型 | 所在 assembly | 用途 |
| --- | --- | --- |
| `ActorAnimation.CampusActorAnimationJob` | `campus-submodule.Runtime.dll` | 包含 hair / arm NativeArray 与 Swing 子结构 |
| `ActorAnimation.CampusActorAnimationRig` | 同上 | `_job` boxed 对象和 `_animator` 身份校验 |
| `ActorAnimation.ActorAnimationSwingJobSkeleton` | `ActorAnimation.Runtime.dll` | Swing 内部数据与 `_time` |
| `UnityEngine.Animations.AnimationStream` | `UnityEngine.AnimationModule.dll` | stream value type |
| `UnityEngine.Animations.TransformStreamHandle` | 同上 | 绑定 / 解析 Transform 的 value handle |
| `UnityEngine.Animations.AnimatorJobExtensions` | 同上 | `InternalBindStreamTransform` |

名称可能在更新后迁移 assembly。查完整命名空间和类名，记录新 assembly；不能只按 short name 找到一个同名类就接受。

`inspect_metadata` 的 `unknown` 表示缺少对应证据。如果输入 probe 没有 value size，size/header 项会为 unknown，这是资料边界。工具不会根据最后一个字段的位置猜 value size，也不会把 unknown 偷换成通过。

## 6. 校验 Stream / Handle 尺寸与所有读取偏移

本桥初始化通过 `il2cpp_class_value_size` 得到 value type 的实际 native payload 大小。它与 `sizeof` 本地镜像结构分别检查。`TransformStreamHandle` 当前 12 字节；`AnimationStream` 当前 56 字节。不能把普通 boxed 对象总大小拿来比较。

当前 x64 value type 的 `il2cpp_field_get_offset` 值包含 16 字节 boxed 头；传给 job 的 unboxed payload 偏移需减 16。该换算只能在已经确认是 value type 和当前 IL2CPP 对象头布局时使用。`CampusActorAnimationRig` 是引用类型，`_job` / `_animator` 实例字段不减 16。任何新平台或 IL2CPP 对象布局变化都必须重新确认头大小。

| AnimationStream 字段 | unboxed 偏移 | 内容 |
| --- | --- | --- |
| `m_AnimatorBindingsVersion` | 0 | uint32；之后 4 字节对齐空隙 |
| `constant` | 8 | native pointer |
| `input` | 16 | native pointer |
| `output` | 24 | native pointer |
| `workspace` | 32 | native pointer |
| `inputStreamAccessor` | 40 | native pointer，当前 Swing 有效性检查允许它为空 |
| `animationHandleBinder` | 48 | native pointer |

| TransformStreamHandle 字段 | unboxed 偏移 |
| --- | --- |
| `m_AnimatorBindingsVersion` | 0 |
| `handleIndex` | 4 |
| `skeletonIndex` | 8 |

公共 Unity C# 参考可以帮助理解字段职责，但 `master` 可能属于比游戏更新的 Unity 版本。它不能替代新客户端自身的反射结果。[Unity AnimationStream bindings](https://github.com/Unity-Technologies/UnityCsReference/blob/master/Modules/Animation/ScriptBindings/AnimationStream.bindings.cs)

2026-10-09 的一次实际故障由桥的新门禁写错字段名引起：`m_HandleIndex` / `m_SkeletonIndex` 不存在，客户端元数据实际名称为 `handleIndex` / `skeletonIndex`。当时 Burst DLL 哈希仍与旧参考一致；不能仅凭门禁拒绝就断言游戏更新破坏了 Hook。先看实际字段，再区分自己的 profile 错误与客户端变化。

当前 `dump_traces` 同时提供只读 ABI 元数据报告，无需维护者另写探针。显式发送 Recorder `PROBE` 后，查看 `burst_trace.json` 的 `abi_classes`：每个类有实际 `value_size` / `alignment`，每个字段有名称、类型名 / type code、原始 `raw_boxed_offset`、flags 和 static 标志。每类最多 64 字段，超限标记 `fields_truncated=true`。即使初始化被布局门禁拒绝，这份报告仍可生成；它不会为收集数据绕过 gate 或启用原生写入。

同一文件的 `observed` 可以直接交给现有检查工具：

```powershell
python tools/inspect_metadata.py --abi-json $traceJson --output build/new-client-abi-report.json
```

报告刻意不把 `profile_assumptions.boxed_header_size=16` 写成实测的 `observed.boxed_header_size`。各 `*_fields_unboxed` 由原始 offset 减 profile 的头大小得到，换算有条件成立；保留原始值并单独核对对象头。检查工具把缺少独立对象头证据标为 unknown，这是正常行为。缺少可选枚举导出时 `metadata_report_available=false`，不能将空 `abi_classes` 视为布局通过。

核对顺序：

1. 在主线程找到完整类名，确认确实是 value type。
2. 读取 value size 与 alignment，记录 native pointer 大小、对象头布局来源。
3. 枚举字段名称、类型和 offset。比较所有被桥读取的字段，不只比较 `constant` 和 Binder。
4. 核对 Get / Set / Resolve icall 返回地址非空、方法签名与参数传递。缺失指针时禁用伴生库。
5. 反汇编新 Swing 入口，看它如何从 RDX 拷贝 stream：旧 SSE 是 16+16+16+8，旧 AVX 是 32+16+8，总计 56。若新代码只复制 48 或多复制一块，先查布局，不能硬保留 56。
6. 核对新入口有效性分支是否仍检查 version>=2，以及 8/16/24/32/48 非空。代码 gate 必须和实际函数一致，不凭旧版习惯简化。

可将实测值保存为下列格式，用 `--abi-json` 参与报告。此段只是格式范例；不要原样复制并声称自己实测过。`observation_source` 应写具体 probe commit、日志行或调试记录。

```json
{
  "observation_source": "填写本次客户端的实际反射日志来源",
  "observed": {
    "pointer_size": 8,
    "boxed_header_size": 16,
    "stream_value_size": 56,
    "handle_value_size": 12,
    "stream_fields_unboxed": {
      "m_AnimatorBindingsVersion": 0,
      "constant": 8,
      "input": 16,
      "output": 24,
      "workspace": 32,
      "inputStreamAccessor": 40,
      "animationHandleBinder": 48
    },
    "handle_fields_unboxed": {
      "m_AnimatorBindingsVersion": 0,
      "handleIndex": 4,
      "skeletonIndex": 8
    }
  }
}
```

## 7. 校验 Job / Swing 布局与目标 Actor

当前关键字段如下，主线程从 boxed job 使用 `il2cpp_object_unbox` 得到 raw payload 后才能按 unboxed 偏移读取。

| 所属 value type / 字段 | Recorder boxed offset | unboxed offset |
| --- | --- | --- |
| CampusJob `_quartzDriverHairJobBones` | 96 | 80 |
| CampusJob `_quartzDriverHumanoidArmJobBones` | 160 | 144 |
| CampusJob `_swingJobSkeleton` | 224 | 208 (`0xd0`) |
| Swing `_time` | 572 | 556 (`0x22c`) |

这些值有反射和反汇编两种独立证据：wrapper 把 CampusJob 指针加 `0xd0` 后传给 Swing；Swing 用 `[swing+0x22c]` 保存并增加时间。新字段 offset 变动时，必须同时核对 wrapper 和 Swing。单改偏移常量而不确认参数来源是不完整适配。

目标身份确认步骤：

1. Rig 必须是目标 Actor 上实际组件；Rig 的 `_animator` 与本次 GakuMotion binding 的 Animator 相同。
2. `_job` 必须是正确 CampusJob 的 boxed 类型，而非其它 namespace 中的近似名称。
3. 主线程读到 hair 与 arm 两个 NativeArray data pointer，形成目标指纹。
4. 在真实 Swing callback 中由 `swing - swingOffset` 得到 CampusJob 地址，再读取相同两处指针。两者都相等才可认为属于目标。
5. Stream 指针必须来自该次真实 callback，仍通过有效性检查；不把以前 probe 的地址保存下来复用。

若有多个 Actor 并行 job，GetDeltaTime 可能产生很多 trace。它们是入口定位线索，不是目标身份。不要只按“第一个 trace”或当前画面的角色选择写入对象。

## 8. 收集真实 GetDeltaTime caller

先在官方动画仍运行的服装预览 / 主场景中，以 observe-only 模式收集短窗口。MMD inject flag 关闭；尽量关闭重复 status 和控制台洪泛。现有桥通过 GetDeltaTime 的原函数地址安装只读 detour，原 dt 原样返回，不改时间。

新 API v2 的 trace JSON 包含模块 image base / size、caller RVA、`RtlLookupFunctionEntry` 得到的 function begin / end RVA、调用数、dt first / min / max、首次 / 末次工作线程 ID。最多 32 槽；overflow 计数明确输出，不逐调用打印，也不保存 stream 指针供以后使用。

普通维护者不需要修改 C++ 来获取未知版本 caller：

1. 在角色和服装已加载的场景确认已选择 native。显式使用 fallback 不会加载 DLL，无法产生原生 trace。
2. 如果元数据门禁仍通过，等待最多 10 秒发现窗口。新入口未知会拒绝安装、自动降 fallback，但短窗口真实 Burst trace 保留在库内。
3. 向 Recorder 发送上节相同 `PROBE` 命令。Recorder 通过可选 API v2 `dump_traces` 在主线程输出 `<localRoot>/GakuMotion/burst_trace.json`。
4. 检查文件生成时间、`api_version=2`、`schema_version=1`、trace 数量和 `dropped_traces`。空数组不代表“没有物理”：可能初始化 gate 已拒绝、场景无活跃 job、调用入口变化或 probe 未发生。看 `state`、`last_result` 与 Motion 状态说明。
5. 将 JSON 与同一客户端的 DLL 交给 PE 工具，检查每个实际 caller 的 unwind 与 CALL slot；下面命令适用于该文件已经由新 Recorder 导出的情形。

```powershell
python tools/inspect_burst.py --dll $burstDll --trace-json $traceJson --output build/new-client-trace-report.json
```

报告 `native_trace_queries.queries` 中会给 `caller_rva`、`traced_begin_rva`、`traced_end_rva`、磁盘 `unwind`、`unwind_matches` 和解码出的 `call.slot_rva`。未知 profile 时总退出码 2 仍是正常的“需维护”结果；不要因此丢弃已生成的 caller 数据。十六进制 JSON 是 RVA，报告数值可由编辑器显示十六进制；不要拿旧日志的 VA 直接输入新进程。

已知入口成功安装也会停止全局 delta 采集，保留少量发现证据。元数据 / stream ABI 先失败时，库不进行布局猜测，也不会偷偷跳过门禁来产出 trace；先按第 6–7 节修复并审查 ABI，再重启游戏尝试发现。

Windows 的 `RtlLookupFunctionEntry` 在给定 PC 下找 runtime function entry，可获得该版函数入口；找不到时不能用对齐搜索代替。[Microsoft API 说明](https://learn.microsoft.com/en-us/windows/win32/api/winnt/nf-winnt-rtllookupfunctionentry)


旧参考仍可用 PE 工具 `--trace-log` 解析历史 `caller/image/rva/begin` 日志，并检查 VA 减 base、磁盘 unwind entry 是否一致。新日常维护以 `--trace-json` 为准；JSON schema 改变时工具明确拒绝，不静默读空数据。

不同 CPU 可能选择 SSE 或 AVX 变体。只在当前机器观察到 AVX，不代表 SSE 已经实机验收。每个被发布支持的变体至少需要静态 ABI / opcode 审查；实机未覆盖的变体在 release notes 明示。

## 9. 在 Burst 中重新定位 Swing

旧地址失效时，从真实 dt caller 开始，而不是全 DLL 搜索 `55 41 57` 然后安装第一个结果。

1. 对每个活跃 trace 确认模块确为 `lib_burst_generated.dll`。同一个 icall 也会被其它引擎路径调用。
2. 从 PE exception directory 或 runtime `RtlLookupFunctionEntry` 找包含该 caller 的函数 begin / end。
3. 在 Ghidra 导入**本机新版本** DLL，按正确 PE image base 转到 begin RVA。导入后先确认 Machine=AMD64，并将虚拟地址转为 RVA 记录。
4. 找到真实 CALL GetDeltaTime 的位置。旧模式 `ff 15 <disp32>`，检查其 RIP 相对槽；运行时槽值必须等于 hook 前的原始 GetDeltaTime entry，不是 detour 或已释放的 trampoline。
5. 查看前后指令：旧代码先把 stream 从 RDX 拷贝到栈，检查有效性，读 `[RCX+0x22c]` 的旧 `_time`，调用 GetDeltaTime，再将和写回同一 `_time`。这是一条强特征链，不是单一字节魔法。
6. 查看函数的 XREF / callers。Campus wrapper 在处理其它解算后，将 CampusJob 加实际 swing offset，放入 RCX；真实 stream 副本地址放入 RDX，再 CALL 该入口。
7. 确认此路径为 `ProcessAnimation` 而非 RootMotion。旧 wrapper 的第三参数 R8D=1 走 animation 分支，R8D=0 是另一路。
8. 查看对动画 handle 的解析与写入，以及 Swing 数据访问，排除只用 dt 的表情、相机或其它模拟函数。
9. SSE / AVX 两变体分别重复检查。记录 entry / end / CALL / return / slot RVA、prefix、wrapper 与证据片段。
10. 最后才修改 `src/NativeProfile.hpp`，通过独立检查与审查后构建 observe-only 候选。

当前参考：

| 变体 | Swing begin..end RVA | dt CALL / return RVA | Campus wrapper |
| --- | --- | --- | --- |
| SSE | `0x280b0..0x28e48`（end exclusive） | `0x281ce / 0x281d4` | `0x697d0` |
| AVX | `0xcc8e0..0xcd3f3`（end exclusive） | `0xcc9c2 / 0xcc9c8` | `0x10ce00` |

旧 prefix 是 `55 41 57 41 56 56 57 53 48 81 ec 38 03 00 00`。它描述栈帧开头，不能独立证明函数就是 Swing；编译器升级可能改变它。如果 prefix 变了，重新检查 ABI 和代码，再选择新的有限特征。不能把 profile gate 改成“允许任意 prefix”。

### 第一次使用 Ghidra 的最小操作

1. `File → New Project`，选择 Non-Shared、本地目录和版本名；`File → Import File` 导入新 DLL，确认自动识别为 PE / x86-64。
2. 打开到 CodeBrowser 并执行 Auto-Analysis。分析结果只是导航辅助，不作为签名或类型证明。
3. 由 PE image base 加已知 RVA 得到 Listing 地址，定位入口及 dt CALL；与工具输出 bytes 对照。
4. 在入口使用 `References → Show References To` 或 XREF 列查看 callers，逐个检查参数设置。
5. `Window → Bytes` 查看机器字节，`Window → Function Graph` 看有效性分支；不启用字节编辑。
6. 为已经证实的函数加自己的分析标签，保存有限 Listing 片段和对应 RVA，不把反编译器猜测的参数名写成事实。

菜单随工具版本可能变化，可参考 [Ghidra 官方入门材料](https://ghidra.re/ghidra_docs/GhidraClass/Beginner/Introduction_to_Ghidra_Student_Guide.html)。分析界面只是导航辅助，不替代调用约定与实际代码证据。

## 10. 确认调用约定，不靠 C++ typedef 猜

当前 Swing ABI 是 `void(void* swing, void* stream)`：RCX 为 Swing 指针，RDX 为真实 AnimationStream 指针，没有 Il2CppMethodInfo 第三参数。IL2CPP 的 C# `ProcessAnimation` 包装函数签名与这个内部 Burst Swing 函数不同，不能互换 detour typedef。

Win64 的整数 / 指针前四参数通常进入 RCX、RDX、R8、R9，但聚合类型与隐藏返回参数会改变具体传递方式，必须看实际 caller。[Microsoft x64 调用约定](https://learn.microsoft.com/en-us/cpp/build/x64-calling-convention)

确认：caller 如何设置 RCX / RDX、callee 是否按对应地址读取、是否返回值 / 隐藏参数、栈对齐和保存寄存器是否合理、detour 原函数 typedef 是否与之相同。新版本出现按值 stream、额外参数或不同 wrapper 时，应调整 ABI 实现并重新 review；不要仅更新 RVA。

icall / bind 接口同样核对：

- `ResolveInternal`：handle 指针 + stream 指针。
- Get / Set LocalRotation Injected：handle 指针 + stream 指针 + Quaternion 输出 / 输入指针。
- Set LocalPosition Injected：同样使用 Vector3 指针。
- `AnimationStream::GetDeltaTime`：stream 指针返回 float。
- `AnimatorJobExtensions.InternalBindStreamTransform`：确认 3 个 C# 参数及本机 IL2CPP method pointer 的 MethodInfo 传递方式。

这里只列当前形状；未来平台 / Unity 变更时重新验证。非公开 API 即使名字未变，native 调用约定也不能从字符串存在性推出。

## 11. 修改 profile 的审查顺序

保持 `src/NativeProfile.hpp` 为运行时唯一权威；JSON 只读参考与 SOP 表格同步更新。

一份完整变更应包含：新客户端文件 hash、meta / value size / offset 实测、SSE / AVX unwind 与代码片段、GetDeltaTime slot 的真实 callee 证明、参数传递证明、类型 / Actor 身份验证、observe-only 日志、短 MMD 实机记录、已测和未测变体、fallback 失效路径测试。

旧 profile 保留在历史文档，不抹掉旧证据。多个 profile 可以按明确版本 / 特征匹配；不能让未知版静默走宽松分支。桥初始化失败时，错误 detail 应让维护者知道是 size、field、opcode、slot、身份还是 hook 安装失败。

## 12. 构建、只读试运行、有限注入

在独立仓库根目录构建 `build/GakuBurstBridge.dll`，检查导出 `GakuBurstBridge_GetApi` 和函数表 version。部署到宿主插件目录 `GakuLoader/plugins/GakuBurstBridge.dll`；Motion 的 DLL 与伴生 DLL 分别保留已知可回退版本。

按三个阶段测试，不同时改变 retarget 公式和原生 ABI：

1. **只读阶段**：加载库并 configure；不注入。检查尺寸 / offset 门禁、入口 profile、原 dt、目标 matched pass、没有错误计数。主线程位置 / 世界姿态诊断可短时开启。
2. **有限注入阶段**：单 Actor、单服装，使用有权使用的动作做连续短窗口；核对输入年龄、实际骨数、local rotation 与 Hips local position，错误数不增长。观察实际头发 / 长裙是否响应。
3. **生命周期阶段**：暂停、seek 后暂停、继续、Stop、换场景 / 重绑、再次播放，确认旧 snapshot 不带进新 Actor。库丢失或 ABI 拒绝时选择 fallback，主功能继续。

读取回目标 local rotation 误差趋近 0 只证明单次读写成功。世界 Head / Hips 检查提供额外空间证据，视觉验收仍由实际画面确认。这套流程用于未来兼容层变更，不能把旧版本结果扩展成新兼容结论；本次仅恢复文档，没有要求重做用户已通过的 GakuMotion 验收。

## 13. 用 Recorder 比较原生和 fallback

Recorder 已支持实际角色完整 rig，包括服装和发骨，通常无需为每套服装再手工列单独骨骼。先检查本次 schema 是否包含目标链，`rig.status=ok`、没有预算截断、全部四元数有限且规范。

录制前先停到 Recorder IDLE，发送 `RIG ON`（含空格），再 `START <filename>.json`；结束发 `STOP`，等待保存结束且 `save_error` 为空。短窗口优先，避免无限循环录制导致大文件与预算尾部。用相同服装、角色、摄像机、动作和同一连续时间段分别录 fallback、native；另录官方舞台作为参考。

`tools/compare_rig_activity.py` 在 GakuRecorder 仓库中比较 local / world 活动。必须区别：

- **local rotation 活动**：与父骨无关的真实摆动分量，能识别“衣服只是跟身体整体运动”。
- **world rotation 活动**：同时包含身体动作和摆动，不能单独证明物理恢复。
- **adjacent step**：只比较相邻、时钟向前、未冻结、无循环 / seek 的样本；忽略时间不连续对。
- **同一时刻姿态误差**：只有两个动作源相同且 clock 严格对齐时才可比较；FMW 官方和 MMD 编舞不同不能把活动差叫精确误差。

拖动进度后的暂停截图与连续播放使用了不同的物理历史状态。暂停后的物理具有历史状态并逐步收敛，不能当成连续播放同一时刻的唯一目标。连续比较要从相同起点播到目标时刻；seek 行为另外评价。


## 14. CPU / 帧节奏测量怎样才有意义

先关闭逐帧日志、短周期全量 trace 打印和世界诊断。两轨同样开关 Recorder RIG，使用相同录制长度、帧上限与后台负载；每个模式先热身，再采样。分别报告主线程 dispatch、bridge publish、worker bridge 额外部分、原始 Swing、fallback solve 的 call count / 总区间耗时 / 平均 / 最大。

两轨优先各用新进程。初始 fallback 从未加载 / 安装原生桥，才能作为无原生 detour 的 baseline。native 后 Clear 仅清输入，空 snapshot 的 detour 检查仍存在；Shutdown 在任意线程结束本库的当前进程会话，保留只调用原函数的 detour，不卸载 DLL 或 trampoline。不要将同一进程 native→clear / shutdown 后的结果叫“零原生开销”。目标 worker 统计只包含双指纹命中的 Actor，其它 Actor / 空输入的微小检查成本未计入该列。

累计计数的时间窗用后值减前值，不使用整个游戏进程累计值；没有调用时标 N/A，不写 0 μs。主线程 dispatch 已含 publish 时不能再次相加。原生 Swing 官方 baseline 本就可能运行，其耗时不能全部叫“桥额外 CPU”。worker 和 main 并行区间不能简单相加当作游戏单帧 wall time。

当前计时是 QPC 区间经过时间，包含被抢占、等待锁等影响。它比盲看整体帧率更能定位成本，但不是操作系统的精确线程 CPU 时间。若比较需要真正 CPU 样本，应另用标准 profiler，并明确该额外测试是否可用。

大瞬时摆幅的检查顺序：找事件前帧 QPC gap、身体 Head / Hips 输入 step、记录预算、seek / loop、日志打印时刻，再看 Swing 响应是否延迟到下一帧。控制台大量打印可能影响帧节奏，但不能据相关性排除其它原因。先减日志量并重复同窗口；仍存在时再分析输入和 solver，避免用任意 damping 常量掩盖错误 ABI。

## 15. 必须验证的降级路径

| 触发 | 预期 |
| --- | --- |
| 伴生 DLL 不存在 | 不加载，不崩溃；fallback 可播放 |
| DLL 无导出或 C ABI version / size 错 | 停止调用；fallback 可播放 |
| 元数据类 / icall 缺失或 ABI gate 拒绝 | 原生不可用；fallback 可播放 |
| 没有符合 profile 的实际 Swing caller | discovering / pending；不按猜测安装 |
| 不匹配目标 Actor | 跳过该次写入，原 Swing 仍执行 |
| 旧快照 / generation 过期 | 跳过注入，不复用旧帧 |
| Resolve / 读写异常 | 熔断注入，原函数保持调用；主线程切 fallback |
| Stop / 场景变化 | 清空输入与必要缓存；旧 Actor 不继续驱动 |
| runtime 切换 fallback | 单一写入者；DLL 保持进程内，不能热卸载 |

SEH 故障保护是最后一道限制，不能保证任意 native 错误都可无伤恢复；因此地址和 ABI 门禁必须在写入前完成。单个 pass 中“先 Resolve 所有 handles”可以提前拒绝无效句柄；后续 Set 仍可能中途故障，不要宣传全骨骼写入是原子事务。

## 16. 回退和交付

测试出现故障时：切 fallback、Stop MMD、保存简短状态 / perf / probe / hash，正常退出游戏，再恢复上一版 Motion / bridge DLL。正常切换模式不需要 FreeLibrary。已安装钩子的桥固定在进程内，trampoline 留到进程退出。

交付应包含源代码 commit、构建命令、DLL hash、支持 profile、新旧 ABI 差异、实机动作 / 服装 / 时间窗口、录制摘要和 CPU 口径、fallback 路径结果、未覆盖项。不要提交几百 MB 的 raw JSON 或游戏 DLL；本机大录制通过 manifest 的 hash / 文件名关联。

发行时保留 MinHook 许可通知；项目许可与第三方通知见 [LICENSE](../LICENSE) 和 [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md)。本仓构建入口只生成本地产物；对外产品包应另列组件版本、支持范围与下载文件，不能把一轮本地测试 DLL 自动当作发行包。

## 17. 熟练后的时间预算

以下是按流程复杂度的**经验估算，未对未来真实更新做计时验证**；包含有限实机测试的正常投入，不是任何一次更新的 SLA。

| 更新类型 | 熟练维护者估算 | 主要工作 |
| --- | --- | --- |
| 小更新，Burst 与 ABI 都未改变 | 30–60 分钟 | fingerprint、只读 gates、短实机与降级复核 |
| Burst 重编译，入口 RVA / prefix 移动，但 ABI 与语义不变 | 1–3 小时 | 实际 caller、unwind、wrapper、SSE / AVX 审查、改 profile、重建和录制 |
| Job 字段 / stream ABI / icall 签名变化 | 半天至 2 天 | 重新确认布局与调用约定、改 native 实现、更多生命周期测试 |
| Unity / Burst 大升级、job 架构或角色链语义改变 | 数天或无法直接估计 | 原方案可能不再适用；继续 fallback，另开设计评审 |

首次接手需要额外学习时间。只在一台机器有 AVX 实测时，SSE 实机覆盖可能还依赖另一台硬件，这类等待不包含在估算内。没有证据时尽早交付“fallback 正常，原生未支持及原因”，比勉强给出新 RVA 更利于项目长期维护。

## 18. 常见问题定位表

| 现象 | 先检查 | 不要做什么 |
| --- | --- | --- |
| C# ProcessAnimation calls 为 0，但官方衣服能动 | Burst bypass，实际 GetDeltaTime caller 与 native wrapper | 因 calls=0 就认定官方 job 被关闭 |
| dt>0、calls 很多，但 MMD 发 / 裙 local 活动不足 1° | Swing 前身体 input 与 MMD target 是否相同 | 仅改 Animator.enabled / graph flag 就宣称解决 |
| publish rejected | Rig / Animator / boxed type / metadata / handles | 用裸 reinterpret_cast 绕过类型检查 |
| profile mismatch | 新 DLL hash、unwind、opcode、slot、CPU variant | 把全部 gates 删掉 |
| 读取成功但画面穿模 / 大甩动 | 同 pass local / world anchors、父链、卡顿 input catchup、记录时钟 | 只看 Set/Get local equality 就认定完整世界姿态正确 |
| 换装后错角色 / 旧惯性 | Scene / Actor / binding clear，双 NativeArray 指纹 | 缓存旧 raw pointer 跨 Actor 复用 |
| exit 卡在 worker / unload | 任意线程 shutdown，仅取消输入和回调，DLL pin 和 retained trampoline | 在 QuitGuard / DLL detach 调 Unity、MinHook thread suspend 或热 RemoveHook + FreeLibrary |

新 fallback 不依赖 private animation stream、Burst RVA 或 PE 结构，因此游戏内部 job 更新不会要求重新定位它；但任何应用层方案仍依赖主功能取得正确 Transform、骨链和生命周期。模型层级、骨骼命名或宿主 API 变化也应检查，不能承诺所有未来客户端更新绝对无需维护。
