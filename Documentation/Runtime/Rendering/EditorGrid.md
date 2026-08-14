# Editor Grid

Summary: Define the procedural editor grid's screen-space generation, camera-relative precision, world anchoring, adaptive appearance, scene-depth ordering, and failure behavior.

Modules: RenderCore, Renderer, LevelEditor, RHI

Last reviewed: 2026-08-14

## Purpose and mental model

The Editor Grid is an editor-assistance visualization of one infinite
horizontal world plane. It is not a finite mesh, Terrain topology, or scene
primitive. The Renderer draws one fullscreen triangle and lets every fragment
reconstruct the view ray that passes through its pixel. A fragment appears only
when that ray reaches the configured grid plane and the resulting grid line is
visible at the current scale.

This design solves four separate problems:

- A finite world-space grid mesh exposes an edge and must follow the camera to
  appear infinite. Reconstructing the plane per pixel provides fullscreen
  coverage without moving geometry.
- Absolute world coordinates lose the low bits needed by periodic `frac()`
  line evaluation. Camera-relative reconstruction plus CPU-computed world
  phases keeps the arithmetic small while preserving a grid fixed to the world.
- One fixed cell size becomes either too dense or too sparse as projected scale
  changes. Three decimal levels cross-fade according to world units per pixel.
- The grid must respect scene occlusion without modifying scene depth. It
  outputs its reconstructed depth, tests against preserved scene depth, and
  leaves depth writes disabled.

The complete path is:

```text
Level Editor builds FSceneView
  -> EditorGridRendering::BuildUniform derives camera-relative matrices,
     relative plane height, decimal world phases, fade policy, and colors
  -> FEditorGridRenderer prepares the demanded shader and output-keyed pipeline
  -> shared fullscreen triangle covers the fitted viewport
  -> fragment shader reconstructs one camera-relative ray
  -> ray intersects the horizontal plane
  -> derivatives select and antialias decimal grid levels
  -> the shader emits color plus minimal coplanar depth priority
  -> preserved scene depth occludes the grid
```

## Render-phase ownership

`FEditorGridRenderer` is a Renderer-private feature beneath
`FEditorAssistanceRenderer`. It owns the Editor Grid shader map, vertex
declaration, Present/Offscreen pipeline variants, preparation, drawing, and
resource release. It reuses `FFullscreenGeometryResources`; it does not own a
dedicated grid vertex or index mesh.

The scene and post-process phases complete before editor assistance. The final
assistance pass loads final color and preserved D32 scene depth, restores the
view's fitted viewport and scissor, then draws the grid before gizmos, overlay
lines, and overlay icons. The grid pipeline uses straight-alpha blending, no
culling, the main view's `GreaterOrEqual` reversed-Z comparison, and disabled depth writes. The grid therefore
participates in scene occlusion but never changes the depth observed by later
editor assistance.

An `FSceneView` with `EditorGrid.bVisible == false` creates no grid demand.
Invalid uniform construction or unavailable shader, pipeline, or fullscreen
geometry skips only the grid operation; other editor-assistance features remain
independently drawable.

## CPU uniform construction

`EditorGridRendering::BuildUniform` converts one immutable `FSceneView` into
the exact shader snapshot. The CPU performs precision-sensitive work in double
precision before converting bounded results to float.

### Camera-relative transform

The ordinary view-projection matrix contains the camera's absolute world
translation. Inverting that complete matrix and unprojecting directly to world
space makes the shader subtract large near/far positions and later evaluate
periodic lines from large absolute coordinates. Both operations lose the low
bits that a one-pixel grid line needs.

Grid construction copies `ViewMatrix`, removes its translation, and builds:

```text
RelativeWorldToClip = Projection * translation-free View
ClipToRelativeWorld = inverse(RelativeWorldToClip)
```

These matrices retain camera rotation and projection but treat the camera as
the relative-world origin. Moving the camera from world `X = 0` to
`X = 1,000,000` no longer changes the magnitude of reconstructed positions.
The two matrices are converted to the established shader matrix layout only
after finite, nonsingular double-precision construction succeeds.

