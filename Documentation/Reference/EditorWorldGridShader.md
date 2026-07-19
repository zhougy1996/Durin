# 编辑器世界网格 Shader 实现参考

本文记录 Durin 编辑器世界网格的当前实现、数学含义、渲染顺序、调参方式和已知约束。它是供实现维护和问题排查使用的参考资料，不是架构规范，也不要求被 `AGENTS.md` 或其他文档索引。

## 1. 功能目标

编辑器世界网格用于在缺少场景物体时提供稳定的空间参照。当前实现具有以下行为：

- 网格位于世界空间 `Z = Height` 平面，Level Editor 默认使用 `Z = 0`。
- 网格中心在 XY 平面跟随编辑器相机，避免使用一个固定且极大的世界网格 Mesh。
- 网格线保持世界坐标对齐，不会跟随相机旋转。
- 根据屏幕像素密度自动选择十进制间距，使远近视角都能看到合适密度的线。
- 相邻 LOD 交叉淡化，尽量避免缩放时发生突然跳变。
- X 轴使用红色，Y 轴使用绿色，并覆盖普通网格线。
- 网格随观察距离和掠射角淡出，降低远处闪烁和地平线摩尔纹。
- 网格参与深度测试但不写入深度，因此会被场景物体遮挡，也不会反过来遮挡后续物体。
- 整个网格只提交一次三角形绘制，不按网格线逐条生成几何。

核心 Shader 位于：

```text
Engine/Shaders/Slang/EditorGrid.slang
```

相关 CPU 和渲染代码位于：

```text
Engine/Source/Editor/LevelEditor/Private/Viewport/LevelEditorViewportClient.cpp
Engine/Source/Runtime/RenderCore/Public/IRendererModule.h
Engine/Source/Runtime/Renderer/Private/RendererModule.cpp
```

## 2. 数据流概览

网格数据从 Level Editor 传入 `FSceneView`，再由 Renderer 转换为动态 Uniform Buffer：

```text
FLevelEditorViewportClient::CalcSceneView()
  填充 FSceneView::EditorGrid
        |
        v
FRendererModule::RenderViewFamily()
  检查 EditorGrid.bVisible
        |
        v
EnsureEditorGridResources()
  创建 Shader、顶点声明和 Pipeline
        |
        v
DrawEditorGrid()
  填充 FEditorGridUniform
  绑定共享全屏三角形
  DrawIndexed(3)
        |
        v
EditorGrid.slang
  VertexMain: 生成世界平面
  FragmentMain: 程序化计算网格颜色与透明度
```

`FViewEditorGrid` 保存的是 View 级描述，而不是一个运行时 Scene Component。这样网格只存在于编辑器可视化层，不影响游戏场景和资源序列化。

## 3. View 参数

Shader 的 `GridUniform` 与 Renderer 侧的 `FEditorGridUniform` 一一对应：

```slang
struct GridUniform
{
    float4x4 WorldToClip;
    float4 CenterHeightExtent;
    float4 ViewPositionFadeDistance;
    float4 MinorColor;
    float4 MajorColor;
    float4 AxisXColor;
    float4 AxisYColor;
};
```

各字段含义如下：

| 字段 | 分量 | 含义 |
| --- | --- | --- |
| `WorldToClip` | 全部 | 当前编辑器视口的世界到裁剪空间矩阵 |
| `CenterHeightExtent` | `xy` | 网格平面中心，使用编辑器相机的世界 XY |
| `CenterHeightExtent` | `z` | 网格世界高度 |
| `CenterHeightExtent` | `w` | 网格三角形半范围 |
| `ViewPositionFadeDistance` | `xyz` | 编辑器相机世界位置 |
| `ViewPositionFadeDistance` | `w` | 网格淡出距离 |
| `MinorColor` | `rgba` | 次网格颜色和最大透明度 |
| `MajorColor` | `rgba` | 主网格颜色和最大透明度 |
| `AxisXColor` | `rgba` | 世界 X 轴颜色和透明度 |
| `AxisYColor` | `rgba` | 世界 Y 轴颜色和透明度 |

Level Editor 当前使用以下默认关系：

```text
Height              = 0
FadeDistance        = EditorFarClip * 0.95
TriangleExtent      = FadeDistance * 1.1
MinorAlpha          = ViewportTextAlpha * 0.14
MajorAlpha          = ViewportTextAlpha * 0.32
AxisAlpha           = ThemeAxisAlpha * 0.82
```

