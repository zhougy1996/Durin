# Asset Package Reload Plan

Summary: Add transactional package reload and live reference replacement, then route editor discard through saved-content restoration.

Last reviewed: 2026-09-07

Status: Active
Completed:

## Current Status

Stage 0 已完成 CPU 内容级回归、源码边界审计及接口冻结。独立的
`AssetDiscardCharacterizationTests` 在 Windows Debug 捕获 Texture2D 和
VolumeTexture 放弃后云组件仍引用修改后的源数据，以及随后保存污染磁盘的错误。
该测试通过表示旧错误被成功捕获，不表示 Reload 已实现；没有 GPU 画面验证证据。
生产 Discard 行为尚未修改，Stage 1 是下一个待执行阶段。

审计确认现有 linker skeleton 已进入 DPackage 注册表和 GDObjectArray，不能直接
作为同路径隔离图；GC 枚举把 TObjectPtr 转为临时指针，不能承担引用回写；
Loose BulkData 每次按路径重新打开文件，注册候选资源还会退休旧资源。这些缺口
必须在 Stage 1/2 先补齐，保留选定的整批原子失败契约。具体冻结边界见 Stage 0
handoff；尚未迁移 Async Task Framework 的生产试点，也未绕过其验收门槛。

## Goal

让“放弃修改”真正恢复当前磁盘已保存的包内容，使场景、编辑器和之后的保存都使用
恢复后的资源。恢复失败时保持原包内容、引用、脏状态及可用历史，不关闭文档，
不报告成功。补齐的能力必须可以服务于显式 Reload，而非仅供两个纹理窗口使用。

## Scope

- 支持已保存的 authored DAST v9 非 World 资源包，包括多顶层资源、内嵌子对象、
  内部循环引用及需要一起替换的包集合；首批生产接入 Texture2D、VolumeTexture、
  Material/MaterialInstance，并审计其他文档调用者。
- 重用现有 linker、未发布对象图、BulkData、注册表、编译与渲染资源契约；
  不增加第二套包解析器、驻留表或文件格式，不写回磁盘，不触发源文件 reimport。
- World/Level 热替换、类型定义热重载、自动文件监控、版本控制回退和任意插件
  原生指针自动修复不在本次范围。存在未接入的消费者时必须有明确拒绝路径。
- 从未保存的包无磁盘基线：本计划的 Reload 明确拒绝，保留脏状态。删除这种包
  属于显式资源删除/DiscardPackage 流程，不得借重载静默删除仍被引用的资源。

## Selected Architecture

### Ownership and reference semantics

| Owner | Responsibility |
| --- | --- |
| CoreDObject | 受控对象注册替换、可写引用遍历、类型校验、旧新对象映射、GC 保活与旧图退休原语；不读文件、不依赖编辑器 |
| Engine | 读取并验证磁盘闭包，准备未发布图，解析依赖，协调资源族准备与提交，提供结构化 Reload 结果和生命周期事件 |
| AssetRegistry | 继续拥有磁盘元数据投影；重载不伪造磁盘保存或修改引用者的磁盘依赖 |
| AssetTools | 编辑器包选择、范围展示、冲突策略和恢复请求；不实现对象复制或引用扫描 |
| DurinEd | 文档状态机、编辑会话收束、事务预检与提交后失效、错误展示、旧新编辑对象重绑定 |
| 资源族及渲染消费者 | 候选对象运行时准备、编译代次隔离、缓存更新和按既有线程契约切换/退休渲染资源 |

对象内存地址不作为重载后的身份承诺。对旧包中的对象以包身份和相对 Outer 路径
建立映射，并验证目标类型可赋值性；持久化对象路径保持既有契约。禁止按数组位置
或裸地址猜测对应对象，不新增永久对象 ID 格式。

CoreDObject 的可写引用遍历需覆盖反射属性、结构体、固定/动态数组及 Map。
Map 键变化必须重建索引、校验碰撞并预留回滚能力。现有 GC 保活枚举不能直接假定
支持引用改写。原生强引用/缓存由注册参与者提供准备、提交和退出钩子；原生弱引用
由其所有者重绑定，普通弱句柄允许失效但不得错误命中新对象。软路径保持不变，
运行时软引用缓存通过既有 epoch 机制失效并重新解析。

