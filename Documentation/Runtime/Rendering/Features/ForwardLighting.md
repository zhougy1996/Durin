# Forward Lighting

Summary: Define scene-light ownership, deterministic view selection, and the shared forward/deferred lighting ABI.

Modules: Engine, Renderer

Last reviewed: 2026-11-11

Directional direct lighting may be attenuated by the selected view-local
shadow described in [Directional Shadows](DirectionalShadows.md). The shadow
record is part of the fixed reflected lighting ABI; all other lighting terms
retain the behavior documented below.

## Production surface ownership

The production hybrid renderer evaluates Lit opaque/masked StaticMesh and
SplineMesh records after the GBuffer. Forward owns
only Unlit opaque/masked surfaces, the globally sorted translucent list,
wireframe or another explicitly named special mode, and SkyBox bootstrap.
Dedicated retained-forward pipeline
variants load existing HDR Scene Color and GBuffer depth;
they never clear or display-map those attachments. No product caller silently
falls back to generic Lit opaque forward rendering.

[Renderer Frame Preparation](../Pipeline/RendererFramePreparation.md) owns the shared scene
pipeline and ordering. Special-forward work is selected by the immutable view
mode; resource failure never selects another lighting owner.

The fixed lighting ABI and the shared helpers below remain common to deferred
Lit evaluation and retained forward translucency. The admitted local
records therefore keep the same stable order, attenuation, BRDF, environment,
emissive, and shadow semantics across the composition boundary. See
[Deferred Directional Lighting](DeferredDirectionalLighting.md) for the
deferred consumption and failure contract.

## Scene ownership

Directional, point, and spot lights cross the game/render boundary as detached
`FLightSceneProxy` values. `DLightComponent` calls the component-level
`FSceneInterface::AddLight(this)`; Renderer-private `FScene` copies the stable
`FLightComponentId`, constructs the family proxy synchronously, and sends only that
proxy to its command queue. The component retains the raw proxy token only after
internal command admission succeeds, uses `RemoveLight(this)` for retirement,
and never reads the token. `FLightSceneRegistry` keys ownership by that exact
pointer and owns authoritative directional, point, and spot `FLightSceneInfo`
views. A rebuild admits removal of the old proxy before adding the new proxy,
so a family change cannot leave stale typed membership.

`DLightComponent` owns identity, registration, hidden-owner, transform,
property-change, and retirement publication through the shared create, destroy,
and dirty render-state hooks. Directional, point, and spot components copy their
family data into concrete proxies. Published values never retain actors,
components, reflected assets, or editor objects.

## Authored values and bounds

Color and intensity are finite and non-negative; compatibility color channels
remain clamped to `[0, 1]`. Local range is finite and positive. Spot angles are
degrees with `0 <= inner <= outer < 90`; production authoring clamps the upper
bound to 89 degrees. Directions are normalized before publication. Invalid
directly submitted proxy values remain in scene ownership but are rejected as a
complete candidate during view preparation and never reach the GPU payload.

Point and spot influence use the conservative sphere `(position, range)`, stored
as its enclosing AABB in `FLightSceneInfo`. Normal view visibility rejects only
bounds classified outside the fitted view frustum, so plane-intersecting and
boundary bounds remain eligible. Fine spot-cone and per-object culling are not
part of this contract.

## Per-view selection and diagnostics

The fitted `FSceneViewRequirements` declares forward and deferred local-light
consumers before collection. Lit views can evaluate local light in forward
materials, including sorted translucency and wireframe. Solid Lit also needs
the production deferred pass. On Unlit views, an isolated deferred Local or
Final output, or a qualification of composed lighting, needs the list. An AO
diagnostic replaces that isolated output. GBuffer material capture and shadow
depth alone request no local lights.

`GatherAndSortLights_RenderThread` visits the scene's point and spot lists
once per submission if either consumer requests them. It applies the fitted
main-view frustum to influence bounds. A disabled or unusable frustum keeps
otherwise valid candidates. Both passes consume the same selection. An empty
selection sets the uniform's local count to zero. Directional light selection
also serves shadow and cloud consumers and has its own lifetime.

