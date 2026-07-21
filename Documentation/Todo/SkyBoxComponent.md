# SkyBoxComponent TODO

Last reviewed: 2026-07-22

## Current Status

Durin 已具备 Texture2D 资产、RGBA8 图像解码、Mip 构建、渲染线程资源上传、
Shader 纹理采样、Scene Color/Depth 中间目标和后处理链路。RHI 也声明了
`TextureCube` 维度，Vulkan 后端能够选择 Cube Image View 类型。

这些能力尚未形成可用的 Cubemap 垂直链路：Cube Texture 没有完整的六层创建与上传约定，
Vulkan Image 缺少 Cube Compatible 创建标志，上传接口固定使用 array layer 0，引擎也没有
`DTextureCube`、`DSkyBoxComponent` 或天空绘制阶段。

本计划是待实施的第一版路线，所有任务尚未开始。

## Goal

用户可以将一个 `DSkyBoxComponent` 放入 Level，为它指定由六张 LDR 图片构建的
`DTextureCube` 资产，并在编辑器主视口、相机预览和游戏视口中看到没有平移视差的
静态天空背景。相机旋转会改变观察方向，Component 旋转可调整天空朝向，场景几何与
编辑器 Overlay 始终正确覆盖天空。

## Scope

- 一个正式的 `DTextureCube` 资产和渲染资源链路。
- 六张等尺寸、等格式、正方形 PNG/JPEG/BMP/TGA 图片导入为 RGBA8 Cubemap。
- Cube Texture 在 RHI 与 VulkanRHI 中的创建、六个 face 的所有 mip 上传、采样和销毁。
- `DSkyBoxComponent` 的反射、序列化、注册、可见性和场景更新。
- 一个便于放入 Level 的 `ASkyBoxActor`，默认持有 `DSkyBoxComponent`。
- Renderer 中独立的天空 Shader、参数、Pipeline State 和绘制入口。
- Content Browser 中可完成的 Cube Texture 导入或创建流程，以及 Details 中的资产赋值。
- 自动化测试、真实 Vulkan 验证和 DurinEditor 可见效果检查。

## Non-Goals

下列能力不得为了“顺便扩展”进入第一版：

- Sky Light、环境漫反射、镜面 IBL、反射探针或场景捕获。
- HDR/EXR 源文件、浮点 Cube Texture、曝光控制或 HDRI 预过滤。
- 经纬度全景图自动转 Cubemap。
- Sky Atmosphere、大气散射 LUT、空中透视、体积云、雾、太阳盘、月亮或星空动画。
- Skydome Mesh、Sky Material 或通用天空材质图。
- Cube Texture Array、体纹理、虚拟纹理或纹理流送。
- 多个 SkyBox 的混合、过渡、区域化、优先级系统或每个 View 的独立天空覆盖。
- 高级资产预览器和无缝边缘专用滤波。

## Design Decisions and Invariants

### 资源契约

- 第一版使用真正的 `TextureCube`，不用 Texture2D 经纬度采样作为过渡实现。
- Face 顺序固定为 `+X, -X, +Y, -Y, +Z, -Z`；资产界面和错误消息必须显示名称，
  不让用户依赖数字下标。
- 六个 face 必须为正方形、尺寸相同，并解码为 RGBA8。任意一面失败时，整次导入失败，
  不保存部分有效的 Cube 资产。
- 第一版按 sRGB 颜色纹理构建完整 mip 链。每个 face 可复用 Texture2D 的颜色 mip 规则，
  但不声称已解决跨 face 边缘滤波。

### RHI 契约

- `TextureCube` 表示一张拥有六个物理 array layer 的纹理；`CreateCube()` 必须产生满足该约束的
  Create Desc。Cube Array 的公开语义留待后续单独设计。
- 2D 子资源上传契约必须显式接收 `MipIndex` 和 `ArraySlice`。Texture2D 继续只使用 slice 0，
  Cube Texture 使用 slice 0–5；不在 VulkanRHI 内根据调用次数猜测 face。
