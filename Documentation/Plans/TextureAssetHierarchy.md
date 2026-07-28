# Texture Asset Hierarchy Plan

Summary: Introduce an abstract shared texture asset and resource-lifecycle layer without changing Texture2D or TextureCube serialized data, import behavior, or sampling semantics.

Last reviewed: 2026-07-29

Status: Active
Completed:

## Current Status

`DTexture2D` and `DTextureCube` are independent reflected `DObject` subclasses.
They intentionally retain different source provenance, platform-data layouts,
import workflows, and concrete render resources, but duplicate the stable
texture-reference ownership, revisioned resource-completion state, resource
replacement, release, deferred cleanup, build status, and public render-state
accessors.

The lower rendering layer already has the correct common boundary:
`FTexture2DResource` and `FTextureCubeResource` derive from
`FTextureResource`, and renderer consumers retain counted
`FRHITextureReferenceRef` values rather than reflected assets or concrete
resources. The missing boundary is therefore between `DObject` and the two
texture asset types, not in the RHI texture hierarchy.

Material parameters remain intentionally typed as `DTexture2D`; sky components
remain typed as `DTextureCube`. No current renderer consumer requires an
unqualified texture asset, so this refactor must preserve those domain-specific
types rather than using the new base class as a reason to widen every API.

Stage 0 is complete. Stage 1 is the current implementation stage.

Stage 0 handoff:

- Baseline commit:
  `eda371e0ac11383987e21baa5d28f12557900eeb`.
- Working set: `SourceLibraryReferenceContractTests.cpp`,
  `TextureCubeTests.cpp`, the affected EngineTests deployment declaration, and
  this plan.
- Key symbols and fixtures: `DTexture2D::StaticClass`,
  `DTextureCube::StaticClass`, `FTextureCubeResourceCompletion`, one legacy
  Texture2D package, one six-face TextureCube package, and one panorama
  TextureCube package.
- Decisions: qualified leaf names and every currently generated leaf property
  remain fixed; the Stage 1 hierarchy test extends the existing `DObject`
  expectations with `DTexture` rather than introducing a test-only base.
- Lifecycle baseline: concrete texture tests plus the parameterized RenderCore
  lifetime contract cover first publication, replacement ordering, stale
  success/failure rejection, current upload failures, invalidation with
  last-good-data retention, queued destruction, unload, and shutdown cleanup
  for both texture kinds.
- Open validation item: concrete asset tests cannot deterministically pause
  render commands during destruction without a test-only hook. Stage 3 retains
  this validation after common ownership exists.
- Schema gap: `DTexture2D::ImportOwner` is annotated with `DPROPERTY` in source
  but absent from current generated reflection metadata and legacy package
  schema. This plan does not change that pre-existing boundary or treat the
  field as serialized hierarchy state.
- Validation: all 41 `EditorAssetWorkflowTests` passed; all 13 texture-focused
  `RenderContractTests` passed; all 39 direct Texture2D, TextureCube, cook, and
  derived-data tests passed; plan validation passed. The complete
  `TextureTests` target remains limited by five unrelated existing
  `FStaticModelImportBuildTests` failures in dependency-graph/sidecar behavior,
  including one access violation.

## Goal

Create a reflected, non-instantiable `DTexture : DObject` base and a shared
asset-resource lifecycle implementation so all texture asset types have one
stable-reference, revision, completion, replacement, and release contract while
each concrete texture type continues to own its topology-specific authored
source, derived platform data, import/build policy, and GPU allocation details.

## Scope

- Add reflected abstract-class support required to declare `DTexture` as
  non-instantiable.
- Add `DTexture` beneath `DObject` and make `DTexture2D` and `DTextureCube`
  derive from it.
- Move shared texture enums and diagnostics out of the Texture2D-specific
  header into a neutral texture header.
- Replace the duplicated 2D and cube render-completion implementations with one
  revision-aware completion type.
- Centralize stable `FTextureReference` ownership, resource revision state,
  concrete-resource replacement, release, and deferred cleanup in `DTexture`.
