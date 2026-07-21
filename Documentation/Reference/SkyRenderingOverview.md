# 天空渲染方案综述

本文记录 Durin 当前与天空渲染相关的能力、Unreal Engine 的常见天空实现，以及后续技术选型需要评估的问题。本文是调研参考，不是架构规范或已经批准的实施方案；其中列出的路线均不代表最终选择。

## 1. 功能范围

“天空盒”在实际渲染系统中可能同时指代几项不同能力：

- 在相机背景中显示静态天空图像。
- 根据太阳方向和大气参数生成动态天空。
- 使用天空数据为场景提供环境漫反射和镜面反射。
- 显示云、太阳、月亮、星空和远景等天空内容。
- 为雾和远处物体提供空气透视。

这些能力可以共享输入资源，但不应被视为同一个渲染职责。尤其需要区分：

```text
可见天空背景
  != 天空环境光照
  != 反射环境
  != 物理大气与空气透视
```

第一版若只要求相机背景中显示天空，所需工作会明显少于完整的物理大气和基于图像的光照系统。

## 2. Unreal Engine 的常见实现

现代 Unreal Engine 没有使用单一的“天空盒系统”，而是提供多条可以组合的路径。

### 2.1 Sky Atmosphere

Sky Atmosphere 是物理大气渲染系统。它根据太阳方向、Rayleigh 散射、Mie 散射和吸收等参数生成天空，并支持从地面到太空的观察尺度。

为了避免对每个像素执行完整的大气积分，系统使用多种低分辨率查找表，包括：

- Transmittance LUT。
- Multi-Scattering LUT。
- Fast Sky View LUT。
- Aerial Perspective LUT。

这条路径适合动态昼夜循环、行星尺度大气以及需要空气透视的户外场景，但实现成本和渲染集成复杂度最高。

官方参考：

