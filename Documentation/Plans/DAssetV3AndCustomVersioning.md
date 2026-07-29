# DAsset V3 And Custom Asset Versioning Plan

Summary: Introduce a real DAST v3 wire format with package-local custom asset-schema versions and compact schema symbols, then use versioned migration chains for authored assets.

Last reviewed: 2026-07-30

Status: Active
Completed:

## Current Status

DAST v2 is a field-tagged object-package format shared by every authored and
cooked `.dasset`, regardless of main asset class. It stores each schema string
inline with a fixed-width `uint64` byte length. Every object record repeats its
qualified class name, every field repeats its declaring class, property name,
and textual type signature, and reflected struct payloads recursively repeat
the same metadata.

The branch baseline `a147fe24` removed legacy Texture2D and TextureCube source
strings from reflected runtime objects and added raw-field structure upgraders.
It also advanced the global `AssetVersion` from 2 to 3 so old fields could be
read and rewritten. That numeric promotion is premature: the serialized
envelope has not gained a v3 layout, and a texture-only schema migration is not
a package-format change. The first implementation checkpoint must restore the
current writer identity to v2 until the complete v3 reader and writer land.

AssetCore already retains unknown serialized fields, supports object-free
inspection, class-owned inspection and materialized upgrade contributors,
package fingerprints, risk classification, save refusal for unconsumed
compatibility data, and atomic upgrade execution. Those facilities can migrate
removed fields but do not provide a durable semantic version carrier. A
compatible field whose meaning changes, or a migration that spans several
historical schemas, cannot be selected reliably from field shape alone.

The selected design follows the useful separation in Unreal Engine without
copying its serialization API: DAST keeps one global envelope version, while a
bounded package-local table maps stable schema GUIDs to monotonic versions.
Asset-owning modules register schema descriptors, class usage, and ordered
migration steps. Texture source provenance is the first required `0 -> 1`
migration and consumes the already retired source-string fields.

The existing compaction measurement remains relevant. A repository sample of
15 `.dasset` files totals 82,312 bytes and contains 1,245 occurrences of
`Durin::`. Interning top-level schema metadata is estimated to save 7,944 bytes,
or 9.7%, before nested structs. DAST v3 will introduce the custom-version table
and compact schema-symbol representation together so the repository has one
canonical v3 contract and one content rewrite.

## Goal

Define and implement a deterministic DAST v3 representation that:

- separates global package encoding from independently evolving asset schemas;
- supports auditable, ordered, loss-aware migration from older authored state;
- removes repeated schema strings from object and nested-struct records;
- keeps v2 packages readable until an explicit save or upgrade rewrites them;
- prevents normal cook and runtime paths from becoming historical parsers; and
- preserves bounded discovery, compatibility payloads, atomic publication, and
  deterministic bytes.

## Scope

- The authored and cooked `.dasset` DAST envelope owned by AssetCore.
- A bounded package-header custom-version table keyed by stable 128-bit schema
  identifiers.
- Registration of schema descriptors, reflected-class usage, and ordered
  migration steps by modules that own the affected asset types.
- Object-free migration inspection and materialized migration execution from
  one registered descriptor.
- A deterministic package-local table for class, struct, property, and type
  signature symbols.
- DAST v2 reading, missing-custom-version semantics, deterministic v3 writing,
  and one bounded authored-content rewrite.
- Texture2D and TextureCube source provenance as the first complete custom
  schema and migration chain.
- Registry, package inspection, project upgrade workflow, compatibility
  reports, cooking preflight, fixtures, and native validation affected by the
  new version metadata.

## Non-Goals

- Using the Durin engine release version as a serialization compatibility key.
- Adding permanent legacy members or per-class version integers to reflected
  runtime asset objects.
- Automatically rewriting every old package merely because it was discovered
  or loaded.
- Running source-file recovery or other historical migration inside asset
  build, DDC generation, cook serialization, or cooked runtime loading.
- Inventing migrations for unknown schema GUIDs, unsupported old schemas, or
  versions written by a newer engine.
- Supporting downgrade serialization.
- Shortening reflected qualified names or assigning process-global numeric
  identities to types or properties.
- Changing object IDs, dependency semantics, reflection identity, DBLK, DMSH,
  TXPL, or DDC payload formats.
- Selecting whole-body compression without measurement against uncompressed
  v3.

## Design Decisions and Invariants

### Independent version domains