三角形范围略大于淡出范围，目的是让网格在到达几何边界前已经完全透明，避免看到有限平面的硬边。

## 4. 为什么只画一个三角形

网格不是由大量 Line Primitive 构成。Renderer 复用 Post Process 的全屏三角形顶点：

```text
(-1, -1)
( 3, -1)
(-1,  3)
```

Vertex Shader 将这三个二维位置解释为以相机 XY 为中心的世界平面坐标：

```slang
output.worldPosition = float3(
    Grid.CenterHeightExtent.xy + input.pos * Grid.CenterHeightExtent.w,
    Grid.CenterHeightExtent.z
);
```

随后使用 `WorldToClip` 投影到屏幕：

```slang
output.pos = mul(Grid.WorldToClip, float4(output.worldPosition, 1.0));
```

采用超大三角形而不是普通四边形有两个目的：

- 只需三个顶点和一次 `DrawIndexed(3)`。
- 不存在由两个三角形拼成四边形时可能出现的内部插值接缝。

`worldPosition` 经过透视正确插值后传入 Fragment Shader，因此每个像素都能恢复它在世界网格平面上的 XY 位置。

这不是数学意义上的无限网格。它是一个跟随相机移动、在几何边界前完成淡出的有限程序化平面。

## 5. 距离淡出

Fragment Shader 首先计算网格像素到编辑器相机的三维距离：

```slang
float3 toView = Grid.ViewPositionFadeDistance.xyz - input.worldPosition;
float distanceToView = length(toView);
```

距离淡出从 `FadeDistance * 0.55` 开始，在 `FadeDistance` 处变为零：

```slang
float distanceFade = 1.0 - smoothstep(
    fadeDistance * 0.55,
    fadeDistance,
    distanceToView
);
```

选择 `smoothstep` 而不是线性截断，可以避免远端出现清晰的圆形边界。

调整规则：

- 减小 `0.55` 会扩大淡出区，远处变化更柔和。
- 增大 `0.55` 会保留更多完整透明度区域，但更容易观察到淡出边界。
- `FadeDistance` 不应超过网格几何可覆盖范围。

## 6. 掠射角淡出

当网格接近侧视时，大量平行线会被压缩到少数像素中。即使单条线使用导数抗锯齿，整体仍可能产生闪烁和摩尔纹。

当前实现使用视线方向的垂直比例衡量观察角度：

```slang
float angleFade = smoothstep(
    0.025,
    0.16,
    abs(toView.z) / max(distanceToView, 1.0e-5)
);
```

其中：

```text
abs(toView.z) / distanceToView
```

可以理解为视线相对地面的仰角或俯角强度：

- 接近 `0`：视线几乎平行于网格，网格淡出。
- 数值较大：从较高角度观察网格，正常显示。

`0.025` 到 `0.16` 是经验区间。它主要控制地平线附近的稳定性，不应被用来控制普通远距离淡出。

## 7. 屏幕导数与世界单位/像素

### 7.1 导数含义

GPU Fragment Shader 以小像素块执行。`ddx()` 和 `ddy()` 用相邻像素估计输入值在屏幕 X/Y 方向的变化。

当前实现先对连续的世界坐标求导：

```slang
float2 worldPositionDx = ddx(input.worldPosition.xy);
float2 worldPositionDy = ddy(input.worldPosition.xy);
```

它们表达：

- 屏幕横向移动一个像素，网格平面世界 XY 大约变化多少。
- 屏幕纵向移动一个像素，网格平面世界 XY 大约变化多少。

用于选择 LOD 的保守标量为：

```slang
float worldUnitsPerPixel = max(
    length(worldPositionDx),
    length(worldPositionDy)
);
```

用于单条 X/Y 网格线抗锯齿的分量宽度为：

```slang
float2 worldPositionWidth = max(
    abs(worldPositionDx) + abs(worldPositionDy),
    float2(1.0e-5)
);
```

这与对连续的 `worldPosition.xy` 使用 `fwidth()` 等价，但它被显式保存在 LOD 计算之前。

### 7.2 必须先求导，再选择 LOD

这是当前实现最重要的稳定性约束之一。

错误写法：