`FPreparedLightView` is command-local copied state. Directional candidates and
the combined point/spot candidate list are each ordered by ascending
`FLightComponentId`. Gather retains one directional light and every visible local. The renderer snapshots a local record budget per submission. Resource resolution packs the ordered prefix across both families up to that budget and the backend storage range. Ordinary views use four. Qualification can select another budget (including zero) independently for each view. Sequential
views prepare independently and do not retain `FLightSceneInfo` pointers.

`FViewRenderTelemetry` records submitted, invalid/disabled, frustum-culled,
visible, selected (GPU-admitted), and overflow values by family. Submitted counts cover lights visited
for this view. Resource resolution records packed bytes after a successful
uniform allocation. Preparation asserts:

```text
directional submitted = rejected + selected + overflow
point submitted = rejected + frustum culled + selected + overflow
spot submitted = rejected + frustum culled + selected + overflow
```

## Shared view lighting ABI

Each view allocates a 16-byte-aligned dynamic uniform. Generated Lit surface shaders and fullscreen deferred share its count and the same read-only structured buffer.

| Field | Layout | Bytes |
| --- | --- | ---: |
| View position | `float4` | 16 |
| Counts | `uint4` (`x` directional, `y` local) | 16 |
| Directional record | direction `float4`, color/intensity `float4` | 32 |
| Directional shadow record | control, depth/splits/transition, three matrix/bias/filter/valid-region cascade records | 448 |
| Environment rotation | inverse source quaternion `float4` | 16 |
| Environment control | intensity, maximum prefilter mip, reserved | 16 |
| Total | 16-byte aligned | 544 |

Each local record is 64 bytes. Its four vectors hold camera-relative position and inverse range, direction and type, color and intensity, and spot cone terms. RDG uploads the ordered prefix once and declares each consuming pass as a reader. A zero count binds a device-lifetime one-record dummy and queues zero bytes. Allocation failure aborts the view before graph execution. The shadow-only fallback allocates a separate zeroed common uniform.

`PackedLightBytes` measures the common uniform. `LocalRecordUploadBytes` measures the queued local payload (64 bytes per admitted light, up to 256 bytes with the four-light profile) and excludes the dummy and shadow fallback.

## Direct-light equations

Directional proxy direction follows emitted travel, so shading uses its
negation as surface-to-light. Local lights use:

```text
d2 = dot(lightPosition - surfacePosition, same)
inverseSquare = 1 / max(d2, 0.05^2)
x = sqrt(d2) / range
rangeWindow = x >= 1 ? 0 : (1 - x^2)^2
```

This is finite at the light position and exactly zero at and beyond range.
Point radiance is `color * intensity * inverseSquare * rangeWindow`.

For a spot light, `c` is the cosine between normalized light direction and
light-to-surface, `ci = cos(inner)`, and `co = cos(outer)`:

```text
t = saturate((c - co) / (ci - co))
cone = t^2 * (3 - 2t)
```

The cone is one at the inner boundary and zero at the outer boundary. When the
angles are equal, the hard edge is one only for `c > co`, so the exact boundary
is zero. Each selected light calls the shared PBR direct-light function;
contributions add before environment lighting and emissive. Unlit surfaces and
Unlit view mode retain their established behavior.

## Performance gate

The qualification target is NVIDIA GeForce GTX 1060 6GB at 1920x1080, using RHI
GPU timestamp queries around Scene Color after warm-up. The representative
fixture is an opaque full-screen receiver plus masked/translucent overdraw. The
accepted incremental median for `1 + 4` over the single-directional baseline is
1.0 ms across at least 120 measured frames. This historical forward-tier
qualification selected `1 + 4` and rejected `1 + 8`. Current hybrid production
timing gates belong to [Deferred Directional Lighting](DeferredDirectionalLighting.md#memory-and-qualification).
The 32-light qualification profile exercises the same shared buffer and counted loop. The cross-device default decision remains open. See the [view local light resources plan](../../../Plans/ViewLocalLightResources.md) for image checks and measurements.
