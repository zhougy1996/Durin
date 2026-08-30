# Bulk Data Legacy Retirement Plan

Summary: Remove the bounded DAST v6/DABK and cooked DBLK compatibility surface after canonical corpus qualification.

Last reviewed: 2026-08-30

Status: Completed
Completed: 2026-08-30

## Current Status

M1 through M3 writers and production Cook paths emit only DAST v7 packages and
headerless raw `.dbulk` segments. The checked-in corpus contains 25 DAST v7
packages, eight raw `.dbulk` companions, no `.dabulk` companion, and one
generated DBLK v2 regression fixture. Stage 0 froze the complete legacy-fixture
inventory and qualified the replacement paths with `AssetPackageTests` (139
tests) and `AssetCookTests` (13 tests) on 2026-08-30. No tracked or generated
legacy input lacks an explicit purpose, and no production writer emits DAST v6,
DABK, or DBLK. Stage 1 removed the v6/DABK codec, migration adapters, fixtures,
suffix policy, and version branches. The permanent reader now accepts only v7
and rejects v6 before object-stream parsing; `AssetPackageTests` passes 127
tests and `AssetMetadataQueryTests` passes six tests with the v7-only policy.
Stage 2 removed the cooked container types and codec, converted texture and
Content Browser summaries to construct-free v7 BulkData metadata, centralized
the ten family registrations behind `RegisterEngineCookContributors`, and made
their implementation methods private. Final validation passed `fast-all` (60
registered targets), the routine `asset-cook` domain, 127 package tests, 10
Cook tests, and the affected texture, mesh, skeletal, material, environment,
Terrain, metadata, and Content Browser suites. The Vulkan-only texture Cook
qualification target built successfully; its host run was separately blocked
by the current MoltenVK device lacking `VK_KHR_swapchain`.

### Frozen legacy fixture inventory

| Representation | Fixture and purpose | Replacement or retirement evidence |
| --- | --- | --- |
| DAST v6 | `current.dasset.hex`: valid compatibility baseline | Generated valid-v6 and external-closure tests prove canonical v7 resave before removal. |
| DAST v6 | `corrupt.dasset.hex`, `truncated.dasset.hex`, and `invalid_object_graph.dasset.hex`: deterministic corruption rejection | Permanent unsupported-version tests replace format-internal corruption coverage. |
| DAST v6 | `unknown_class.dasset.hex`, `unknown_field.dasset.hex`, and `incompatible_signature.dasset.hex`: construct-free reflection compatibility inspection | DAST v7 metadata/object-stream inspection remains covered by `AssetMetadataQueryTests` and package compatibility tests. |
| DAST v6 | Dynamically generated canonical, unknown-skippable, unknown-required, directory-byte mutation, count/index, and measured-reader cases in `PackageV6Tests.cpp` | These characterize only the retired reader and are deleted with it. |
| DAST v6 + DABK v2 | Dynamically generated external closure in `CanonicalResaveMigratesLegacyExternalClosureAndRestoresItOnFailure` | The test proves transactional v7/raw-`.dbulk` publication, verification rollback, and `.dabulk` removal before retirement. |
| DBLK v2 | Dynamically encoded two-payload logical fixtures and corrupt derivations in `CookedAssetTests.cpp`; no binary DBLK file is checked in | DAST v7 field projection and headerless raw-segment publication are covered by `CookPublishesHeaderlessRawPlatformDataFields` plus family Cook suites. |
| `.dabulk` | No checked-in companion; test-local files are generated only for DABK migration, stable-companion recovery, conflict, and rollback cases | After Stage 1, a `.dabulk` beside a v7 package is rejected as an unsupported legacy companion and is never adopted or removed. |

The tracked corpus count is derived from Git rather than build/test output:
six Engine and nineteen Sandbox `.dasset` files, plus eight Sandbox `.dbulk`
companions. Build products under `Engine/Binaries`, test work directories, and
ignored local content are not corpus inputs.

### Deletion-test map

- `AssetPackageTests` owns the v6/DABK canonical-resave proof, the lasting v6
  unsupported-version diagnostic, package/storage matrix, and raw field Cook
  publication.