- [Sky Atmosphere Component](https://dev.epicgames.com/documentation/en-us/unreal-engine/sky-atmosphere-component-in-unreal-engine)
- [Sky Atmosphere Component Properties](https://dev.epicgames.com/documentation/unreal-engine/sky-atmosphere-component-properties-in-unreal-engine?lang=en-US)

### 2.2 Skydome Mesh 与 Sky Material

需要由美术直接控制云层、星空、行星或风格化背景时，Unreal Engine 可以使用包围场景的球体或半球体 Mesh。天空材质通常为 Opaque、Unlit，并启用 `Is Sky`。

官方文档说明，这类天空材质作为 Base Pass 中最后一个不透明 Mesh 绘制。它可以组合纹理内容和 Sky Atmosphere Material Expressions，同时避免对已经包含大气效果的天空重复应用空气透视。

这条路径的主要特点是：

- 天空表现由普通 Mesh 和 Material 驱动。
- 美术内容和材质系统具有较高自由度。
- 可与程序化大气组合，而不必独立承担环境光照职责。

### 2.3 HDRI Backdrop

HDRI Backdrop 主要面向摄影棚、产品展示和静态环境。其输入通常为 HDR 全景图或 Cubemap，并通过 backdrop/dome 几何显示背景。

该方案还会结合 Sky Light，使同一个 HDR 环境参与场景光照和反射。可见背景与环境照明在使用体验上被组合，但底层仍是不同职责。

官方参考：

- [HDRI Backdrop](https://dev.epicgames.com/documentation/unreal-engine/hdri-backdrop?application_version=4.27)

### 2.4 Sky Light

Sky Light 负责把远处场景或指定 Cubemap 转换为天空环境光照。其来源可以是：

- 捕获当前场景中的大气、云、天空材质和远景。
- 使用显式指定的 Cubemap。
- 在运行时执行实时捕获。

捕获结果会经过过滤，以供漫反射和镜面反射使用。因此 Sky Light 解决的是照明和反射问题，而不是单纯把背景画到屏幕上。

官方参考：

- [Sky Lights](https://dev.epicgames.com/documentation/unreal-engine/sky-lights-in-unreal-engine?lang=en-US)

## 3. 可选技术路线

### 3.1 Cubemap 背景

使用六面 Cubemap 或由经纬度全景图转换得到的 Cubemap，通过全屏三角形或包围相机的立方体进行采样。

优点：

- 运行时成本低。
- 与 HDRI、环境反射和后续 IBL 容易共享数据。
- 第一版功能边界清晰。

限制：

- 天空内容本身是静态的。
- 动态太阳、云层和昼夜变化需要额外系统。
- 需要完整的 Cube Texture 资产和 RHI 支持。

### 3.2 经纬度全景图直接采样

使用一张二维 equirectangular texture，根据视线方向转换为经纬度 UV 后直接采样。

优点：

- 可以复用现有 Texture2D 资源链路。
- 导入常见 HDRI 时不必立即构建 Cube Texture 资产。
- 适合快速验证天空背景的完整渲染链路。

限制：

- 两极区域纹理畸变明显。
- 采样分布和缓存局部性不如 Cubemap。
- 若后续用于反射探针和 IBL，通常仍需转换成 Cubemap。

### 3.3 Skydome Mesh 与天空材质

使用普通球体/半球体 Mesh 和专用 Unlit Material 表示天空。

优点：

- 可以复用 Mesh、Material 和资源编辑流程。
- 容易叠加星空、云层、太阳和风格化效果。
- 与 Unreal Engine 的美术天空工作流较接近。

限制：

- 需要定义天空材质与普通表面材质的差异。
- 必须处理相机平移、剔除、深度状态和渲染顺序。
- 普通材质系统尚未必适合表达所有天空专用输入。

### 3.4 程序化物理大气

根据大气模型和太阳光实时生成天空，并通过 LUT 降低每帧积分成本。

优点：

- 支持动态太阳、昼夜循环和大气参数。
- 可以统一生成天空、透射率、散射和空气透视数据。
- 适合大型户外或行星场景。

限制：

- Shader、数学模型、LUT 管理和跨 Pass 集成复杂。
- 对纹理格式、计算能力和资源同步提出更高要求。
- 验证成本远高于静态天空背景。

## 4. Durin 当前基础

Durin 已经具备以下相关能力：

- RHI 定义了 `TextureCube` 和 `TextureCubeArray` 维度。
- Vulkan 后端定义了对应的 Cube 和 Cube Array Image View 类型。
- Renderer 已具备 Slang Shader、纹理和采样器参数绑定。
- 场景渲染已有独立 Scene Color、Depth 和后处理 Pass。
- `FSceneView` 已提供 View、Projection、ViewProjection 和相机世界位置。
- `DTexture2D` 已具备导入、平台数据、Mip 生成、渲染线程上传和生命周期管理。
- Texture2D 可以作为 Material 参数传入静态网格渲染。

当前渲染流程的主要相关位置为：

```text
Engine/Source/Runtime/RenderCore/Public/IRendererModule.h
Engine/Source/Runtime/Renderer/Private/RendererModule.cpp
Engine/Source/Runtime/RHI/Public/RHIResources.h
Engine/Source/Runtime/VulkanRHI/Private/VulkanTexture.cpp
Engine/Source/Runtime/Engine/Public/Texture/Texture2D.h
Engine/Source/Runtime/Engine/Private/Texture/Texture2DRenderResource.cpp
```

## 5. 当前缺口

### 5.1 Cube Texture RHI

RHI 虽然声明了 Cube Texture 维度，但目前尚未形成可用的端到端实现：

- `CreateCube()` 没有自动建立六个 array layers。
- Vulkan Image 创建没有设置 Cube Compatible 标记。
- `RHIUpdateTexture2D()` 的上传目标固定为 array layer 0。
- 没有能够指定 face/array layer 的通用纹理上传接口。
- 缺少六面所有 mip 的 layout transition 和上传测试。
- 缺少立方体尺寸、格式和 layer 数量约束验证。

### 5.2 天空资源

当前只有 `DTexture2D`，没有：

- `DTextureCube`。
- Cube Texture 平台数据和 Render Resource。
- 六面图导入或经纬度图转换流程。
- Cube Texture 缩略图和预览。
- 默认或错误回退 Cubemap。

当前图片解码流程只公开 PNG、JPEG、BMP 和 TGA，并统一生成 RGBA8 数据。HDR 浮点源数据尚未进入资产管线。

### 5.3 场景数据

当前 `IScene` 主要保存 Primitive 和 Directional Light，没有天空或环境数据。天空功能需要决定由以下哪一种对象拥有：

- World/Level Settings。
- 独立 Sky Component/Sky Actor。
- View 级编辑器设置。
- Renderer 全局设置。

无论选择哪种方式，运行时对象都应向渲染场景提交不可变或线程安全的快照，Renderer 不应直接读取反射对象。

### 5.4 Renderer

当前 `RenderScene()` 依次绘制静态网格、编辑器网格和 Overlay，没有天空绘制阶段。静态天空至少需要：

- 天空 Shader 和 Shader 参数定义。
- TextureCube 或 Texture2D 全景图采样。
- 独立 Graphics Pipeline State。
- 只保留相机旋转的观察方向计算。
- 明确的深度测试、深度写入、剔除和绘制顺序。
- Lit、Unlit、Wireframe 等 View Mode 下的显示策略。
- 主视口、辅助相机预览和独立游戏窗口的一致行为。

### 5.5 环境光照

当前方向光只提供简单直接光照和 Ambient Intensity。若天空还要参与物体照明，需要额外实现：

- Diffuse irradiance convolution 或其他低频表示。
- Specular prefiltered environment map。
- BRDF integration LUT。
- 材质 roughness、metallic 和反射模型支持。
- 天空资源变化后的预计算、缓存和更新策略。

这些工作不属于“只显示天空背景”的必要条件。

## 6. 后续选型需要回答的问题

正式确定实现方案前，需要先明确产品需求：

1. 第一阶段只需要可见背景，还是必须同时影响物体照明与反射？
2. 输入资源是六面图片、经纬度 HDRI，还是两者都需要？
3. 是否必须支持 HDR 和高动态范围后处理？
4. 是否需要动态太阳、昼夜循环和物理大气？
5. 天空由 Level、World、Actor、Component 还是 View Settings 拥有？
6. 编辑器需要怎样的导入、预览和场景配置体验？
7. 移动平台或低端 GPU 是否属于近期目标？
8. 第一版是否允许使用 Texture2D 全景图作为过渡实现？
9. 天空背景、环境光照和反射捕获是否必须使用同一资源？
10. 后续是否计划加入云、雾和空气透视，要求当前接口预留组合能力？

## 7. 验证维度

无论最终选择哪条路线，都应覆盖以下验证：

- 相机平移不会造成无限远天空发生视差。
- 相机旋转和坐标轴方向正确。
- 主视口、辅助相机预览、PIE 和独立游戏行为一致。
- 固定宽高比和视口缩放不会拉伸天空。
- 天空与 Scene Depth、Mesh、Editor Grid 和 Overlay 的顺序正确。
- 资源缺失、尚未就绪或被替换时有稳定回退。
- 多 mip 采样不会出现明显接缝。
- Vulkan Validation 没有 Image View、layout 或 descriptor 错误。
- 完整构建后通过 DurinEditor 运行时 Shader 和渲染 smoke test。

## 8. 当前结论

现阶段只能确认 Durin 已具备实现天空渲染的部分基础，但 Cube Texture、天空场景表达和 Renderer 绘制阶段仍不完整。Cubemap、二维全景图、Skydome Material 和程序化物理大气均是可选路线。

本文不确定第一版的技术方案和实施顺序。后续应根据第 6 节的产品需求、目标平台和环境光照范围完成技术选型，再形成正式架构设计与实施计划。
