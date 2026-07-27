# Unified Mount Source References Plan

Summary: Give each logical mount typed Content and SourceAssets domains so asset and source paths share one portable namespace without sharing physical resolution.

Last reviewed: 2026-07-28

## Current Status

Stages 0 through 4 are complete for the unified-mount revision. Core now publishes
one immutable registry with validated owner metadata, optional Content and
SourceAssets domains, dependency edges, and source-write policy. Typed
resolution and reverse classification enforce canonical containment and return
the frozen structured failure taxonomy. Active-project descriptors strictly
validate additional plugin-shaped and source-only mounts before project
initialization succeeds.

`FAssetPath`, AssetCore lookup/scanning/cache identity, source
consumers, source-thumbnail identity, import destination mapping, and content
browser navigation now use the typed registry rather than direct backing-vector
searches or `FPaths::Resolve`. StaticMesh, Texture2D, and both TextureCube
layouts now persist reflected `FSourcePath`; DAST v2 string carriers load only
through exact nested-field aliases, migrate after byte-hash verification, and
are transient on subsequent saves. Shared mounted-source operations now
classify registered files as no-copy references, ingest external files only to
explicit writable destinations, reuse only byte-identical collisions, and
rollback failed publication. Reimport is source-read-only; change-reference,
external ingestion, and shared-source replacement are separate operations.
Editor importers now expose reference-existing and ingest-external as separate
states with independent asset and source destinations, mounted-domain policy
diagnostics, and explicit repair. A bounded revision-aware reverse index previews
shared-source impact, while replacement and multi-package relocation use
recoverable source and package transactions. Stage 5 is next and will migrate
repository content and publish the lasting contracts.

Baseline commit `ee94ad4e` established reflected-name serialization coverage and
five DAST v2 legacy provenance fixtures covering project and engine StaticMesh,
project Texture2D, and both TextureCube source layouts. Those fixtures and the
recorded legacy fields remain valid migration evidence; only the superseded
name representation test was removed.

The former `{Library, RelativePath}` source-library representation and separate
`SourceLibraries` registry are superseded before implementation. The selected
model now uses one logical mount identity such as `/Engine/`, `/Game/`, or
`/Plugins/PCG/`, with typed `Content` and `SourceAssets` domains. This revision
removes duplicate naming systems, naturally supports plugins, and lets a
project reference Engine source files without copying them.

Texture2D currently accepts user-selected organization anywhere beneath the
owning package mount's typed SourceAssets domain, defaults new copies to
`SourceAssets/Textures/<filename>`, and can copy a source to another location
within that domain. StaticMesh and TextureCube remain more tightly coupled to
legacy category paths. These are interim provenance workflows until Stage 2
persists complete mounted source paths.

Redirect files, stable source GUIDs, and transparent moves remain deferred.

## Goal

- Use one portable logical namespace for each engine, project, plugin, or
  source-only owner.
- Resolve `FAssetPath` through a mount's Content domain and `FSourcePath`
  through the same mount's SourceAssets domain.
- Let `/Game/` assets reference `/Engine/` or declared plugin sources without
  copying them.
- Keep source organization independent from runtime asset organization.
- Support mounts with Content only, SourceAssets only, or both domains.
- Preserve portable provenance across workstations without storing absolute
  paths, symlink targets, or physical root names in packages.
- Apply one dependency, permission, containment, and failure model to all
  mounted domains.

## Scope

- A Core-owned immutable logical mount registry.
- Typed Content and SourceAssets domain resolution.
- Built-in `/Engine/` and `/Game/` mounts.
- A mount shape that supports future roots such as `/Plugins/PCG/` and
  source-only roots such as `/Libraries/StudioArt/`.
- A typed-domain model that can add Shaders later without creating a second
  logical mount namespace.
- A reflected `FSourcePath` used by StaticMesh, Texture2D, and TextureCube
  provenance.
- Import, reimport, source-reference change, source ingestion, shared-source
  replacement, duplication, and migration semantics.
- Mount dependency and source-write authorization.
- Canonical containment for ordinary directories, junctions, and symbolic
  links.
- Editor selection, diagnostics, reverse-reference impact reporting, native
  tests, package fixtures, and lasting documentation.

## Non-Goals

- Treating source files as runtime assets or allowing `FAssetPath` to resolve
  through SourceAssets.
- Encoding the physical `Content` or `SourceAssets` directory name in virtual
  paths.
- Renaming the existing `/Engine/` and `/Game/` namespaces.
- Implementing a plugin manager; this plan defines the mount contract that a
  future plugin descriptor can register.
- Migrating RenderCore shader mounts in the first implementation. The unified
  registry reserves an extensible typed-domain contract, but RenderCore keeps
  ownership of shader compilation, includes, caching, and hot reload.
- Redirect files, source GUID sidecars, automatic transparent source moves, or
  source garbage collection in the first implementation.
- Remote URLs, object storage, archives, overlays, or network streaming.
- Persisting workstation absolute paths or environment-variable expansions.
- Supporting more than one active game project in a process.
- Changing DMSH, TXPL, DBLK, or cooked payload formats unless validation proves
  an explicit version bump is required.