- Vulkan Image 必须使用 Cube Compatible 标志，Image View 覆盖六层，每个 mip/face 都在拷贝前后
  进入正确 layout。
- 非法尺寸、layer 数、mip 索引、slice 索引和不支持的格式必须在靠近公开边界的位置
  报错，不把无效描述传给 Vulkan。

### Component 与 Scene 契约

- `DSkyBoxComponent` 继承 `DSceneComponent`。它的平移和缩放没有渲染含义，只有世界旋转作用于
  Cubemap 方向。
- 第一版属性收敛为 `TextureCube`、`Tint`、`Intensity` 和继承的 Transform/Actor Visibility。
  `Intensity` 在线性空间中不小于 0，`Tint` 不作为 Sky Light 输入。
- Scene 保存 Renderer 可安全读取的 `FSkyBoxSceneData` 快照，而不是在绘制时回读反射对象。
  资产渲染资源通过跨线程生命周代理保活。
- 第一版每个 Scene 最多有一个生效 SkyBox。如果编辑内容中出现多个可见且已注册的
  `DSkyBoxComponent`，Scene 使用稳定的 Component/Scene ID 选择 ID 最小者，并在编辑器中提示冲突。
  不使用“最后一个注册者”这种随加载顺序变化的规则。
- Component 注册、反注册、Actor 可见性、Transform 旋转、纹理赋值和可编辑参数提交都必须
  更新 Scene，并拒绝已过期的队列命令。

### Renderer 契约

- 天空使用全屏三角形，不引入 Cube Mesh 或 Skydome Mesh 资产依赖。
- 天空在 Scene Color Render Pass 中，设置好当前 View 的 viewport/scissor 后、不透明几何之前绘制。
  它不执行深度测试，不写深度；后续几何、Editor Grid 和 Overlay 自然覆盖它。
- Shader 从 View/Projection 重建世界视线，去除相机平移，再应用 Component 旋转的逆变换采样
  Cubemap。相机平移不得改变天空画面。
- 天空在 Lit、Unlit 和 Wireframe 视图模式中都可见；本阶段不定义专用的 Sky View Mode。
- 解析不到资产、资源尚未就绪或资源替换期间，使用 Renderer 拥有的 1×1 黑色回退
  Cubemap；没有生效 Component 时保留现有 Scene Color 清除结果。
- Cubemap 按 sRGB 解码为线性颜色，乘以 `Tint * Intensity` 后写入 Scene Color，继续经过现有
  Post Process；不为 SkyBox 建立第二条色调映射路径。

## Current Foundations and Gaps

| 层级 | 可复用基础 | 必须补齐的第一版缺口 |
| --- | --- | --- |
| Image/Asset | `DTexture2D` 的 RGBA8 解码、Mip 和资产导入模式 | 共享图像构建逻辑、`DTextureCube` 数据、六面事务导入 |
| Render Resource | Texture2D 的 shared proxy、revision 和渲染线程上传 | Cube 平台数据、六层上传和黑色回退 Cube |
| RHI | `ETextureDimension::TextureCube` 和 Create Desc | 六层语义、slice 上传接口、约束验证 |
| VulkanRHI | Cube Image View 类型映射 | Cube Compatible Image、layer range transition/copy/view |
| Component | `DSceneComponent` 注册、Transform 和 Actor Visibility | `DSkyBoxComponent`、`ASkyBoxActor`、属性变更通知 |
| Scene | Primitive 和 Directional Light 场景入口 | SkyBox ID、快照、更新、冲突规则与 Release |
| Renderer | Slang、纹理/采样器绑定、Scene Color/Depth/Post Process | SkyBox Shader、Pipeline、绘制顺序和资源回退 |
| Editor | Content Browser 资产操作与反射 Details | Cube 导入界面、资产识别、Actor/Component 创建与冲突诊断 |

## Implementation Stages

