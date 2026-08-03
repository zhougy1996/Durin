# Single-Root Mount Semantics Plan

Summary: Replace Content/SourceAssets dual-domain mounts with one configurable content directory per virtual mount while preserving typed asset and source identities.

Last reviewed: 2026-08-03

Status: Active
Completed:

## Current Status

- Stages 0 through 2 were initially completed on 2026-08-03. Stage 1 started
  from baseline `dd63da85` (`docs(mounts): freeze single-root migration
  contract`); Stage 2 started from baseline `9064a467` (`refactor(mounts):
  unify virtual mount roots`). Before the Stage 3 move was committed, the
  mount contract was amended by owner decision. The amended Stage 1 and Stage 2
  have now passed validation; Stage 3 is current.
- The breaking migration remains one physical content namespace per logical mount, with no
  compatibility parser, fallback resolver, or legacy `SourceAssets` lookup.
- The amended C++ mount fields are `Root`, `ContentPath`, `bAutoScan`, and
  `bAuthoringWritable`; `GetContentDir()` is the only physical namespace root.
  `Root` identifies the project, plugin, or library root while `ContentPath` is
  a configurable relative path beneath it. Public Content-domain path terminology becomes asset
  terminology: `FAssetPathResult`, `ResolveAssetPath`, and
  `ClassifyAssetPath`; the raw-file mutation check becomes
  `CheckAuthoringMutation`. `FMountPathResult` is the shared resolver result.
- The amended descriptor fields are exactly `VirtualRoot`, `Owner`, `Root`,
  `ContentPath`, `AutoScan`, `AuthoringWritable`, and `Dependencies`. `Root` is
  resolved relative to the descriptor directory; `ContentPath` is resolved
  beneath that root and may be `.`. `AutoScan` controls automatic `.dasset`
  discovery only and never changes asset or source path validity.
- Checked-in `Engine/Engine.dproject` and `Sandbox/Sandbox.dproject` contain no
  custom mounts. The only custom-mount contract fixture is
  `Engine/Tests/Native/EngineTests/Data/SourceLibraryReferences/UnifiedMountContract.json`:
  `/Plugins/PCG/` uses root `Plugin` with content path `Content` and automatic
  scanning, while `/Libraries/StudioArt/` uses root `StudioArt` with content
  path `.` and manual discovery. Both mounts can resolve asset and source paths. No
  real custom mount needs splitting or persisted virtual-root migration.
- The five exact physical moves below have no existing target collision:
  `Engine/SourceAssets/Models/Editor/MaterialPreview/Box.obj` to
  `Engine/Content/Models/Editor/MaterialPreview/Box.obj`;
  `Engine/SourceAssets/Models/Editor/MaterialPreview/Sphere.obj` to
  `Engine/Content/Models/Editor/MaterialPreview/Sphere.obj`;
  `Sandbox/SourceAssets/Models/Models/Mesh_Teapot.obj` to
  `Sandbox/Content/Models/Models/Mesh_Teapot.obj`;
  `Sandbox/SourceAssets/Textures/Textures/TEXCUBE_PureSky_512x512_panorama.hdr`
  to `Sandbox/Content/Textures/Textures/TEXCUBE_PureSky_512x512_panorama.hdr`;
  and `Sandbox/SourceAssets/Textures/Textures/TEX_StoneHead.jpg` to
  `Sandbox/Content/Textures/Textures/TEX_StoneHead.jpg`.
- Persisted-source inventory found five non-empty `FSourcePath` values in the
  seven tracked `.dasset` packages: the two material-preview meshes retain
  their `/Engine/Models/Editor/MaterialPreview/*.obj` identities, and the
  teapot, stone-head texture, and panorama retain their existing `/Game/`
  identities. No tracked standalone ImportRecord exists and no package resave
  is required for the physical-only move.
- Direct mount initializers are confined to Core plus ten native-test files;
  root-field consumers are confined to the 24 files returned by the Stage 0
  `ContentRoot|SourceAssetsRoot` inventory. The legacy PathUtilities test
  registration helper has 26 test/test-support consumers; shader mount helpers
  with the same unqualified name are unrelated and remain unchanged. Resolver
  consumers are confined to the 20 files returned by the
  `ResolveContentPath|ClassifyContentPath|ResolveSourcePath|ClassifySourcePath|CheckSourceMutation`
  inventory. These inventories, the two descriptors, and the unified fixture
  account for every production and test mount surface before implementation.
