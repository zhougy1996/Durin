# Minimal GBuffer Contract

Summary: Define the qualified opaque/masked geometry transport consumed by deferred lighting and later screen-space effects.

Modules: RenderCore, Renderer, RHI

Last reviewed: 2026-09-03

## Scope and Ownership

The minimal GBuffer represents eligible lit opaque and masked StaticMesh and
SplineMesh draws. Unlit, translucent, special forward, sky,
and editor-assistance surfaces do not publish records. Masked rejection occurs
before any attachment write.

Production solid Lit views always execute this pass, and it is the
sole depth/material owner for eligible Lit opaque/masked records. Explicit A/B
tests may still request the isolated qualification route with the Renderer-private
`FScopedRendererQualificationPolicy`; the graph executor snapshots that policy
once and the isolated capture does not replace the selected product result.
Production `FSceneViewRenderOptions` contains no qualification route switch.
Debug replacement is separately selected by `GBufferDebugMode`; it is not a
fallback or a second production renderer.

## Record Encoding

Every extent owns four cleared color attachments and uses the existing sampled
`D32` scene depth:

| Attachment | Format | Channels |
| --- | --- | --- |
| `GBufferMaterial` | `RGBA8_UNORM` | Base color RGB, metallic A |
| `GBufferNormals` | `RGBA8_UNORM` | Octahedral shading normal RG, octahedral geometric normal BA |
| `GBufferSurface` | `RGBA8_UNORM` | Effective perceptual roughness R, ambient occlusion G, effective opacity B, flags/255 A |
| `GBufferEmissive` | `R11G11B10_FLOAT` | Finite non-negative scene-linear emissive RGB |

All attachments clear to zero. Bit 0 of the decoded flags is
`StandardLit`; a valid record has the exact flag value `1`, while zero is
background or invalid. Unused flag bits must remain zero. Primitive identity
is counter-owned and is not stored per pixel.

The shading normal includes the authored normal-map perturbation. The
geometric normal is the independently encoded, oriented visible-side normal
before normal mapping. Both use the shared octahedral contract. Base color,
metallic, authored roughness, AO, emissive, opacity, and mask decisions come
from the same material evaluation used by forward rendering rather than a
parallel material model. For standard-Lit records, `GBufferSurface.R` stores
the effective roughness after the shared derivative-based specular-AA policy
has combined authored roughness with bounded variance of the final world-space
shading normal. Deferred consumers use that stored result directly and never
filter the decoded/quantized neighboring normals a second time. Disabling the
per-view development A/B seam stores the existing authored/clamped result;
material assets and the GBuffer format do not change.

## Decode and Reconstruction

CPU validation uses `GBufferContract`; shaders include
`Material/GBufferDecode.slang`. These implementations own channel decode,
octahedral normal decode, `R11G11B10_FLOAT` interpretation, validity, and
pixel-center analytic depth reconstruction.

World position is not stored. Consumers reconstruct view-relative position
from D32 and the view projection parameters with the qualified analytic
reversed-Z perspective or orthographic equation. They must not substitute a
cancellation-prone inverse view-projection multiply. The result must be finite
and remain within `max(0.002, 3e-5 * distance-to-view)` world units of the
forward interpolant. Quantized normals allow at most `1.0 degree` angular
error. UNORM material channels allow `1/510` absolute error. Emissive allows
`1.0%` relative error in `[2^-14, 64]` and `2^-14` absolute error below that
range.

## Pass and Resource Lifecycle

The geometry pass clears once, writes every attachment, and
leaves all four color targets graphics-shader-readable. D32 follows the
existing reversed-Z clear/write contract and is shader-readable after the
pass. Static and spline pipelines differ only in vertex
transport; fragment material evaluation and record encoding are shared.

Target publication is transactional. A partially created extent is not
published. Shader or pipeline refresh retains a same-device last-known-good
payload when permitted by the renderer resource coordinator. Device
invalidation, explicit release, and shutdown clear dependent resources before
retry. A failed isolated qualification pass increments its per-family
attempted/skipped counters and leaves the selected result authoritative. A
failed production pass returns `RendererResourcesUnavailable`; it does not
present a partial image or select another lighting owner. No
view may sample stale attachments from another view or extent.