- Preserve the public behavior and concrete types of Texture2D, TextureCube,
  material parameters, sky components, editors, thumbnails, DDC, cook, and RHI
  upload.
- Add compatibility, lifecycle, and rendering validation for both concrete
  texture types.
- Update the implemented texture rendering contracts after the migration is
  validated.

## Non-Goals

- Changing `FMaterialParameterValue::TextureValue` or material APIs from
  `DTexture2D` to `DTexture`.
- Adding cube, array, volume, render-target, virtual, dynamic, or writable
  texture sampling to materials.
- Unifying `FTexturePlatformData` and `FTextureCubePlatformData`.
- Unifying Texture2D, six-face cube, or panorama source/import data.
- Moving existing reflected properties between declaring classes.
- Changing `.dasset`, TXPL, DDC-key, cooked payload, source-provenance, or asset
  qualified-name formats.
- Adding asynchronous texture build, residency accounting, or streaming.
- Changing editor-visible texture settings, import dialogs, thumbnails, or
  preview presentation.
- Mirroring the full Unreal Engine `UTexture` API or streaming hierarchy.

## Design Decisions and Invariants

- `DTexture` is a reflected abstract class. It is discoverable as the common
  superclass of texture assets but cannot be constructed directly, saved as a
  concrete asset, or registered as a Content Browser asset type.
- DurinHeaderTool will recognize the narrow `DCLASS(Abstract)` class specifier,
  emit `EClassFlags::Abstract`, and omit the generated object constructor for
  that class. Unknown class specifiers remain errors. `NewObject` and default
  object creation continue to reject abstract classes through the existing
  class-flag check.
- The qualified names `Durin::DTexture2D` and `Durin::DTextureCube` remain
  unchanged. Existing packages continue resolving those leaf classes.
- No existing `DPROPERTY` moves into `DTexture` in this plan. Keeping serialized
  source, build setting, and cooked descriptor fields on their current leaf
  classes avoids declaring-class/schema churn and keeps legacy package
  compatibility independently testable from the runtime ownership refactor.
- `DTexture` owns exactly one stable `FTextureReference`, at most one current
  concrete texture resource, one shared completion object, the current build
  revision, and whether stable-reference initialization has been queued.
- `DTexture` exposes the existing common public observations:
  `GetTextureReferenceRHI`, `GetRenderResourceState`,
  `GetAppliedRenderRevision`, and `GetBuildRevision`. Leaf call sites retain
  source compatibility through inherited methods.
- A shared Engine-layer texture asset resource derives from RenderCore's
  `FTextureResource`. It owns revision/completion bookkeeping common to all
  asset textures; concrete 2D and cube resources retain only platform data and
  topology-specific RHI creation/upload.
- Resource creation remains a leaf hook. `DTexture` controls ordering and
  ownership, while `DTexture2D` and `DTextureCube` create validated candidates
  from immutable copies of their own platform data.
- A replacement candidate is initialized before it becomes the stable
  reference target. Stale or failed candidates cannot replace the last
  successful allocation.
- Invalidation advances the revision before releasing the current resource.
  Destruction prevents later publication, releases the concrete resource,
  transfers it to ordered deferred cleanup, then releases the stable reference.
- The render thread never reads `DTexture`, `DTexture2D`, or `DTextureCube`.
  Accepted renderer, material, scene, preview, and thumbnail work continues to
  retain only counted stable RHI references and immutable data.
- `DTexture2D` and `DTextureCube` keep their existing build status, error, DDC,
  source, platform-data, cook, and import behavior. Common status types may move
  headers; status storage and mutation do not move unless they can do so without
  serialized or behavioral changes.
- Material and sky APIs remain dimension-specific. A later consumer may accept
  `DTexture*` only when it can correctly validate texture class and sampling
  dimension.
- Every implementation stage leaves both leaf texture types buildable and
  testable; there is no intermediate state with two owners of a stable reference
  or concrete resource.