- Baseline validation passed with `CoreFileSystemTests` (30 passed, 1 skipped),
  `AssetPackageTests` (29/29), `AssetImportCoreTests` (20/20),
  `AssetCookTests` (11/11), `EditorAssetWorkflowTests` (40/40), and
  `TextureTests` (61/61) using `Win64-Debug-DurinEditor-Tests`.
- Stage 0 handed Stage 1 the working set `Paths.h`, `Paths.cpp`, `PathsTests.cpp`,
  `ProjectTests.cpp`, and `UnifiedMountContract.json`. The key symbols are
  `FMountPoint`, the typed resolver/classifier APIs, descriptor parsing,
  registry publication, and `FScopedMountRegistryFixture`; no open ownership
  or schema question remains.
- The initial Stage 1 replaced the dual roots with `FMountPoint::Root`, added package and
  authoring policies, unified forward and reverse physical mapping,
  rejected canonical root overlap, replaced the descriptor parser atomically,
  and removed the sibling-`SourceAssets` test helper. Package-disabled mounts
  resolve both asset and source paths; automatic scanning remains separately configurable. Public API
  and fixture call sites were migrated mechanically so the repository remains
  buildable.
- Stage 1 validation passed `CoreFileSystemTests` (30 passed, 1 skipped),
  `EditorAssetWorkflowTests` (40/40), and a full `all` build on
  `Win64-Debug-DurinEditor-Tests`. Searches found no old Core root fields,
  Content resolver symbols, domain branches, or old fields in the active
  unified-mount fixture.
- The reopened Stage 1 working set is `Paths.h`, `Paths.cpp`, `PathsTests.cpp`,
  `ProjectTests.cpp`, and `UnifiedMountContract.json`. The required correction
  is to introduce configurable `ContentPath`, make overlap and containment use
  `GetContentDir()`, rename package capability to scan policy, and remove the
  scan flag from typed asset validation. The five Stage 3 physical moves remain
  uncommitted in the working tree.
- The amended Stage 1/2 implementation makes `Root` the owner root and derives
  the single mapping root through configurable `ContentPath` and
  `GetContentDir()`. Built-in mounts use their engine/project roots plus
  `Content`; custom descriptors require both fields. Canonical containment and
  overlap compare effective content directories, so owner roots may be shared
  when their content subdirectories are disjoint.
- `AssetPackages` and `AssetPackagesDisabled` are removed. Every mount admits
  `FAssetPath` and `FSourcePath`; `bAutoScan`/`AutoScan` controls recursive
  Asset Registry discovery only. A focused package test creates, saves, and
  directly reloads a package from a manual-scan mount while confirming registry
  enumeration remains zero.
- Amended validation passed `CoreFileSystemTests` (32 passed, 1 skipped),
  `AssetPackageTests` (30/30), `AssetImportCoreTests` (20/20),
  `EditorAssetWorkflowTests` (41/41), and a full `all` build on
  `Win64-Debug-DurinEditor-Tests`.
- Stage 2 working set is the runtime and editor consumers named in Related Code,
  plus their focused native tests. Key decisions carried forward are that raw
  and package paths share `Mount.GetContentDir()`, package discovery filters only on
  `bAutoScan`, all mounts admit typed asset and source paths, and raw-file mutation
  filters only on `bAuthoringWritable` plus existing ownership/dependency rules.
- Stage 2 removed every production `SourceAssets` convention and diagnostic.
  Texture, TextureCube, and StaticMesh defaults now use mount-relative
  `Textures` and `Models` locations; explicit destinations remain inside the
  selected mount without a hidden directory prefix. Asset discovery and
  Content Browser snapshots filter on `bAutoScan`, while source resolution
  has no capability gate and authoring mutation uses `bAuthoringWritable` plus
  the preserved ownership and dependency checks. Content Browser no longer
  enumerates or offers a toggle for raw authoring files.