当前内存新增、磁盘没有的对象允许随旧图退休，但若图外仍有必须保留的强引用且
没有替代目标，则在提交前拒绝整个请求。类型不兼容同样拒绝；不得静默写 null。
磁盘存在而内存缺失的导出按正常加载创建。跨包循环按替换集合准备所有 skeleton，
集合内引用指向候选图，集合外依赖保持当前驻留对象；不得顺带覆盖其他脏包。

### Saved baseline and admission

“恢复保存版本”在此明确为恢复请求所读取的当前磁盘闭包，而不是打开窗口时的内存
快照或 undo 游标。磁盘被外部修改时展示版本变化，允许用户明确重试最新版本；
读取后到提交前指纹改变则返回 stale，不混用两个版本。

读取并保有一致的 main/bulk 数据及所需源载荷，按现有完整闭包验证规则处理缺失、
损坏和版本不兼容；惰性 BulkData 不得在提交后悄悄读取另一代文件。保存投影仍处于
ContentCommittedProjectionPending 或路径被 fence 时，先完成已有协调流程再重载。

请求按包去重；同包多个文档明确共同受影响。显式批次具有全有或全无的内存提交
语义，设置内存/包数量预算并在准备阶段拒绝超限，不宣称项目级无限批次原子性。
读文件和纯数据验证可在 worker 上执行；DObject 操作、引用扫描和提交均在
GameThread 上执行。准备期间封锁目标包的编辑、保存、重入重载和相关历史操作；
提交前重新验证包代次与引用者集合，阻止准备期间新增引用漏出扫描结果。

### State machine and failure boundary

```text
Requested -> Preflight -> Quiesce -> ReadAndPrepare -> ReadyToCommit
          -> CommitRegistrationAndReferences -> Publish -> RetireOldGraph -> Succeeded
任一提交前失败/取消 -> AbortPreparedGraph -> 恢复准入 -> Failed/Cancelled
```

- 先保存编辑会话的实际修改到当前内存状态并完成事务，不以丢掉尚未落入事务的
  输入作为恢复。Recording/Undoing/Redoing/异步事务未终止时返回 Busy 或延后执行。
- Quiesce 阻止目标旧图新编译请求，取消并等待所选对象任务进入终态；取消仅是
  advisory，不能代替 selected finish。不得在 GameThread 阻塞等待需要它执行的
  continuation，也不得通过 finish-all 等待无关资源。失败后重新开放旧图工作，
  必要时重新请求被取消的原状态构建，保持原有可用资源。
- 准备图、依赖、引用写入计划、事务失效计划、原生参与者及必要运行时产品。
  PostLoad 的候选图副作用需纳入隔离，不能提前进入全局渲染/编辑器可见列表。
- 所有可预期失败（I/O、校验、构建、预算、参与者拒绝、资源准备）放在提交前。
  Commit 在禁止观察半状态的边界执行；预分配写入所需存储，提交回调不得失败、
  重入或任意广播。若某原语无法提供此保证，必须先实现可验证补偿并通过故障注入，
  不能将部分提交包装成普通失败。
- 发布后才通知观察者、失效相关历史并建立新包保存检查点。旧图保持保活直到所有
  线程消费者脱离；GPU/C++ 资源按既有 deferred cleanup 规则退休，不能立即 delete。
- Succeeded 表示 authored 内容、引用及恢复所需资源切换已被接受；若渲染提交需要
  完成回执，则保持 Pending 至回执完成。后续设备丢失遵循既有资源恢复契约，不把
  未保存内容重新发布。资源准备失败保留旧图且返回失败。

### Editor and history contract

共享 Discard 只调用包恢复服务，不再接受可省略的 void “恢复”回调。异步恢复使用
明确的 Pending/Succeeded/Failed/Cancelled 结果；关闭确认在 Pending 和失败时保留，
只在成功后重绑定同包所有文档并完成原关闭请求。窗口销毁或模块退出必须分离 UI
观察者并安全终止/收束已接受的操作，不能通过捕获 Widget 裸指针管理完成通知。

成功时按旧图目标、包参与者和历史载荷中引用枚举失效受影响的完整事务，不能只改
事务对象指针后让旧值重新覆盖恢复内容。跨包事务完整退休，但不撤销另一包已经
应用的修改、不清理另一包脏标记；必要时使其保存检查点失效。无关事务保留。
失效方案在提交前预检并预留资源，执行点在引用提交之后、对外发布成功之前。
失败不调用 ForgetPackage/ClearDirty。其他引用者仅因指针替换不应变脏。

