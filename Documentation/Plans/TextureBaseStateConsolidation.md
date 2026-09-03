# Texture Base State Consolidation Plan

Summary: Move shared texture source, import metadata, and cooked bulk storage into DTexture while preserving existing asset data and family-specific platform behavior.

Last reviewed: 2026-09-03

Status: Active
Completed:

## Current Status

Planning only; no stage has started. The preceding interface refactors
(`b1213e289`, `e8bc7db63`) established common texture access and blocking cooked
loading. Their latest affected validation passed 45 native-test targets.
That evidence is a baseline, not validation of the member moves in this plan.

`DTexture2D`, `DTextureCube`, and `DVolumeTexture` still each own identical
`Source`, `AssetImportData`, and `CookedPlatformData` storage. Their source and
import accessors override base virtual interfaces; a protected mutable source
hook exists only to let `DTexture::SetSource` assign derived storage.

The package field identity includes its declaring type. The current loader
discards removed authored fields on known types, so moving reflected members
without explicit compatibility handling can discard old texture data.
Stage 0 must settle and verify this boundary before production fields move.

## Goal

Give DTexture one authoritative copy of common asset state and one implementation
of its access and validation. Keep typed platform data, build recipes, and GPU
resource creation in the concrete texture families. Existing authored and cooked
assets must retain their data and behavior across the refactor.

## Selected Ownership And Boundaries

| State or operation | Final owner and behavior |
| --- | --- |
| `FTextureSource Source` | Private `DTexture` reflected `EditorOnly` field; public const non-virtual `GetSource`, protected validated `SetSource` |
| `TObjectPtr<DAssetImportData> AssetImportData` | Private `DTexture` reflected `EditorOnly` field; common non-virtual const/mutable getters and setter |
| `FBulkData CookedPlatformData` | `DTexture` storage; common non-virtual const getter; narrow internal access for family serialization/loading |
| Installed `PlatformData` | Concrete family retains its existing typed `unique_ptr` and validators |
| Authored build parameters | Remain in the existing concrete family, including 2D usage/compression/alpha policy, cube panorama settings, and volume build settings |
| `bSRGB` | Remains in 2D/Cube; this plan does not introduce an unused volume setting |
| Resource references, completion, revision, teardown | Remain owned by `DTexture` with existing sequencing |
| `HasPlatformData`, `LoadCookedPlatformData`, `CreateRenderResourceCandidate` | Remain virtual because concrete data validation, decoding, and resource construction differ |

Remove `GetTextureSourceStorage` entirely once Source lives in DTexture.
Remove the derived source/import/cooked getters and import setters when the
base owns their implementations. Do not introduce templates, type erasure, or
a new platform-data hierarchy merely to combine different family payloads.

Source replacement retains GameThread validation and existing resource/update
semantics. Import data must remain a validated inner object of the texture;
rejection leaves the previous pointer intact. Importing, reimporting, and
building continue through their current family providers and application paths.

Cooked storage relocation must preserve the existing explicit native field
identities (`Durin::DTexture2D::PlatformData`, `Durin::DTextureCube::PlatformData`,
`Durin::DVolumeTexture::PlatformData`). Family codecs and serializers remain in
their current owners. Moving C++ storage alone does not require moving those
wire identities or changing the TXPL payload, DAST version, or DDC keys.

## Compatibility Requirements

- Preserve legacy derived `Source` and `AssetImportData` values through load,
  including bulk payload identity and inner-object references. Their imports
  must not be pruned as dependencies of discarded fields.
- New saves emit only the canonical DTexture identities for these two reflected
  fields. Loading never rewrites source packages or marks them Dirty solely
  because their fields need canonical resave.
- Preserve authored override intent when field ownership changes. Resolve
  historical identity before value application and before family PostLoad builds
  from the source. Inspection, schema probing, and live loading must agree.
- Reject a package that supplies both legacy and canonical identities for the
  same logical field; do not silently choose a value. Incompatible types,
  malformed bulk ranges, and invalid references retain strict failure behavior.
- Do not make every same-named field in an ancestor interchangeable. Any
  compatibility route must match the exact historical owner, name, and type.
- The plan does not authorize rewriting the tracked content corpus, dropping
  old texture values, broadening support to older DAST versions, or introducing
  a persistent second authoritative Source/import pointer.

## Implementation Stages

### Stage 0: Prove historical field migration

Dependency: none. Outcome: a selected, tested compatibility mechanism and
fixtures captured before moving the production fields.

- [ ] Inspect existing serialized-property aliases and deprecated-property
  routes for cross-declaring-type moves, reference admission, bulk ownership,
  and authored override restoration. Prefer the existing mechanism if it meets
  every requirement above; do not assume a rename alias also changes its owner.
- [ ] Resolve the mechanism in this stage: either use the proven existing route
  or specify the smallest exact-identity extension required. Record the selected
  API, registration owner/lifetime, canonicalization order, collision policy,
  and whether temporary deprecated fields are needed. Such fields must remain
  non-authoritative and have an explicit retirement policy.
- [ ] Capture deterministic pre-move authored fixtures for 2D, Cube, and Volume,
  with nondefault source data, owned import data, and explicit override intent;
  include inline and external bulk coverage and corresponding pre-move cooked
  packages for Stage 2. Use the existing fixture conventions and do not depend
  on mutable user content or a local absolute path.