- Stage 2 validation passed `CoreFileSystemTests` (30 passed, 1 skipped),
  `AssetPackageTests` (29/29), `AssetImportCoreTests` (20/20),
  `EditorAssetWorkflowTests` (41/41), `TextureTests` (61/61),
  `StaticMeshTests` (44/44), `ThumbnailTests` (45/45), and a full `all` build on
  `Win64-Debug-DurinEditor-Tests`. A production/test search found no remaining
  `SourceAssets` use except the deliberate old-descriptor rejection literal.
- After the reopened Stage 1 and Stage 2 pass, Stage 3 working set is exactly the five files frozen in the Stage 0 move
  manifest plus this plan. Their persisted `/Game/` and `/Engine/` source
  identities remain unchanged, so no `.dasset` resave or ImportRecord rewrite
  is required. Validation will compare pre/post hashes and resolve every tracked
  source reference through the new Content roots.

## Goal

Give every virtual mount one stable and unsurprising physical interpretation:

```text
/Game/<relative>       -> <active project>/Content/<relative>
/Engine/<relative>     -> <engine>/Content/<relative>
/<custom>/<relative>   -> <custom root>/<content path>/<relative>
```

`FAssetPath` and `FSourcePath` remain distinct value types, but neither type may
select a different physical root for the same mount. Asset paths remain
extensionless package identities resolved as `.dasset`; source paths retain a
filename extension and identify ordinary authoring files.

## Scope

- Replace `FMountPoint::ContentRoot` and `FMountPoint::SourceAssetsRoot` with a
  normalized absolute owner `Root`, configurable relative `ContentPath`, and
  derived `GetContentDir()`.
- Replace the project-descriptor `Domains` object with direct `Root` and
  `ContentPath` fields.
- Give `/Game/` and `/Engine/` their real project/engine roots and the explicit
  content path `Content`.
- Preserve explicit mount ownership, dependency edges, authoring-write policy,
  canonical containment, immutable publication, and longest-prefix lookup.
- Preserve an automatic package-scan flag so external art-library mounts can
  opt out of recursive discovery without becoming a different mount type.
- Make asset and source typed resolvers delegate to the same single-root
  primitive and remove domain-specific error states and terminology.
- Migrate checked-in files from Engine and Sandbox `SourceAssets` into the
  corresponding `Content` trees without changing their virtual relative paths.
- Update source ingestion, reimport, source-reference indexing, thumbnails,
  import dialogs, asset discovery, project parsing, tests, and active contract
  documentation.
- Reject old custom mount descriptors instead of interpreting `Domains` or
  deriving a legacy sibling `SourceAssets` directory.

## Non-Goals

- Merging `FAssetPath` and `FSourcePath` into one value type.
- Making raw source files loadable as `DObject` assets or serializing them as
  `.dasset` packages.
- Shipping raw authoring files in cooked runtime output.
- Adding dynamic or session-local mounts; registry publication remains
  immutable for the active project lifetime.
- Supporting arbitrary absolute paths in committed project descriptors.
- Reorganizing source-relative names such as existing repeated
  `Models/Models` or `Textures/Textures` segments. The initial migration
  preserves virtual identities exactly.
- Retaining a hidden `SourceAssets` directory convention, implicit sibling
  lookup, compatibility reader, or automatic fallback.
- Redesigning the Content Browser to become a general raw-file browser.

## Design Decisions and Invariants

### One virtual root has one effective content directory

- `FMountPoint::Root` is the absolute normalized project, plugin, or library
  root. `ContentPath` is an explicit relative path beneath it, and
  `GetContentDir()` is the only root used for virtual-path mapping.
- `/Game/` uses the active project root plus `Content`; `/Engine/` uses the
  engine root plus `Content`.
- A custom descriptor's `Root` is resolved relative to the active project
  descriptor and its `ContentPath` may name `Content`, `Assets`, `.`, or another
  contained directory. Neither field selects an asset/source domain.
- Reverse classification considers the same effective content directories used
  for forward resolution. Published content directories may not alias or canonically overlap in a way that makes a
  physical path classify to more than one mount.

### Typed paths remain typed

- `FAssetPath` remains extensionless. Every registered mount may form package
  identities, and AssetCore appends the package extension at its existing
  ownership boundary.
- `FSourcePath` retains its complete filename and extension. It may reference
  an ordinary file under any registered mount, subject to dependency and
  existence checks.
- Asset and source typed APIs may report different validation errors, but both
  must resolve the virtual relative path against the same `GetContentDir()`.