```slang
float2 coordinate = worldPosition / spacing;
float2 width = fwidth(coordinate);
```

`spacing` 并不是连续变量。它通过 `floor(logarithmicSpacing)` 选择十进制 LOD，会在相邻 LOD 之间跳变 10 倍。如果一个 GPU 导数像素块横跨 LOD 边界，`fwidth(worldPosition / spacing)` 会把这个 10 倍离散跳变误认为屏幕坐标变化。

结果不是普通的摩尔纹，而是一条沿 LOD 等值线出现的锯齿带。它通常表现为：

- 接近横向或缓慢倾斜的长线。
- 由小三角或短虚线状块组成。
- 随视角和相机高度移动。
- 网格在线的两侧仍然连续存在。

正确写法是先计算连续世界坐标的导数，再对每个已选定的固定间距进行换算：

```slang
float2 worldPositionWidth = fwidth(worldPosition);
float2 coordinateWidth = worldPositionWidth / spacing;
```

维护时不要把 `fwidth()` 移回 `GridLine()` 内部并直接作用于包含动态 `spacing` 的表达式。

## 8. 十进制 LOD 选择

网格希望每个单元在屏幕上大约占据 24 像素。理想世界间距近似为：

```text
idealSpacing = worldUnitsPerPixel * 24
```

为了得到便于判断尺度的工程网格，实际间距只选择 10 的整数次幂：

```slang
float logarithmicSpacing = log10(max(worldUnitsPerPixel * 24.0, 1.0e-4));
float lowerSpacing = pow(10.0, floor(logarithmicSpacing));
```

例如：

| `worldUnitsPerPixel * 24` | `lowerSpacing` | 同时采样的三级间距 |
| ---: | ---: | ---: |
| `0.37` | `0.1` | `0.1 / 1 / 10` |
| `3.7` | `1` | `1 / 10 / 100` |
| `37` | `10` | `10 / 100 / 1000` |

`frac(logarithmicSpacing)` 表示当前位于一个十倍区间中的位置。为了在区间两端保留稳定区域，只在 `0.15` 到 `0.85` 之间进行平滑过渡：

```slang
float transition = smoothstep(
    0.15,
    0.85,
    frac(logarithmicSpacing)
);
```

24 像素目标控制总体密度：

- 数值增大：更早切换到较粗网格，屏幕上的线更稀疏。
- 数值减小：保留更细的网格，线更密集，也更容易产生远处闪烁。

## 9. 单级网格线计算

`GridLine()` 将世界坐标转换到指定间距的网格坐标：

```slang
float2 coordinate = worldPosition / spacing;
```

整数坐标对应网格线。以下表达式计算当前点到最近整数线的周期距离：

```slang
abs(frac(coordinate - 0.5) - 0.5)
```

计算结果的每个分量位于接近 `0..0.5` 的范围：

- X 分量接近零，说明当前像素靠近一条与 Y 方向平行的线。
- Y 分量接近零，说明当前像素靠近一条与 X 方向平行的线。

世界坐标的像素宽度除以网格间距，得到网格坐标中的抗锯齿宽度：

```slang
float2 width = max(
    worldPositionWidth / spacing,
    float2(1.0e-5)
);
```

距离再除以像素宽度，转换成大致以一个像素为单位的覆盖率：

```slang
float2 distanceToLine = distance / width;
```

两组正交线取较近的一组：

```slang
return 1.0 - min(
    min(distanceToLine.x, distanceToLine.y),
    1.0
);
```

返回值：

- `1`：位于网格线中心。
- `0..1`：位于线边缘的抗锯齿过渡。
- `0`：不属于任何网格线。

## 10. 三级网格与交叉淡化

每个像素同时计算三级十进制网格：

```slang
lowerLine     = GridLine(spacing)
upperLine     = GridLine(spacing * 10)
nextMajorLine = GridLine(spacing * 100)
```

它们的职责是：

- `lowerLine`：当前最细一级。
- `upperLine`：当前主网格，同时也是下一个 LOD 的细网格。
- `nextMajorLine`：下一个主网格。

次网格覆盖率：

```slang
minorAmount = lowerLine * (1.0 - transition);
upperMinorAmount = upperLine * transition;
```

主网格覆盖率：

```slang
majorAmount = upperLine * (1.0 - transition)
    + nextMajorLine * transition;
```