## Design Decisions and Invariants

### One mount, typed domains

- A mount is the logical identity. `/Engine/`, `/Game/`, and
  `/Plugins/PCG/` are examples.
- A mount may expose these independently:

  - `Content`: packaged runtime/editor assets.
  - `SourceAssets`: authoritative editor input files.

- The domain set is typed and versioned rather than an arbitrary string map.
  The first implementation supports only Content and SourceAssets. A later
  Shader domain uses the same mount identity without changing source or asset
  path semantics.
- A mount is registered once. Do not register two `/Engine/` entries pointing
  separately at `Engine/Content` and `Engine/SourceAssets`; identical virtual
  roots would be ambiguous.
- The path type selects the physical domain:

  ```text
  FAssetPath("/Engine/Materials/M_Default")
      -> Engine/Content/Materials/M_Default.dasset

  FSourcePath("/Engine/Textures/T_Default.png")
      -> Engine/SourceAssets/Textures/T_Default.png
  ```

- The virtual source path omits the physical `SourceAssets` segment. Likewise,
  an asset path omits `Content`.
- Asset and source paths may otherwise have unrelated relative organizations.
  No importer derives one from the other.

### Mount definition and ownership

Core owns a definition equivalent to:

```cpp
enum class EMountOwner
{
    Engine,
    ActiveProject,
    Extension,
    ExternalSources,
    Test
};

struct FMountPoint
{
    std::string VirtualRoot;
    std::filesystem::path OwnerRoot;
    std::optional<std::filesystem::path> ContentRoot;
    std::optional<std::filesystem::path> SourceAssetsRoot;
    EMountOwner Owner;
    bool bSourceWritable = false;
    std::vector<std::string> Dependencies;
};
```

- `EMountOwner` is process configuration, not serialized package data.
- `Dependencies` contains canonical virtual roots, never physical paths. The
  registry canonicalizes their spelling at publication and rejects unknown,
  duplicate, or self entries; self-reference remains implicitly allowed.
- `VirtualRoot` is the portable identity. It is absolute, normalized, uses
  forward slashes, and ends in `/`.
- `OwnerRoot` and domain roots are process-local configuration and are never
  serialized into `.dasset`.
- `ContentRoot` and `SourceAssetsRoot` are explicit. Consumers never derive
  `SourceAssets` by walking to the parent of a directory named `Content`.
- A missing optional domain means that path type is unsupported by the mount.
  This permits Content-only and SourceAssets-only mounts.
- Built-in mounts are:

  | Virtual root | Owner root | Content | SourceAssets |
  | --- | --- | --- | --- |
  | `/Engine/` | `Engine/` | `Content/` | `SourceAssets/` |
  | `/Game/` | active project root | `Content/` | `SourceAssets/` |

- A future PCG plugin can register:

  | Virtual root | Owner root | Content | SourceAssets |
  | --- | --- | --- | --- |
  | `/Plugins/PCG/` | plugin root | `Content/` | `SourceAssets/` |

- A source-only shared checkout can register `/Libraries/StudioArt/` with no
  Content domain.
- Built-in roots cannot be overridden by project descriptors. Duplicate or
  case-only virtual identities, ambiguous canonical domain roots, and malformed
  definitions fail initialization.
- Definitions are assembled after active-project selection and become
  immutable in one publication step before `InitDefaultMountPoints` is replaced
  and before package scanning starts. Registration after publication fails;
  there is no production reset or mutable backing-vector access. Tests use
  scoped registry fixtures rather than permanently mutating production global
  state.

### Persisted source path

Source provenance uses an Engine-owned reflected value:

```cpp
DSTRUCT()
struct FSourcePath
{
    GENERATED_BODY()

    DPROPERTY()
    std::string Path;
};
```

- `Path` is either empty or a complete normalized virtual file path such as
  `/Engine/Textures/Common/Stone.png`.
- Empty means no source dependency.
- A non-empty path must identify a registered mount with a SourceAssets domain,
  include a non-empty relative file path, use forward slashes, and contain no
  empty, `.` or `..` segment.
- The string preserves display case. Mount-root matching follows the
  registry's case policy; the next successful package save canonicalizes the
  root spelling to the registered definition.
- Relative file organization remains a string and does not populate the global
  `FName` pool.
- `FSourcePath` is not interchangeable with `FAssetPath`. Asset paths reject
  file extensions and require a Content domain; source paths retain extensions
  and require a SourceAssets domain.
- Core resolution accepts source-path string components without depending on
  Engine reflection.

### Typed registry APIs

Core provides result-bearing queries equivalent to:

```cpp
enum class EMountPathError
{
    None,
    InvalidVirtualPath,
    UnknownMount,
    UnsupportedDomain,
    UnavailableDomain,
    InvalidRelativePath,
    EscapedRoot,
    MissingFile,
    ForbiddenDependency,
    ReadOnlySource,
    IoFailure
};

enum class EPathExistence
{
    AllowMissing,
    RequireFile
};

struct FMountLookupResult
{
    const FMountPoint* Mount = nullptr;
    std::string NormalizedVirtualPath;
    std::filesystem::path RelativePath;
    EMountPathError Error = EMountPathError::None;
    std::string Message;
};

struct FDomainPathResult
{
    const FMountPoint* Mount = nullptr;
    std::string NormalizedVirtualPath;
    std::filesystem::path RelativePath;
    std::filesystem::path PhysicalPath;
    EMountPathError Error = EMountPathError::None;
    std::string Message;
};

FMountLookupResult FindMountForVirtualPath(std::string_view);
FContentPathResult ResolveContentPath(
    const FAssetPath&, EPathExistence = EPathExistence::AllowMissing);
FSourcePathResult ResolveSourcePath(
    std::string_view, EPathExistence = EPathExistence::RequireFile);
FContentPathResult ClassifyContentPath(const std::filesystem::path&);
FSourcePathResult ClassifySourcePath(const std::filesystem::path&);
```

- `FContentPathResult` and `FSourcePathResult` are distinct wrappers over the
  common domain result shape; callers cannot pass one where the other is
  required. Success is exactly `Error == None` with a non-null mount.
- Results carry the matched mount, normalized relative path, physical path,
  and a structured error.
- Longest-prefix matching is internal to the registry. Consumers do not scan a
  backing mount vector or repeat raw `starts_with` loops.
- Unknown mount, unavailable domain root, unsupported domain, invalid relative
  path, escaped root, missing file, forbidden dependency, read-only source, and
  I/O failure are distinct outcomes.
- `UnsupportedDomain` means the mount does not declare that typed domain.
  `UnavailableDomain` means it declares the domain but its configured root is
  absent. `MissingFile` applies only to `RequireFile`; `AllowMissing` still
  performs canonical nearest-existing-ancestor containment.
- Dependency and source-write checks use the same error enum but remain
  explicit policy queries because a path can resolve physically while a
  referencing mount is forbidden to use or mutate it.
- `FPaths::Resolve` is removed from production asset call sites. Returning an
  unresolved virtual input as though it were a physical path is not an accepted
  failure contract.
- AssetCore uses typed Content resolution for `FAssetPath` validation, package
  lookup, registry-cache identity, mounted-content scanning, and cook mapping.
- Importers, asset editors, and thumbnails use typed SourceAssets resolution.

### Future Shader domain

- Shader paths may later reuse the same logical roots:

  ```text
  FShaderPath("/Engine/Common/Lighting.dshader")
      -> Engine/Shaders/Common/Lighting.dshader

  FShaderPath("/Plugins/PCG/Compute/Noise.dshader")
      -> Plugins/PCG/Shaders/Compute/Noise.dshader
  ```

- Core would own mount identity, typed Shader-root availability, normalization,
  and physical containment.
- RenderCore would continue to own shader include semantics, compilation,
  dependency discovery, cache roots, hot reload, and diagnostics.
- The initial Content/SourceAssets stages must not hard-code a two-domain enum
  into serialized data or public APIs in a way that prevents adding the typed
  Shader domain.
- Shader-domain registration and migration of the current `FShaderPaths`
  implementation remain a deferred, separately validated stage.

### Physical containment

- Lexical normalization is necessary but not sufficient.
- Existing paths compare canonical physical candidates against canonical
  domain roots.
- Destination paths that do not yet exist validate their nearest existing
  canonical ancestor before mutation.
- A domain root may itself be a symlink or junction to another checkout.
- A nested link that escapes the resolved domain root is rejected. Its target
  must be registered as another mount.
- Reverse classification selects the most specific matching canonical domain
  root.
- Filesystem equivalence or canonical comparison is used where available
  instead of string equality.

### Dependency and mutation policy

- Mount dependencies govern both asset and source references.
- Every mount may reference itself.
- Built-in direction is:

  | Referencing asset mount | Allowed referenced mounts |
  | --- | --- |
  | `/Engine/` | `/Engine/` |
  | `/Game/` | `/Game/`, `/Engine/`, and explicitly declared plugin/source mounts |
  | `/Plugins/PCG/` | itself, `/Engine/`, and its declared dependencies |

- `/Game/` may therefore store
  `/Engine/Textures/Common/Stone.png` as its source path without copying it.
- `/Engine/` cannot depend on `/Game/` or project-declared mounts, so Engine
  packages remain usable without an active project.
- There is no permissive global fallback search. A path names exactly one
  logical mount.
- Read permission and write authorization are separate:

  - Reimport reads the referenced source.
  - External ingestion writes to an explicitly selected writable SourceAssets
    domain.
  - Project-owned ordinary operations may read Engine sources but may not
    ingest into or replace Engine sources.
  - Engine source mutation requires an Engine-authoring context and an explicit
    shared-source command.
  - Plugin/shared mounts apply their declared writability plus owner-context
    authorization.

### Project and extension registration