## Current Foundations and Gaps

Existing foundations:

- `FTextureReference` provides stable consumer-facing RHI identity.
- `FTextureResource` owns a concrete texture allocation and publishes it through
  the stable reference.
- Texture2D and TextureCube already use the same revision ordering and deferred
  cleanup model.
- Both asset types have focused import, DDC, cook, render-resource, thumbnail,
  and Vulkan validation.
- `EClassFlags::Abstract` and object-creation rejection already exist in
  CoreDObject.

Gaps to close:

- DurinHeaderTool always emits `EClassFlags::None` and a generated constructor
  for reflected classes, so abstract reflected asset classes are not authorable.
- Shared texture status and diagnostic types currently live in
  `Texture2D.h`, making the cube API depend on a 2D-owned declaration boundary.
- `FTexture2DResourceCompletion` and `FTextureCubeResourceCompletion` implement
  the same state machine independently.
- Both reflected assets separately own and release their stable reference,
  completion object, concrete resource, build revision, and initialization flag.
- Both assets separately implement common observation and replacement ordering,
  allowing future texture types to drift from the established lifetime
  contract.
- There is no reflection test proving that a common texture base is recognized
  while existing leaf class identity and package loading remain stable.

## Implementation Stages

### Stage 0: Freeze Compatibility and Lifecycle Baselines

- [x] Record the baseline commit and the initial working set for the stage
  handoff.
- [x] Add or identify package fixtures for existing `DTexture2D` and
  `DTextureCube` assets and assert they resolve to the same qualified leaf
  classes after a superclass is inserted.
- [x] Add compile-time and reflected inheritance baselines covering `DObject`
  and both leaf types; extend the same test to `DTexture` when Stage 1 adds the
  production type.
- [x] Capture the existing lifecycle cases for both types: first publication,
  successful replacement, stale candidate, upload failure, invalidation,
  destruction with queued work, and shutdown cleanup.
- [x] Confirm through reflection and package inspection tests that the plan
  moves no existing reflected property to a different declaring class.
- [x] Record any uncovered legacy fixture or lifecycle gap before production
  ownership begins moving.

Dependencies: none.

#### Acceptance Gate

- Existing Texture2D and TextureCube packages, reflected leaf identities, and
  resource-lifecycle behavior have executable regression coverage sufficient to
  distinguish the hierarchy refactor from a format or rendering change.

### Stage 1: Add the Neutral Texture Type Boundary

- [ ] Add parsing, model, cache/manifest, and code-generation support for the
  single `DCLASS(Abstract)` specifier.
- [ ] Emit `EClassFlags::Abstract` with no generated object constructor and add
  DurinHeaderTool tests for accepted, cached, invalid, and unknown class
  specifiers.
- [ ] Add CoreDObject tests proving abstract classes participate in reflection
  and inheritance but cannot create ordinary instances or default objects.
- [ ] Add a neutral Engine public texture header containing `DTexture` and
  shared texture lifecycle/status declarations.
- [ ] Move common enums and non-owning diagnostic structures out of
  `Texture2D.h`; preserve names, enumerator values, reflection metadata, and
  source include compatibility where practical.
- [ ] Change `DTexture2D` and `DTextureCube` to derive from `DTexture` without
  moving ownership or behavior in this stage.
- [ ] Add `DTexture` to forward declarations and verify reflected casts and
  subclass filters recognize both leaves.

Dependencies: Stage 0.

#### Acceptance Gate

- The generated reflection hierarchy is
  `DObject -> DTexture -> {DTexture2D, DTextureCube}`; direct `DTexture`
  construction is rejected; existing leaf construction, serialization,
  loading, and public include consumers remain valid.

### Stage 2: Unify Completion and Concrete Resource Contracts

- [ ] Replace `FTexture2DResourceCompletion` and
  `FTextureCubeResourceCompletion` with one Engine-layer
  `FTextureResourceCompletion`.