这种三级采样保证 LOD 跨过整数 decade 时语义能够连续平移：

```text
旧 LOD 的 upperLine     -> 新 LOD 的 lowerLine
旧 LOD 的 nextMajorLine -> 新 LOD 的 upperLine
```

普通网格和主网格的透明度取较强者：

```slang
neutralAlpha = max(
    minorCoverage * MinorColor.a,
    majorCoverage * MajorColor.a
);
```

这样主网格交叉处不会因为两层简单相加而过亮。

## 11. 世界轴线

世界轴线独立于动态 LOD，不会随缩放消失：

```slang
float xAxis = AxisLine(input.worldPosition.y);
float yAxis = AxisLine(input.worldPosition.x);
```

注意坐标关系：

- 世界 X 轴上的所有点满足 `y = 0`，所以红色 X 轴使用 `worldPosition.y`。
- 世界 Y 轴上的所有点满足 `x = 0`，所以绿色 Y 轴使用 `worldPosition.x`。

`AxisLine()` 使用连续世界坐标直接计算 `fwidth()`，不涉及离散 LOD，因此不存在第 7.2 节所述问题。

轴线颜色依次覆盖普通网格：

```slang
color.rgb = lerp(color.rgb, AxisXColor.rgb, xAxis);
color.rgb = lerp(color.rgb, AxisYColor.rgb, yAxis);
```

原点处 Y 轴最后混合，因此当两条轴完全重叠时绿色具有最终颜色优先级。透明度取普通网格和轴线中的最大值。

## 12. 最终透明度与丢弃

所有颜色和覆盖率计算完成后，统一应用距离淡出与角度淡出：

```slang
color.a *= distanceFade * angleFade;
```

极低透明度像素被丢弃：

```slang
if (color.a <= 0.002)
    discard;
```

这样可以避免对几乎不可见的远端区域继续执行 Alpha Blend，也使有限三角形边缘在正常参数下不可见。

## 13. Pipeline 与渲染顺序

`EnsureEditorGridResources()` 创建的 Pipeline 具有以下状态：

```text
Alpha Blend       = Enabled
Back-face Culling = Disabled
Depth Test        = Enabled
Depth Write       = Disabled
```

关闭背面剔除允许从网格上方和下方查看。开启深度测试使场景 Mesh 可以遮挡网格；关闭深度写入避免网格作为透明编辑器辅助层污染场景深度。

当前主场景 Pass 中的相对绘制顺序为：

```text
Static Meshes
Editor World Grid
Editor Overlay Lines / Icons / Gizmos
```

因此：

- 网格在已有场景深度之后绘制，会被 Mesh 遮挡。
- 相机视锥、图标和 Transform Gizmo 在网格之后绘制，仍然清晰可见。
- 网格不是 ImGui 绘制内容，能够使用场景深度和 Renderer Pipeline。

## 14. 性能特征

每个可见编辑器 View 的网格成本大致为：

- 一个共享静态三角形。
- 一次动态 Uniform Buffer 分配。
- 一次 Pipeline 绑定。
- 一次 `DrawIndexed(3)`。
- 覆盖区域内每像素执行若干导数、对数、幂、周期距离和淡出运算。

它不随可见网格线数量增加 Draw Call 或顶点数量。主要成本是像素覆盖率，因此距离淡出和角度淡出不仅改善视觉稳定性，也限制了无效像素工作。

如果未来需要进一步优化，优先考虑：

1. 缩小实际覆盖区域或更早丢弃地平线区域。
2. 在不破坏 LOD 连续性的前提下减少网格级别采样。
3. 使用 Shader 性能分析确认 `log10()` 和 `pow()` 是否为真实瓶颈。

不要为了减少几次数学运算改回逐线 CPU 几何生成；那会显著增加动态顶点、分配和 Draw Call 管理成本。

## 15. 常用调参位置

