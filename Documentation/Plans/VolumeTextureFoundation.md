# Volume Texture Foundation Plan

Summary: Complete the backend-neutral 3D texture path and add a cookable `DVolumeTexture` asset without introducing a volumetric rendering algorithm.

Last reviewed: 2026-08-21

Status: Active
Completed:

## Current Status

Planning is complete and implementation has not started. Durin already declares
`ETextureDimension::Texture3D`, `ERHITextureDimensionFlags::Texture3D`,
`FRHITextureCreateDesc::Create3D`, and depth in the generic texture descriptor.
The declarations are not a usable capability: Vulkan does not advertise or
create 3D images, `FRHITexture` does not retain depth, public uploads are 2D,
and Engine has no volume-texture asset, payload, render resource, or build
recipe.

This plan selects one vertical slice from validated voxel data to a sampled GPU
texture. It is independent of the persistent-view-state plan; volumetric clouds
may consume both foundations later, but neither plan depends on cloud rendering.

## Goal

Ship a backend-neutral, queryable, and validated 3D texture capability and a
package-backed `DVolumeTexture` whose built mip chain can be loaded, cooked,
uploaded, referenced, sampled, replaced, and released through the same public
resource-lifecycle contracts as existing texture assets.

## Scope

- Dimension-correct RHI description, capability limits, validation, resource
  identity, texture views, copies, transitions, and recorded-command lifetime
  for non-array 3D color textures.
- A public three-dimensional upload contract with explicit row and depth pitch.
- Vulkan 3D image creation, sampled/storage views, upload, copy/readback
  qualification, and capability publication.
- Reflected `Texture3D` and `RWTexture3D` shader bindings through public RHI.
- Engine-owned `DVolumeTexture`, source/build settings, platform mip data,
  stable payload encoding, cooked payload identity, post-load, render resource,
  texture-reference publication, replacement, and failure fallback.
- A `TextureBuild` recipe that consumes already-normalized voxel source data,
  validates the selected formats, and deterministically produces a complete 3D
  mip chain and DDC key.
- Focused RHI, Vulkan, asset serialization/cook, resource lifecycle, shader
  binding, and runtime sampling validation.

## Non-Goals

- Volumetric clouds, fog, smoke, participating-media integration, ray marching,
  temporal reprojection, lighting, shadows, or scene composition.
- A `VolumetricRendering` runtime module or cloud-specific types in RHI,
  RenderCore, `DVolumeTexture`, or `TextureBuild`.
- Material-system volume domains or general `DVolumeTexture` material
  parameters; the first production consumer may add those at its own boundary.
- Sparse/tiled volumes, virtual textures, bricked streaming, bindless access,
  texture arrays of 3D images, or 3D render/depth attachments.
- Runtime automatic mip generation, asynchronous transfer queues, or a render
  graph.
- DDS/KTX/VDB import, slice-stack or atlas import policy, editor preview,
  thumbnail rendering, Content Browser actions, or volume-painting tools.
- Block-compressed 3D payloads in the first implementation.

## Design Decisions and Invariants

### RHI dimensional contract

- `Texture3D` means one non-array image with width, height, depth, mip count,
  one sample, and `ArraySize == 1`. Depth is reduced independently by
  `max(1, BaseDepth >> MipIndex)`.
- `FRHITexture` retains and exposes depth through `GetSizeZ()`; code must not
  infer volume depth from array layers. Existing 2D and cube resources report
  depth one.
- `FRHICapabilities` publishes `MaxTextureDimension3D` from the backend. Exact
  format/usage admission still goes through `RHIIsTextureSupported`; a
  dimension flag alone is not sufficient.
- The first 3D usage set is sampled, storage, source-copy, and destination-copy.
  Render-target, resolve, depth/stencil, multisample, cube-compatible, and array
  semantics are rejected by backend-neutral validation.
- 3D subresources remain mip-based. Z slices are texels within one mip, not
  array subresources; state tracking and texture-view ranges therefore use one
  array layer.

### Upload, copy, and command lifetime

- Add `FUpdateTextureRegion3D` and a public `UpdateTexture3D` command with source
  X/Y/Z, destination X/Y/Z, width/height/depth, row pitch, and depth pitch.
- Validation proves all offsets, pitches, block geometry, mip bounds, byte
  footprints, and integer arithmetic before recording. The initial asset path
  admits only uncompressed color formats, while the generic API retains
  block-aware validation without claiming unsupported compressed uploads.