## Implementation Stages

### Stage 0: Freeze reload boundaries and regression fixtures

- [x] 复现两个纹理编辑器的丢弃后场景仍变化、再次保存污染磁盘问题，建立失败断言。
- [x] 审计 live 注册、未发布 linker graph、引用遍历、原生保活/弱缓存、事务载荷、
  TextureReference 与场景消费者；列出首批参与者和不支持资源族的确定拒绝规则。
- [x] 固定可写引用 API、注册切换原语、Reload 结果、资源准备回执及预算，记录具体
  头文件归属；确认 PostLoad 可隔离性和磁盘闭包稳定读取的缺口。
- [x] 固定故障注入位置与 GameThread/RenderThread 提交边界。上述选定原子契约若
  无法实现，先更新决策和理由再进入 Stage 1，禁止静默降为部分成功。

完成条件：接口和参与者清单可直接指导实现；回归测试在旧实现上捕获内容错误，
而不是只检查 dirty 标记。仅此阶段允许冻结尚未命名的内部接口。

#### Stage 0 handoff: audited gaps and frozen interfaces

以下名称是后续实现的冻结设计，不是已经导出的生产 API。路径以
`Engine/Source/` 为根；测试只证明既有缺陷，不能用于接受 Stage 1–5。

| Source boundary | Observed behavior and required change |
| --- | --- |
| `Runtime/CoreDObject/Private/DObject/Package.cpp` | `InitializeAssetPackage` 立即注册，重复路径断言；不能先卸载旧包再试读候选图。新建候选初始化方式只设置包身份，注册切换原地替换现有表项的值，禁止 erase/emplace 造成提交分配。 |
| `Runtime/CoreDObject/Public/DObject/DObjectArray.h` | 所有对象共用句柄/Outer 索引；为同一对象系统增加候选图可见性，LiveOnly 查询排除候选，GC 仍可保活。旧句柄保持旧代次直到退休，禁止交换槽中指针来模拟重绑定。 |
| `Runtime/CoreDObject/Private/DObject/GCReferenceSchema.cpp` | Object 操作只把局部 `DObject*` 交给 collector；Map 使用 const traversal。`ForEachObjectReference` 不调用对象原生 `AddReferencedObjects`。不能直接把 collector 当 writable visitor。 |
| `Runtime/CoreDObject/Public/DObject/ContainerOps.h` | 固定/动态容器、结构体需递归准备；现有 STL adapter 仅在 swap noexcept 时提供 TransactionalCommit。Map 在 detached storage 重建键和值，InsertCopy 的 DuplicateKey 在准备期失败；只接受可证明无失败的提交 adapter。 |
| `Runtime/CoreDObject/Private/DObject/StrongObjectPtr.cpp` | 原生强指针注册的是句柄计数，并非可写 owner slot。需要 owner 参与者逐个重绑定并核对外部计数；没有参与者的额外强持有必须拒绝，不能通过改句柄解析重定向普通弱指针。 |
| `Runtime/Engine/Private/Asset/AssetPackageLinkerLoader.cpp` | skeleton 使用 NewObject/ForceRegistration；立即加载依赖、恢复 ledger、调用 PostLoad，失败 CollectGarbage 并按快照释放依赖。抽取显式 prepare 上下文和批次内解析器，候选取消只清理自身拥有的对象和依赖。 |
| `Runtime/Engine/Private/Asset/PackageResource.cpp` | Loose range 读取重新打开 SegmentPath；RegisterLoosePackage 替换 manager slot 并退休 previous。候选必须拥有验证过的不可变闭包资源，准备期不得调用这个发布入口；提交时再替换既有资源 slot。 |
| `Runtime/Engine/Private/Texture/Texture2D.cpp`, `VolumeTexture.cpp` | PostLoad 同步构建并安装产品；DTexture 构造拥有独立 TextureReference，UpdateResource 可排队渲染初始化。候选反序列化/迁移与运行时产品准备需拆开，不能执行普通 PostLoad 后宣称没有副作用。 |
| `Editor/DurinEd/Private/Editor/Transactor.cpp` | ReferencesPackage 只扫描 PackageTransitions；ForgetPackage 非 Idle 时静默返回，且不能发现仅存在于历史载荷的引用。使用 collector 只读枚举完整事务载荷，再准备完整事务退休计划。 |

