# Asset Import Framework

Durin editor imports use one provider-neutral framework for source capture,
planning, preview, candidate construction, validation, publication, diagnostics,
and cancellation. `AssetImportCore` owns the generic contracts;
`StandardAssetImport` supplies the built-in StaticMesh, texture, material, and
Scene providers. Runtime targets depend on neither module.

## Ownership and layering

The dependency direction is `Core/CoreDObject -> AssetCore -> AssetImportCore
-> provider modules -> editor hosts`.

- `AssetCore` owns packages, the registry, disposable DDC objects, unpublished
  packages, and failure-atomic package-bundle publication. It has no knowledge
  of providers or concrete imported asset classes.
- `AssetImportCore` owns source snapshots, provider discovery, generic plans,
  diagnostics, candidates, import records, record indexing, and synchronous and
  asynchronous orchestration.
- Provider modules own format recognition, dependency discovery, normalized
  data, settings schemas, output identities, typed state exchange, and
  reconciliation policy.
- Editor hosts query framework capabilities. A concrete asset class alone does
  not imply that import or reimport is available.

`StandardAssetImport` is the default aggregate for built-in providers and the
only owner of their Assimp and editor image-decoder dependencies. Runtime Engine
assets contain only runtime state and lightweight single-asset provenance.

## Source snapshots and plans

Import first resolves a mounted `FSourcePath` and captures its bytes. A provider
may inspect those captured bytes and declare bounded dependencies. The framework
resolves and captures each dependency once, enforces containment and resource
limits, and freezes the complete closure as an immutable source snapshot.

Parsing, hashing, normalized CPU data, plan fingerprints, candidates, and DDC
keys consume the same snapshot bytes. No later phase reopens a physical source.
A plan is mutation-free, canonically ordered, and records target revisions and
management preconditions. Publication rejects a stale plan before changing any
loaded identity or package.

## Single-asset and record reimport

Single-asset reimport requires an asset identity, complete lightweight source
provenance, and an available compatible provider. It produces one detached
candidate and saves one authored package. StaticMesh geometry and texture
reimport use this path without a companion record.

Multi-output import creates an editor-only `DImportRecord`. Its outputs are
independent peer assets; no StaticMesh, material, texture, or other output owns
the rest. The record persists normalized settings, source identities and hashes,
stable output identities and policies, output fingerprints, accepted diagnostic
identities, and bounded provider reconciliation state. A rebuildable editor
index maps managed output paths back to their record.

Record reimport starts from the record and reconciles every managed output. It
reports removed outputs as orphans and never silently deletes or adopts an
occupied path. Detach, recreate, and repair are explicit record actions.

## Candidates and publication

Providers build detached authored state and validate it before an existing
identity changes. Loaded assets update only through typed, symmetric, no-fail
state exchange. Package publication revalidates the plan, exchanges prepared
state, and saves every changed output plus the record as one root-last bundle.
If publication fails, exchanges reverse and authored fingerprints must match
their pre-publication values.

Source ingestion and DDC writes have separate lifetimes. A failed import may
leave an explicitly ingested source or disposable cache entry, but it must not
leave a partially published authored asset graph.

## Asynchronous preparation

The synchronous executor is the semantic reference. One `AssetImportCore`
coordinator may schedule immutable capture, hashing, dependency discovery,
parsing, normalization, and CPU preparation. It does not create `DObject`
instances or touch packages, the registry, editor models, render resources, or
RHI state on a worker.

Workers move value-only results into synchronized request state and publish a
request serial to a mailbox. An editor-thread tick consumes only the current
serial after its task handle is terminal. Dialog close, project switch,
provider unload, and process shutdown cancel and drain their request scopes
before releasing providers or editor state.

## Persistence, DDC, and cooking

Authored runtime assets persist only the provenance needed for their own
single-asset rebuild. Provider contracts, settings serialization, provider
state, and DDC builders have independent versions. DDC paths are never
serialized and cache contents are never authoritative.

`DImportRecord` packages, provider state, accepted editor diagnostics, and the
record index are editor-only. Records carry an explicit cook-exclusion marker
and are never runtime dependencies. StaticMesh and texture cook paths strip
source provenance, derived-data keys, and editor diagnostics while publishing
validated runtime payloads into cooked ownership. A runtime-only target loads
cooked outputs without `AssetImportCore`, `StandardAssetImport`, Assimp, an
editor image decoder, source files, or DDC fallback.

## Compatibility baseline

The supported authored baseline is the current asset set versioned in this Git
repository. There are no production asset-specific structure upgraders. The
retired StaticMesh-owned Scene `ImportManifest`, generated material/texture
`ImportOwner`, StaticMesh `SourceFile` and duplicate `ImportSettings`,
StaticMeshComponent `Material`/`Materials`, Texture2D `SourceFile`, and
TextureCube face/panorama source-string fields have no compatibility reader or
migration API. Import-record schema 1 and StaticMesh material-slot schema 0 are
also unsupported. Repository assets must be upgraded or regenerated in the
same change that removes an old schema.

AssetCore's generic unknown-field reporting and explicit data-loss save guard
remain safety boundaries, not a migration promise. A package containing a
retired or unknown import field is reported as incompatible and is not silently
rewritten. Recreate or reimport that asset with the current provider framework.

## Editor workflow

External files are first ingested into a writable mounted SourceAssets domain;
import then references the mounted source. Preview shows the complete source
closure, outputs, policies, collisions, replacements, warnings, resource
estimates, missing outputs, and orphans before execution. Content Browser
actions inspect framework capabilities, navigate from any managed output to its
record, and keep single-asset reimport separate from record reimport.

See [Mounted Source Workflows](../Guides/MountedSourceWorkflows.md),
[Asset Packages](../../Runtime/Assets/AssetPackages.md), and
[Asset Data Lifecycle and Storage](../../Runtime/Assets/AssetDataLifecycle.md).
