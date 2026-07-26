# Source Library References Plan

Summary: Decouple source-file organization from runtime asset paths through named source libraries, portable references, and shared-source import workflows.

Last reviewed: 2026-07-27

## Current Status

Planning is complete and no implementation stage has started. The current
StaticMesh, Texture2D, and TextureCube provenance remains rooted beneath the
owning package's project or engine `SourceAssets` directory, and new imports
still derive a destination source path from the runtime asset path.

This plan selects `SourceLibrary` as the user-facing and persisted concept.
A source library is a logical collection of authoritative editor inputs;
filesystem mounting is only one implementation detail of resolving that
collection on a workstation. Multiple libraries may be registered at once,
and multiple assets or projects may reference the same library file while
retaining independent import settings and derived data.

## Goal

- Let an active project register and resolve multiple source libraries.
- Let source-library organization differ completely from mounted `Content`
  organization.
- Let multiple assets reference one source file with independent import
  settings and deterministic derived-data keys.
- Preserve portable `.dasset` provenance across workstations and projects
  without storing absolute paths or symlink targets.
- Treat ordinary directories, directory junctions, and symbolic links
  consistently while enforcing physical containment.
- Migrate existing project- and engine-relative `SourceAssets` provenance
  without requiring source recopying.

## Scope

- A Core-owned source-library registry and active-project descriptor entries.
- Built-in `Engine` and `Project` source libraries.
- Named additional libraries such as `StudioArt`.
- A shared logical source-location value used by StaticMesh, Texture2D, and
  TextureCube provenance.
- Import, reimport, source replacement, source repair, and asset duplication
  semantics.
- Transactional migration of existing package provenance and repository-owned
  packages.
- Editor selection, diagnostics, and source-reference impact reporting.
- Native tests, package fixtures, editor smoke coverage, and lasting
  documentation updates.

## Non-Goals

- Mounting source libraries as runtime `Content` or creating runtime asset
  identities for source files.
- Synchronizing, cloning, locking, or versioning an external source repository.
- Persisting workstation absolute paths, environment-variable expansions, or
  symlink targets in `.dasset` packages.
- Automatically deleting or moving a source file when an asset is moved or
  deleted.
- Stable source-file GUIDs, sidecar metadata, or redirectors in the first
  implementation.
- Remote URL, object-storage, archive-overlay, or network-streaming libraries.
- Supporting more than one active project in a process.
- Changing DMSH, TXPL, DBLK, or cooked runtime payload formats unless validation
  proves an explicit version bump is required.

## Design Decisions and Invariants

### Terminology and identity

- `SourceLibrary` is the public concept. Do not expose `MountId` in package
  metadata or editor UI.
- A library name identifies a logical source collection, not its current
  physical directory, repository checkout, symlink, or junction.
- Persisted source locations use:

  ```cpp
  struct FSourceLocation
  {
      FName Library;
      std::string RelativePath;
  };
  ```

- `Library` uses `FName` because library names form a small, process-wide,
  repeatedly compared identifier namespace and are valid reflected package
  properties. Asset serialization already writes `FName` values as strings,
  and `FName` retains a display spelling independently from its
  case-insensitive comparison identity; no process-local name-pool index may
  be persisted.
- `RelativePath` remains `std::string`. Source paths are case-preserving,
  potentially numerous, and must not populate the global name pool.
- A library may use any valid non-empty `FName` spelling, including the
  project's usual capitalization, such as `StudioArt`, `Megascans`, or
  `ProjectSources`. `None`, path separators, control characters, invalid
  `FName` spellings, case-only aliases, and duplicate identities are invalid.
- Library identity is case-insensitive because `FName` comparison is
  case-insensitive, while the registry definition supplies the authoritative
  display spelling. Import, migration, and package save canonicalize the
  persisted display spelling to the registered definition rather than forcing
  lowercase.
- `Engine` and `Project` are reserved built-in identities regardless of input
  case. Shared libraries use stable organization-owned names such as
  `StudioArt`; changing identity is a provenance migration, while a
  capitalization-only display correction does not create a new library.