CoreDObject 的 `Public/DObject/ObjectGraphReplacement.h` 拥有
`FObjectReplacementMap`、`FObjectReferenceReplacementPlan`、
`IObjectReplacementParticipant`、`FObjectGraphReplacement`。
映射键为包身份与相对 Outer 路径，包本身也入表；使用实际 FName/包路径比较语义，
拒绝重复路径、目标不兼容、图外必需引用没有目标。`Prepare` 返回诊断且不修改
live slot，`Validate` 在冻结边界重查源/目标句柄、属性值、容器内容和原生参与者
revision；`Commit() noexcept` 仅消费已预留的写入和注册切换，`Abort() noexcept`
释放候选与重新开放旧图，`Retire` 等待消费者回执。结构体内自定义 collector
必须有参与者或明确拒绝；不把 native collector 的临时 slot 留作提交地址。

`Public/DObject/Package.h` 增加 `InitializePreparedAssetPackage` 和仅由
replacement coordinator 调用的 `CommitPreparedPackageRegistration`；
`Public/DObject/DObjectArray.h` 拥有候选可见性与对象集合 revision。
`Public/DObject/SoftObjectPtr.h` 继续以 `InvalidateSoftObjectCaches` 统一失效，
不新增软引用 epoch。提交全程在 GameThread，冻结期间不得 pump 任意回调或 GC；
编辑器有跨帧等待时保持包级 lease，最终扫描到提交之间不得再让出 GameThread。
raw pointer 的任意插件自动发现不在范围内，但参与者注册与模块卸载必须受 lease
约束；可枚举但未声明可替换的 native owner 统一 Unsupported。

Engine 的 `Public/Asset/PackageReload.h` 拥有 `FPackageReloadRequest`、
`FPackageReloadBudget`、`FPackageReloadResult`、`FPackageReloadOperation` 和
`ReloadPackages`。结果状态固定为 Pending/Succeeded/Failed/Cancelled；失败码区分
Unsupported、Unsaved、Busy、Stale、BudgetExceeded、IoError、InvalidClosure、
IncompatibleGraph、UnmappedReference、ParticipantRejected、ResourcePreparationFailed。
诊断携带包路径、对象路径、阶段、消息；拒绝不得伪造成功或部分成功。
默认每批最多 16 包、65,536 对象、1,048,576 引用 slot、512 MiB retained CPU
数据和 256 MiB 候选 GPU 存储；闭包、解压后的源数据、写入计划、runtime 产品与
候选资源都计入预算并使用溢出检查，未知 GPU 大小须在运行时准备前拒绝。
旧 live 图不计入候选预算，但其保活期限必须记录；这些是保守准入上限，不是性能承诺。

`Private/Asset/AssetPackageLinker.h` 拥有 `FPreparedPackageGraph` 与
`PreparePackageGraphs`，重用同一 parser/codec/schema；先准备批次全部 skeleton，
再解析集合内引用，集合外只复用驻留包或记录本次新加载依赖。
`Public/Asset/PackageResource.h` 增加 `FPreparedPackageResource`，持有 main/bulk
一致快照；重读完整闭包摘要作为 commit 前 stale 检查，保留快照给惰性载荷，
不得只检查时间戳/文件大小。资产路径 fence 或 projection pending 必须先走现有
协调流程，不能由 Reload 清除。读取与纯验证可使用现有已验收的任务入口，
不需要 Async Task Framework Stage 5 迁移才能开始 Stage 1/2。

`Public/Asset/PackageReload.h` 同时定义资源族 `IPackageReloadParticipant` 和
`FPackageReloadResourceReceipt`。Prepare 可失败并拥有候选产品；回执状态为
Pending/Ready/Failed/Retired，Ready 代表候选资源已准备好且发布命令已预留，
而非仅仅发出了 BeginInit。Commit 在 GameThread 禁止广播；RenderThread 在
同一有序发布批次切换 reference/proxy，确认后才允许 Succeeded 与旧图退休。
队列准入失败必须发生在 CPU 引用提交之前；不能在不可逆提交后把普通排队失败
当作整批失败。现有资源初始化 API 本身不满足这一保证，Stage 3 必须补齐回执。

首批参与者和确定拒绝规则：