- A raw file and a package may coexist by using distinct physical filenames,
  for example `Robot.gltf` and `Robot.dasset`.

### Capabilities are policy, not alternate mappings

- Every mount may contain packages and raw authoring files. `bAutoScan` controls
  only whether Asset Registry recursively discovers `.dasset` files beneath
  that mount; it never gates `FAssetPath`, direct package loading, or source
  resolution.
- Authoring-file mutation remains an explicit mount policy. Rename the current
  source-domain-specific field to an authoring-oriented name and keep the
  existing same-mount, dependency, and Engine-authoring-context restrictions.
- Read access to a registered source file does not depend on a separate source
  capability or domain.
- Dependency checks remain mount-to-mount and independent of file type.

### Descriptor contract is replaced atomically

Custom mounts use one selected schema equivalent to:

```json
{
  "VirtualRoot": "/Libraries/StudioArt/",
  "Owner": "ExternalSources",
  "Root": "Libraries/StudioArt",
  "ContentPath": ".",
  "AutoScan": false,
  "AuthoringWritable": false,
  "Dependencies": ["/Engine/"]
}
```

- `Root` is the project, plugin, or library root; `ContentPath` is relative to
  it and must remain canonically contained by it.
- `AutoScan` controls automatic `.dasset` discovery only.
- `AuthoringWritable` controls ingestion and other raw authoring-file
  mutations.
- `Domains`, `Content`, `SourceAssets`, `AssetPackages`, and `SourceWritable` are invalid fields
  after the migration.
- Descriptor-relative containment and the ban on `..` and absolute committed
  paths remain unchanged.

### Persistence and migration

- Existing virtual source identities beneath `/Game/` and `/Engine/` remain
  unchanged. Their physical files move from `SourceAssets/<relative>` to
  `Content/<relative>`.
- Existing ImportRecords and reflected `FSourcePath` properties are rewritten
  only when an inventory finds a virtual-root change. No reader accepts a
  physical `SourceAssets` segment or consults the removed directory.
- Custom dual-root mounts must choose one root before migration. If package and
  source files cannot share it, they are split into two explicitly named
  virtual mounts and all affected persisted paths are migrated in the same
  stage.
- Import, reimport, hashing, dependency capture, and derived-data keys continue
  to use normalized virtual source identities plus captured bytes; moving the
  physical root alone must not change source identity.

### Runtime and cooking boundaries

- Asset Registry scans only mounts with `AutoScan` enabled and only package
  files recognized by the existing package contract.
- Raw FBX, glTF, OBJ, image, and other authoring files under an automatically scanned
  root remain editor inputs and are excluded from cooked runtime output.
- Cooked loading must succeed when authoring files are absent, exactly as under
  the current SourceAssets model.
- Content Browser remains package-oriented. Source-file selection continues
  through source-aware pickers and import workflows, now rooted at the selected
  mount's single physical directory.

## Current Foundations and Gaps

### Foundations to preserve

- `PathUtilities` already centralizes mount publication, longest-prefix lookup,
  dependency policy, canonical containment, reverse classification, and test
  fixtures.
- `FAssetPath` and `FSourcePath` already separate package identity from complete
  source filenames.
- Asset Registry already recognizes `.dasset` packages rather than treating
  every file below a Content root as an asset.
- Mounted-source import already requires explicit destinations and checks
  dependency and mutation policy before publication.
- ImportRecords persist virtual source paths and captured fingerprints rather
  than absolute workstation paths.

### Gaps to close

- `FMountPoint` owns two optional roots, and typed resolution selects one by
  pointer-to-member.
- `/Game/` and `/Engine/` publish implicit sibling `SourceAssets` directories.
- Custom descriptors require a `Domains` object and may map one virtual identity
  to unrelated physical trees.
- Editor pickers, import helpers, thumbnails, Texture2D, StaticMesh, and source
  reference workflows query `SourceAssetsRoot` directly.
- Asset validation and Content Browser snapshots query `ContentRoot` directly.
- Tests and active documentation encode domain-specific error cases and
  `SourceAssets` terminology.
- The compatibility-only `RegisterMountPoint` fixture helper creates a sibling
  `SourceAssets` directory and must be replaced rather than preserved.

## Implementation Stages

### Stage 0: Freeze the migration manifest and public contract