- `/Engine/` and `/Game/` are automatic.
- Additional active-project mounts are declared with a selected descriptor
  shape equivalent to:

  ```json
  {
      "Mounts": [
          {
              "VirtualRoot": "/Libraries/StudioArt/",
              "Owner": "ExternalSources",
              "Root": "Libraries/StudioArt",
              "Domains": {
                  "SourceAssets": "."
              },
              "SourceWritable": false,
              "Dependencies": ["/Engine/"]
          }
      ]
  }
  ```

- Every entry has exactly these fields: required string `VirtualRoot`, required
  string enum `Owner` (`Extension` or `ExternalSources`), required relative
  string `Root`, required object `Domains`, required Boolean
  `SourceWritable`, and required string array `Dependencies`. `Domains`
  accepts optional `Content` and `SourceAssets` relative strings in the first
  implementation and requires at least one. Unknown entry or domain fields,
  missing fields, wrong types, empty paths, absolute paths, and traversal fail
  project initialization.
- Descriptor `Root` is relative to the `.dproject` directory. Each domain is
  relative to `Root`; `"."` names `Root` itself. Committed descriptors do not
  accept absolute workstation paths.
- Project descriptors cannot declare `Engine`, `ActiveProject`, or `Test`
  owners and cannot declare `/Engine/` or `/Game/`. `/Game/` automatically
  depends on every valid additional mount declared by its active project;
  each additional mount's `Dependencies` controls its own outgoing edges.
- Virtual roots and dependency roots use the same normalized, absolute,
  forward-slash, trailing-slash syntax as registry definitions. Duplicate,
  case-only, built-in, unknown-dependency, and self-dependency entries fail
  initialization.
- A root may be a directory, junction, or symlink. Teams may place a link at
  the declared relative location when the real checkout differs per machine.
- Missing valid roots register as unavailable rather than failing startup, so
  DDC-backed editor loads can continue with actionable diagnostics.
- Plugin descriptors may later supply the same mount definition shape; plugin
  discovery and loading are outside this plan.

### Import and source operations

- Selecting a file already inside an allowed registered SourceAssets domain is
  **Reference Existing Source**. Import records its `FSourcePath` and performs
  no copy or rename.
- Selecting an Engine source while importing a Game asset is this no-copy
  workflow.
- Selecting an external file is **Ingest External Source**. It requires an
  explicitly selected writable target mount and relative destination. The
  default suggestion for a Game texture is
  `/Game/Textures/<filename>`.
- Ingestion copies transactionally and refuses ambiguous overwrites.
- `Reimport` rebuilds one asset from its persisted source and settings. It does
  not copy, rename, or overwrite source files.
- `Change Source Reference` validates and rebuilds one asset against another
  `FSourcePath`; it moves or copies no source files.
- `Replace Shared Source` is a separate explicit command. It requires write
  authorization, reports known referencing assets, and publishes the file and
  package changes transactionally.
- Duplicating an asset preserves its source path by default. Moving or deleting
  an asset never moves or deletes source files.
- StaticMesh, Texture2D, six-face TextureCube, and panorama TextureCube share
  the same classification, collision, rollback, and permission rules.

### Provenance, derived data, and cook

- Asset-specific provenance retains source path, exact observed content hash,
  importer/decoder identity and version, and per-asset import settings.
- Multiple assets may persist the same `FSourcePath`.
- Derived-data keys include exact source content and every semantic build
  input. They exclude virtual source path, physical path, mount identity,
  package path, and asset path.
- Equal bytes and settings converge to one DDC key even through different
  paths; different settings produce different keys for one shared source.
- A changed shared source does not silently rewrite referencing packages. Each
  asset updates its own hash only after a successful rebuild and package save.
- Cooked packages strip mounted source provenance unless an explicit diagnostic
  policy retains it. Cooked runtime loading never requires SourceAssets roots.

### Reverse references and source moves

- The editor builds a rebuildable process-local index:

  ```text
  FSourcePath -> [FAssetPath]
  ```

- It supports shared-source badges, diagnostics, impact previews, and explicit
  multi-package relocation.
- The first implementation does not promise transparent source moves. An
  explicit relocation updates every discovered reference transactionally or
  leaves all references unchanged.
- Redirect sidecars such as `Old.png.redirect` remain a deferred extension of
  `ResolveSourcePath`, not part of the initial mount contract.

### Compatibility and migration

- Legacy project-relative paths such as
  `SourceAssets/Textures/Environment/Wood.png` map to
  `/Game/Textures/Environment/Wood.png`.
- Legacy Engine-owned paths map to `/Engine/...`.
- The leading `SourceAssets/` segment is removed exactly once.
- Migration determines legacy ownership from the package's typed Content mount,
  resolves the new `FSourcePath`, and verifies bytes against the persisted hash
  before publishing changed provenance.
- Migration never copies, moves, deletes, or rebuilds an unchanged source.
- Existing compatibility carriers remain reflected until every repository
  package and fixture passes migration. New saves then emit only `FSourcePath`.