- `AssetCookTests` owns the DBLK fixture removal, raw save-plan/output-store
  contracts, contributor registration, and the production API inventory gate.
- `AssetMetadataQueryTests` owns construct-free DAST v7 field metadata and
  reference inspection; texture and Content Browser inspection must consume
  those fields without `FCookedPayloadDescriptor`.
- Texture2D, TextureCube, VolumeTexture, StaticMesh, SkeletalMesh, Skeleton,
  AnimationClip, TerrainHeightmap, Material, and EnvironmentLighting family
  suites own contributor-driven project-Cook coverage before their public
  `AddToCook` helpers are deleted.

## Goal

Delete legacy wire readers, adapters, inspection routes, fixtures, and the
temporary public family Cook helpers without removing any supported canonical
resave or runtime-load path.

## Named Retirement Inventory

- DAST v6/DABK read and canonical-resave code in `AssetPackageV6Codec.*`,
  `AssetCanonicalResave.cpp`, `AssetPackageOperations.cpp`,
  `AssetPackageObjectStreamArchiveAdapter.cpp`, `PackageVersionPolicy.h`, and
  `EditorBulkDataStorage.h`. Deletion test: `PackageV6Tests` first proves every
  checked-in or generated v6 fixture canonical-resaves to v7, then is replaced
  by a test that rejects v6 and `.dabulk` with the selected unsupported-version
  diagnostic.
- DBLK v2 descriptors, encoder/decoder, container helpers, and overloads in
  `CookedAsset.h`, `Cook.h`, and `CookedAsset.cpp`. Deletion test:
  `AssetCookTests` removes the generated DBLK fixture and proves all production
  family output is DAST v7/raw-segment only before the decoder is deleted.
- Legacy payload inspection in `TexturePayloadInspection.cpp` and
  `ContentBrowserItemView.cpp`. Deletion test: `AssetMetadataQueryTests` proves
  Content Browser metadata and payload summaries use DAST v7 field inspection
  without constructing a DBLK descriptor.
- Ten public family `AddToCook` helpers for Texture2D, TextureCube,
  VolumeTexture, StaticMesh, SkeletalMesh, Skeleton, AnimationClip,
  TerrainHeightmap, Material, and EnvironmentLighting. Deletion test: each
  affected family suite invokes the class-keyed contributor/project-Cook path;
  an API inventory test rejects production calls outside contributor
  registration.

## Implementation Stages

### Stage 0: Freeze corpus and replacement evidence

- [x] Record every generated and checked-in v6, DABK, DBLK, and `.dabulk`
  fixture with its canonical-resave or intentional-corruption purpose.
- [x] Add the named replacement tests above and confirm no production writer
  emits a legacy representation.

Completion condition: every inventory item has a passing deletion test and no
unknown legacy consumer remains.

### Stage 1: Remove DAST v6 and DABK

- [x] Delete the v6 codec, DABK suffix/storage policy, resave adapter, and
  version branches; preserve explicit unsupported-version diagnostics.
- [x] Remove obsolete fixtures and update package/storage contracts.

Completion condition: the repository builds and the package matrix passes with
no DABK symbol, `.dabulk` path, or v6 reader.

### Stage 2: Remove DBLK and temporary Cook APIs

- [x] Delete DBLK descriptors, encoder/decoder, inspection adapters, and the
  generated regression fixture.
- [x] Migrate family tests to contributor-driven project Cook, then remove the
  public family `AddToCook` helpers.

Completion condition: only DAST v7 field metadata and headerless raw package
segments remain, and the full registered test matrix passes.

## Related Documentation

- [Package Bulk Data System roadmap](../Roadmaps/PackageBulkDataSystem.md)
- [Package bulk data](../Runtime/Assets/BulkData.md)
- [Asset data lifecycle](../Runtime/Assets/AssetDataLifecycle.md)

## Related Code

- `Engine/Source/Runtime/Engine/Private/Asset/CookedAsset.cpp`
- `Engine/Source/Runtime/Engine/Private/Asset/EngineCookContributors.cpp`
- `Engine/Source/Editor/ContentBrowser/Private/Panels/ContentBrowserItemView.cpp`
- `Engine/Tests/Native/AssetCoreTests/`
