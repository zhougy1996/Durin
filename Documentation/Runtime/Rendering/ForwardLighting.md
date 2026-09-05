# Forward Lighting

Summary: Define scene-light ownership, deterministic view selection, and the shared forward/deferred lighting ABI.

Modules: Engine, Renderer

Last reviewed: 2026-09-03

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

[Renderer Frame Preparation](RendererFramePreparation.md) owns the shared scene
pipeline and ordering. Special-forward work is selected by the immutable view
mode; resource failure never selects another lighting owner.

The fixed lighting ABI and the shared helpers below remain common to deferred
Lit evaluation and retained forward translucency. The selected four local
records therefore keep the same stable order, attenuation, BRDF, environment,
emissive, and shadow semantics across the composition boundary. See
[Deferred Directional Lighting](DeferredDirectionalLighting.md) for the
deferred consumption and failure contract.

## Scene ownership

Directional, point, and spot lights cross the game/render boundary as detached
`FLightSceneProxy` values. `DLightComponent` calls the component-level
`FSceneInterface::AddLight(this)`; Renderer-private `FScene` copies the stable
`FLightSceneId`, constructs the family proxy synchronously, and sends only that
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

`FPreparedLightView` is command-local copied state. Directional candidates and
the combined point/spot candidate list are each ordered by ascending
`FLightSceneId`. Selection takes at most one directional light and four local
lights; point and spot lights compete in the same local budget. Sequential
views prepare independently and do not retain `FLightSceneInfo` pointers.

`FViewRenderTelemetry` records submitted, invalid/disabled, frustum-culled,
selected, and overflow values by family plus packed bytes. Preparation asserts:

```text
directional submitted = rejected + selected + overflow
point submitted = rejected + frustum culled + selected + overflow
spot submitted = rejected + frustum culled + selected + overflow
```

## Fixed forward ABI

One view allocates one 16-byte-aligned, 768-byte dynamic uniform range. StaticMesh
and SplineMesh opaque, masked, and translucent draws bind the same range.

| Field | Layout | Bytes |
| --- | --- | ---: |
| View position | `float4` | 16 |
| Counts | `uint4` (`x` directional, `y` local) | 16 |
| Directional record | direction `float4`, color/intensity `float4` | 32 |
| Directional shadow record | control, depth/splits/transition, three matrix/bias/filter/valid-region cascade records | 448 |
| Four local records | position/inverse-range, direction/type, color/intensity, spot terms; four `float4` each | 256 |
| Total | 16-byte aligned | 768 |

The Vulkan dynamic-upload page is 4 MiB, leaving 4,193,536 bytes after one
lighting payload before alignment and other allocations. A null or wrong-sized
range fails the view before Scene Color; there is no previous-view fallback or
persistent lighting resource.

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
Larger tiers require separate measured qualification.