- [ ] Add one Engine-layer asset texture resource base over
  `FTextureResource` for revision, release revision, and completion reporting.
- [ ] Make concrete 2D and cube resources derive from the new resource base and
  retain only their immutable platform-data snapshots and topology-specific RHI
  upload logic.
- [ ] Preserve the atomic ordering and mutex-protected state semantics of the
  current completion implementations.
- [ ] Preserve distinct unsupported-format and create/upload failure reporting.
- [ ] Add shared completion tests for stale build, stale success, stale failure,
  release, and monotonic applied revision.
- [ ] Run the existing Texture2D and TextureCube render-resource tests against
  the shared implementation before changing asset ownership.

Dependencies: Stage 1.

#### Acceptance Gate

- Both concrete resource types use one completion state machine and common
  revision/release contract, while their RHI descriptors, slice/mip uploads,
  format checks, publication results, and existing focused tests remain
  unchanged.

### Stage 3: Move Asset Resource Ownership into DTexture

- [ ] Move stable-reference, shared-completion, generic concrete-resource,
  build-revision, and initialization-queued storage from both leaf assets into
  `DTexture`.
- [ ] Implement common reference initialization, candidate replacement,
  invalidation release, destruction release, and deferred cleanup ordering in
  `DTexture`.
- [ ] Add a protected leaf hook that creates a concrete resource candidate from
  already validated immutable platform data; the base owns the candidate before
  any render command can observe it.
- [ ] Replace leaf `QueueRenderResourceBuild` implementations with thin
  platform-data validation and candidate-creation hooks.
- [ ] Replace leaf invalidation cleanup with the base release primitive while
  retaining each leaf's platform-data reset and build-status behavior.
- [ ] Remove duplicate public render-state accessors and update direct field
  access in tests or diagnostics to use the common contract.
- [ ] Verify constructors, failed imports, package unload, runtime shutdown, and
  destruction before/after RHI startup do not leak, double-release, or publish
  stale allocations.

Dependencies: Stage 2.

#### Acceptance Gate

- Each texture asset has exactly one base-owned stable reference, completion,
  revision counter, and current concrete resource; both leaf types preserve
  successful replacement and failure fallback behavior; render-resource
  registry and deferred-cleanup counts return to baseline after unload and
  shutdown.

### Stage 4: Integrate, Validate, and Document the Contract

- [ ] Update texture, cube texture, render-resource lifetime, and related code
  documentation so `DTexture` is authoritative for common ownership and leaf
  types are authoritative for topology-specific data.
- [ ] Verify Content Browser class filtering, Texture2D editor previews,
  TextureCube and material thumbnails, source relocation, material texture
  pickers, and sky assignment retain their existing concrete-type restrictions.
- [ ] Verify existing editor and cooked package fixtures load without upgrade or
  resave requirements.
- [ ] Run focused CoreDObject, DurinHeaderTool, texture, cube, material,
  thumbnail, RenderCore, cook, and Vulkan texture sampling validation.
- [ ] Run the complete native test suite, successful full `all` build, and
  normal editor startup/exit using the repository build workflow.
- [ ] Record final validation evidence, move lasting contracts to their owning
  runtime documentation, and complete the plan lifecycle metadata.

Dependencies: Stage 3.

#### Acceptance Gate

- All focused and full validation passes; existing assets require no upgrade;
  user-visible editors and rendering are unchanged; documentation identifies
  one common asset-resource lifecycle owner and preserves dimension-specific
  consumer APIs.

## Validation Matrix