### Stage 0: 锁定坐标、Face 和测试契约

本阶段不产生用户可见效果，用于防止六个 face 的轴向、朝向和纹理原点在后续阶段反复翻转。

- [ ] 记录 Durin 世界坐标、相机 Forward/Up 与 Vulkan Cubemap face 的对应。
- [ ] 定义 `+X, -X, +Y, -Y, +Z, -Z` 六个源图的朝向、上方向和是否需要翻转。
- [ ] 在测试数据目录加入小尺寸、可明确识别方向的六色/带文字边标 Cubemap。
- [ ] 定义一组 CPU 方向到预期 face/UV 的对照用例，作为 Shader 可视验证的真值表。
- [ ] 将最终约定写入 Cube Texture 导入错误和用户文档，不只留在 Shader 注释中。

#### Acceptance Gate

- 可以不依赖渲染结果，根据文档唯一确定六张源图应放入的 face 和方向。
- 六个主轴视线都有明确的预期颜色/face 可供后续自动或人工校验。

### Stage 1: 完成 RHI 和 VulkanRHI Cube Texture 基础

依赖 Stage 0。本阶段只证明 GPU 可以正确创建、上传和采样六层纹理，不引入 UObject。

- [ ] 为 `FRHITextureCreateDesc::CreateCube()` 设置六层约定，并校验 Width/Height 为相同的非零值。
- [ ] 在 RHI 上传接口中加入显式 `ArraySlice`，保持 Texture2D slice 0 调用点行为不变。
- [ ] 在 RHI 公共边界验证 mip/slice/region/source pitch，为非法调用提供可定位诊断。
- [ ] 使 Vulkan Cube Image 包含 `eCubeCompatible`，使 Image View 从 base layer 0 覆盖六层。
- [ ] 更新 Vulkan staging copy 和 layout transition，使其只操作指定 mip/slice，且不破坏其他已上传 face。
- [ ] 检查 descriptor/view 维度映射，确保 Shader 声明的 `TextureCube` 不会接收 2D Image View。
- [ ] 补充 Create Desc 和非法子资源的 RHI 单元测试。
- [ ] 加入一个最小 Vulkan Cube 创建/六层上传/采样 smoke 路径，并开启 Validation 检查。

#### Acceptance Gate

- Texture2D 现有单层上传测试不回归。
- 六个 face 与多个 mip 可分别上传并被 Cube View 采样。
- Vulkan Validation 不报 Image Create Flag、subresource range、layout、copy 或 descriptor 维度错误。

### Stage 2: 实现 DTextureCube 资产和渲染资源

依赖 Stage 1。本阶段完成从六张源图到可供渲染线程使用的 Cube RHI 资源的非编辑器链路。

- [ ] 将 Texture2D 中可复用的 RGBA8 解码、尺寸限制和颜色 mip 构建抽到 Engine 共享的图像构建工具，
  不复制两份 codec 选择与错误处理。
- [ ] 定义 `FTextureCubeSourceData`、`FTextureCubePlatformData` 和每个 face/mip 的存储结构，保留明确的 face 枚举。
- [ ] 实现 `DTextureCube` 的六个源文件引用、反射/序列化、`PostLoad` 重建和错误报告。
- [ ] 实现原子导入：先验证六面，再创建/保存资产并复制源文件；任一步失败时清理本次产物。
- [ ] 为六个 face 生成完整 mip 链，验证每一级尺寸、row pitch、数据长度和 pixel format。
- [ ] 实现 `FTextureCubeRenderResource` shared lifetime proxy，使 build/rebuild/release 只在渲染线程访问 RHI。
- [ ] 为快速连续重建增加 revision 拒绝，防止旧命令覆盖新 Cube 资源。
- [ ] 确保 Cube 资产引用被序列化、依赖跟踪与 GC 正确保留，删除/移动资产时不留悬空引用。
- [ ] 增加导入、尺寸不匹配、非正方形、缺少 face、重载、序列化、移动、删除和旧 revision 测试。