### Relative plane height

The configured plane remains the exact horizontal world plane
`Z = EditorGrid.Height`. The CPU subtracts camera height in double precision:

```text
RelativeGridHeight = EditorGrid.Height - ViewLocation.z
```

The shader consequently intersects a small camera-relative height instead of
subtracting two potentially large float world heights.

### Decimal world phases

Camera-relative coordinates alone would make the pattern follow the camera.
The grid must instead remain anchored at exact world multiples of each spacing.
For every decimal spacing from `10^-4` through `10^8`, the CPU computes in
double precision:

```text
Phase(spacing) = PositiveModulo(CameraWorldXY, spacing) / spacing
```

`PositiveModulo` maps negative coordinates into `[0, spacing)`, so crossing
the world origin is continuous. The shader evaluates a level with:

```text
coordinate = RelativeHitXY / spacing + Phase(spacing)
```

Taking `frac(coordinate)` is mathematically equivalent to evaluating the
absolute world position, but the values that carry the periodic fraction stay
small. Separate phases are required because a remainder for spacing `1` cannot
represent the world anchoring of spacing `10`, `100`, or other levels.

The absolute float camera XY retained in the uniform is used only to locate the
unique red/green world axes. Ordinary repeated grid lines never add the full
camera position back to the relative hit.

## Fragment generation

### Fullscreen coverage and ray reconstruction

The vertex shader forwards the shared fullscreen triangle's clip XY and emits
it directly as `SV_Position`. It does not construct world-space grid vertices.

For each covered pixel, the fragment shader unprojects the convention-selected
near depth and device depth `0.5` through `ClipToRelativeWorld`, performs
homogeneous division, and forms a normalized ray. Any two depths define the
same view ray; the middle sample avoids the large float magnitude and precision
loss of a very long or infinite far endpoint. Main reversed-Z perspective views
select near depth `1`.

The CPU also transforms the relative horizontal plane into homogeneous clip
space. The shader solves this plane equation at the pixel's NDC XY, then
unprojects that solved depth to obtain the relative hit position.

The shader rejects a fragment when:

- either homogeneous point is non-finite or has unusable `w`;
- the near/middle points do not form a finite ray;
- the ray is effectively parallel to the horizontal plane;
- the clip plane cannot be solved at the pixel;
- the solved hit is outside depth range `0..1` or cannot be unprojected;
- the hit reaches or exceeds the configured fade distance.

For an accepted ray:

```text
HitDepth = -(ClipPlane.x * NdcX + ClipPlane.y * NdcY + ClipPlane.w)
           / ClipPlane.z
RelativeHit = ClipToRelativeWorld * (NdcX, NdcY, HitDepth, 1)
```

`RelativeHit` is the authoritative visual grid position. All line derivatives,
distance fading, and line coordinates derive from it.

### Distance and grazing-angle fades

The grid begins its smooth distance fade at 55 percent of `FadeDistance` and
reaches zero at the full distance. The hard distance discard occurs only after
the smooth fade has reached zero, avoiding a visible finite edge.

Near the horizon, projected grid cells collapse into a noisy band. The shader
therefore fades using the absolute vertical ray component, transitioning over
`abs(RayDirection.z) = 0.025..0.16`. The numerical parallel-ray rejection lies
beyond the visible fade and should not expose a hard horizon seam.

## Adaptive decimal grid

The shader first evaluates `ddx` and `ddy` of continuous `RelativeHit.xy`.
Discrete LOD selection never participates in the derivative expression; this
prevents a 2x2 derivative quad from straddling a 10x level change and producing
a false sawtooth boundary.

The larger derivative length estimates world units per pixel. The target cell
scale is approximately 24 pixels:

```text
logarithmicSpacing = log10(max(WorldUnitsPerPixel * 24, 10^-4))
lowerExponent = clamp(floor(logarithmicSpacing), -4, 6)
lowerSpacing = 10^lowerExponent
```