- [ ] Add focused compatibility coverage showing exact old-to-new field mapping,
  preserved dependency admission and override intent, canonical resave, and
  rejection of duplicate/incompatible identities. If infrastructure needs a
  change, validate it with a bounded fixture before production cutover.

Completion: fixtures reproduce the old schema and the chosen route passes the
required load/inspection/resave contract. Record any added module scope before
starting Stage 1. No texture field has moved yet.

### Stage 1: Consolidate source and import metadata

Dependency: Stage 0. Outcome: one base-owned source and import pointer for all
three families, with historical data retained.

- [ ] Move `Source` and `AssetImportData` into DTexture with their existing
  EditorOnly policy, GC participation, defaults, and ownership validation.
- [ ] Install the Stage 0 historical routes in the same change as the field move.
  Apply migrated state before PostLoad and family build-input creation.
- [ ] Make `GetSource`, both `GetAssetImportData` overloads, and
  `SetAssetImportData` common non-virtual methods. Make SetSource assign base
  storage and remove every mutable source-storage hook and derived duplicate.
- [ ] Audit direct reflection lookups, property owners, compilation friends,
  transactions, duplication, reimport, payload inspection, and editor consumers;
  use the canonical base field identity where ownership is required.
- [ ] Validate all three legacy fixtures and new assets through load, duplicate,
  edit/undo-redo where applicable, explicit save, reload, and reimport. Check
  exact source payload identity/content, import Outer/references, and override
  intent; successful load alone is not sufficient evidence.

Completion: old assets preserve their data, new/resaved assets contain one
canonical field of each kind, and family build inputs/results remain unchanged.

### Stage 2: Consolidate cooked bulk storage

Dependency: Stage 1. Outcome: one base-owned cooked bulk slot with unchanged
family wire formats and lazy-loading behavior.

- [ ] Move CookedPlatformData storage and its const getter to DTexture. Keep
  mutation restricted to the serialization/loading implementation; do not add a
  public mutable bulk accessor.
- [ ] Retain each family serializer's native PlatformData field identity and
  typed payload codec. Remove redundant derived storage/getters and review
  include/export boundaries across Engine and editor modules.
- [ ] Validate pre-move cooked packages and new cooks for 2D, Cube, and Volume:
  passive getters do not load bulk or update resources; the first explicit
  blocking load installs data; repeated calls preserve pointer/revision; missing
  or corrupt data fails without publishing an invalid replacement.
- [ ] Confirm authored-source filtering, source-free cooked loading, payload
  content/identity, and the existing Win64 Game target restriction are unchanged.

Completion: all three families share bulk storage while existing cooked assets
load and resource lifecycle behavior matches the baseline.

### Stage 3: Qualify and document the final boundary

Dependency: Stages 1-2. Outcome: verified cleanup and durable ownership guidance.

- [ ] Audit the final headers/callers for duplicate storage, forwarding overrides,
  and unused hooks. Retain family-specific APIs that still have real consumers.
- [ ] Run affected native coverage under the repository testing workflow,
  including texture build/reimport, thumbnail, cooked integration, and package
  compatibility coverage. Discover current registry selections rather than
  inferring target names from test source directories.
- [ ] Complete a full `all` build on the selected host profile to verify the
  exported base API across the editor/module closure. No visual UI change or
  application-hosted smoke is required by this plan.
- [ ] Update the owning runtime asset/serialization documents only for newly
  implemented contracts, linking to existing package and volume contracts rather
  than duplicating their specifications.
- [ ] Record exact validation evidence and remaining limitations, validate plan
  metadata, and mark Completed only after all acceptance gates pass.

Completion: implementation and documentation agree, all required checks pass,
and every stage's changes and status are committed with the required provenance.

## Execution And Validation

Follow [Build And Run](../Agents/BuildAndRun.md),
[Testing](../Agents/Testing.md), and
[Documentation](../Agents/Documentation.md). Native test metadata/fixture changes
also follow [Native Test Authoring](../Development/Build/NativeTestAuthoring.md).
Each stage must leave the checkout buildable and commit its implementation,
tests, and status together using the exact Plan and Stage trailers required by
the repository. Never treat the earlier interface-refactor test results as
acceptance evidence for this plan.

## Related Code

- `Engine/Source/Runtime/Engine/Public/Texture/{Texture,Texture2D,TextureCube,VolumeTexture}.h`
- `Engine/Source/Runtime/Engine/Private/Texture/`
- `Engine/Source/Runtime/Engine/Private/Asset/AssetPackageLinkerLoader.cpp`
- `Engine/Source/Runtime/Engine/Private/Asset/PackageSchema.cpp`
- `Engine/Source/Runtime/CoreDObject/Public/DObject/Class.h`
- `Engine/Source/Runtime/CoreDObject/Private/DObject/Class.cpp`
- `Engine/Source/Editor/AssetForgeBuiltins/Private/`
- `Engine/Source/Editor/TextureEditor/Private/`
- `Engine/Tests/Native/EngineTests/Private/Texture/`
- `Engine/Tests/Native/EngineTests/Private/TextureCubeTests.cpp`
- [Serialization](../Runtime/Core/Serialization.md)
- [Asset Packages](../Runtime/Assets/AssetPackages.md)
- [Versioning](../Runtime/Assets/Versioning.md)
- [Volume Textures](../Runtime/Assets/VolumeTextures.md)

UE reference: [UTexture](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Engine/UTexture)
owns Source and AssetImportData. This informs the ownership choice; Durin's
existing family platform types, bulk format, and compatibility policy remain
the implementation constraints.