#### Acceptance Gate

- 合法六面可导入、保存、重启加载并重建为完整 Cube RHI 资源。
- 非法导入不留下部分 Package/源文件，错误信息指明具体 face 和失败原因。
- 资产重建和销毁时没有游戏线程直接访问 RHI 或队列命令使用已销毁 UObject。

### Stage 3: 实现 DSkyBoxComponent 与 Scene 快照

依赖 Stage 2。本阶段建立游戏对象到 Renderer Scene 的数据边界，但可以先不绘制。

- [ ] 新增反射类 `DSkyBoxComponent : DSceneComponent`，添加 `DTextureCube` 引用、Tint 和 Intensity。
- [ ] 为 Component 分配稳定 Scene ID，定义 `FSkyBoxSceneData` 中渲染所需的最小快照：ID、
  Cube Render Resource proxy、旋转、Tint、Intensity 和 revision。
- [ ] 扩展 `IScene`/`FScene` 的 add-or-replace、remove 与 active-sky query，让实际容器只在渲染线程变更。
- [ ] 实现 Component 的 `OnRegister`、`OnUnregister`、`OnOwnerVisibilityChanged` 和 Transform 变更处理。
- [ ] 接入 `PostEditChangeProperty` 或等价的统一脏标记入口，使 Details 交互编辑与提交都能及时
  更新 Scene，同时合并无意义的重复更新。
- [ ] 实现多个已注册 Component 按最小 ID 选择的规则，并保证隐藏/删除当前生效者后能选中下一个。
- [ ] 新增 `ASkyBoxActor`，在构造时创建默认 `DSkyBoxComponent`，但不把绘制逻辑放入 Actor。
- [ ] 更新 Engine 模块的反射输入与生成元数据，不手写代替 DHT 产物。
- [ ] 测试注册/反注册、可见性、旋转、属性更新、多 Component 选择、Scene Release 和过期 revision。

#### Acceptance Gate

- 不启动 Renderer 绘制也能通过测试证明 Scene 快照与 Component 状态一致。
- 渲染线程读取天空时不触及 `DSkyBoxComponent` 或 `DTextureCube` UObject。
- 同一 Scene 中多个 Component 的生效结果在重新加载和可见性切换后保持确定。

### Stage 4: 实现天空渲染阶段

依赖 Stage 1 和 Stage 3。本阶段产生第一个可见的 SkyBox 结果。

- [ ] 新增独立 Slang SkyBox Shader 与绑定声明，使用全屏三角形生成屏幕覆盖。
- [ ] 传入重建视线所需的 View/Projection 逆变换数据、SkyBox 旋转、Tint 和 Intensity。
- [ ] 根据 Stage 0 的真值表实现方向重建与 Cube 采样，不在 Shader 中用无文档的负号/轴交换“试出”结果。
- [ ] 建立独立 Pipeline State：无顶点缓冲、无深度测试/写入、无混合、无面剔除，目标格式与 Scene Color 一致。
- [ ] 新增 Renderer 拥有的线性采样器和 1×1 黑色回退 Cubemap，并在模块 Release 时安全释放。
- [ ] 在 `RenderScene()` 中先画 SkyBox，再画 Static Mesh、Editor Grid 和 Overlay；没有 Component 时不发出 SkyBox draw。
- [ ] 遵守 `FSceneView::ViewportX/Y/Width/Height`，使固定宽高比的信箱区域保持黑色，天空不被拉伸到整张目标。
- [ ] 使 Lit、Unlit 和 Wireframe 共享同一 SkyBox 背景策略，不与 Static Mesh Pipeline 的初始化成功绑定。
- [ ] 确保天空先写入 Scene Color 再经现有 Post Process，并测试 sRGB 解码、Tint/Intensity 的线性计算。

#### Acceptance Gate