Outcome: the exact schema, symbol migration, physical moves, and persisted-path
impact are recorded before changing the resolver.

- [x] Confirm the final `FMountPoint` field names for the single root, package
  discovery capability, and authoring-write policy.
- [x] Inventory every checked-in `.dproject`, custom-mount fixture, direct
  `FMountPoint` initializer, `SourceAssetsRoot`/`ContentRoot` consumer, and
  persisted `FSourcePath`/ImportRecord source reference.
- [x] Compare the five checked-in SourceAssets files against their target
  Content paths and fail the stage on any non-identical collision.
- [x] Record the exact move list, preserving each path relative to its old
  SourceAssets root.
- [x] Decide whether any custom dual-root fixture becomes one combined mount or
  two explicit mounts; update this plan before implementation if a real project
  mount is discovered.
- [x] Capture the baseline Core path, source reference, import, asset registry,
  cook, and editor workflow test results.

Dependencies: none.

#### Acceptance Gate

- The migration manifest accounts for every production and test mount and every
  checked-in SourceAssets file.
- The replacement descriptor fields and C++ API names are unambiguous.
- There is no unresolved physical collision or persisted-path ownership choice.

### Stage 1: Replace the Core mount and descriptor model

Outcome: registry publication and path resolution use one effective content
directory per mount, with no dual-domain parser or fallback.

- [x] Replace `ContentRoot` and `SourceAssetsRoot` with owner `Root`, relative
  `ContentPath`, derived `GetContentDir()`, and the selected policy fields.
- [x] Add one shared forward-resolution and reverse-classification primitive;
  make typed asset and source entrypoints enforce type rules around that
  primitive.
- [x] Rename Content-domain result and API types to asset/package terminology
  where they remain public; remove unsupported-domain branches that existed
  only because a second root was optional.
- [x] Build `/Game/` and `/Engine/` from their actual roots plus explicit
  `Content` paths.
- [x] Replace descriptor `Domains` parsing with the new exact-field schema and
  reject old descriptors as unknown/invalid.
- [x] Validate normalized roots, content-path containment, effective-directory
  canonical overlap, traversal, longest-prefix behavior, missing roots,
  dependency edges, and authoring-write policy.
- [x] Replace the legacy test registration helper with a unified fixture API
  and migrate all direct fixture initializers.
- [x] Rewrite Core path tests and the unified mount contract fixture around
  configurable content paths, including non-auto-scanned mounts that still
  admit asset and source identities.

Dependencies: Stage 0.

#### Acceptance Gate

- No Core code or active fixture contains `ContentRoot`, `SourceAssetsRoot`, or
  descriptor `Domains` handling.
- The same virtual path produces the same mount-relative physical path in asset
  and source typed resolution.
- Old descriptor shapes fail deterministically; no compatibility branch or
  sibling-directory inference exists.
- Core path and descriptor contract tests pass.

### Stage 2: Migrate runtime and editor consumers

Outcome: all package and source workflows consume the single-root contract
without direct domain knowledge.

- [x] Update `FAssetPath`, AssetCore package resolution, registry scanning, and
  Content Browser mount snapshots to use `GetContentDir()` and the auto-scan policy.
- [x] Update `FSourcePath`, source reference validation, source relocation,
  hashing, thumbnail caching, Texture2D, TextureCube, StaticMesh, and material
  preview source loading.
- [x] Update mounted-source reference and ingestion operations to choose a
  mounted destination path without assuming `SourceAssets`.
- [x] Update Texture, TextureCube, Scene Source, and Texture Editor pickers to
  browse the applicable mount root and use neutral “source destination” wording.
- [x] Ensure glTF relative dependencies remain inside and resolve through the
  root source's mount after ingestion.
- [x] Replace domain-specific diagnostics with mount, dependency, containment,
  existence, and authoring-write diagnostics.
- [x] Keep Content Browser package-only even when raw files share its physical
  root.

Dependencies: Stage 1.

#### Acceptance Gate

- All runtime and editor targets compile without domain-root fields or helpers.
- Existing import and reimport workflows resolve source bytes from the new root,
  preserve dependency checks, and never consult `SourceAssets`.
- Non-auto-scanned mounts can form valid asset and source identities but do not
  enter recursive Asset Registry discovery.