| Boundary | Required validation |
| --- | --- |
| Header tool | `DCLASS(Abstract)` parse, cache round-trip, generated flags and null constructor, unknown-specifier rejection |
| Core reflection | Abstract class registration, superclass traversal, `IsChildOf`/cast behavior, direct/default-object construction rejection |
| Package compatibility | Existing Texture2D and TextureCube fixtures resolve unchanged leaf qualified names and properties without upgrade or resave |
| Common completion | Pending/building/ready/failed/released transitions, stale revision rejection, applied revision monotonicity |
| Asset ownership | First build, replacement, invalidation, unload, destruction with queued commands, RHI-unavailable construction/destruction |
| Texture2D | Import, DDC hit/miss, rebuild settings, cook/load, preview, material fallback and replacement |
| TextureCube | Six-face and panorama import, DDC hit/miss, cook/load, thumbnail, sky fallback and replacement |
| Rendering | Unsupported format, upload failure, multi-mip 2D sampling, six-face cube sampling, stable-reference retargeting |
| Editor | Concrete picker filters, Texture Editor, cube/material thumbnails, source relocation, clean shutdown |
| Repository | Plan validation, complete native tests, full `all` build, normal editor startup and exit |

All configure, build, test, and runtime actions follow
`Documentation/Development/Build/BuildAndRun.md`; this plan does not duplicate
command lines or profile selection.

## Definition of Done

- `DTexture` is the reflected non-instantiable superclass of `DTexture2D` and
  `DTextureCube`.
- DurinHeaderTool and CoreDObject have tested abstract reflected-class support.
- Common lifecycle/status declarations no longer belong to the Texture2D
  header.
- One completion implementation serves both concrete resource types.
- One `DTexture` implementation owns stable reference, revision, replacement,
  release, and deferred cleanup for both leaf asset types.
- No existing reflected property, leaf qualified class name, package schema,
  DDC key, TXPL payload, or cooked payload contract changes.
- Materials remain `DTexture2D`-typed and sky components remain
  `DTextureCube`-typed.
- Existing packages load without upgrade or resave and focused plus full
  validation passes.
- Lasting ownership and thread-boundary rules are documented under
  `Documentation/Runtime/Rendering/`.
- The completed plan records its baseline commit, working set, decisions, open
  questions, validation evidence, and stage handoffs.

## Deferred Follow-ups

- Decide a material texture-dimension type system before allowing material
  parameters to reference `DTexture`, cube, array, or volume assets.
- Add texture class/surface descriptors when a real generic consumer requires
  them; do not add speculative virtual APIs during this refactor.
- Evaluate moving common serialized build settings into `DTexture` only through
  an explicit asset-schema migration plan.
- Reuse the hierarchy for texture arrays, volumes, render targets, virtual
  textures, or runtime-writable textures only when each type has a defined
  source, cook, residency, and sampling contract.
- Coordinate asynchronous build and residency work with
  `TextureSupport.md`.

## Related Documentation

- `Documentation/Runtime/Rendering/TextureSystem.md`
- `Documentation/Runtime/Rendering/CubeTextures.md`
- `Documentation/Runtime/Assets/AssetDataLifecycle.md`
- `Documentation/Development/Build/BuildAndRun.md`
- `Documentation/Plans/TextureSupport.md`
- `Documentation/Plans/MaterialSystem.md`

## Related Code

- `Engine/Source/Runtime/Engine/Public/Texture/Texture2D.h`
- `Engine/Source/Runtime/Engine/Public/Texture/TextureCube.h`
- `Engine/Source/Runtime/Engine/Public/Texture/Texture2DRenderResource.h`
- `Engine/Source/Runtime/Engine/Public/Texture/TextureCubeRenderResource.h`
- `Engine/Source/Runtime/Engine/Private/Texture/Texture2D.cpp`
- `Engine/Source/Runtime/Engine/Private/Texture/TextureCube.cpp`
- `Engine/Source/Runtime/Engine/Private/Texture/Texture2DRenderResource.cpp`
- `Engine/Source/Runtime/Engine/Private/Texture/TextureCubeRenderResource.cpp`
- `Engine/Source/Runtime/RenderCore/Public/RenderResource.h`
- `Engine/Source/Runtime/CoreDObject/Public/DObject/ObjectMacros.h`
- `Engine/Source/Runtime/CoreDObject/Private/DObject/DObjectGlobals.cpp`
- `Engine/Source/Programs/DurinHeaderTool/`