The four attachments cost exactly 16 bytes per pixel, or `33,177,600` bytes at
1920x1080. Scene Color/depth and final SDR output are accounted separately.
Allocation and retention follow [frame resource lifetimes](RendererFramePreparation.md#resource-lifetime-classes);
feature byte costs do not define independent cache quotas.

Directional contact visibility is an optional downstream consumer of material
flags, geometric normal, and D32. It accepts only standard-Lit records and owns
its separate on-demand single-channel output; it does not change GBuffer
packing or keep the GBuffer debug target resident in ordinary production.

## Diagnostics and Qualification

`FSceneViewCounters` reports attempted, successful, and skipped GBuffer draws
separately for StaticMesh and SplineMesh, plus active
attachment and retained bytes. Development captures can read the four color
attachments directly. Sampled D32 is inspected through the depth and
reconstruction debug routes because generic depth-image copies are not an RHI
contract.

Debug modes expose material, shading normal, geometric normal, surface,
emissive, flags, depth, reconstructed view position, reconstruction error, and
the combined material inputs. The material-input view is the forward/deferred
A/B seam; it is compared against the existing unlit forward fixture within
`2/255` display-channel error.

`GBufferQualificationTests` owns image, memory, and production timing gates;
[Deferred Directional Lighting](DeferredDirectionalLighting.md#memory-and-qualification)
lists the current production budgets. Main, auxiliary, preview, thumbnail,
Present, offscreen, resize, alternating-extent, reload, device-invalidation,
and shutdown coverage must remain qualified as consumers are added.

Hybrid production qualification records GBuffer, Scene, deferred lighting,
retained opaque/masked, volumetric-cloud, sorted-translucency, post-process,
and shadow intervals from the same 30-warm-up plus 120-measured frames. Scene
is the strict-LIFO outer interval around its deferred and three retained scene
subpasses; the aggregate is formed per frame as shadow + GBuffer + Scene +
post-process before statistics are calculated. Hard p95
budgets belong to this synchronized production route. Isolated GTAO and contact
route sweeps publish absolute timing characterization and retain their explicit
relative checks; independently scheduled validation batches do not gate
cross-batch absolute timing or p95 values.

Performance evidence requires an exclusive quiet GPU lane. The `durin-gpu`
resource lock serializes targets inside one CTest scheduler, but it does not
coordinate independent DevTool/CTest processes, other worktrees, applications,
or GPU-accelerated desktop tools. Results collected while those competitors are
active are diagnostic only and must not rebaseline a gate. The production route
also requires p95 to remain within 125% of its median; violation reports an
unstable qualification environment and requires a quiet rerun. Stable sustained
external load cannot be distinguished from a renderer regression by timestamps
alone, so this statistical guard does not replace exclusive execution.

## Deferred Input Contract

[Deferred lighting](DeferredDirectionalLighting.md) consumes valid standard-Lit
records, D32, immutable view parameters, and shared lighting/shadow resources.
It owns lighting composition and parity while this document owns packing,
decode, and reconstruction. Missing or incomplete required production inputs
fail the view; cleared or stale data must never be silently shaded as geometry.
Packing changes require an explicit contract and qualification rebaseline.

## Related Code

- `Engine/Source/Runtime/Renderer/Public/GBufferContract.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/GBufferRenderer.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/GBufferDebugRenderer.cpp`
- `Engine/Shaders/Slang/Material/SurfaceMaterial.slang`
- `Engine/Shaders/Slang/Material/GBufferDecode.slang`
- `Engine/Shaders/Slang/GBufferDebug.slang`

## Related Documentation

- [Forward Lighting](ForwardLighting.md)
- [HDR Scene Color and Display Mapping](HDRSceneColorAndDisplayMapping.md)
- [Viewport Rendering](ViewportRendering.md)
- [Hybrid Deferred Rendering Roadmap](../../Roadmaps/Archive/2026-08/HybridDeferredRendering.md)