- Recorded commands own the exact source bytes they replay. Caller memory may
  be released or changed immediately after command recording in threaded and
  inline executors.
- Generic buffer/texture copy regions use their existing Z offset and depth
  extent. Their validators, state ranges, footprint calculations, and Vulkan
  replay are made dimension-correct rather than adding a private upload path.
- Upload and copy transitions use public RHI access states and restore the
  texture to the requested readable/read-write state. No raw Vulkan barrier or
  device-idle synchronization supplies correctness.

### Vulkan contract

- A volume creates `vk::ImageType::e3D` with `arrayLayers = 1`, no cube flag,
  and `vk::ImageViewType::e3D`. Capability publication uses
  `maxImageDimension3D`; exact image-format properties remain authoritative.
- Image allocation size, mip footprint, upload staging, and readback
  qualification include depth with checked 64-bit arithmetic.
- Sampled and storage views are complete-or-null. Unsupported format/usage
  combinations fail `RHIIsTextureSupported` or fallible creation without
  publishing a partial object.

### Asset and payload contract

- `DVolumeTexture` derives from `DTexture` and follows the existing texture
  reference/revision/render-completion lifecycle. Failed rebuild or upload does
  not replace the last-known-good referenced texture.
- `FVolumeTextureMipData` stores width, height, depth, row pitch, depth pitch,
  and bytes. `FVolumeTexturePlatformData` owns format and a complete mip chain.
  Validation requires exact pitches and payload sizes, finite limits, and the
  three-axis halving rule.
- The initial portable asset formats are `R8_UNORM`, `RG8_UNORM`,
  `RGBA8_UNORM`, `R16_FLOAT`, and `RGBA16_FLOAT`. Stage 0 may remove a format
  only when public-RHI qualification rejects it; any change is recorded before
  implementation. Backend support is still queried per exact descriptor.
- The texture payload schema gains a distinct volume dimension and stable
  format identifiers without renumbering existing values. Volume payloads have
  their own producer version and cooked-payload GUID; old 2D/cube bytes retain
  their current meaning.
- Authoring input is a normalized, tightly described voxel buffer plus build
  settings. `TextureBuild` owns deterministic mip construction and DDC keys.
  File-format decoding and UI remain future adapters.
- Mip filtering is box filtering in linear numeric space. Integer/normalized
  and float conversion, odd extents, channel handling, and rounding are frozen
  in Stage 0 and covered by golden fixtures.
- Cooked/runtime loading accepts only validated payloads within the existing
  texture payload byte ceiling. Corrupt, truncated, oversized, mismatched-key,
  or unsupported payloads fail transactionally.

### Ownership and dependency direction

- RHI owns dimension-neutral contracts; VulkanRHI owns Vulkan translation and
  device admission; Engine owns the runtime asset and render resource;
  TextureBuild owns deterministic derived-data production.
- Engine must not depend on VulkanRHI or branch on Vulkan handles. TextureBuild
  must not create GPU resources. Renderer is only a sampling qualification
  consumer and does not own `DVolumeTexture`.

## Current Foundations and Gaps

| Area | Existing foundation | Gap closed by this plan |
| --- | --- | --- |
| RHI description | `Texture3D`, `Create3D`, depth, copy-region Z/depth, and dimension flags exist. | Resource depth, 3D validation, limits, uploads, views, footprints, and qualification are incomplete. |
| Vulkan | Dimension-to-view translation recognizes `Texture3D`. | Capability publication, image creation, support queries, staging, and memory accounting admit only 2D/array/cube paths. |
| Shader interface | Reflected sampled/storage texture infrastructure and compute dispatch exist. | No qualified `Texture3D`/`RWTexture3D` binding and sampling/write path exists. |
| Texture assets | `DTexture2D` and `DTextureCube` provide package, DDC, cook, reference, and render-resource patterns. | No volume source/platform data, stable payload, asset class, build recipe, or resource exists. |
| Validation | Texture creation, copy, upload, readback, shader, asset, and cook fixtures exist for other dimensions. | No odd-depth, multi-mip, 3D lifetime, payload-corruption, or runtime-sampling matrix exists. |

## Implementation Stages

### Stage 0: Freeze the 3D format, layout, and payload contracts

