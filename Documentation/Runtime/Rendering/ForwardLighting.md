# Forward Lighting

## Scene ownership

Directional, point, and spot lights cross the game/render boundary as detached
`FLightSceneProxy` values. `IScene::AddOrReplaceLight` transfers one complete
candidate under a stable `FLightSceneId`; `RemoveLight` removes any family under
that identity. `FScene` owns one identity map and authoritative directional,
point, and spot `FLightSceneInfo` views. Replacement detaches the old typed
membership before attaching the new one, including a same-ID family change.

`DLightComponent` owns identity, registration, hidden-owner, transform,
property-change, and retirement publication. Directional, point, and spot
components copy their family data into concrete proxies. Published values never
retain actors, components, reflected assets, or editor objects.

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

`FViewRenderCounters` records submitted, invalid/disabled, frustum-culled,
selected, and overflow values by family plus packed bytes. Preparation asserts:

```text
directional submitted = rejected + selected + overflow
point submitted = rejected + frustum culled + selected + overflow
spot submitted = rejected + frustum culled + selected + overflow
```

## Fixed forward ABI

One view allocates one 16-byte-aligned, 320-byte dynamic uniform range. StaticMesh
and SkeletalMesh opaque, masked, and translucent draws bind the same range.

| Field | Layout | Bytes |
| --- | --- | ---: |
| View position | `float4` | 16 |
| Counts | `uint4` (`x` directional, `y` local) | 16 |
| Directional record | direction `float4`, color/intensity `float4` | 32 |
| Four local records | position/inverse-range, direction/type, color/intensity, spot terms; four `float4` each | 256 |
| Total | 16-byte aligned | 320 |

The Vulkan dynamic-upload page is 4 MiB, leaving 4,193,984 bytes after one
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
1.0 ms across at least 120 measured frames. Qualification on 2026-08-12 measured
1.376288 ms for the single-directional baseline and 1.978080 ms for `1 + 4`, an
incremental 0.601792 ms, so the production tier passed. The original `1 + 8`
candidate measured 1.748 ms incremental and was rejected. Larger tiers remain
deferred and require a reviewed evidence-backed plan revision.