- Each new wrapper keeps the reflected field name `SourcePath`. Its former
  string member is renamed in C++ and reflection to `LegacySourcePath`. Before
  ordinary reflected deserialization, the asset-specific compatibility handler
  recognizes a legacy string payload named `SourcePath`, retags that payload as
  `LegacySourcePath`, and leaves the new wrapper absent. A wrapper payload is
  never retagged. This type-aware pre-load rename is the only point where the
  old and new fields share a serialized name; reflection never registers two
  `SourcePath` members in one struct.
- DAST remains format version 2 because the reflected domain field changes
  without changing the package envelope. Legacy rejection begins only after
  the compatibility carriers are deliberately removed.

The compatibility inventory is:

| Asset/layout | New field | Legacy carrier | Hash owner |
| --- | --- | --- | --- |
| StaticMesh | `FStaticMeshSourceImportData::SourcePath` (`FSourcePath`) | existing string `SourcePath` under a temporary renamed carrier | `SourceContentHash` |
| Texture2D | `FTextureSourceFile::SourcePath` (`FSourcePath`) | existing string `SourcePath` under a temporary renamed carrier | `SourceContentHashLow/High` |
| TextureCube six-face | each face's `FSourcePath` | each face's existing string path | per-face hash |
| TextureCube panorama | panorama `FSourcePath` | panorama existing string path | panorama hash |

The repository fixture inventory remains:

| Package | Owner | Provenance |
| --- | --- | --- |
| `Engine/Content/Editor/MaterialPreview/Box.dasset` | `/Engine/` | StaticMesh |
| `Engine/Content/Editor/MaterialPreview/Sphere.dasset` | `/Engine/` | StaticMesh |
| `Sandbox/Content/Models/Mesh_Teapot.dasset` | `/Game/` | StaticMesh |
| `Sandbox/Content/Textures/TEX_StoneHead.dasset` | `/Game/` | Texture2D |

There is no repository-owned TextureCube package at the revision point, so
checked-in fixtures cover both cube layouts.

## Current Foundations and Gaps

### Foundations

- `FAssetPath` already provides an absolute portable Content path shape.
- Core owns `FPaths`, active-project state, and current mount registration.
- AssetCore scans registered Content roots and serializes cross-mount asset
  dependencies.
- StaticMesh, Texture2D, and TextureCube persist portable legacy source paths
  and exact source hashes.
- Existing DDC keys already separate source content from package and asset
  locations.
- DDC-backed editor loading can succeed while source files are unavailable.
- Asset move/delete contributors already preserve potentially shared sources.

### Gaps

- `FMountPoint` contains only virtual and physical Content strings; it has no
  owner, optional domains, dependency edges, permissions, lifecycle freeze, or
  structured result.
- `RegisterMountPoint` silently replaces duplicate roots.
- `FPaths::Resolve` silently returns unresolved input.
- AssetCore, `FAssetPath`, importers, and editors scan the mutable mount vector
  directly.
- Texture2D, TextureCube, StaticMesh, ContentBrowser, and TextureEditor
  duplicate `GetMountOwnerRoot` logic that assumes a directory named
  `Content`.
- Source provenance cannot identify Engine, Game, plugin, or external mounts
  independently of its owning asset.
- Import workflows disagree on category restrictions, copy behavior, existing
  destinations, and rollback.
- There is no reverse source-reference index or shared mutation impact view.

## Implementation Stages

### Stage 0: Revise and freeze the unified mount contract

Dependencies: none.

- [x] Preserve executable reflected serialization coverage and the five legacy
  package provenance fixtures from baseline `ee94ad4e`.
- [x] Inventory repository-owned source-bearing packages and legacy fields.
- [x] Replace the superseded `{Library, RelativePath}` decision and tests with
  the reflected `FSourcePath` contract.
- [x] Freeze the mount definition, optional-domain, owner, dependency, and
  source-write fields.
- [x] Freeze the `.dproject` `Mounts` entry schema, validation, built-in
  override rules, unavailable-root behavior, and registration lifetime.
- [x] Freeze typed Content/Source result shapes and failure taxonomy.
- [x] Add fixture-backed cases for `/Game/ -> /Engine/`, forbidden
  `/Engine/ -> /Game/`, plugin-shaped mounts, and source-only mounts.
- [x] Record the compatibility field rename needed to deserialize the existing
  string `SourcePath` into the new reflected wrapper.

#### Acceptance Gate

- New packages have one unambiguous virtual source-path representation; mount
  and project descriptor shapes, dependency/write policy, failure taxonomy,
  and legacy migration are executable with no unresolved serialization choice.

#### Stage 0 Handoff

- Baseline: `55a6d89a`.
- Working set: this plan, the Engine reflection manifest,
  `Source/SourcePath.h`, the focused AssetCore package test cleanup, and the
  Engine source-path contract fixture/tests.
- Key decisions: one reflected string path; one immutable mount identity with
  optional typed domains; exact `Mounts` descriptor fields; distinct unsupported
  and unavailable domains; type-aware legacy carrier retagging.
- Open questions: none for Stage 1.
- Validation: focused `FSourcePathContractTests` and `FPackageAssetTests`, plus
  the active-plan validator.