- `{None, ""}` means no source dependency. A present dependency requires both
  a non-None library and a non-empty normalized relative path.

### Library registration and ownership

- Core owns source-library descriptor parsing, registration, lookup, physical
  resolution, reverse physical-path classification, and path-containment
  policy. This belongs beside active-project and filesystem path state, not in
  any one asset importer.
- Engine owns asset-specific provenance, source hashing, import settings,
  derived-data key contribution, rebuild policy, and package migration.
- Editor/import modules own library and source selection, copy-versus-reference
  workflow, reimport commands, warnings, and impact presentation.
- The `Engine` library resolves to `Engine/SourceAssets`.
- The `Project` library resolves to `<ActiveProject>/SourceAssets`.
- Additional libraries are declared by the active `.dproject`, for example:

  ```json
  {
      "SourceLibraries": [
          {
              "Name": "StudioArt",
              "Path": "SourceAssets/StudioArt",
              "Writable": false
          }
      ]
  }
  ```

- Descriptor paths are relative to the directory containing the `.dproject`.
  A declared path may itself be an ordinary directory, symlink, or junction.
  Committed descriptors do not accept absolute workstation paths. A team that
  keeps a checkout elsewhere creates a link at the declared project-relative
  location.
- `Writable` controls editor ingestion and source-replacement operations.
  Read-only libraries remain valid reference and reimport sources.
- Missing physical roots are registered as unavailable rather than making
  project startup fail. This preserves valid DDC-backed editor loads while
  producing an actionable source-unavailable diagnostic.
- Invalid names, duplicate names, malformed descriptor entries, and ambiguous
  duplicate physical roots fail project initialization with the offending
  entries identified.
- Registration is immutable after active-project startup. Changing library
  definitions requires reopening the project.

### Resolution and containment

- A package stores only `FSourceLocation` and the existing exact source-content
  hash. It never stores the configured physical root.
- Resolution selects the library by `FName`, appends the normalized relative
  path, and validates the result against the library's resolved physical root.
- Relative paths use forward slashes, contain no empty, `.` or `..` segment,
  are not absolute, and preserve source-library organization. They are not
  required to begin with `Models` or `Textures`.
- The library root itself may resolve through a symlink or junction to a
  directory outside the project. Containment compares the canonical physical
  candidate against the canonical physical library root.
- A nested symlink or junction that escapes the resolved library root is
  rejected. Its target must instead be registered as a separate source
  library.
- Reverse classification of a selected physical file uses resolved physical
  containment and selects the most specific matching library root. Registration
  rejects roots that would remain indistinguishable after canonicalization.
- Existing files use filesystem-equivalence or canonical-path comparison where
  available, not lexical path equality alone. Destination parents for new
  files use the nearest existing canonical ancestor before containment is
  accepted.
- Resolution failures distinguish unknown library, unavailable library,
  invalid relative path, escaped root, missing file, and I/O failure.

### Provenance and derived data

- Source location, observed content hash, importer/decoder identity, importer
  version, and per-asset import settings remain compact editor provenance.
- Two assets may persist identical source locations and hashes.
- Derived-data keys continue to use exact source content plus every semantic
  import/build input, builder/schema version, platform, and profile. They do
  not include library name, relative path, physical path, package path, or
  asset path.
- Same source bytes plus the same semantic settings converge to one DDC key;
  different settings produce different keys even when the source location is
  identical.
- A changed shared source does not silently rewrite every referencing package.
  Each asset independently reports that its persisted hash differs, then
  updates its own provenance only after a successful transactional rebuild and
  package save.
- Cooked packages continue to strip source-library provenance unless an
  explicit diagnostic policy retains it. Cooked runtime loading never requires
  source-library registration.

### Import and source-operation semantics

- Asset virtual path and source-library path are independent user choices.
  No importer derives an authoritative source location from the destination
  asset path.