| 目标 | 参数或代码 | 当前值 | 主要副作用 |
| --- | --- | ---: | --- |
| 调整网格屏幕密度 | `worldUnitsPerPixel * 24.0` | `24` px | 太小容易密集闪烁，太大缺少尺度细节 |
| 调整 LOD 过渡范围 | `smoothstep(0.15, 0.85, ...)` | `0.15..0.85` | 范围太窄会看到切换，太宽会长期混合多级线 |
| 调整距离淡出起点 | `fadeDistance * 0.55` | `55%` | 太晚可能看到几何边界，太早会缩小参考范围 |
| 调整地平线淡出 | `smoothstep(0.025, 0.16, ...)` | `0.025..0.16` | 太激进会在低视角丢失网格，太宽松会产生摩尔纹 |
| 调整次线透明度 | Level Editor `MinorColor.a` | `0.14` | 太高使画面拥挤 |
| 调整主线透明度 | Level Editor `MajorColor.a` | `0.32` | 太低不易判断十进制层级 |
| 调整轴线透明度 | Level Editor Axis alpha | `0.82` | 太高可能压过场景内容 |
| 调整网格范围 | `FadeDistance` | `FarClip * 0.95` | 增大会提高远处像素覆盖并接近裁剪极限 |

## 16. 问题诊断指南

### 16.1 一条由小三角组成的横向或斜向长线

优先检查导数是否包含离散 LOD 间距：

```text
错误：fwidth(worldPosition / dynamicallySelectedSpacing)
正确：fwidth(worldPosition) / selectedSpacing
```

该问题通常沿 LOD 等值线出现，不属于普通摩尔纹。

### 16.2 地平线附近出现高频闪烁

检查：

- `angleFade` 是否仍在最终 Alpha 上应用。
- 目标单元像素数是否被调得过小。
- `worldUnitsPerPixel` 是否仍使用世界坐标屏幕导数。
- 是否关闭了 FXAA 或其他最终抗锯齿路径。

### 16.3 远处出现清晰边界

检查：

- 三角形范围是否仍大于 `FadeDistance`。
- `distanceFade` 是否在边界前降到零。
- View 传入的 `FadeDistance` 是否为正数且与编辑器 Far Clip 匹配。

### 16.4 网格穿过场景物体

检查 Pipeline 是否仍然：

```text
Depth Test  = Enabled
Depth Write = Disabled
```

同时检查网格是否仍在 Static Mesh 之后、Overlay 之前绘制。

### 16.5 轴线颜色方向错误

确认：

```text
X axis -> y = 0 -> AxisLine(worldPosition.y)
Y axis -> x = 0 -> AxisLine(worldPosition.x)
```

不要因为轴名称是 X 就把 `worldPosition.x` 传给 X 轴距离函数。

### 16.6 缩放时出现明显明暗跳变

检查三级网格的映射关系和过渡权重。十进制边界前后的级别必须能连续重命名，不能只绘制一个会瞬间变为 10 倍间距的单级网格。

## 17. 修改后的验证建议

网格 Shader 修改至少应检查以下视角：

1. 低空接近水平观察，确认地平线附近平滑淡出。
2. 中等高度俯视并缓慢缩放，确认 LOD 没有跳变或横向锯齿带。
3. 高空俯视，确认大尺度主网格仍能提供方向和距离参照。
4. 相机跨过世界原点，确认红色 X 轴和绿色 Y 轴位置稳定。
5. 网格上放置静态 Mesh，确认深度遮挡正确且没有写深度副作用。
6. 调整视口为极窄和极宽比例，确认屏幕导数仍保持稳定。
7. 切换 Lit/Unlit 等视图模式，确认网格作为 Editor Overlay 保持一致。

仓库级验证应遵循项目构建规则：

```text
BuildTool.bat build --target all --plain
```

由于 Slang Shader 在运行时加载和编译，完整构建后还应启动同一 Profile 的 `DurinEditor`，确认日志包含 `/Engine/EditorGrid` 编译成功，并检查运行时没有 Shader、Pipeline 或 Validation 错误。

## 18. 当前设计边界

当前网格是面向 Level Editor 的世界 XY 参考平面，不支持：

- 任意旋转工作平面。
- 正交视口专用的不同密度策略。
- 用户配置网格基本单位或 1/2/5 工程刻度。
- 局部坐标系网格。
- 吸附设置与视觉网格间距联动。
- 无限精度的大世界坐标原点重定位。

如果未来添加这些能力，应继续保持以下不变量：

- 网格世界定位与屏幕抗锯齿职责分离。
- 离散 LOD 选择不能参与 GPU 屏幕导数。
- 几何边界必须位于完全淡出的区域之外。
- 网格参与深度测试但不写入深度。
- 网格保持单次或少量批量绘制，不退化为逐线 Draw Call。