| Version domain | Owner | Meaning | Increment rule |
| --- | --- | --- | --- |
| Engine release | Core/build metadata | User-visible Durin release | Release policy only |
| DAST format | AssetCore | Package-header and body byte encoding | Only when the wire contract changes |
| Custom asset schema | Owning runtime module | Meaning and invariants of authored object state | When a registered compatibility domain changes |
| Generated payload | Owning builder/cooker | DMSH, TXPL, DBLK, or DDC representation | When that rebuildable/runtime representation changes |

DAST v3 is justified by adding the custom-version table and compact symbol
encoding. Future texture, material, mesh, or level migrations increment only
their custom schema version while the DAST format remains v3.

### Package custom-version table

- The uncompressed, self-contained v3 header stores a count followed by
  `(SchemaGuid, Version)` entries. A schema GUID occupies 128 bits and a version
  is an unsigned 32-bit integer.
- Entries are sorted by GUID bytes and contain no friendly name. Names and
  current policy come from the registered descriptor; an unknown GUID remains
  diagnosable by its exact value.
- Version `0` is reserved for `BeforeCustomVersionWasAdded` and is never
  serialized as an entry. A missing entry, including every v2 package, queries
  as version `0`.
- Duplicate GUIDs, serialized zero versions, excessive entry counts,
  truncation, and non-canonical ordering are corrupt input.
- The table is package-wide. A save records only schemas declared by at least
  one serialized object and records the registered latest version after every
  participating object has passed current-schema validation.
- Header-only registry discovery remains bounded. Exact count and byte limits
  are frozen in Stage 0 and enforced before allocation.

### Schema ownership and granularity

- A schema represents one coherent compatibility domain, not the whole engine
  and not automatically one reflected class.
- Related classes may share a schema when they evolve under the same semantic
  contract. Texture2D and TextureCube initially share
  `Engine.TextureSourceProvenance`.
- A module registers a stable GUID, unique diagnostic name, latest version, and
  earliest version for which a complete migration chain is available.
- Classes explicitly declare every schema they use. Collection walks the class
  hierarchy and deduplicates GUIDs, allowing base and derived classes to
  participate in multiple independent domains.
- Independent schema migrations may not rely on inter-schema execution order.
  State that requires ordered evolution belongs to one schema stream. AssetCore
  may use deterministic GUID order operationally, but it is not a semantic
  dependency mechanism.

### Ordered migrations

- A migration step owns exactly one edge `N -> N+1`. Loading version `N`
  executes every required edge in ascending version order; direct
  `N -> Latest` shortcuts are not registered.
- One registration owns the handler ID, affected classes, inspection callback,
  apply callback, validation callback, consumed legacy fields, summary, and
  risk classification. Inspection and execution cannot be registered as
  unrelated policies.
- Multiple schema contributors and contributors inherited from base classes are
  composable; registration is not limited to one upgrader per class.
- A successful step must produce the same represented state when re-audited,
  consume every legacy field it claims, and leave the object valid for the next
  step.
- Migration callbacks receive logical inspected fields and AssetCore decoding
  helpers, never raw assumptions about v2 or v3 field encoding.
- Existing legacy-field structure upgraders become a version-0 adapter. Retired
  fields remain only in inspection/load snapshots and are never restored as
  reflected runtime members.

### Compatibility and failure policy

- Stored versions newer than a registered latest version are
  `BlockedUnsupported`; they are never treated as old data.
- Unknown schema GUIDs are retained in inspection diagnostics and block
  ordinary materialization/save because the current process cannot prove that
  represented state is preserved.
- A version below the earliest complete migration chain is blocked with a
  targeted manual-repair or older-tool diagnostic.
- Lossless deterministic steps are safe upgrades. Steps requiring external
  source recovery, ambiguous inference, or data removal are risky and require
  explicit package-scoped consent.
- Unconsumed incompatible fields retain their current
  `UnknownNewerSchema`/data-loss protection.
- Loading a supported v2 envelope alone is a rewrite opportunity, not an asset
  mutation and not a reason to mark the package Dirty.

### Load, upgrade, cook, and runtime boundaries

- Object-free audit reads the package custom versions and logical field
  snapshots, computes the complete migration plan, and reports risk without
  constructing objects.
- Explicit upgrade execution freshly materializes the package, runs the same
  planned steps, validates current state, verifies audit parity and package
  fingerprint, then atomically saves.
- An ordinary editor-authored load may materialize supported safe migrations
  to provide a canonical in-memory object, but it must report an upgrade
  mutation and mark the package Dirty. It never silently publishes the result.
- Cook preflight requires current authored custom versions. Cook does not invoke
  historical migration or source recovery and reports the explicit upgrade
  action required.