- [ ] Inventory all texture-dimension switches, size/footprint helpers, view
  validators, state trackers, copy validators, shader reflection mappings,
  stable format tables, texture reference paths, and texture build recipes.
- [ ] Freeze dimension-specific create rules and the exact behavior of depth,
  mips, array size, samples, allowed flags, views, transitions, and copies.
- [ ] Freeze `FUpdateTextureRegion3D`, row/depth-pitch validation, recorded byte
  ownership, and uncompressed odd-extent fixtures.
- [ ] Qualify the selected five asset formats for sampled 3D creation on the
  target adapter and qualify storage access for at least `R8_UNORM` and
  `RGBA16_FLOAT`; record any portable format reduction before Stage 1.
- [ ] Freeze source/build/platform structures, mip-filter math, limits, stable
  payload IDs, producer version, DDC-key inputs, and corruption behavior.
- [ ] Record exact module/file ownership and focused native-test targets before
  production changes.

#### Acceptance Gate

- Width/height/depth semantics, supported usages and formats, pitches,
  footprints, transitions, payload bytes, build determinism, failure behavior,
  and module ownership are unambiguous. No importer, material, cloud, streaming,
  or render-graph decision can enter implementation.

### Stage 1: Complete the public RHI and Vulkan 3D path

- [ ] Retain resource depth, publish the 3D limit, and make descriptor/support
  validation dimension-correct with checked footprint arithmetic.
- [ ] Add recorded `UpdateTexture3D` commands and context/backend entry points
  with exact source-byte retention in inline and threaded execution.
- [ ] Create Vulkan 3D images and views and repair copy, transition, state,
  allocation, upload, and readback paths for mip-based 3D subresources.
- [ ] Complete reflected sampled/storage 3D shader bindings without
  backend-specific renderer code.
- [ ] Add focused RHI/Vulkan tests for valid and rejected descriptors, odd
  extents, partial regions, multi-mip uploads, copy round trips, sampled output,
  storage writes, command retention, and device failure.

#### Acceptance Gate

- Public RHI can query, create, upload, transition, copy, view, sample, write,
  and destroy qualified 3D textures through both executors. Invalid requests
  fail before replay, and Vulkan validation reports no errors.

### Stage 2: Add the runtime `DVolumeTexture` asset and render resource

- [ ] Add reflected asset/build settings and validated volume source, mip, and
  platform-data types with copy/move/serialization behavior.
- [ ] Extend stable texture payload encoding/decoding with the volume dimension,
  formats, producer version, cooked GUID, hashes, limits, and diagnostics.
- [ ] Add `DVolumeTexture` load, post-load, cook, duplication, replacement,
  derived-key, last-known-good, and render-completion behavior.
- [ ] Add `FVolumeTextureResource` creation, per-mip upload, texture-reference
  publication, revision ordering, failure retention, and deferred cleanup.
- [ ] Add asset/payload/resource tests for valid, empty, odd, corrupt,
  unsupported, oversized, stale-revision, replacement, and shutdown cases.

#### Acceptance Gate

- A programmatically supplied valid platform payload round-trips through an
  asset package and cook, publishes one sampled 3D texture, survives replacement
  and queued rendering, and releases without leaking or exposing partial data.

### Stage 3: Add deterministic volume texture building

- [ ] Register one `TextureBuild` volume recipe/function transaction using
  normalized voxel source and the frozen build settings.
- [ ] Generate deterministic three-axis mip chains for the admitted formats,
  including 1xN xM, odd extents, one-voxel depth, and full 1x1x1 termination.
- [ ] Include source identity, dimensions, format, filter settings, builder
  version, platform/profile, and relevant policy in the DDC key.
- [ ] Integrate cache hit/miss, rebuild, corrupt-entry rejection, cook lookup,
  and atomic Engine publication without introducing an Engine-to-TextureBuild
  public dependency.
- [ ] Add golden mip, key sensitivity, DDC round-trip, and cook-without-source
  tests.

#### Acceptance Gate

- Identical source/settings produce byte-identical payloads and keys; every
  relevant input changes the key; cached and rebuilt assets render identically;
  and cook consumes validated built data without invoking a file importer.

### Stage 4: Qualify the end-to-end foundation and publish contracts

- [ ] Run the focused RHI, Vulkan, shader, TextureBuild, Engine asset, package,
  cook, and render-resource test matrix using the repository test workflow.
