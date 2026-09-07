# RHI Resource Creation Refactor Plan

Summary: Decouple resource factories from command replay, introduce bounded asynchronous PSO creation and shared requests, and migrate resource consumers with explicit readiness and lifetime contracts.

Last reviewed: 2026-09-07

Status: Active
Completed:

## Current Status

已完成当前 Durin 调用链与本机 UE 5.8 源码对照，选定下述重构方向。
本计划尚未实施；所有阶段验收均未执行。源码能够确认同步等待机制，
不能据此宣称已测得首次使用卡顿或吞吐下降。

当前 `ExecuteFallibleVulkanCreationOperation` 访问全局执行器；启用 RHI
线程时，非 RHI 线程调用会入队并等待工作序号。PSO 缓存查询在该操作
内部，命中也经过同步通道。顶点声明主要复制 CPU 数据，却走相同路径。
在 RHI 线程内直接执行、关闭 RHI 线程时内联执行；这里的 CPU 工作等待
不等同于 GPU idle，也不自动提交尚未提交的录制命令。

已知契约差异：`RendererResourceRecovery.md` 声明每次 graphics PSO 创建
返回独立完整对象，但当前 `FVulkanPipelineManager` 按 key 复用对象。
Stage 0 必须核对调用者和测试，明确底层创建与上层缓存请求的身份语义；
不能将两种行为同时写成既有契约。

## Goal

让 RHI 门面及资源创建服务负责请求、调度和生命周期，Vulkan 工厂负责
具体对象创建及原生错误转换。消除纯 CPU 资源和已完成 PSO 缓存命中的
回放队列往返，将昂贵 PSO 创建移出回放线程，并让消费端显式处理就绪依赖。

范围包括 graphics/compute PSO、顶点声明，以及现有 shader、sampler、
buffer、texture 工厂的执行域审计和迁移。先完成 PSO 路径，再扩展其他资源；
不要求所有原生操作都并行。原生对象创建与上传、布局转换、提交分别建模。

不重写 Core 任务调度器、Render Graph 或材质编译系统，不引入多队列 GPU
调度，不照搬 UE 的全部缓存体系，不承诺首次使用永不等待。

## Selected Design

### Ownership and execution

- RHI 创建服务按设备生命周期拥有请求缓存、任务组、容量和并发预算。
  复用 [CPU Task System](../Runtime/Core/TaskSystem.md) 的结果、依赖、
  取消和 admission；不再创建通用 future 框架。其 API 正由
  [Async Task Framework Refactor](AsyncTaskFrameworkRefactor.md) 演进，
  开始实现前核对已落地接口，不假定该计划全部验收完成。
- 后端声明可直接创建、可后台创建、需要上下文的操作边界。
  Vulkan 工厂不访问 `GCommandListExecutor`，也不隐式等待回放工作。
  上下文相关操作由门面显式调度，禁止再套入统一同步创建包装器。
- 顶点声明等不可变 CPU 数据直接创建。PSO 使用受限后台创建通道；
  支持并发上限为一的兼容策略，但该通道仍独立于回放线程。
  不能用阻塞大量 Core worker 的信号量等待模拟容量控制。
- 上传、资源状态转换、命令池和提交继续由指定上下文拥有。资源对象
  存在、CPU 创建完成和 GPU 数据可消费是不同条件；上传依赖保持显式。

### Requests and cache publication

- 同步创建入口继续返回完整资源或失败。异步入口返回独立请求，具有
  Pending、Ready、Failed、Canceled 状态及完成通知；请求拒绝与已接收
  请求失败分开表示。不得向现有资源接口发布半初始化对象。
- 缓存请求采用规范化描述和设备代际作为身份基础，诊断名称不参与身份。
  命中 Ready 直接取强引用；命中 Pending 共享进行中的创建结果。
  key 相等必须校验完整语义，不能只比较哈希值。
- 未命中时在短临界区内登记 Pending 并预留容量，在锁外创建，再在锁内
  发布结果。任务 admission 失败必须撤销预留并终结所有观察者，不能遗留
  永久 Pending。失败重试遵循所属资源的相关代际策略。