The upper two sampled levels are `lowerSpacing * 10` and
`lowerSpacing * 100`, so the lower exponent stops at `6` while the phase table
extends through exponent `8`.

Three line evaluations provide:

- the current fine/minor level;
- the next decimal level, used as both the incoming minor level and current
  major level;
- the following decimal level, used as the incoming major level.

The fractional logarithmic spacing cross-fades over `0.15..0.85`. At a decade
boundary, the previous upper level and next lower level represent the same
world lines, so density and major-line identity remain continuous.

`GridLine` converts camera-relative position to phase-correct normalized cell
coordinates. Its component-wise derivative width produces analytic one-pixel
coverage, and the minimum of X/Y distance forms the union of both grid
directions. Minor and major alpha are combined before world-axis color takes
priority. The red X axis is `world Y = 0`; the green Y axis is
`world X = 0`.

## Depth ordering and coplanar priority

The CPU transforms the world grid plane into homogeneous clip space in double
precision. The fragment shader solves that clip-plane equation directly at the
pixel's NDC coordinate, then unprojects the solved hit for line appearance.
Planar depth therefore follows the same screen-space interpolation model as
scene triangles without accumulating ray-intersection/reprojection error.

Scene geometry is already present in the loaded D32 depth attachment. Exact
coplanarity cannot rely on bit-identical depth because scene meshes obtain
depth through vertex projection and raster interpolation, while the grid uses
fragment plane evaluation. The grid therefore moves only its submitted D32
value through a bounded window of 1024 representable float steps toward the
camera. Reversed-Z increments the positive float depth value; forward-Z
decrements it. For normalized positive floats this is approximately `1.2e-4`
relative depth.

This bounded priority covers the numerical disagreement observed while a
camera rotates over coplanar Terrain without moving the plane by a world-space
distance. Geometry with a meaningfully closer depth still occludes the grid,
and depth writes remain disabled. The design deliberately avoids both a fixed
normalized offset and a world-space view-ray offset: either can represent an
unexpectedly large separation at some distance and suppress the grid across a
broad surface.

A fixed-point or integer depth format requires a format-specific
representable-step policy.

## Problems solved by the current design

### Finite-grid edge and camera-following geometry

Fullscreen ray-plane reconstruction removes finite geometry extent. Distance
and angle fades, rather than mesh boundaries, define visibility.

### Large-world line fragmentation

The former complete inverse view-projection path reconstructed large absolute
positions. Precision loss in `far - near` and
`frac(WorldPosition / Spacing)` could make neighboring pixels alternate
between line and non-line coverage, especially in large Terrain scenes.
Translation-free matrices keep ray math camera-relative, while double-precision
decimal phases retain exact world anchoring without restoring large absolute
coordinates in the periodic path.

### Decimal LOD popping and derivative seams

Continuous-position derivatives are evaluated before discrete spacing
selection, and adjacent decimal levels cross-fade through shared world lines.
This avoids whole-region scale pops and false derivative boundaries.

### Coplanar grid disappearance and z-fighting

The visual plane remains exact, while its emitted depth receives a bounded D32
camera-side tolerance. Direct clip-plane depth plus that tolerance keeps the
grid stable across camera rotation on a coplanar Terrain surface without
allowing it to pass genuinely closer geometry.

### Scene and post-process interaction

The grid renders after scene post-processing, so FXAA does not blur the editor
reference lines. Loading preserved scene depth retains mesh occlusion, while
drawing the grid before other assistance leaves gizmos, icons, and overlay
lines readable over it.

## Limits and extension rules

- The qualified work plane is horizontal. An arbitrary plane requires an
  explicit normal plus stable two-dimensional grid basis and phase definition.
- The active decimal exponent family is fixed to `10^-4..10^8`, with lower
  sampling clamped to `10^-4..10^6`. Extending it requires matching CPU phase
  storage, shader bounds, uniform layout assertions, and tests.