- Cooked runtime packages are emitted with current schema versions and runtime
  loading rejects stale or newer authored schemas. Generated payload versions
  remain independently validated by their payload readers.

### V3 schema-symbol representation

- V3 owns one package-local symbol table shared by object records and nested
  reflected-struct records.
- It stores exact UTF-8 class names, declaring type names, property names, and
  canonical textual type signatures, deduplicated and sorted bytewise.
- Records use a frozen bounded unsigned index encoding. Empty valid symbols are
  represented explicitly; invalid or out-of-range indices are corrupt input.
- Authored strings, source paths, names, and object-reference paths are not
  interned by this table.
- Legacy and inspected field payloads retain their original bytes plus an
  immutable decoding context, so v2 and v3 structured payloads remain readable
  after the package reader is destroyed.

### Publication and determinism

- The v3 writer is enabled only after the full v3 reader, header/table
  validation, compact body writer, inspection support, and golden fixtures pass
  together.
- Equivalent package state produces identical custom-version ordering, symbol
  ordering, object records, and bytes independent of hash iteration,
  allocation addresses, or reflection registration order.
- Upgrade persistence keeps the existing expected-fingerprint check, writable
  policy, compatibility-data-loss consent, and atomic package publication.
- Current content is rewritten once after v3 and the custom migration framework
  pass compatibility validation. Purpose-built v2 fixtures remain v2.

## Current Foundations and Gaps

| Area | Current foundation | Gap |
| --- | --- | --- |
| Package envelope | Field-tagged DAST body and bounded header reader | Version was numerically advanced without a real v3 layout |
| Compatibility | Retained legacy payloads, risk classification, save refusal | No durable custom schema version or migration chain |
| Inspection | All-object snapshots without object construction | Inspection and apply contributors are separately registered |
| Execution | Fingerprinted fresh load, audit parity, atomic save | Migration selection depends mainly on legacy field shape |
| Reflection | Class-owned structure upgraders | One effective upgrader per class; no composable schema domains |
| Object metadata | Exact class/property/type identities | Schema strings and 8-byte lengths repeat inline |
| Registry | Package format and dependencies from bounded headers | No custom-version metadata in v2 |
| Texture migration | Legacy strings retired; canonical provenance migration exists | Global DAST version is carrying a texture-only semantic change |
| Cook | Asset-specific validation and deterministic publication | No shared strict-current authored-schema preflight |

## Implementation Stages

### Stage 0: Freeze The V3 And Migration Contracts

Dependencies: baseline `a147fe24`.

- [ ] Restore the active writer and runtime documentation to DAST v2 until the
  real v3 reader and writer are delivered together.
- [ ] Assign and record stable GUIDs and names for test schemas and
  `Engine.TextureSourceProvenance`.
- [ ] Freeze the byte-level v3 header, custom-version table, schema-symbol
  table, object record, field record, and nested-struct layouts in the owning
  runtime documentation.
- [ ] Select maximum custom-version count, symbol counts and bytes, field and
  object counts, payload sizes, and every overflow/error classification.
- [ ] Freeze descriptor registration, class schema declaration, migration-step
  ordering, missing-version, unknown/newer-version, and minimum-readable rules.
- [ ] Add golden byte vectors for an empty version table, multiple
  deterministically ordered schemas, repeated symbols, nested structs, an
  external reference, and each malformed table case.

#### Acceptance Gate

- The current writer truthfully identifies the existing wire format as v2.
- A reviewer can derive every v3 header boundary, version lookup, symbol
  lookup, migration decision, and unsupported-input result from the documented
  contract.
- No ownership, ordering, risk, or normal-load/cook boundary remains unresolved.

### Stage 1: Add The Custom Schema Registry And Migration Planner

Dependencies: Stage 0.

- [ ] Add public schema ID, descriptor, version-container, declaration, and
  migration-step types in AssetCore without depending on Engine asset classes.
- [ ] Register schemas and class usage with duplicate/conflict validation and
  deterministic class-hierarchy collection.
- [ ] Query missing versions as zero and compute complete `N -> N+1` chains
  with explicit unknown, newer, too-old, and missing-step failures.
- [ ] Replace separate inspection/materialization policy registration with one
  contributor descriptor while preserving compatibility shims for existing
  callers during migration.
- [ ] Allow multiple schema domains and base/derived contributors to compose
  without overwriting one another.
- [ ] Extend audit reports and load reports with schema ID, stored/current
  version, selected steps, risk, and stable handler information.