### Stage 1: Implement the Core unified mount registry

Dependencies: Stage 0.

- [x] Replace mutable two-string mount entries with validated definitions,
  optional domains, owner metadata, dependencies, and source permissions.
- [x] Implement immutable startup publication plus scoped test fixtures.
- [x] Implement typed find, Content resolve, SourceAssets resolve, and reverse
  classification with structured failures.
- [x] Implement canonical containment for existing and not-yet-created paths,
  including supported junction and symlink cases.
- [x] Auto-register `/Engine/` and `/Game/`; parse additional project mounts.
- [x] Implement shared dependency and mutation-policy queries.
- [x] Migrate `FAssetPath`, AssetCore lookup/scanning/cache identity, and cook
  mapping off direct vector iteration and `FPaths::Resolve`.
- [x] Remove `GetMountOwnerRoot` and raw mount-vector use from asset/source
  consumers.

#### Acceptance Gate

- One active project resolves `/Engine/`, `/Game/`, a plugin-shaped mount, and
  a source-only mount through the correct typed domains; project-to-Engine
  source access succeeds, inverse access fails, all escapes are rejected, and
  production consumers have no ambiguous resolver fallback.

#### Stage 1 Handoff

- Baseline: `133a1d18`.
- Working set: Core paths/project initialization, `FAssetPath`, AssetCore
  package lookup and registry scanning, Engine legacy source consumers,
  source-aware editor consumers, Core mount/project tests, and this plan.
- Key symbols: `FMountPoint`, `FMountLookupResult`, `FContentPathResult`,
  `FSourcePathResult`, `PublishMountRegistry`, `ResolveContentPath`,
  `ResolveSourcePath`, `ClassifyContentPath`, `ClassifySourcePath`,
  `CheckMountDependency`, `CheckSourceMutation`, and
  `FScopedMountRegistryFixture`.
- Decisions: production publication is immutable and idempotent at startup;
  legacy `RegisterMountPoint` remains only as a pre-publication compatibility
  bridge for existing tests; missing declared domains stay registered and
  resolve as unavailable; project descriptors are rejected before current
  project state is published.
- Open questions: none for Stage 2.
- Validation: all 132 Core tests (131 passed; the link-containment case skipped
  because this Windows account cannot create directory symlinks), all 49
  AssetCore tests, 40 focused Engine source/import/DDC tests, a full `all`
  build, and active-plan validation.

### Stage 2: Migrate source provenance

Dependencies: Stages 0 and 1.

- [x] Add reflected `FSourcePath` to StaticMesh, Texture2D, and TextureCube
  provenance while retaining legacy carriers.
- [x] Replace package-owner inference with typed SourceAssets resolution.
- [x] Migrate legacy project and Engine paths, verify persisted hashes, and
  preserve importer versions and settings.
- [x] Preserve DDC keys when bytes and semantic settings do not change.
- [x] Update diagnostics to report mount, domain, dependency, availability,
  containment, and missing-file failures.
- [x] Prove cook and cooked runtime loading remain source-independent.

#### Acceptance Gate

- Legacy fixtures migrate without file mutation, new packages round-trip
  `/Engine/` and `/Game/` source paths, unchanged inputs retain DDC keys, and
  cooked runtime tests require no SourceAssets domain.

#### Stage 2 Handoff

- Baseline: `4f47cd6e`.
- Working set: AssetCore nested-field compatibility dispatch; reflected Engine
  source provenance and migration helper; StaticMesh, Texture2D, TextureCube,
  content-browser, import/cache/cook tests; and this plan.
- Key symbols: `FAssetSerializedStructFieldAlias`,
  `RegisterAssetSerializedStructFieldAlias`, `FSourcePath`,
  `TryMigrateLegacySourcePath`, `FStaticMeshSourceImportData`,
  `FTextureSourceFile`, and the three asset-specific mounted-source upgraders.
- Decisions: legacy nested strings are recognized only by exact declaring
  struct, name, kind, and type signature; aliases deserialize into transient
  `LegacySourcePath` carriers; migration requires the package Content mount,
  typed SourceAssets resolution, and an exact persisted hash match; new saves
  omit compatibility carriers without changing DAST v2; DDC identity remains
  path-independent.
- Open questions: none for Stage 3.
- Validation: all 49 AssetCore tests; 50 focused Engine source-contract,
  StaticMesh, Texture2D, and TextureCube tests; all four StaticMesh DDC/cook
  tests; and `TextureCookTests`. One full `EngineTests` attempt reached an
  existing cross-test assertion after a rendering test published the immutable
  default mount registry and a later legacy fixture attempted
  `RegisterMountPoint`; the Stage 2 focused suites pass independently.

### Stage 3: Unify import and source operations

Dependencies: Stage 2.

- [x] Implement shared physical-file classification and
  reference-versus-ingest decisions.
- [x] Make Game import of existing Engine/plugin sources a no-copy reference.
- [x] Add explicit writable target mount and relative destination for external
  ingestion.
- [x] Make ordinary reimport read-only with respect to source files.
- [x] Add change-reference and shared-source-replacement operations with
  transactional publication.