- The line family is decimal rather than `1/2/5`; snapping and displayed units
  are separate future contracts.
- Perspective views are the qualified path. The near/far unprojection form is
  compatible in principle with orthographic rays, but orthographic appearance
  and density policy are not currently qualified.
- Camera-relative reconstruction fixes absolute-translation precision. It does
  not guarantee that every subpixel line remains visually stable at extreme
  grazing angles or unusual viewport resolutions; remaining defects in those
  cases belong to analytic coverage, derivative width, or fade policy.
- The unique world axes still use float camera XY. They are useful only when
  the corresponding world-zero axis lies within the finite fade radius; the
  repeated grid pattern does not share this limitation.
- The D32 ULP adjustment must be revisited together with any depth format,
  clear value, comparison direction, or reversed-Z change.

## Failure diagnosis

Use the symptom to isolate the responsible stage:

| Symptom | First check |
| --- | --- |
| Lines fragment at large absolute coordinates but not near origin | Camera-relative matrices and decimal phase upload/indexing |
| Entire distant region disappears | Fade distance, grazing fade, projected depth range, and far-plane saturation |
| Only intersections with scene surfaces flicker | Coplanar depth separation, D32 format, and the submitted depth convention |
| Scale changes as a sharp band | Derivatives occurring after LOD selection or broken decimal transition continuity |
| Grid moves when the camera translates | Missing or incorrect per-spacing world phase |
| Grid has a hard outer edge | Early hard discard or accidental finite geometry/scissor mismatch |
| Grid is soft while overlays are sharp | Grid was moved back into the scene post-process phase |

A useful rendering isolation order is:

1. Hide scene geometry while preserving the identical camera.
2. Temporarily use an always-pass grid depth state to separate depth rejection
   from line generation.
3. Output solid color after ray intersection to separate reconstruction and
   clipping from `GridLine`/LOD coverage.
4. Restore lines one level at a time, then restore blending and depth ordering.

## Validation contract

Focused CPU tests verify mutually inverse camera-relative transforms, relative
height, finite failure behavior, and positive decimal phases at million-scale
positive and negative camera coordinates. The hardware-backed Vulkan regression
compares empty and coplanar-Terrain captures across rotated reversed-Z views,
then verifies that meaningfully closer Terrain still occludes the grid. A full
editor build verifies the C++ uniform layout. Real Vulkan startup must compile
`/Engine/EditorGrid`, create the demanded pipeline, render a first frame, and
produce no Shader, Pipeline, or Validation error.

Visual validation should retain the same camera while toggling Terrain or
other geometry, then cover near/far views, large positive and negative world
coordinates, origin crossing, grazing angles, decade transitions, exact
coplanarity, geometry slightly in front/behind the plane, Present/Offscreen
outputs, and FXAA on/off.

## Related documentation

- [Viewport Rendering](ViewportRendering.md)
- [Graphics State and Bindings](GraphicsStateAndBindings.md)
- [Editor World Grid V2 historical plan](../../Plans/Archive/2026-07/EditorWorldGridV2.md)
- [Scene Post-Process and Editor-Assistance historical plan](../../Plans/Archive/2026-07/ScenePostProcessEditorAssistanceBoundary.md)

## Related code

- `Engine/Shaders/Slang/EditorGrid.slang`
- `Engine/Source/Runtime/Renderer/Private/EditorGridRendering.h`
- `Engine/Source/Runtime/Renderer/Private/EditorGridRendering.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/EditorAssistance/EditorGridRenderer.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/EditorAssistance/EditorGridRenderer.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/EditorAssistance/EditorAssistanceRenderer.cpp`
- `Engine/Source/Runtime/Renderer/Private/Resources/RenderTargetLayouts.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Viewport/LevelEditorViewportClient.cpp`
- `Engine/Tests/Native/EngineTests/Private/EditorGridRenderingTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/EditorGridVulkanTests.cpp`