- [ ] Add focused tests for registration conflicts, absent versions, complete
  chains, gaps, newer input, minimum-readable input, multiple domains,
  inheritance, inspection/apply parity, and unconsumed legacy fields.

#### Acceptance Gate

- Synthetic packages can be classified and migrated from version zero through
  at least three ordered versions without inspecting a class-owned version
  property.
- Audit and execution select identical handlers, risks, consumed fields, and
  final versions.
- Existing structure-upgrade behavior remains available through the temporary
  compatibility adapter.

### Stage 2: Implement Dual-Version Reading And Deterministic V3 Writing

Dependencies: Stage 1.

- [ ] Separate logical package records from v2 and v3 wire readers.
- [ ] Read and validate the bounded v3 custom-version table from the header and
  expose it through header, inspection, registry, and load contexts.
- [ ] Decode and validate the v3 schema-symbol table before resolving record
  indices, including nested struct payloads and retained legacy fields.
- [ ] Keep valid v2 packages readable with an empty custom-version container
  and unchanged represented object state.
- [ ] Gather used schemas and symbols from the complete object graph before
  writing, then emit canonical v3 tables and compact records.
- [ ] Reject undeclared schemas, non-current participating objects, invalid
  indices, duplicates, oversized declarations, truncation, and trailing bytes.
- [ ] Advance the registry snapshot compatibility and reported writer version
  only when new saves actually emit the frozen v3 representation.

#### Acceptance Gate

- Existing v2 assets load and inspect with unchanged logical results.
- New authored and cooked `.dasset` saves emit real v3 packages containing
  canonical custom-version and symbol tables.
- V3 save-load-save is byte deterministic and complete field/reference
  round-trips pass.
- Header reads for v2 and v3 remain bounded independently of body size.

### Stage 3: Migrate Texture Source Provenance Through Version Zero

Dependencies: Stage 2.

- [ ] Register `Engine.TextureSourceProvenance` latest version 1 for Texture2D
  and TextureCube.
- [ ] Convert the existing legacy-source inspection and apply callbacks into
  the shared `0 -> 1` migration descriptor.
- [ ] Migrate complete resolvable old paths into canonical
  `SourceImportData.Source`, hashes, layout, decoder identity, and decoder
  version; classify incomplete or ambiguous input as requiring manual repair.
- [ ] Treat already canonical provenance plus redundant old strings as safe
  cleanup while consuming every retired field.
- [ ] Keep all seven cube strings and the Texture2D string absent from reflected
  and runtime objects; no cook/build rollback path stores or restores them.
- [ ] Require schema version 1 during cook preflight and ensure cooked/runtime
  paths contain no historical field parser.
- [ ] Add v2 legacy-only, v2 canonical-plus-legacy, malformed legacy, v3
  current, v3 newer, audit/apply parity, resave, cook rejection, and runtime
  load tests for both texture classes.

#### Acceptance Gate

- Every supported old texture package either upgrades losslessly to one
  canonical provenance source or produces a precise manual-repair report.
- A successful save writes DAST v3 with
  `Engine.TextureSourceProvenance = 1` and no retired source fields.
- Texture build, DDC, cook serialization, and runtime loading do not recognize
  legacy source strings.

### Stage 4: Connect Project Upgrade And Strict Cook Policy

Dependencies: Stages 2 and 3; coordinate with the active Project Asset Upgrade
Workflow plan.

- [ ] Make the project audit aggregate envelope rewrites, custom schema steps,
  structural legacy fields, risk, read-only state, and unknown/newer schemas in
  one deterministic package report.
- [ ] Make upgrade execution persist only the versions whose migration and
  current-state validation succeeded.
- [ ] Add a shared cook preflight that rejects stale authored schemas before
  asset-specific build/cook work begins.
- [ ] Preserve existing non-upgrade load-mutation blocking, expected
  fingerprints, cancellation, and atomic publication.
- [ ] Expose stable schema GUID/name/version diagnostics to editor and
  automation consumers without duplicating migration policy.

#### Acceptance Gate

- The same package has matching audit, explicit-upgrade, and cook-preflight
  results.
- Safe packages batch-upgrade atomically; risky, stale, read-only, unknown, and
  newer inputs remain separately actionable.
- Neither editor presentation nor cook code contains asset-specific historical
  field parsing.

### Stage 5: Measure, Migrate Content, And Finalize Contracts

Dependencies: Stage 4.

- [ ] Measure v2/v3 size, complete parse time, peak temporary memory, and
  allocation count across level, material, mesh, Texture2D, TextureCube,
  cooked, and synthetic large packages.