- 请求数、描述字节、缓存驻留量及并发创建数量均有界；批量请求保留每项
  结果和输入顺序。批量 API 本身不承诺资源集合原子发布，Renderer 聚合
  资源由所属 slot 完成事务发布。
- 共享请求的单个观察者取消不应取消其他消费者需要的编译。设备关闭可以
  取消未开始工作；已进入驱动调用的工作必须等待实际返回再释放依赖。

### Concurrency and lifetime

- 审计 PSO、render-pass、descriptor-layout、pipeline-layout 缓存、LRU、
  统计、调试命名和释放路径；禁止持有全局缓存表锁进行昂贵驱动编译。
  容量预留、淘汰与强引用获取必须在一致的并发协议内。
- 驱动 pipeline cache 的创建、编译访问、合并、序列化和销毁统一制定锁
  协议；依据实际 Vulkan 配置及规范确认并发能力，不能机械复制 UE 的读锁。
- VMA 配置不代表整个引擎分配器线程安全；同时核对引擎内存池、统计、
  映射写入、staging 分配、回收和资源状态跟踪。
- 请求拥有描述数据并强引用 shader、顶点声明和布局依赖；清除捕获
  `&Result`、裸 `this` 及借用数组对同步返回的隐含依赖。
- 普通候选创建失败保持可恢复；设备丢失、回放和提交故障保持终止语义，
  不得因 Core task 捕获异常而静默降级成普通 PSO 失败。
- 设备关闭先关闭请求入口，再取消/收拢创建、消费依赖及发布任务，最后
  释放缓存和设备。旧设备/旧 shader 结果不得提交给新代际 owner。
  CPU 引用结束不能替代 GPU 完成后的延迟销毁条件。

### Consumers and ordering

- 在 shader/材质和渲染配置已知时预热，不在每次提交请求后立即等待。
  Renderer 准备阶段检查结果；Pending 不计入失败代际、不重复打印失败。
- 只有身份和绑定兼容的旧完整 payload 可以继续使用；否则使用该渲染特性
  已定义的回退或跳过依赖绘制，不绑定不匹配 PSO。
- 必须消费未完成请求的路径以命令批次依赖表达。依赖在调度层就绪后才
  进入回放，不在唯一回放线程中调用创建任务的 Wait。
- 保留已有录制顺序、提交序号、fence、拒绝重试和有界队列语义。未就绪
  批次不能让后续有序工作随意越过；已接收批次在关闭或依赖失败时必须
  得到可观察的终态，不能造成 fence 永久等待。
- 创建任务不得反向等待依赖它的回放批次。同步兼容入口禁止在会产生
  依赖环的线程/上下文阻塞；inline 模式有明确且可测试的执行策略。

## UE Reference Evidence

参考本机 `E:/Programs/Epic Games/UE_5.8/Engine/Source/Runtime/` 下的源码，
核对日期为 2026-09-07。以下路径和行号是研究时定位信息，不作为仓库链接
或跨版本承诺；不将 UE 源码复制进本仓库。

| Source relative to UE Runtime | Observed behavior | Adoption boundary |
| --- | --- | --- |
| `RHI/Public/DynamicRHI.h:1125` | PSO/vertex declaration 包装直接调用 DynamicRHI | 工厂与调度分开 |
| `RHI/Private/PipelineStateCache.cpp:4138` | 能力和配置控制异步；任务图或预缓存线程池编译 | 后台创建、受限并发、能力门控 |
| `RHI/Private/PipelineStateCache.cpp:4831` | 消费未完成 PSO 时添加完成依赖 | 就绪依赖独立于资源对象 |
| `RHI/Private/RHICommandList.cpp:240` | Android/Mac 延迟 dispatch；其他平台在命令翻译位置等待 | 不宣称 UE 无等待；Durin 选调度层依赖 |
| `RHI/Private/PipelineStateCache.cpp:3808` | 编译任务持有 shader/顶点声明等引用 | 请求必须拥有依赖生命周期 |
| `VulkanRHI/Private/VulkanVertexDeclaration.cpp:60` | 缓存局部加锁后直接构造 | CPU 对象无需回放往返 |
| `VulkanRHI/Private/VulkanPipeline.cpp:2722` | 可配置单线程编译；缓存表锁不覆盖完整创建 | 独立控制并发与锁粒度 |
| `VulkanRHI/Private/VulkanPipeline.cpp:2841` | 发布时二次查询并清理重复候选 | UE 不保证同 key 只编译一次；Durin 选择共享 Pending |
| `VulkanRHI/Private/VulkanBuffer.cpp:571` | buffer 创建与需要 upload context 的初始化分开 | 区分原生创建与命令初始化 |