- [ ] Render a deterministic 3D sampling fixture into a 2D target and compare
  expected slices/interpolation for all admitted formats and both executors.
- [ ] Exercise shader reload, asset replacement, failed rebuild/upload, device
  invalidation, retry, render-thread backlog, and shutdown.
- [ ] Record logical payload bytes, backend allocation bytes, upload bytes, and
  bounded diagnostics for a representative multi-mip volume.
- [ ] Publish lasting RHI capability and volume-texture asset contracts, then
  close this plan only when all gates have evidence.

#### Acceptance Gate

- The complete validation matrix passes on the qualification adapter; runtime
  sampling uses only public contracts; failure preserves the last-known-good or
  explicit null fallback; and lasting behavior is documented outside the plan.

## Validation Matrix

| Layer | Required evidence |
| --- | --- |
| Pure validation | Dimension/flag/sample/array/depth/mip rules, pitches, byte overflow, subregions, stable IDs, mip math, and DDC key sensitivity. |
| RHI command | Inline/threaded upload retention, partial and full 3D copies, transitions, sampled/storage views, and recorded-resource lifetime. |
| Vulkan | Capability limits, exact format admission, 3D image/view types, validation-clean replay, allocation/upload/readback accounting, and fallible failure. |
| Shader | Reflected `Texture3D` sampling and `RWTexture3D` write qualification with deterministic 2D readback. |
| Asset/build | Source/platform validation, golden mips, payload round-trip/corruption, DDC hit/miss, package reload, duplication, cook, and replacement. |
| Runtime lifecycle | Reference revision ordering, last-known-good retention, render backlog, reload, device invalidation/retry, and shutdown release. |
| End to end | Cooked `DVolumeTexture` loads and produces expected sampled pixels without importer or backend escape hatches. |

## Definition of Done

- Every Stage 0 decision is recorded and every stage acceptance gate passes.
- `Texture3D` is advertised only when the complete selected public path works.
- `DVolumeTexture` is a cookable, package-backed asset with deterministic built
  data and a validated render resource.
- No cloud/material/import/streaming concern leaks into the foundation.
- Lasting contracts are published and plan/document validators pass.

## Deferred Follow-ups

- Volume material parameters and a volume material domain.
- DDS/KTX/VDB, slice-stack, or atlas import and Texture Editor visualization.
- Block-compressed 3D formats and platform-specific compression policy.
- Streaming, sparse/bricked storage, residency budgets, and procedural GPU
  generation.
- Volumetric cloud/fog consumers and their quality policies.

## Related Documentation

- [Asset data lifecycle](../Runtime/Assets/AssetDataLifecycle.md)
- [Asset packages](../Runtime/Assets/AssetPackages.md)
- [RHI capabilities and Vulkan startup](../Runtime/Rendering/RHICapabilitiesAndVulkanStartup.md)
- [Render resource lifecycle](../Runtime/Rendering/RenderResourceLifecycle.md)
- [Renderer resource recovery](../Runtime/Rendering/RendererResourceRecovery.md)
- [Persistent view state foundation plan](PersistentViewStateFoundation.md)
- [Agent build and run workflow](../Agents/BuildAndRun.md)
- [Agent testing workflow](../Agents/Testing.md)

## Related Code

- [`RHIDefinitions.h`](../../Engine/Source/Runtime/RHI/Public/RHIDefinitions.h)
- [`RHIResources.h`](../../Engine/Source/Runtime/RHI/Public/RHIResources.h)
- [`RHICapabilities.h`](../../Engine/Source/Runtime/RHI/Public/RHICapabilities.h)
- [`VulkanTexture.cpp`](../../Engine/Source/Runtime/VulkanRHI/Private/VulkanTexture.cpp)
- [`Texture.h`](../../Engine/Source/Runtime/Engine/Public/Texture/Texture.h)
- [`Texture2D.h`](../../Engine/Source/Runtime/Engine/Public/Texture/Texture2D.h)
- [`TextureDerivedData.h`](../../Engine/Source/Runtime/Engine/Public/Texture/TextureDerivedData.h)
- [`TextureRenderResource.h`](../../Engine/Source/Runtime/Engine/Public/Texture/TextureRenderResource.h)
- [`TextureBuild.dmodule`](../../Engine/Source/Developer/TextureBuild/TextureBuild.dmodule)