- Selecting a file already contained by a registered library performs
  **Reference Existing Source**: record its library-relative location and do
  not copy or rename it.
- Selecting a file outside every registered library performs **Ingest External
  Source**: require a writable destination library and explicit relative
  destination, copy transactionally, then record the resulting location.
  The editor may suggest a destination but the asset path does not dictate it.
- `Reimport` rebuilds one asset from its persisted source reference and that
  asset's settings. It does not copy, rename, or overwrite the source.
- `Change Source Reference` changes only the selected asset after validating
  and building from the new reference.
- `Replace Shared Source` is a distinct explicit command. It requires a
  writable library, reports known referencing assets before mutation, stages
  the replacement transactionally, and never masquerades as ordinary
  reimport.
- Duplicating an asset preserves the source reference by default. Moving or
  deleting an asset never moves or deletes source files.
- StaticMesh, Texture2D, six-face TextureCube, and panorama TextureCube use the
  same classification, reuse, collision, and rollback rules.

### Reference index and source moves

- The editor builds a process-local reverse index from loaded/scanned package
  provenance:

  ```text
  FSourceLocation -> [asset paths]
  ```

- The index supports impact previews, shared-source badges, and diagnostics.
  It is rebuildable and is not authoritative package data.
- The first implementation does not promise transparent source moves. Moving a
  source file is an explicit operation that updates every discovered
  referencing package transactionally or leaves all package references
  unchanged.
- Stable source GUIDs and redirects remain deferred until actual library
  reorganization frequency justifies sidecar or manifest ownership.

### Compatibility and migration

- Existing provenance such as
  `SourceAssets/Textures/Environment/Wood.png` or
  `SourceAssets/Models/Characters/Hero.fbx` is recognized only by an explicit
  legacy migration path.
- Legacy provenance owned by an `/Engine/` package maps to library `Engine`;
  other mounted project packages map to library `Project`. The leading
  `SourceAssets/` segment is removed, preserving the remaining organization.
- Migration verifies that the resolved new location identifies the same file
  bytes as the persisted content hash before publishing changed provenance.
- Package schema evolution keeps a compatibility carrier for the old
  `SourcePath` fields until every repository package and migration fixture has
  passed. Generic reflection's unknown-field tolerance must not be relied upon
  to recover discarded legacy data.
- New package saves emit only the selected source-library representation after
  the migration boundary is closed. Old representations are then deliberately
  rejected by compatibility tests rather than resolved indefinitely.
- Package migration does not copy, move, or delete source files and does not
  change derived-data keys when source bytes and semantic settings are
  unchanged.

## Current Foundations and Gaps

### Foundations

- `FName` is reflected and serialized as its string value by AssetCore.
- `FPaths`, active-project initialization, and `PathUtilities` already own
  process-wide project and mount state.
- StaticMesh, Texture2D, and TextureCube persist portable source paths and exact
  source hashes.
- Existing DDC key inputs already separate source content identity from
  package and asset paths and include per-asset semantic settings.
- Asset move and delete contributors already avoid implicitly moving or
  deleting potentially shared source art.
- DDC-backed editor loading can succeed while source files are unavailable.

### Gaps

- Source provenance infers the physical root from the owning package's Content
  mount and cannot name an independent source library.
- Validation requires `SourceAssets/Models` or `SourceAssets/Textures` prefixes
  instead of validating a library-relative path.
- Importers derive source destinations from asset virtual paths, coupling two
  unrelated organizations and duplicating shared inputs.
- TextureCube initial import rejects an already-present canonical destination
  instead of applying the same existing-source semantics as other importers.
- Path comparisons are primarily lexical and do not define a complete
  symlink/junction containment contract.
- There is no shared library registry, reverse source-reference index, or
  editor workflow that distinguishes reference, ingest, reimport, and shared
  replacement.
- Current runtime documentation still describes project- or engine-relative
  `SourceAssets` as the only persistent provenance form.

## Implementation Stages

### Stage 0: Freeze source-library and migration contracts

Dependencies: none.