## Implementation Stages

### Stage 0: Establish contracts and measurement baseline

- [ ] 核对代码、调用者和测试中的 PSO 身份语义，明确底层独立创建与上层
  缓存请求的命名边界；记录前述恢复文档差异的解决决定。
- [ ] 列出所有现有 Vulkan 创建包装器调用者、共享状态、线程断言和
  释放路径，逐项归类直接/后台/上下文执行。
- [ ] 确认 Core 可复用 API、创建预算、驱动 cache 锁协议、inline 策略及
  fence/依赖失败处理方案；剩余设计选择在本阶段定案。
- [ ] 建立相同场景的冷/热 PSO、重复 key、批量资源和材质首次使用基线；
  分别记录请求排队、CPU 准备、原生创建、消费等待、帧耗时及峰值内存。
  记录硬件、驱动、构建配置、并发数、缓存状态和采样次数。
- [ ] 在改动前固定性能比较口径、噪声容忍和可接受回归预算，避免事后调整。

Completion: 执行域清单和接口决定无冲突，测量可复现；未具备硬件证据的
项目保持开放，不能把计数器推断当成实际耗时结果。

### Stage 1: Separate factories and synchronous scheduling

Depends on Stage 0.

- [ ] 将原生错误分类与执行器调度拆开，由 RHI 门面显式选择执行域。
- [ ] 顶点声明直接构造，必要的缓存以局部锁保护；保留失败注入语义。
- [ ] 其余尚未迁移工厂通过门面的兼容路径运行，维持 complete-or-null、
  错误传播、资源引用和执行顺序。
- [ ] 验证顶点声明在非 RHI 线程创建时不增加同步操作和队列提交次数，
  并验证 threaded/inline 模式一致性。

Completion: Vulkan 创建工厂不隐式访问全局执行器；CPU 描述对象零回放往返。

### Stage 2: Make PSO construction independent of replay

Depends on Stage 1.

- [ ] 分离 immutable 创建输入、依赖获取、原生创建和缓存发布。
- [ ] 实现 PSO 及依赖缓存、统计、LRU 和驱动 cache 的并发协议与容量预留。
- [ ] 按资源实际要求替换构造/销毁线程断言；确保候选失败和重复发布的
  回收遵守 GPU 生命周期，持有引用期间不会被淘汰销毁。
- [ ] 使用受控后台创建验证 graphics/compute PSO，包括原生失败、缓存满、
  并发查找/淘汰，以及编译期间回放无依赖工作的进展。

Completion: 原生 PSO 创建可独立于回放执行，相关共享状态和释放路径通过验证。

### Stage 3: Add bounded asynchronous and batch PSO requests

Depends on Stage 2.

- [ ] 实现请求状态、共享 Pending、批量结果、完成通知和同步兼容入口。
- [ ] 接入 Core 任务组、payload/result 预算和有界 admission；实现
  单观察者取消、设备关闭、旧代际结果丢弃及候选错误/设备故障分流。
- [ ] 验证同 key 并发请求仅触发一次原生创建、Ready 命中零回放往返，
  并验证 admission 拒绝、部分批量失败、缓存压力和关闭时所有请求终结。

Completion: 请求可独立返回并批量推进，无悬空捕获、无限排队或永久 Pending。