- Source reference, thumbnail, texture import, scene import, and editor asset
  workflow tests pass.

### Stage 3: Move checked-in source files and migrate persisted records

Outcome: the workspace contains no live SourceAssets tree and all checked-in
authoring references resolve through the new roots.

- [ ] Move the two Engine authoring meshes from `Engine/SourceAssets` to the
  same relative paths beneath `Engine/Content`.
- [ ] Move the three Sandbox authoring files from `Sandbox/SourceAssets` to the
  same relative paths beneath `Sandbox/Content`.
- [ ] Apply the Stage 0 custom-mount fixture/configuration migration.
- [ ] Audit checked-in `.dasset` packages and ImportRecords. Preserve virtual
  source strings when their mount root is unchanged; explicitly resave or
  regenerate every record whose virtual root changes.
- [ ] Remove obsolete SourceAssets directories and update version-control or
  tooling paths that name them.
- [ ] Verify that all checked-in source references resolve and their persisted
  fingerprints still match the moved bytes.

Dependencies: Stage 2.

#### Acceptance Gate

- `Engine/SourceAssets` and `Sandbox/SourceAssets` contain no tracked or required
  files.
- No checked-in package or ImportRecord references an unknown mount or requires
  a compatibility resolver.
- Reimport of representative Engine and Game assets succeeds from the moved
  source files without changing their virtual source identities.
- Asset registry scans do not expose raw authoring files as assets.

### Stage 4: Validate cooking and end-to-end editor behavior

Outcome: the unified mount works in authoring, reimport, discovery, cooking, and
source-absent runtime scenarios.

- [ ] Validate Game-to-Engine, Game-to-external-library, forbidden
  Engine-to-Game, same-mount mutation, read-only mount, missing root, escape,
  and canonical-overlap cases.
- [ ] Import a texture and an FBX/glTF Scene Source into `/Game/`, close/reload
  their packages, and reimport from the single-root source paths.
- [ ] Validate asset and source references from a non-auto-scanned external mount.
- [ ] Validate Asset Registry and Content Browser behavior when `.dasset` and raw
  source files coexist beneath `/Game/`.
- [ ] Cook representative assets, remove or hide the authoring inputs from the
  runtime environment, and verify cooked loading and rendering.
- [ ] Run the complete related native suites, a successful full `all` build, and
  an editor startup smoke test through the documented development tool entrypoint.

Dependencies: Stage 3.

#### Acceptance Gate

- Every row in the validation matrix passes from a clean build profile.
- Cooked runtime loading has no source-file or authoring-mount dependency.
- The editor can import, reload, and reimport both local and externally mounted
  sources using the same mount semantics.
- No old resolver symbols, dual-domain fields, SourceAssets runtime paths, or
  compatibility diagnostics remain in production code.

### Stage 5: Publish lasting contracts and complete the plan

Outcome: authoritative documentation describes only the implemented
single-root model, and the plan carries complete validation evidence.

- [ ] Update Workspace, runtime asset, editor import, version-control, mesh,
  texture, material, cube-texture, and level-system documents that currently
  describe SourceAssets domains.
- [ ] Keep historical archived plans unchanged except for mechanically repaired
  direct links; they remain historical evidence rather than current contracts.
- [ ] Update this plan's status, checklist, baseline/working-set handoff, and
  validation evidence after every substantive stage.
- [ ] Run changed-document validation and the all-plan validator.
- [ ] Set `Status: Completed` and the completion date only after every required
  gate has passed and lasting rules exist in their owning documentation domains.

Dependencies: Stage 4.

#### Acceptance Gate

- Active documentation contains one consistent single-root mount explanation.
- Documentation and all-plan validation pass.
- The definition of done is satisfied and completion evidence is recorded in
  `Current Status`.

## Validation Matrix