| Participant / consumer | Admission and work |
| --- | --- |
| Texture2D 编译 manager | 使用 exact DClass 的 selected cancel/finish；新请求封锁、候选独立句柄、代次校验，失败重新开放旧图；禁止 FinishAllCompilation。 |
| VolumeTexture | 同步 provider，当前没有 Texture2D 异步 class route；单独准备体素与 GPU 产品，不能按派生关系调用 Texture2D selected finish。 |
| Material / MaterialInstance | 隔离 PostLoad 的缓存变化、编译、parent 与参数图；准备 render proxy 与 uniform texture bindings，重建 MaterialRenderTypes 的 FRHITextureReferenceRef 缓存。 |
| DVolumetricCloudComponent / 场景 proxy | 反射 Weather/Base/Detail 引用改写后，必须重新生成场景数据中的 FRHITextureReferenceRef；仅改 DObject 指针不够。StaticMesh 场景的材质 binding 同样注册参与者，无法准备则拒绝该次请求。 |
| 纹理/材质窗口和预览 | OpenTextures 的 native unordered_map、OpenMaterials、参数面板、PropertyView 编辑对象和预览缓存逐一 Prepare/Rebind；观察者由 operation token/weak owner 管理。 |
| DTransBuffer | Idle 准入，扫描 context、records、custom change、PackageTransitions 和 payload collector；预留全事务删除/事件存储，提交保留其他包内容、脏标记与无关历史。 |
| StaticMesh inspector / LevelEditor | StaticMesh 使用 FReadOnlyAssetDocumentModel，不是两个纹理的共享 Discard 调用者；不新增 reload 支持。Level 的 World 恢复路径保持其所有权边界。 |
| 未注册资源族、未知派生类型、未知 native consumer | exact-class allowlist 与参与者能力预检；TextureCube、StaticMesh、World/Level、类型定义、cooked 包和未保存包不能通过 Texture/Asset 基类宽泛放行。 |

故障注入点固定为 PreflightBudget、QuiesceSelected、ReadMain、ReadBulk、
CreateSkeleton、ResolveDependency、ApplyValues、RestoreLedger、PreparePostLoad、
PrepareReferences、PrepareNativeParticipant、PrepareHistory、PrepareRuntimeProduct、
ReserveRenderPublish、RevalidateDisk、RevalidateReferencers、BeforeCommit。
注入按包/对象序号选择，必须包含批次第二包失败；每个失败核对旧 registry/句柄、
源身份、强引用/Map、dirty、历史与候选资源数量。无失败 Commit 内只允许测试暂停
观察边界，不添加可返回普通失败的钩子。若需要补偿回滚而不是 noexcept swap，
先记录该原语的决策与故障注入证据，再接受 Stage 1，禁止默默降低原子要求。

验证：`Win64-Debug-DurinEditor`，
`DevTool.bat test AssetDiscardCharacterizationTests --mode characterization`
通过两个 case；回执
`Build/.agent-state/logs/20260907-153330-060182-24752-ctest.log`。
每个 case 先 Save/Unload/Load 验证磁盘基线，再用实际云组件持有纹理，调用编辑器
所用共享 Discard 路径；两处 `EXPECT_EQ(source identity, saved identity)` 被
`EXPECT_NONFATAL_FAILURE` 捕获，分别证明内存未恢复和再次保存污染磁盘。
Stage 4 必须移除捕获包装并将这些断言迁入正常 feature 验证；本阶段没有创建
GPU 场景、渲染截图或实际窗口关闭验收，相关矩阵仍由 Stage 3–5 验收。

交付验证：`DevTool.bat test affected` 因新增目标注册选择 78 个目标，构建成功，
77 个通过；`CoreConcurrencyTests` 的
`FAsyncOperationGroupTests.SharedResultAliasesAndExternalSourcesRetainModuleStorage`
在未修改的 `AsyncOperationGroupTests.cpp:260` 即时 Join readiness 断言失败。
保留首次失败回执
`Build/.agent-state/logs/20260907-153705-870975-33144-ctest.log`。
该 case 隔离重跑通过，随后完整 `DevTool.bat test CoreConcurrencyTests` 的
176 个 case 全部通过，回执
`Build/.agent-state/logs/20260907-153940-356314-17236-CoreConcurrencyTests.log`。
这不是第一次 affected 全绿的证据；本阶段没有修改任务系统或它的测试。
`doc validate --scope changed`、`doc plan validate --scope all` 与
`git diff --check` 通过。

### Stage 1: Add controlled object graph replacement primitives

依赖 Stage 0。