- [ ] Add focused tests proving reflected `FName` package serialization stores
  text rather than process-local name-pool indices, preserves the registered
  display spelling, and compares differently cased spellings as one identity.
- [ ] Inventory every reflected StaticMesh, Texture2D, and TextureCube source
  field and every repository-owned package that carries it.
- [ ] Freeze the shared `FSourceLocation` representation, empty-value
  invariant, source-hash ownership, and compatibility carrier for each existing
  reflected layout.
- [ ] Freeze `.dproject` `SourceLibraries` parsing, built-in names, validation,
  unavailable-root behavior, registration lifetime, and writable policy.
- [ ] Create legacy package fixtures before changing serialization so migration
  tests exercise real former bytes rather than newly generated equivalents.
- [ ] Record the package-schema/version impact and the exact point at which old
  fields become rejected.

#### Acceptance Gate

- The data shape, ownership boundary, descriptor schema, failure taxonomy, and
  old-package migration path have executable fixtures and no unresolved
  serialization decision.

### Stage 1: Implement the Core source-library registry

Dependencies: Stage 0.

- [ ] Add immutable source-library definitions keyed by case-insensitive
  `FName` identity while retaining each definition's display spelling.
- [ ] Auto-register `Engine` and `Project`, parse additional active-project
  descriptor entries, and expose lookup plus availability diagnostics.
- [ ] Implement logical-to-physical resolution with normalized relative paths,
  canonical root handling, and distinct error results.
- [ ] Implement physical-file-to-library classification using the most specific
  canonical root.
- [ ] Support ordinary directories, Windows junctions, and symbolic links at
  library roots while rejecting nested-link escape.
- [ ] Add test-only registry reset/setup without weakening production
  immutability.
- [ ] Cover missing roots, duplicate names, case-only duplicates, duplicate
  physical roots, malformed paths, traversal, unicode, long paths, and
  permission/I/O failures.

#### Acceptance Gate

- One active project can resolve at least three simultaneous libraries,
  classify files back to the correct logical location, tolerate unavailable
  roots, and reject every tested containment escape consistently for native
  directories and supported link types.

### Stage 2: Migrate shared source provenance

Dependencies: Stages 0 and 1.

- [ ] Add the selected `FSourceLocation` representation and shared validation
  helpers without interning relative paths as `FName`.
- [ ] Replace package-owner-root inference in StaticMesh, Texture2D, and
  TextureCube resolution with source-library lookup.
- [ ] Load legacy project/engine-relative provenance into its compatibility
  carrier, validate bytes, and migrate it to `Project` or `Engine`.
- [ ] Preserve source hashes, importer/decoder versions, import settings, and
  DDC keys across migration.
- [ ] Update diagnostics and source-status APIs to report library name and
  library-specific failures.
- [ ] Prove cooked package generation and cooked runtime loading remain
  independent of source-library registration.

#### Acceptance Gate

- Old package fixtures migrate without source-file mutation, new packages
  round-trip named-library provenance, unchanged semantic inputs retain their
  DDC keys, and cooked runtime tests require no registered source library.

### Stage 3: Decouple import and reimport workflows

Dependencies: Stage 2.

- [ ] Replace asset-path-derived source destination helpers with shared
  reference-or-ingest classification.
- [ ] Make existing-library import persist the selected location without any
  copy, including when the picker returns a resolved target path rather than
  the visible symlink/junction path.
- [ ] Add explicit writable-library and relative-destination inputs for
  external-file ingestion.
- [ ] Make StaticMesh, Texture2D, six-face TextureCube, and panorama TextureCube
  share collision, equality, transaction, rollback, and failure behavior.
- [ ] Make ordinary reimport read-only with respect to source files.
- [ ] Add explicit change-reference and shared-source-replacement operations
  with transactional package/source publication.
- [ ] Preserve one source reference when an asset is duplicated and preserve
  all source files across asset move/delete.

#### Acceptance Gate