- [ ] Compare uncompressed v3 with available deterministic fast codecs and
  record one decision: no body compression, thresholded compression, or a
  separately scoped follow-up.
- [ ] Resave current Engine and Sandbox authored packages through the validated
  v3 writer while preserving deliberate v2 compatibility fixtures.
- [ ] Run focused package, registry, inspection, migration, texture, cooking,
  determinism, and corruption tests through DurinDevTool.
- [ ] Run required integration/full validation, update lasting runtime
  contracts, validate all plans, and record completion evidence.

#### Acceptance Gate

- Current content loads, audits, upgrades, cooks, unloads, and reloads as v3
  with current declared schemas.
- Representative content records a positive aggregate size result and no
  unaccepted critical load regression.
- Long-lived wire, schema, migration, and cook rules live in the owning runtime
  documentation rather than only in this plan.

## Validation Matrix

| Area | Evidence |
| --- | --- |
| Version separation | DAST changes only for wire layout; texture changes only its schema entry |
| Header discovery | Bounded v2/v3 reads with empty, maximum, duplicate, zero, and truncated version tables |
| Schema registry | Stable IDs, conflict rejection, multiple domains, inheritance, absent and newer versions |
| Migration chain | `0 -> 1 -> 2 -> 3`, missing edge, too-old input, deterministic ordering, inspection/apply parity |
| Compatibility | Safe cleanup, lossless migration, risky external recovery, unknown fields and unknown schema IDs |
| Symbol table | Ordering, deduplication, empty value, invalid index, duplicate entry, and aggregate bounds |
| Round trip | Scalars, strings, names, enums, GUIDs, structs, arrays, maps, nested containers, and references |
| Texture provenance | 2D and cube legacy-only, canonical-plus-legacy, incomplete, current, newer, and clean resave |
| Cook/runtime | Stale authored schema rejection and current cooked-package loading without migration callbacks |
| Publication | Fingerprint mismatch, read-only package, cancellation, save failure, and atomic success |
| Determinism | Equivalent object state produces identical v3 package bytes |
| Performance | V2/v3 size, warm parse time, peak temporary memory, and allocation comparison |

Build and test execution must follow [Build And Run](../Development/Build/BuildAndRun.md)
and [Native C++ Tests](../Development/Build/NativeTests.md).

## Definition of Done

- DAST v3 has a documented, bounded, deterministic custom-version table and
  compact schema-symbol encoding.
- Asset modules can register stable schemas and composable ordered migrations
  without reflected legacy fields or class-owned version properties.
- V2 remains readable, new saves write v3, newer/unknown schemas cannot be
  silently overwritten, and supported migrations are auditable before load.
- Texture2D and TextureCube provenance migrate through the shared framework and
  retain one canonical source of truth.
- Cook and runtime require current authored schemas and contain no historical
  migration policy.
- Existing project upgrade safety, compatibility payload retention, and atomic
  publication guarantees remain intact.
- Current content and required fixtures pass the validation matrix, lasting
  contracts are updated, and the plan is completed.

## Deferred Follow-ups

- Downgrade or multi-writer interoperability tooling.
- Source-control-aware checkout before changing read-only packages.
- Persistent audit caches beyond registry fingerprints.
- Per-object custom-version tables if future partial-package saving makes a
  package-wide version insufficient.
- Structural type bytecode or process-global reflection IDs.
- Authored-value string interning.
- Chunked or seekable body compression.

## Related Documentation

- [Asset Packages](../Runtime/Assets/AssetPackages.md)
- [Asset Data Lifecycle and Storage](../Runtime/Assets/AssetDataLifecycle.md)
- [Versioning](../Runtime/Assets/Versioning.md)
- [Content Version Control](../Development/VersionControl/ContentVersionControl.md)
- [Project Asset Upgrade Workflow Plan](ProjectAssetUpgradeWorkflow.md)

## Related Code

- `Engine/Source/Runtime/AssetCore/Public/AssetSystem.h`
- `Engine/Source/Runtime/AssetCore/Private/AssetSystem.cpp`
- `Engine/Source/Runtime/Engine/Public/Texture/Texture2D.h`
- `Engine/Source/Runtime/Engine/Public/Texture/TextureCube.h`
- `Engine/Source/Runtime/Engine/Private/Texture/Texture2D.cpp`
- `Engine/Source/Runtime/Engine/Private/Texture/TextureCube.cpp`
- `Engine/Tests/Native/AssetCoreTests/Private/PackageTests.cpp`
- `Engine/Tests/Native/EngineTests/Data/AssetStructureUpgrade/`
