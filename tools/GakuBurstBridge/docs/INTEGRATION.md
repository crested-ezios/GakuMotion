# GakuMotion 与伴生库的集成契约

> 本文档由机器生成，尚未经过开发者完整人工审阅。

## 多演员上下文（ABI v3）

`GakuBurstBridge_GetApi(3, GbbApiV3*)` 返回新的上下文函数表。原 ABI v2 的表大小、结构与全局单演员语义保留；v3 的 Config、Frame、Status、Perf 载荷仍使用版本 2。宿主必须同时校验表的 size/version 和所有函数指针。

v3 按需创建非零 `GbbContext`，不再以三个为固定上限；实际容量受内存和游戏性能约束。token 只表示本进程中的一次演员生命周期；销毁后不能复用，旧 token 返回 `GBB_E_CONTEXT`，容量满返回 `GBB_E_CAPACITY`。首次有效 Publish 固定其 actor；换演员应销毁并新建，Animator/rig 重绑则递增该上下文 generation。创建、销毁、发布、清理保持 Unity 主线程限制，状态与性能查询只读。只有一个模块级 Shutdown，任意线程调用后取消所有上下文。

每个上下文保留独立快照、stream handle、generation、故障、诊断和 Quartz/Swing 配对状态。实际 job 的 hair/arm 双指纹必须唯一命中一个上下文，否则原样转发；发布与其他上下文指纹冲突会拒绝并取消本次上下文，不清除另一个演员。原生 worker 使用地址稳定的 runtime 和快照副本，不访问 Unity 元数据、不创建演员上下文，也不保存 AnimationStream 指针；线程局部配对状态按需扩展。销毁取消 generation，但保留 runtime 内存到进程退出，因此已在执行的 worker 不会访问已释放上下文。整模块 shutdown 门禁同时阻止各上下文开始新的 stream 写入。

`clear_context(token, flags)` 只清该演员；`get_context_status` 与 `get_context_perf` 不会返回别人的最近一次成功。宿主应检查 token 对应 actor、generation、源帧与数据时效，按演员单独降级 fallback。失败的 clear/destroy 不能被宿主当作“没有待取消写入”：必须保留 token 和未确认状态，直到成功取消或场景销毁处理。BeforeQuartz 与 sleeve 诊断仍为显式开关，默认关闭。旧 v2 桥可仅由固定主演员使用，其他演员的 fallback 状态须明确显示，不能将其标成多人原生成功。

离线测试覆盖超过三个上下文的创建和回收，以及三路模拟 Publish、实际 job 路由、同 worker 的 A/B Quartz 配对交错、单演员故障/清理、token 失效与全局退出。尚无当前客户端三人场景的加载、原生 job 或视觉验收；三人指纹唯一性及发型/服装呈现仍须实机验证。

本文以 `include/GakuBurstBridge.h` 中的 v2 与 v3 接口为依据。`src/NativeProfile.hpp` 是运行时支持门禁的权威实现；`profiles/supported-build-20261008.json` 是只读工具的参考记录，修改后者不会修改 DLL。

## 所有权与线程

| 职责 | GakuMotion | GakuBurstBridge |
| --- | --- | --- |
| 配置、动作采样、重定向、正常骨骼写入 | 主线程 | 无 |
| 捕获实际写出的 local 姿态 | 主线程 | 接收副本 |
| IL2CPP 元数据查找、组件与 Animator 验证、stream handle 绑定 | 提供 Actor / Animator / Transform 指针 | `configure` / `publish` 中主线程处理 |
| Burst / icall 地址发现与钩子 | 无 | 原生实现独立维护 |
| Swing 前真实 stream 写入 | 无 | 官方 job 所在工作线程，使用已复制快照 |
| 原生 Swing 执行 | 无 | 原函数原样调用 |
| 轻量通用物理 | 主线程，在 MMD 身体 / 辅助骨写入后运行 | 无 |
| CPU 统计与故障状态 | 采样汇总到统一诊断 | 固定大小累计计数，无逐帧打印 |

`configure`、`publish`、`clear` 和 `dump_traces` 只能在 Unity 主线程调用。元数据、组件、Transform 的 IL2CPP/Unity 访问不能移入原生工作线程。工作线程只使用真实回调参数、已经绑定的动画句柄和快照副本。不得在持有共享快照 mutex 时访问 Unity，否则可能与等待 job 完成形成互锁。

`shutdown` 可在任意线程调用，包括宿主 QuitGuard 工作线程和 DLL detach 路径。它先关闭观察 / 注入，再取消输入 generation、清空快照和日志回调，并将状态置为 shutdown；重复调用成功且不重复递增 generation。此路径不调用 Unity、宿主日志或 MinHook，也不等待原生 job。已安装的 detour 保留为直接调用原函数的惰性转发层，DLL 和 trampoline 留到进程退出。因此不会因卸载线程并非 Unity 主线程而返回 `GBB_E_THREAD` 并留下有效注入。