### Stage 4: Migrate Renderer readiness and command dependencies

Depends on Stage 3.

- [ ] 迁移 static-mesh 材质 PSO 和固定 graphics/compute renderer 的创建
  调用者；建立预热触发点和兼容回退，保持所属 slot 的一次性完整发布。
- [ ] 命令记录持有请求/结果的必要引用，增加调度层就绪依赖和失败终态；
  不在回放线程等待未来的创建任务。
- [ ] 核对 compute 绑定验证、shader/layout 元数据在 Pending 阶段的可用性，
  不为记录命令而提前暴露未完成原生句柄。
- [ ] 验证录制顺序、提交序号、fence、拒绝重试、inline 模式、依赖环，
  以及预热太晚、shader 更新、设备失效和旧候选晚到场景。

Completion: 迁移调用者不再逐资源 submit-and-wait；未就绪资源有明确消费策略，
依赖失败不会执行无效 draw/dispatch 或阻塞 fence 终结。

### Stage 5: Migrate remaining resource creation domains

Depends on Stage 4 and the Stage 0 resource inventory.

- [ ] 按清单迁移 shader、sampler、buffer、texture 创建；原生对象创建与
  upload/state-transition 分离，明确每类批量请求和数据就绪条件。
- [ ] 验证分配器封装、映射范围、staging、统计、状态跟踪及回收约束；
  需要上下文的操作保留显式门面调度，不宣称所有工厂可任意线程调用。
- [ ] 清理旧统一同步包装器及已无调用者的桥接代码；逐项记录必须保留的
  同步入口、原因及线程前置条件。
- [ ] 验证批量创建中部分失败的候选清理、上传可见性、设备关闭和消费者
  生命周期，保持现有 Renderer/Render Graph 事务边界。

Completion: 清单内每种工厂都有已验证的执行域，剩余同步均为显式契约。

### Stage 6: Qualify performance and publish contracts

Depends on Stages 0–5.

- [ ] 重跑基线场景，比较 median/p95、同步往返次数、原生创建次数、
  回放进展、首次消费等待和峰值内存；记录串行后台及选定并发配置。
- [ ] 使用可控慢创建夹具验证 CPU 准備/原生创建确实不占用回放线程；
  该确定性测试不能替代实际驱动性能测量。
- [ ] 完成故障/并发/退出测试及实际 Vulkan 场景验证，结果满足 Stage 0
  预算；其他受影响后端编译及必要契约验证无回归。
- [ ] 将已实现契约写入所属 Runtime 文档，修正 PSO 身份描述；更新本计划
  状态、证据和未完成项，所有 gate 通过后再标记 Completed。

Completion: 正确性和预先确定的性能门槛均有证据，长期契约不依赖本计划正文。

## Validation and Handoff

构建或运行前读取 [Build and Run workflow](../Agents/BuildAndRun.md)，选择
原生测试前读取 [Testing workflow](../Agents/Testing.md)，不在本计划固定
易过期的构建命令。纯文档创建仅运行文档及计划验证，不声称完成运行时验证。

每阶段记录实际变更、执行配置、测试/测量回执和限制；成功验证后按仓库规则
提交，并使用本计划路径与精确阶段标题作为 Plan/Stage trailers。

## Related Code

- `Engine/Source/Runtime/RHI/Public/DynamicRHI.h`
- `Engine/Source/Runtime/RHI/Public/RHIResources.h`
- `Engine/Source/Runtime/RHI/Public/RHICommandList.h`
- `Engine/Source/Runtime/RHI/Private/RHICommandList.cpp`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanRHIPrivate.cpp`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanResources.cpp`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanPipeline.cpp`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanMemory.cpp`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanBuffer.cpp`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanTexture.cpp`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanShader.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/StaticMeshRenderer.cpp`
- [Renderer resource recovery](../Runtime/Rendering/RendererResourceRecovery.md)
- [Render resource lifecycle](../Runtime/Rendering/RenderResourceLifecycle.md)