- 相机仅平移时，同一像素方向采样的天空不变；相机旋转时，六个主轴 face 符合 Stage 0 约定。
- Static Mesh、Grid、Gizmo、Line 和 Icon Overlay 不被天空覆盖。
- 主视口、辅助相机预览和窗口直出路径共享一致的天空方向和宽高比行为。
- 资源未就绪、重建或删除时不崩溃、不绑定悬空 descriptor，并显示黑色回退。

### Stage 5: 打通编辑器创建与赋值工作流

依赖 Stage 2–4。本阶段使功能无需测试代码或硬编码资产即可使用。

- [ ] 在 Content Browser 中增加 `Texture Cube` 创建/导入操作。
- [ ] 提供显式的六个 face 文件选择槽，选择前后都显示 `+X/-X/+Y/-Y/+Z/-Z` 与方向提示。
- [ ] 在用户确认前同步验证缺失、解码失败、尺寸不同和非正方形，禁止提交无效组合。
- [ ] 让 Content Browser 正确识别 Cube Texture 类型，第一版至少提供稳定的类型图标和尺寸/mip/face 摘要。
- [ ] 在 Actor 创建流程中暴露 `ASkyBoxActor`，并让 Details 对 `DTextureCube` 只显示兼容资产。
- [ ] 为 Tint、Intensity 和旋转提供正常的反射编辑体验，更改后实时刷新视口并正确标记 Package dirty。
- [ ] 当同一 Scene 有多个可见 SkyBox 时显示非阻断诊断，标识当前生效者和被忽略者。
- [ ] 增加编辑器流程测试：导入 Cube、创建 Actor、赋值、保存 Level、重载并验证 Scene 快照。

#### Acceptance Gate

- 一个新用户可仅通过编辑器完成“六面图片 → Texture Cube → SkyBox Actor → 视口天空”。
- 保存并重启编辑器后，资产引用、Component 属性、旋转和生效者选择不变。
- 错误资产组合在保存前给出可操作的错误，不等到 Vulkan 创建时失败。

### Stage 6: 完成端到端验证与收尾

依赖前面所有 Stage。本阶段不扩展效果，只关闭可靠性、回归和文档缺口。

- [ ] 运行 Texture2D、RHI、VulkanRHI、Asset、Reflection、Scene、Renderer 和 Editor 相关的定向测试。
- [ ] 增加可重复的渲染图像或像素取样测试，覆盖六个主轴、相机平移、相机旋转、Component 旋转和几何遮挡。
- [ ] 在不同宽高比、编辑器主视口、相机预览、PIE/游戏窗口中进行人工可见性检查。
- [ ] 快速替换/重建/删除 Cube 资产，反复隐藏/显示/删除 SkyBox Actor，检查过期命令与资源生命周期。
- [ ] 在 Vulkan Validation 开启时验证六层、多 mip、descriptor 绑定、layout transition 和模块销毁。
- [ ] 使用同一 Preset 先完成全量 `all` 构建，再启动 `DurinEditor` 完成隐藏窗口运行时 smoke test。
- [ ] 更新本 TODO 的勾选项、Current Status 和 `Last reviewed`，将长期契约写入相应 Architecture 文档。
- [ ] 在 `Documentation/Todo/TextureSupport.md` 中更新 Cube Map 的 Later Scope 状态，避免两份 TODO 对已实现能力互相矛盾。

#### Acceptance Gate

- 所有定向测试、全量构建、Vulkan Validation 和 DurinEditor smoke test 通过。
- 人工可见性矩阵没有 face 方向错误、平移视差、宽高比拉伸或绘制顺序错误。
- 架构文档描述实际落地的线程、资源和渲染边界，本 TODO 可标记为完成。

## Validation Matrix