shutdown 是终止会话，不能在同一进程重新 configure / publish / clear。它不能原子撤回已经开始的骨骼写入，也不能终止已经借出并开始执行的日志回调。宿主应固定回调代码所在模块，并保持 callback context 存活，直至已在执行的回调返回；GakuMotion 的接入在 configure 前固定自己的 DLL，日志 context 为 null。不要以 `shutdown` 返回为依据立即释放可能仍被外部回调引用的代码或内存。状态更新和回调指针 / context 对使用同一个 mutex，停止后的旧结果不能覆盖 shutdown 状态。

## 加载协议

GakuMotion 在需要原生物理时，从宿主提供的 `pluginsRoot/GakuBurstBridge.dll` 加载配套伴生库；标准安装路径为游戏目录下的 `GakuLoader/plugins/GakuBurstBridge.dll`。GakuLoader 扫描插件时跳过这个辅助 DLL，由 GakuMotion 按需调用；它不是独立的 Loader 插件。加载使用受控 DLL 搜索标志，不搜索当前工作目录或环境 PATH。历史版本曾使用 `gakumas-local/GakuMotion/GakuBurstBridge.dll`，新包不使用该路径。

先解析唯一导出 `GakuBurstBridge_GetApi`，再请求 `GBB_API_VERSION=2`。每个调用方拥有的结构体设置 `size=sizeof(结构体)`、`version=GBB_API_VERSION`，reserved 字段清零。函数表版本、结构大小和所有必需函数指针校验通过后，才能调用。加载失败、导出缺失、版本拒绝、configure 失败都返回 fallback；不得继续调用空指针或不兼容函数表。

`GbbResult` 的 `GBB_PENDING` 表示尚未从活跃官方动画 job 找到支持的入口。等待状态不是注入成功。`GBB_E_*` 表示明确失败；主功能继续播放，并在 UI / 低频状态里说明实际使用 fallback。不能每帧反复重试 LoadLibrary 或打印同一条失败。

## 姿态提交协议

1. 正常 MMD 重定向与 override 写入结束后，捕获真实 Transform 输出。
2. 设置 `GbbFrame.actor`、`animator`、`unity_frame`、`clip_time`；`bone_count` 不超过 64。
3. 每个 `GbbBonePose` 包含 Transform、Humanoid ID、local rotation 与 local position。普通播放无需开启世界姿态诊断。
4. Native 注入需显式设置 `GBB_FRAME_INJECT`。只读诊断时不设置此 flag。
5. `publish` 返回错误、`get_status` 显示 faulted / unavailable，客户端选择 fallback 并清空输入。

原生库同时匹配 job 中 hair 和 arm 两个 NativeArray 数据指针来识别目标 Actor；“发现某个 Swing 流”“Binder 地址一样”“角色看起来是同一人”都不足以代替双重身份验证。快照的年龄上限、generation、有限 quaternion 和可解析 handle 是每次实际写入的条件。

输入来自上一轮已完成的主线程输出，因此当前方案通常有一帧延迟。读取回同一 local quaternion 能证明该次读写对应，不能单独证明整个 stream 世界空间与最终画面完全相同。世界 Head / Hips 诊断用于调查，常规发布关闭，避免额外读写开销。

## BeforeQuartz 同源输入实验（默认关闭）

ABI v2 结构大小不变；`GbbFrame.flags` 新增显式 `GBB_FRAME_BEFORE_QUARTZ=4` 与独立 `GBB_FRAME_SLEEVE_DIAGNOSTICS=8`，默认均不设置。flag 4 在已验证的真实 Quartz 入口前写入 MMD local Transform 姿态，并在同一 job 的 Swing 前再次使用该副本；不保留跨回调 stream 指针，不手动重跑阶段。配对检查 caller、Actor 双指纹、job、线程、generation、时效与消费顺序；首个完整配对前返回 pending，2 秒未配对则要求 fallback。切换 flag、Actor、绑定或实验时间线边界使旧配对失效。

运行中卡顿导致的过期包、缺失配对和旧 generation 只跳过当次注入，下一次完整有效配对可自然恢复，不解除已有真实故障。过期的残留 Quartz 配对在下一次有效 Quartz 前丢弃；未过期的重复、阶段/身份/调用入口异常和原生读写失败仍保持熔断。缺失配对不借用其它线程的输入，不重复消费；初次启用 2 秒无法配对的门禁保留。`before_quartz_experiment.rejected` 保留各类拒绝计数，过期跳过同时进入 `GbbPerf.expired_skips`。