| Area | Required evidence |
| --- | --- |
| Core mount parsing | New exact descriptor schema accepted; `Domains` and old fields rejected |
| Forward resolution | Asset and source typed paths share one root and relative mapping |
| Reverse classification | Game, Engine, custom, missing, escaped, symlink/junction, and overlap cases |
| Automatic discovery | Auto-scanned mounts enter registry discovery; manual mounts retain valid asset/source identities without recursive scanning |
| Dependency policy | Same mount, Game-to-Engine, external dependency, and forbidden reverse edge |
| Mutation policy | Writable same-mount ingestion succeeds; read-only, cross-mount, and unauthorized Engine writes fail |
| Asset packages | Save, load, move, delete, registry rebuild, and reload remain correct |
| Source references | Texture, mesh, cube, material preview, index, relocation, and fingerprint verification |
| Import framework | Texture and Scene import/reimport, external ingestion, glTF dependency closure, ImportRecord reload |
| Editor | Source pickers, output destination, Content Browser filtering/navigation, error diagnostics |
| Cooking | Raw sources excluded; cooked assets load and render with source files unavailable |
| Repository hygiene | No production `SourceAssetsRoot`, `ContentRoot`, `Domains.SourceAssets`, legacy parser, or required SourceAssets directory |
| Build and smoke | Related native tests, full `all` build, and editor startup smoke pass using the documented workflow |

Build, test, run, timeout, and recovery behavior follow
[Build and Run](../Development/Build/BuildAndRun.md); this plan does not duplicate
machine-local commands.

## Definition of Done

- Every registered virtual mount has exactly one normalized physical root.
- `/Game/` and `/Engine/` map directly to their Content directories; every
  custom mount maps directly to its declared root.
- No source or asset API can reinterpret one virtual mount against a second
  physical directory.
- `FAssetPath` and `FSourcePath` retain their type-specific validation and
  persistence contracts.
- Automatic discovery and authoring-write policies remain explicit and do not
  introduce alternate path mappings.
- All checked-in SourceAssets files and affected persisted records are migrated
  without a compatibility reader or fallback.
- Raw sources remain outside cooked runtime requirements and are not presented
  as object assets.
- All implementation stages and validation-matrix rows have evidence-backed
  completion.
- Lasting behavior is documented in the authoritative Workspace, Runtime, and
  Editor documents.
- The workspace is clean after the final implementation commit.

## Deferred Follow-ups

- A general raw-source browser or combined asset/source Content Browser view.
- Dynamic user-local absolute mounts and workstation-specific mount overlays.
- Mount aliases, remapping, or redirectors for third-party project migration.
- Automatic deduplication or cleanup of repeated relative directory segments.
- General-purpose packaging of non-asset project files.

## Related Documentation

- [Workspace Projects](../Workspace/WorkspaceProjects.md)
- [Asset Packages](../Runtime/Assets/AssetPackages.md)
- [Asset Data Lifecycle](../Runtime/Assets/AssetDataLifecycle.md)
- [Asset Import Framework](../Editor/Architecture/AssetImportFramework.md)
- [Mounted Source Workflows](../Editor/Guides/MountedSourceWorkflows.md)
- [Content Version Control](../Development/VersionControl/ContentVersionControl.md)
- [Build and Run](../Development/Build/BuildAndRun.md)

## Related Code

- `Engine/Source/Runtime/Core/Public/Misc/Paths.h`
- `Engine/Source/Runtime/Core/Private/Misc/Paths.cpp`
- `Engine/Source/Runtime/CoreDObject/Private/DObject/AssetPath.cpp`
- `Engine/Source/Runtime/AssetCore/Private/AssetSystem.cpp`
- `Engine/Source/Runtime/Engine/Private/Source/SourcePath.cpp`
- `Engine/Source/Runtime/Engine/Private/Texture/Texture2D.cpp`
- `Engine/Source/Runtime/Engine/Private/Texture/TextureCube.cpp`
- `Engine/Source/Runtime/Engine/Private/StaticMesh/StaticMesh.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Assets/MountedSourceImport.h`
- `Engine/Source/Editor/LevelEditor/Private/Assets/TextureImportDialog.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Assets/TextureCubeImportDialog.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Assets/SceneImportDialog.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Panels/ContentBrowserModel.cpp`
- `Engine/Source/Editor/TextureEditor/Private/Widgets/MTextureEditor.cpp`
- `Engine/Tests/Native/CoreTests/Private/PathsTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/SourceLibraryReferenceContractTests.cpp`
- `Engine/Tests/Native/EngineTests/Data/SourceLibraryReferences/UnifiedMountContract.json`
- `Engine/Tests/Native/EngineTests/Private/SourceReferenceIndexTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/Texture/TextureImportAndCacheTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/Texture/SceneImportTests.cpp`