| 维度 | 必须覆盖的情况 | 主要证据 |
| --- | --- | --- |
| 资产输入 | 正常六面、缺面、非正方形、尺寸不匹配、损坏文件 | Asset/Editor 自动化测试 |
| RHI 子资源 | 6 faces × 全 mip、非法 slice/mip/region | RHI 单元测试和 Vulkan Validation |
| 资源生命周期 | 初次构建、快速重建、替换、删除、模块 Release | Revision/渲染线程集成测试 |
| Scene 状态 | 注册、隐藏、旋转、参数更改、多 Component 冲突 | Scene 快照测试 |
| 视线 | 六主轴、相机平移/旋转、Component 旋转 | 像素取样或图像测试 |
| 绘制顺序 | Static Mesh、Grid、Gizmo、Overlay 遮挡天空 | 渲染图像与人工检查 |
| Viewport | 主视口、相机预览、窗口直出、固定宽高比 | DurinEditor 可见 smoke |
| 失败回退 | 无 Component、空资产、未就绪、重建失败、资产删除 | 自动测试 + 黑色回退检查 |
| 视图模式 | Lit、Unlit、Wireframe | Editor 视口检查 |

## Definition of Done

只有以下条件全部满足，才可将“简单 SkyBoxComponent”标记为完成：

- [ ] 用户可在编辑器中导入六面 Cube Texture，创建 `ASkyBoxActor` 并在 Details 赋值。
- [ ] Level 保存/重载后 SkyBox 资产、Tint、Intensity 和旋转保持正确。
- [ ] 主视口、相机预览和游戏窗口显示同一方向正确的静态天空。
- [ ] 相机平移无视差，相机和 Component 旋转符合文档化的坐标约定。
- [ ] 天空不覆盖场景几何、Editor Grid 或 Overlay，不使固定宽高比画面拉伸。
- [ ] 不存在游戏线程直接访问 SkyBox RHI 或渲染线程回读反射 UObject 的路径。
- [ ] 缺失或未就绪资源使用稳定黑色回退，快速重建/删除不产生悬空资源。
- [ ] 定向测试、全量 `all` 构建、Vulkan Validation 和 DurinEditor 运行时 smoke test 全部通过。
- [ ] 实际落地的资源、Scene 与 Renderer 长期契约已迁入 Architecture 文档。

## Deferred Follow-ups

以下内容必须在本 TODO 完成后再单独选型和排期，不作为本计划未完成的判定条件：

- HDR/EXR 导入和浮点 `DTextureCube`。
- 经纬度 HDRI 转 Cubemap 与离线边缘处理。
- `DSkyLightComponent` 与漫反射/镜面 IBL。
- Skydome Material 与程序化 Sky Atmosphere。
- 多 SkyBox 过渡、区域化与 View 级覆盖。
- Cube Texture 压缩、派生数据缓存、异步构建和 residency 管理。
- 互动式 Cube Texture 预览器与更高级的资产编辑工作流。

## Related Documentation

- [TODO 计划文档指南](README.md)
- [天空渲染方案综述](../Reference/SkyRenderingOverview.md)
- [Texture Support TODO](TextureSupport.md)
- [Runtime Architecture](../Architecture/RuntimeArchitecture.md)
- [Viewport Rendering](../Architecture/ViewportRendering.md)
- [Build and Run](../Setup/BuildAndRun.md)
- [Native Tests](../Setup/NativeTests.md)

## Related Code

- `Engine/Source/Runtime/RHI/Public/RHIResources.h`
- `Engine/Source/Runtime/RHI/Public/DynamicRHI.h`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanTexture.cpp`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanRHIPrivate.cpp`
- `Engine/Source/Runtime/Engine/Public/Texture/Texture2D.h`
- `Engine/Source/Runtime/Engine/Private/Texture/Texture2DRenderResource.cpp`
- `Engine/Source/Runtime/Engine/Public/Components/DirectionalLightComponent.h`
- `Engine/Source/Runtime/Engine/Public/IScene.h`
- `Engine/Source/Runtime/Renderer/Public/Scene.h`
- `Engine/Source/Runtime/Renderer/Private/Scene.cpp`
- `Engine/Source/Runtime/Renderer/Private/RendererModule.cpp`