- Two or more assets can reference one source with different import settings,
  produce distinct expected DDC keys, reimport independently, and incur no
  source copy; external ingestion copies exactly once to the selected writable
  location and rolls back cleanly on every injected failure.

### Stage 4: Add editor library and impact workflows

Dependencies: Stage 3.

- [ ] Update import dialogs to show source library and relative source path
  separately from destination asset path.
- [ ] Present **Reference Existing Source** and **Ingest External Source** as
  distinct states with no implicit overwrite.
- [ ] Add source-library availability, read-only, missing-file, changed-hash,
  and containment diagnostics to relevant asset editors.
- [ ] Build a revision-aware reverse source-reference index from asset-registry
  metadata or bounded package inspection.
- [ ] Show reference counts and affected asset paths before shared-source
  replacement or source relocation.
- [ ] Add explicit source-reference repair and multi-package source-relocation
  transactions.
- [ ] Keep the editor responsive and cache derived reference views against the
  asset-registry revision rather than rescanning packages every frame.

#### Acceptance Gate

- An editor user can select among multiple libraries, create two differently
  configured assets from one source, distinguish reimport from replacement,
  inspect the known impact of a shared-source mutation, and recover from an
  unavailable library without ambiguous or destructive actions.

### Stage 5: Migrate repository content and publish lasting contracts

Dependencies: Stages 2 through 4.

- [ ] Migrate all repository-owned StaticMesh, Texture2D, and TextureCube
  packages and verify their persisted source hashes against resolved files.
- [ ] Remove the temporary legacy compatibility carrier only after clean
  registry scans and fixture-backed rejection coverage.
- [ ] Update asset lifecycle, content version-control, texture, cube, material,
  and package documentation that currently states the old source-root model.
- [ ] Document multi-library project setup, Git/LFS ownership, symlink/junction
  bootstrap, unavailable-library recovery, and read-only shared libraries.
- [ ] Run the focused native suites, package migration validation, cook/load
  tests, and editor workflow smoke tests.
- [ ] Complete the repository-required full `all` build and launch the editor
  from the same Agent Build Profile for the user-visible workflow change.
- [ ] Update this plan's status and checklists with validation evidence, move
  lasting rules into their owning documentation, validate all plans, and
  archive the completed plan.

#### Acceptance Gate

- Repository packages use only named source-library provenance; old metadata is
  deliberately rejected; all focused, cook, and editor smoke coverage passes;
  the full editor build succeeds; and the authoritative documentation no
  longer couples source organization to runtime asset organization.

## Validation Matrix

| Area | Scenario | Expected result |
| --- | --- | --- |
| Identity | `StudioArt` and `studioart` are declared | Project initialization rejects the case-only duplicate |
| Serialization | Register `StudioArt` and load a differently cased reference | Lookup succeeds and the next package save persists `StudioArt` |
| Multiple libraries | Two libraries contain `Textures/Wood.png` | Library name disambiguates the references |
| Shared source | Two assets use one location and equal settings | Both resolve one file and converge to the same DDC key |
| Per-asset settings | Two assets use one location and different settings | Source is not copied and DDC keys differ deterministically |
| Organization | Source and asset paths have unrelated hierarchies | Import persists the selected source path unchanged |
| Cross-project | Two projects map `StudioArt` to the same checkout | Equivalent logical references resolve without workstation paths in packages |
| Native directory | Library root is a normal directory | Resolution, ingest, and reimport succeed |
| Junction/symlink | Library root redirects outside the project | Root resolution succeeds and persists only the logical reference |
| Escape | A nested link exits the resolved library root | Resolution and ingestion reject it before file mutation |
| Unavailable library | Shared checkout is absent but DDC is valid | Editor load succeeds with a source-unavailable diagnostic |
| Read-only library | User references then attempts replacement | Reference/reimport succeeds; ingestion/replacement is refused |
| Ingest | External file targets a writable library | One transactional copy occurs at the explicitly selected relative path |
| Reimport | Existing referenced source is rebuilt | No source copy, rename, or overwrite occurs |
| TextureCube | Existing six-face or panorama sources are selected | Initial import references them instead of reporting destination collision |
| Migration | Legacy project and engine packages load | They map to `Project` and `Engine` without source movement or DDC-key drift |
| Source mutation | Shared bytes change | Each asset reports its own hash mismatch and commits only after successful rebuild |
| Move/delete | Referencing asset moves or is deleted | Source files remain untouched |
| Cook | Editor provenance exists during cook | Runtime package/load has no source-library dependency |
| Rollback | Copy, build, package save, or replacement fails | No partial source or package state is published |