- [ ] 在 CoreDObject 实现旧新图映射、类型检查、反射/容器引用写入计划及原生
  参与者契约，覆盖 Map 键和嵌套字段，保持 GC 枚举与写入职责清晰。
- [ ] 实现同路径候选图隔离、唯一注册切换、软引用缓存失效和旧图延迟退休，
  不增加平行驻留表、不让旧弱句柄因槽位复用命中新对象。
- [ ] 验证强引用、弱句柄、循环/子对象、缺失替代目标、类型不兼容、Map 冲突、
  准备中新增引用及无支持参与者的拒绝路径。

完成条件：不依赖磁盘/编辑器的内存图替换可原子提交；失败后所有原引用可用，
成功后无可访问旧图残留或悬空引用。

### Stage 2: Prepare saved packages without publishing live state

依赖 Stage 1。

- [ ] 从 Engine 现有加载事务抽取候选图准备能力，重用 canonical linker、schema、
  authored provenance 和依赖闭包，不通过普通 LoadPackage 返回当前驻留对象。
- [ ] 实现一致闭包读取、指纹重查、BulkData 生命周期与预算；明确已驻留外部依赖
  复用和替换集合内部循环绑定。
- [ ] 实现新包、缺失/损坏文件、fenced 路径、Unsupported/Busy/Stale/Cancelled
  的结构化结果及准备失败时仅释放本次依赖和对象。

完成条件：能得到完整保存版本候选图；任何准备失败均不改变当前包、注册、磁盘
或其他包脏状态。多包准备失败整批退出。

### Stage 3: Coordinate compilation and runtime resource publication

依赖 Stage 2；使用当前已落地任务契约，不绕过其他计划的生产切换门槛。

- [ ] 实现目标图任务准入封锁、selected cancel/finish 和请求代次校验；候选图任务
  使用独立身份，旧完成通知在取消、提交后和对象退休后均不能重新应用。
- [ ] 接入 Texture2D、VolumeTexture、Material/MaterialInstance 的候选准备及资源
  发布；按实际编译路由处理 VolumeTexture，不假设其注册了 Texture2D 异步域。
- [ ] 接入场景/材质/预览的纹理引用和缓存刷新，完成渲染线程有序切换与旧资源
  deferred cleanup；验证恢复失败时旧资源仍可用。
- [ ] 建立 Engine Reload 协调器及分阶段通知；验证准备失败、取消、关闭、退出、
  stale 回调、渲染准入失败和无关任务不被全局排空。

完成条件：场景持续引用资源时能够重载，CPU 内容与渲染结果均恢复；错误或取消
不留下半切换状态，提交后的旧任务不会覆盖恢复结果。

### Stage 4: Route editor discard through package reload

依赖 Stage 3。

- [ ] AssetTools 增加包级恢复策略；DurinEd 接入 Pending 关闭流程、冲突封锁、
  同包文档影响范围和错误信息，并对从未保存的包明确拒绝恢复。
- [ ] 实现事务失效预检/提交、跨包历史检查点处理和文档旧新对象重绑定。
- [ ] 将 Texture2D、VolumeTexture 及 Material 编辑器迁移到共享恢复入口，移除
  已被替代的 discard 专用快照和无恢复的清脏路径；审计 StaticMesh 等调用者，
  未接入类型明确拒绝且保持原脏状态。
- [ ] 验证恢复成功才关闭请求文档，同包其他文档继续操作新对象；失败可重试，
  关闭一个窗口不会遗失其余文档的资源状态。

完成条件：所有 Discard 入口均不能在未恢复时报告成功；两个纹理 P1 回归通过，
材质不再依赖窗口打开时的基线表达磁盘保存状态。

### Stage 5: Qualify recovery and publish lasting contracts

依赖 Stage 4。

- [ ] 完成下方验收矩阵和受影响模块验证，记录实际测试目标、配置、命令回执及
  人工场景验证证据；不能只以文档验证或编译成功结项。
- [ ] 在 AssetPackages、AssetCompilation、Transactors 和相应渲染/编辑器契约中
  记录实现后的所有权、失败、引用及关闭语义，并从本计划链接到权威文档。
- [ ] 清理兼容桥接/重复入口，确认无新增格式与第二驻留表，完成最终跨模块构建，
  更新本计划状态及证据后再结项。

完成条件：全部必需场景通过，故障路径可复现地保留原状态，长效规则已归属对应
契约文档，未接入资源类型的拒绝行为有测试。