- [x] Apply identical collision, equality, rollback, dependency, and permission
  behavior to StaticMesh, Texture2D, and both TextureCube layouts.
- [x] Preserve source paths across asset duplication and preserve source files
  across asset move/delete.

#### Acceptance Gate

- Multiple assets and mounts can share one source without duplicate copies,
  independent settings produce expected DDC keys, external ingestion copies
  exactly once, and every injected failure leaves source and package state
  unchanged.

#### Stage 3 Handoff

- Baseline: `a351c9e9`.
- Working set: shared Engine mounted-source operations; StaticMesh, Texture2D,
  and TextureCube import/reimport/reference APIs; TextureEditor reimport action;
  focused source/import/cube/static-mesh tests; and this plan.
- Key symbols: `FMountedSourceFile`, `ESourceFileDisposition`,
  `PrepareMountedSourceFile`, `ResolveMountedSourceReference`,
  `FMountedSourceReplacement`, `PrepareMountedSourceReplacement`,
  `ChangeSourceReference`, `IngestAndChangeSource`,
  `ChangePanoramaSourceReference`, and `ChangeSourceReferences`.
- Decisions: selecting a file already classified beneath an allowed
  SourceAssets domain records that mount's virtual path without copying;
  external inputs require a complete target `FSourcePath` and mutation
  authorization; equal destination bytes are reused while unequal collisions
  fail; ordinary reimport accepts only the persisted physical source; shared
  replacement retains a rollback backup until commit; reflected duplication
  retains provenance, while asset move and delete retain source files.
- Open questions: none for Stage 4.
- Validation: 51 focused source-contract, Texture2D, TextureCube, and
  StaticMesh tests; all four StaticMesh DDC/cook tests; `TextureCookTests`; and
  a full `all` build.

### Stage 4: Add mounted-source editor workflows

Dependencies: Stage 3.

- [x] Display asset destination and source virtual path independently.
- [x] Present **Reference Existing Source** and **Ingest External Source** as
  distinct states.
- [x] Let pickers browse allowed SourceAssets domains and show mount identity,
  writability, and dependency status.
- [x] Add unavailable-domain, read-only, forbidden-dependency, changed-hash,
  missing-file, and containment diagnostics.
- [x] Build a revision-aware reverse `FSourcePath -> assets` index using bounded
  package inspection.
- [x] Show reference counts and affected assets before shared replacement or
  relocation.
- [x] Add explicit repair and transactional multi-package relocation.

#### Acceptance Gate

- An editor user can reference Engine source from a Game asset, ingest external
  source into Game, distinguish reimport from shared replacement, preview
  mutation impact, and recover from unavailable mounts without ambiguous or
  destructive actions.

#### Stage 4 Handoff

- Baseline: `aa69f0bd`.
- Working set: mounted-source import UI for StaticMesh, Texture2D, and
  TextureCube; TextureEditor source actions and diagnostics; DurinEd reverse
  reference indexing and multi-package relocation; Engine source relocation
  primitives and explicit import destinations; focused tests; and this plan.
- Key symbols: `EMountedSourceImportMode`, `InspectMountedSourceImport`,
  `FSourceReferenceIndex`, `FSourceReference`,
  `FMountedSourceRelocation`, `PrepareMountedSourceRelocation`,
  `RelocateMountedSourceAcrossPackages`, `ETextureSourceStatus::Changed`, and
  `EStaticMeshSourceStatus::Changed`.
- Decisions: import forms validate reference and ingestion policy before
  submission and always show complete source virtual paths; package inspection
  is bounded and reruns only after registry revision changes; incomplete impact
  scans block shared mutation; relocation rejects dirty affected packages,
  stages the destination first, atomically snapshots/restores package bytes on
  failure, and removes the original only after every package saves; ordinary
  reimport remains source-read-only.
- Open questions: none for Stage 5.
- Validation: 51 focused source-contract, reverse-index/relocation, Texture2D,
  TextureCube, StaticMesh source/DDC, and cook tests; plus a full `all` build.

### Stage 5: Migrate repository content and publish contracts

Dependencies: Stages 2 through 4.

- [ ] Migrate all repository-owned source-bearing packages and verify hashes.
- [ ] Remove legacy carriers only after clean scans and fixture-backed rejection
  coverage.
- [ ] Publish lasting mount, package, source workflow, content version-control,
  texture, cube, and model contracts in their owning documentation.
- [ ] Document plugin-shaped and source-only mount configuration, Git/LFS
  ownership, links, unavailable roots, and read-only recovery.
- [ ] Run focused native suites, package migration, cook/load, editor smoke,
  full `all` build, and verified editor launch.
- [ ] Update status/checklists, validate all plans, and archive this plan.

#### Acceptance Gate

- Repository packages use only mounted `FSourcePath` provenance, old metadata
  is deliberately rejected, all validation passes, and authoritative
  documentation no longer derives SourceAssets from Content.

## Validation Matrix