清除 flag 4 恢复原 Swing-only 路径；宿主切换时应 `clear`，若已熔断，在明确边界用 `GBB_CLEAR_RESET_FAULT` 恢复。Quartz 可选入口验证失败不会把原 OFF 路径永久标为不支持。flag 8 只保存自然 HumanoidSleeve `InternalGetMuscle_Injected` 的实际返回值，最多 8192 条；不写 muscle，不额外调用 reader 或动画求值。`dump_traces` 增加 `before_quartz_experiment` 配对计数及 `sleeve_muscle_probe` 样本，跨 `clear` 保留已捕获证据。首次启用后，钩子仍留到进程退出。


## 两种求解器只能有一个写入者

Native 成功时，fallback 不写同一批头发 / 裙骨。Native 未就绪或故障时，先清空 stream 输入，再让 fallback 接管。切换轨道、Stop、Actor 丢失、服装重绑、场景变化或 seek 时，重置对应状态，避免把旧角色 / 旧时间的惯性带进新姿态。

`clear(GBB_CLEAR_RESET_FAULT)` 可以在主线程的明确生命周期边界解除熔断；不要每帧自动解除同一错误。generation 能阻止旧结果重新发布，但不构成“已在执行的写入全部原子取消”的屏障，不应在此协议上承诺热卸载。

## 日志与 CPU 口径

库只在初始化或状态变化时通过 main-thread `GbbLogFn` 回调记录有限日志；不在 worker 每次回调打印。`get_status` / `get_perf` 为读接口。`GBB_FRAME_WORLD_DIAGNOSTICS` 默认关闭，维护时按短窗口启用。

成功发现并安装支持的 Swing 入口后，库停用全局 GetDeltaTime 发现 hook，保留其 trampoline 到进程结束，避免常规播放继续承担全局 trace / 锁开销。发现期当前有 10 秒超时；不支持的入口保持 unavailable，初始化门禁失败，本进程不自动重试。

API v2 增加 `dump_traces(const wchar_t* absolute_file)`，只在主线程把已捕获的固定 32 槽 Burst caller / unwind / dt 计数写成 JSON；不在 worker 打印、不主动执行 job。GakuRecorder 的 PROBE 可选调用该接口，输出 `<localRoot>/GakuMotion/burst_trace.json`。已知入口安装或 10 秒未知入口超时后停止采集，保留数据供维护者读取。未知 ABI 被初始化门禁拒绝时不会开始采集；先修复经过验证的 ABI gate，再收集真实 callback。

同一显式导出还在主线程读取 Stream、Handle、CampusJob、Swing 的 `value_size` / `alignment` 和每类最多 64 项字段元数据，输出 `abi_classes` 与检查工具可读的 `observed`。布局拒绝不会阻止此只读报告；缺少可选枚举导出时标记为 unavailable。字段保留原始 boxed offset、type name / type code、static flags，unboxed 对照值明确依赖 profile 的 boxed-header 假设。报告不把假设头大小当成独立实测，也不放宽运行时字段 gate。

`GbbPerf` 的计数为累计纳秒。比较时间窗使用两次快照之差，再除以调用数：

| 字段 | 实际测量内容 | 解释限制 |
| --- | --- | --- |
| publish_* | 伴生库主线程接收、校验、绑定 / 复制输入 | 不是整个 GakuMotion 主线程耗时 |
| native_* | 工作线程 bridge 的验证、stream 读写与诊断 | 不含原始 Swing 求解器 |
| solver_* | 被钩住目标函数的原始 Swing 用时 | 通常官方 baseline 原本就有此成本；不可全部记为额外成本 |
| GakuMotion main dispatch | 主线程二级物理调度，包括 bridge publish 或 fallback dispatch | 已包含 publish 时不能再次相加 |
| GakuMotion fallback solve | 应用层 fallback 的求解与写入 | 单独报告，不能直接与 worker 并行 wall time 相加 |

QueryPerformanceCounter 的区间是执行经过时间，不是操作系统线程 CPU time；被抢占、锁等待或抢占后的恢复会进入记录值。报告要写“区间耗时”，不要将它等同于整机 CPU 百分比。录制 rig / 控制台打印也可能改变帧节奏，性能对照必须同样开关 Recorder 和诊断，不把相关性写成已证明的唯一因果。

worker 统计只累计双指纹命中的目标 Actor；其它角色、空 snapshot 的微小 detour 检查成本不在 `native_*` 内。比较显式选择的 fallback 与 native 的完整开销应各用新进程：fallback 进程从未加载伴生库；native 之后 `clear` 虽然不再写入，已安装 detour 仍存在。`shutdown` 结束当前进程内库会话，保留仅调用原函数的 detour；不要以“先 native 再 clear / shutdown”的同一进程作为零原生开销 baseline。