## Acceptance Matrix

| Scenario | Required observation |
| --- | --- |
| Texture2D/VolumeTexture 被场景引用后修改并放弃 | authored 值、源数据与画面回到磁盘版本；再次保存并全新加载无被放弃内容 |
| 成功保存 B，再编辑 C 后放弃 | 恢复 B；保存失败不产生新的磁盘基线 |
| 文档打开前包已脏，或已有编辑历史被驱逐 | 仍从磁盘恢复，不依赖窗口快照/undo 完整性 |
| 多顶层导出、多个文档、子对象新增/删除与循环 | 包级一致切换；不可映射的图外强引用使请求失败 |
| 强/弱/软引用、原生缓存与 Map 键 | 强引用正确切换，弱引用安全失效/重绑定，软路径不变，无容器损坏 |
| 跨包事务和历史载荷引用旧图 | 完整退休相关事务，无关历史保留，其他包内容及脏状态不被清除 |
| 编译中取消、迟到结果、准备后新编辑/引用 | 不发生 stale 应用；冲突返回 Busy/Stale，原状态可继续工作 |
| I/O、bulk、schema、构建、资源准入及参与者失败 | 原图、引用、dirty、历史和打开文档保留，候选图无泄漏 |
| 新建未保存、缺失文件、外部修改、fenced 路径 | 明确诊断，不猜测基线、不覆盖磁盘、不伪成功 |
| 同批次第二包失败、GC 压力、窗口销毁、模块退出 | 无部分提交、悬空回调或泄漏；旧渲染存储按线程边界退休 |

## Validation and Dependencies

实现前遵循 [Build and run workflow](../Agents/BuildAndRun.md) 与
[Testing workflow](../Agents/Testing.md)；新增原生测试时遵循仓库测试注册规范。
本次计划落盘仅运行文档和计划生命周期验证，不代表上方任何实现阶段完成。

[Async Task Framework Refactor](AsyncTaskFrameworkRefactor.md) 正在改造包读取与
纹理编译的任务接入。这里复用当时已经验收的生产 API；若需要该计划尚未通过的
能力，将对应任务明确标为依赖阻塞，不新建第二调度器，也不提前迁移其试点。

UE 的 [ReloadPackages](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/CoreUObject/ReloadPackages)、
[FPackageReloadedEvent](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/CoreUObject/FPackageReloadedEvent)
提供包替换、引用映射与通知的参考。本计划的原子失败、参与者准入和资源准备门槛
是 Durin 的选定要求，不将公开 UE API 文档当作这些保证的实现证据。

## Related Code

- `Engine/Source/Runtime/CoreDObject/Public/DObject/DObjectArray.h`
- `Engine/Source/Runtime/CoreDObject/Public/DObject/Package.h`
- `Engine/Source/Runtime/CoreDObject/Public/DObject/ObjectPtr.h`
- `Engine/Source/Runtime/Engine/Public/Asset/Load.h`
- `Engine/Source/Runtime/Engine/Private/Asset/AssetPackageLinkerLoader.cpp`
- `Engine/Source/Runtime/Engine/Public/Asset/PackageSerialization.h`
- `Engine/Source/Runtime/Engine/Private/Texture/TextureCompilingManager.cpp`
- `Engine/Source/Editor/DurinEd/Private/Editor/WorkspaceRootWindow.cpp`
- `Engine/Source/Editor/DurinEd/Private/Editor/WorkspaceManager.cpp`
- `Engine/Source/Editor/DurinEd/Private/Editor/Transactor.cpp`
- `Engine/Source/Editor/TextureEditor/Private/Widgets/MTextureEditor.cpp`
- `Engine/Source/Editor/TextureEditor/Private/Widgets/MVolumeTextureEditor.cpp`
- `Engine/Source/Editor/MaterialEditor/Private/MaterialDocumentSnapshot.cpp`

## Related Contracts

- [Asset packages](../Runtime/Assets/AssetPackages.md)
- [BulkData](../Runtime/Assets/BulkData.md)
- [Asset compilation](../Runtime/Assets/AssetCompilation.md)
- [Async asset operations](../Editor/Architecture/AsyncAssetOperations.md)
- [Transactors](../Editor/Architecture/Transactors.md)
- [Render resource lifecycle](../Runtime/Rendering/RenderResourceLifecycle.md)