| Area | Scenario | Expected result |
| --- | --- | --- |
| Typed domains | Resolve one relative path as asset and source | Asset uses Content; source uses SourceAssets |
| Unsupported domain | Source-only mount receives an `FAssetPath` | Explicit unsupported-domain failure |
| Project to Engine | Sandbox imports an Engine source | `/Engine/...` is persisted and no copy occurs |
| Engine to Project | Engine asset selects `/Game/...` source | Dependency failure before mutation |
| Plugin | Game asset references `/Plugins/PCG/...` | Succeeds only when Game declares the dependency |
| Source-only mount | Game references `/Libraries/StudioArt/...` | Source resolves; asset resolution is unavailable |
| Duplicate root | Two definitions use `/Plugins/PCG/` | Initialization rejects the duplicate |
| Unknown root | Resolve `/Unknown/File.png` | Explicit unknown-mount failure |
| Missing root | A configured source checkout is absent | DDC-backed load succeeds with diagnostic |
| Traversal | Source contains `..` | Rejected before filesystem access |
| Root link | Source root is a junction/symlink | Resolves against canonical target |
| Nested escape | Nested link exits the source root | Rejected before read or write |
| Reverse classification | Physical Engine source is selected | Produces canonical `/Engine/...` |
| Read-only | Project tries replacing Engine source | Read succeeds; write is rejected |
| Shared source | Two assets use one source and equal settings | Same DDC key, no duplicate copy |
| Per-asset settings | Shared source has different settings | Deterministically different DDC keys |
| Ingest | External file targets writable Game source | One transactional copy |
| Reimport | Asset rebuilds from mounted source | No source write |
| Migration | Legacy Game and Engine packages load | Correct virtual roots, no file move, no DDC drift |
| Cook | Editor provenance exists | Cooked runtime needs no SourceAssets root |
| Rollback | Copy, build, save, or replacement fails | No partial source/package publication |

Validation commands and profile selection follow
[Build and Run](../Development/Build/BuildAndRun.md).

## Definition of Done

- Content and SourceAssets are typed domains of one immutable logical mount
  registry.
- `/Engine/`, `/Game/`, plugin-shaped, and source-only mounts resolve without
  physical directory names in persisted paths.
- `FAssetPath` and `FSourcePath` cannot cross domains accidentally.
- AssetCore and importers use structured resolver results with no silent
  fallback or direct backing-vector scans.
- Game assets reference Engine and declared plugin sources without copying;
  Engine assets remain independent of project mounts.
- Source organization is independent from asset organization.
- Shared sources support independent settings and deterministic derived data.
- Native directories, junctions, and symlinks obey one containment contract.
- StaticMesh, Texture2D, TextureCube, cook, editor UI, diagnostics, and
  repository packages use the unified model.
- Legacy provenance is migrated with fixture-backed compatibility, then
  deliberately removed.
- Focused tests, cook/load, editor smoke, full build, and plan validation pass.
- Lasting contracts are published and the completed plan is archived.

## Deferred Follow-ups

- `Old.png.redirect` and other transparent source redirectors.
- A typed Shader domain and migration of RenderCore `FShaderPaths` onto unified
  mount identity.
- Stable source GUIDs and sidecar metadata.
- Persistent reverse-reference indexes.
- Remote synchronization, checkout/version pinning, and source locking.
- Overlay/fallback mounts and multiple physical replicas.
- Per-user physical-root overrides if project-relative links are insufficient.
- Automated source garbage collection after verified zero-reference state.

## Related Documentation

- [Workspace And Projects](../Workspace/WorkspaceProjects.md)
- [Asset Data Lifecycle and Storage](../Runtime/Assets/AssetDataLifecycle.md)
- [Asset Packages](../Runtime/Assets/AssetPackages.md)
- [Content Version Control](../Development/VersionControl/ContentVersionControl.md)
- [Texture Support Plan](TextureSupport.md)

## Related Code

- `Engine/Source/Runtime/Core/Public/Misc/Paths.h`
- `Engine/Source/Runtime/Core/Private/Misc/Paths.cpp`
- `Engine/Source/Runtime/Core/Public/Misc/Project.h`
- `Engine/Source/Runtime/Core/Private/Misc/Project.cpp`
- `Engine/Source/Runtime/CoreDObject/Private/DObject/AssetPath.cpp`
- `Engine/Source/Runtime/AssetCore/Private/AssetSystem.cpp`
- `Engine/Source/Runtime/Engine/Public/StaticMesh/StaticMesh.h`
- `Engine/Source/Runtime/Engine/Public/Texture/Texture2D.h`
- `Engine/Source/Runtime/Engine/Public/Texture/TextureCube.h`
- `Engine/Source/Runtime/Engine/Private/StaticMesh/StaticMesh.cpp`
- `Engine/Source/Runtime/Engine/Private/Texture/Texture2D.cpp`
- `Engine/Source/Runtime/Engine/Private/Texture/TextureCube.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Assets/StaticMeshImportDialog.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Assets/TextureImportDialog.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Assets/TextureCubeImportDialog.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Panels/ContentBrowserPanel.cpp`