Validation commands and profile selection follow
[Build and Run](../Development/Build/BuildAndRun.md); this plan does not
duplicate those operational instructions.

## Definition of Done

- Multiple named source libraries are configured and resolved independently of
  Content mount points.
- Persisted library identity uses the registered display spelling of a
  case-insensitive `FName`; relative source paths remain strings and never
  encode physical roots.
- Source organization is independent of asset virtual path organization.
- Same-source multi-asset imports perform no duplicate copy and support
  independent settings and DDC results.
- Ordinary directories, junctions, and symlinks satisfy the same containment
  and failure contracts.
- StaticMesh, Texture2D, TextureCube, repository packages, cook, editor UI, and
  diagnostics use the shared model.
- Legacy provenance is migrated with fixture-backed compatibility and then
  removed from the accepted new-package contract.
- Focused tests, package validation, cook/load coverage, full `all` build, and
  editor smoke validation pass.
- Lasting behavior is published in the owning documentation and this completed
  plan is archived according to the plan lifecycle rules.

## Deferred Follow-ups

- Stable source GUIDs, `.dsource` sidecars, library manifests, and redirectors
  for transparent source moves.
- Library-name aliases for coordinated organization-wide renames.
- Persistent reverse-reference indexes if registry-derived rebuilding becomes
  too expensive.
- Remote library synchronization, checkout/version pinning, LFS locking, and
  source-control provider integration.
- Overlay libraries, fallback search chains, and multiple physical replicas of
  one logical library.
- Per-user physical-path overrides if project-relative symlink/junction
  bootstrap proves insufficient.
- Automated source garbage collection based on verified zero-reference state.

## Related Documentation

- [Asset Data Lifecycle and Storage](../Runtime/Assets/AssetDataLifecycle.md)
- [Asset Packages](../Runtime/Assets/AssetPackages.md)
- [Content Version Control](../Development/VersionControl/ContentVersionControl.md)
- [Asset Derived Data and Cooking Plan](Archive/2026-07/AssetDerivedDataAndCooking.md)
- [Texture Support Plan](TextureSupport.md)

## Related Code

- `Engine/Source/Runtime/Core/Public/Misc/Name.h`
- `Engine/Source/Runtime/Core/Public/Misc/Paths.h`
- `Engine/Source/Runtime/Core/Public/Misc/Project.h`
- `Engine/Source/Runtime/Core/Private/Misc/Project.cpp`
- `Engine/Source/Runtime/AssetCore/Private/AssetSystem.cpp`
- `Engine/Source/Runtime/Engine/Public/StaticMesh/StaticMesh.h`
- `Engine/Source/Runtime/Engine/Public/Texture/Texture2D.h`
- `Engine/Source/Runtime/Engine/Public/Texture/TextureCube.h`
- `Engine/Source/Runtime/Engine/Private/StaticMesh/StaticMesh.cpp`
- `Engine/Source/Runtime/Engine/Private/Texture/Texture2D.cpp`
- `Engine/Source/Runtime/Engine/Private/Texture/TextureCube.cpp`
- `Engine/Source/Runtime/Engine/Public/StaticMesh/StaticMeshDerivedData.h`
- `Engine/Source/Runtime/Engine/Public/Texture/TextureDerivedData.h`
- `Engine/Source/Editor/LevelEditor/Private/Assets/StaticMeshImportDialog.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Assets/TextureImportDialog.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Assets/TextureCubeImportDialog.cpp`
