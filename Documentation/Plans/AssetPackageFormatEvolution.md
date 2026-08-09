# Asset Package Format Evolution Plan

Summary: Harden DAST codec dispatch and explicit migration execution so future package-format upgrades add isolated codecs and exact edges without reopening every AssetCore path.

Last reviewed: 2026-08-09

Status: Active
Completed:

## Current Status

Planning baseline `9a13a105`. The DAST v4 rollout is complete: v4 is the sole
authored reader, ordinary writer, migration writer target, and tracked-content
baseline. The built-in migration registry is empty and no authored package
change is authorized by this plan.

The post-rollout architecture review found that the bounded v4 codec,
construct-free inspection, compatibility audit, deterministic writer, and
journal-compensated publication boundaries are strong foundations. The next
format upgrade would nevertheless require broad edits because shared
orchestration still names `DastV4` directly, the migration descriptors do not
carry executable semantics, and the permanent test suite no longer exercises
the dormant apply/rollback path after v3 retirement.

No implementation stage has started. Stage 0 first freezes the internal codec
capability contract and the intentionally narrow early-development migration
model before production code moves.

## Goal

Make a future DAST format upgrade a bounded addition: implement one isolated
format codec, register its capabilities and policy, add one explicit lossless
migration edge, and qualify it without changing frozen older codecs or
duplicating version branches across AssetCore.

The resulting boundaries must keep unsupported input fail-closed, preserve the
existing no-implicit-write policy, and continuously test the generic migration
transaction even while the repository has only one production format.

## Scope

- Give each package format an immutable identity and an internal, statically
  registered set of high-level reader and writer capabilities.
- Route header, validation, inspection, compatibility, live loading,
  serialization, and current-format byte mutations through one codec lookup
  boundary.
- Make Archive capture use the selected codec's target format version rather
  than the mutable ordinary-writer policy.
- Replace metadata-only migration handlers with an exact, executable migration
  edge model and revalidate the selected edge at apply time.
- Keep the existing migration lock, staging journal, bundle-atomic publication,
  post-audit, registry publication, and compensation behavior.
- Define cache/policy identity independently from an individual wire version.
- Add a permanent test-only upgrade harness that keeps dispatch, apply,
  determinism, stale-input rejection, and rollback behavior live between real
  format upgrades.
- Move lasting codec and migration invariants into Runtime documentation after
  implementation.

## Non-Goals

- Designing DAST v5 bytes, schemas, compression, or size targets.
- Adding a production v5 reader or writer or migrating any tracked `.dasset`.
- Changing any frozen DAST v4 byte, ordering, default-relative, retained-data,
  or custom-version semantic.
- Establishing external-project compatibility windows, downloadable migration
  bundles, or a release deprecation policy.
- Supporting dynamically loaded or plugin-provided package codecs; the registry
  remains an engine-owned static composition boundary.
- Building a general multi-hop migration DAG before a real compatibility policy
  requires retaining more than one historical production reader.
- Creating a global asset-schema version. Independent authored schemas continue
  to use named custom versions and existing class/struct upgrade boundaries.
- Replacing the existing journal with a new publication or recovery system.

## Design Decisions and Invariants

- The codec registry is private AssetCore infrastructure. Public policy may
  expose supported versions and selected writers, but callers do not branch on
  codec enums or construct version-specific decoded types.
- A frozen codec always serializes with its own format identity. Changing
  `OrdinaryAssetPackageWriterVersion` must not change Archive-visible versions,
  custom-version discovery, bytes, or diagnostics produced by an older writer.
- Supported readers, the ordinary writer, and the migration target remain
  separate policies. Startup, audit, inspection, loading, and cache discovery
  never select a writer or mutate authored bytes.
- Shared read entrypoints parse the common magic/version preamble once, look up
  a complete reader capability set, and fail before version-specific parsing if
  no capability exists. There is no legacy catch-all parser.
- Ordinary authoring mutations operate only on the ordinary-writer format unless
  a codec explicitly advertises a safe version-preserving rewrite capability.
  A package-format transition remains exclusive to explicit migration apply.
- During major-zero early development, package migration uses one exact edge
  from the selected source format to `AssetPackageMigrationWriterVersion`.
  Arbitrary graph resolution is not treated as executable capability. A future
  multi-version compatibility plan may add chained execution when concrete
  intermediate transforms and retention requirements exist.
- An exact edge declares its execution strategy, source and target codec
  identities, risk, and stable handler id. The ordinary lossless strategy may
  load through the source reader, run registered source-aware schema upgrades,
  and serialize through the target writer; any special transform is an explicit
  edge-owned callback rather than a report-only descriptor.
- Apply does not trust a mutable plan as authorization. Under the migration
  writer lock it rechecks the source preamble and fingerprint, resolves the
  registered exact edge, verifies its lossless risk and target codec, then
  executes that edge. A missing, changed, or mismatched edge blocks before
  staging.
- `AssetSchema` is not a package-format migration kind. The current unused,
  identity-free enum path is removed or replaced only by a separately planned
  schema-keyed contract with an owning GUID/class and executable semantics.
- Reader-policy cache identity is an explicit generation or deterministic set
  fingerprint. It cannot alias merely because two policies share the same
  latest wire version.
- Every version-specific decoded representation remains inside its codec adapter.
  Shared operations consume version-neutral AssetCore results or invoke a
  high-level capability such as reference rewrite or relocation.
- All existing failure-atomicity, compatibility-risk, retained-unknown,
  deterministic-output, dependency-closure, and no-implicit-write guarantees
  remain required.

The initial capability matrix is:

| Operation | Required formats | Ownership |
| --- | --- | --- |
| Preamble and codec lookup | Every input | Shared AssetCore boundary |
| Header, validate, inspect, compatibility, reference projection, live load | Every supported reader | Version codec reader capabilities |
| Ordinary and bundle serialization | Ordinary writer only | Version codec writer capability |
| Reference fixup, relocation, redirector/cook rewrite | Ordinary writer unless explicitly advertised otherwise | Version codec mutation capabilities |
| Package-format transition | Exact registered source-to-migration-target edge | Migration orchestration plus source/target codecs |
| File publication, rollback, registry/cache commit | Version-neutral | Existing AssetCore transaction boundary |

## Current Foundations and Gaps

The following foundations are retained:

- `DastV4::ReadHeader`, `DecodePackage`, `InspectPackage`,
  `ProbeCompatibility`, and `LoadAssetPackage` already provide bounded,
  transactional version-specific reader operations.
- `DastV4::WriteAssetPackage` and canonical re-emission provide deterministic
  output and exact retained-closure validation.
- `FAssetPackageLoadSnapshot`, migration sidecars/manifests, writer locking,
  staged publication, post-audit, and registry projection already define the
  correct failure boundary.
- `FArchiveVersionContext` separates DAST and GUID-keyed custom versions from
  the engine release version.
- Runtime documentation already freezes v4 and requires explicit migration for
  later format changes.

The implementation gaps are concrete:

- `AssetPackageVersionPolicy.h` selects a reader enum, while
  `AssetSystem.cpp` and `AssetCompatibility.cpp` repeat direct v4 branches for
  header, validation, inspection, compatibility, live load, cook, relocation,
  redirector, and reference-rewrite paths.
- `ValidateAssetPackageBytes` checks the supported-reader policy and then always
  calls the v4 decoder. Adding another supported version without editing that
  function would dispatch valid new bytes to the wrong codec.
- `ProbeAssetPackageCompatibility` retains an unreachable legacy fallback.
  Adding a new reader enum without a matching branch would enter that parser
  instead of failing closed.
- `FAuthoredCaptureArchive` uses `OrdinaryAssetPackageWriterVersion` inside the
  v4 production writer. Selecting v5 for ordinary saves would therefore expose
  v5 to serializers while the retained v4 writer emits v4 bytes.
- `FAssetMigrationHandlerDescriptor` contains only identity, versions, kind,
  and risk. `ApplyAssetPackageMigrations` does not execute or re-resolve its
  recorded steps and directly invokes the v4 writer/decoder.
- `EAssetMigrationKind::AssetSchema` has no production registration or
  execution path and lacks a schema identity.
- The current permanent package tests cover migration graph validation but no
  longer exercise apply, staging, publication failure, rollback, recovery, or
  post-audit after the v3 fixtures were retired.
- `SupportedAssetPackageReaderVersions`, reader selection, and
  `AssetPackageReaderPolicyFingerprint` are separate manually synchronized
  declarations, and the fingerprint aliases the v4 wire number.

## Implementation Stages

### Stage 0: Freeze codec ownership and exact-edge semantics

- [ ] Inventory every shared package operation and classify it as preamble,
  supported-reader, ordinary-writer, current-format mutation, migration, or
  version-neutral publication.
- [ ] Freeze the private codec capability signatures needed by header,
  validation, inspection, compatibility, reference projection, live load,
  deterministic serialization, and current-format byte mutation.
- [ ] Freeze the exact-edge execution contract, including generic load/write,
  optional edge-owned transformation, lossless risk, source/target validation,
  cancellation, diagnostics, and plan/apply revalidation.
- [ ] Decide the narrow disposition of the unused `AssetSchema` kind: remove it
  in this plan or replace it only if a concrete schema-keyed owner and fixture
  is identified.
- [ ] Record the allowed direct version references and an upgrade change-budget
  check: a synthetic next codec may require new codec/adapter files, policy
  registration, an exact edge, tests, and docs, but not new shared orchestration
  branches.

#### Acceptance Gate

- The operation matrix has one owner per decision, every capability has a
  version-neutral shared signature, migration has executable semantics, and no
  unresolved ownership or schema-migration question remains for Stage 1.

### Stage 1: Pin codec identity and centralize policy

- [ ] Pass an explicit target format version through Archive discovery and
  emission; make the v4 writer pass `DastV4::Version` rather than the ordinary
  writer policy.
- [ ] Introduce the private static codec table and a single preamble-to-reader
  lookup. Derive or validate the supported-reader list against registered
  capabilities.
- [ ] Add writer lookup for ordinary and migration targets and reject startup or
  tool initialization when selected writer capabilities are incomplete.
- [ ] Replace the wire-number cache fingerprint alias with an explicit reader
  policy generation or deterministic supported-set identity.
- [ ] Add focused tests proving v4 Archive-visible identity remains v4,
  registered policies are complete and unique, and missing capabilities fail
  closed without invoking another codec.

#### Acceptance Gate

- V4 golden bytes and deterministic saves are unchanged, every selected policy
  resolves to exactly one complete codec, and changing an injected ordinary
  writer identity cannot alter the frozen v4 writer's Archive context.

### Stage 2: Route read-only and live-load boundaries through codecs

- [ ] Route header reads, whole-package validation, inspection, compatibility
  probes, reference extraction, and live loading through the codec table.
- [ ] Remove the unreachable compatibility fallback and all shared
  `EAssetPackageReaderKind::DastV4` conditionals from these entrypoints.
- [ ] Preserve shared fingerprint, freshness, cancellation, residency,
  dependency-cycle, report, and transaction publication behavior around codec
  calls.
- [ ] Parameterize reader contract tests over all registered test/production
  codecs and add synthetic unsupported/truncated inputs to every shared read
  entrypoint.

#### Acceptance Gate

- Every supported codec passes equivalent shared header, validate, inspect,
  compatibility, reference, and live-load contracts; unsupported versions fail
  before codec parsing or runtime-state mutation; shared orchestration contains
  no version-specific read branch.

### Stage 3: Isolate writer and current-format mutation paths

- [ ] Route ordinary serialization, single save, atomic bundle save, and
  migration-target serialization through selected writer capabilities.
- [ ] Move v4-specific Archive-to-wire adaptation out of generically named
  orchestration into a frozen v4 adapter, retaining only genuinely
  version-neutral capture/load support in common code.
- [ ] Route reference fixup, relocation, redirector creation, cook
  canonicalization, and other decoded-byte rewrites through explicit codec
  mutation capabilities and enforce the ordinary-format precondition.
- [ ] Remove shared v4 decoded types and diagnostics from `AssetSystem.cpp` and
  the migration transaction layer.
- [ ] Qualify byte-identical repeated v4 serialization, redirector/fixup,
  relocation, and cook output plus fail-before-mutation behavior for a supported
  non-ordinary test codec.

#### Acceptance Gate

- Frozen v4 code owns all v4 decoded representations and writer adaptation;
  shared saves and mutations select capabilities from policy; adding a test
  writer requires no edit to shared save, relocation, fixup, or cook control
  flow.

### Stage 4: Make migration authorization executable and permanent

- [ ] Replace or narrow the metadata-only migration graph with registered exact
  executable edges and stable report descriptors.
- [ ] Make planning record the source codec, target codec, exact edge identity,
  risk, input fingerprint, and policy/registry identity.
- [ ] Make apply re-read and verify the source version, re-resolve the exact
  edge under lock, reject changed/tampered plans, and execute the registered
  strategy through codec capabilities rather than naming v4.
- [ ] Preserve deterministic double-write validation, dependency closure,
  compatibility/retained-data gates, journal staging, bundle publication,
  post-audit, registry commit, rollback, and interrupted-operation recovery.
- [ ] Add a permanent test-only source/target codec pair or equivalent injected
  harness. Keep successful migration, missing edge, non-lossless edge, stale
  input, tampered plan, dependency omission, cancellation, every injected
  apply phase, rollback failure, recovery, post-audit, and registry publication
  tests active after real legacy codecs are retired.

#### Acceptance Gate

- A test-only exact upgrade executes only its registered edge and target writer;
  missing, changed, lossy, or fabricated authorization fails before staging;
  every publication failure restores the complete bundle or reports a verified
  recovery requirement; no production legacy codec is needed to keep these
  tests live.

### Stage 5: Rehearse the next-version change budget and qualify

- [ ] Add a synthetic next-format adapter in tests and demonstrate that all
  shared reader, writer, mutation, and migration contract suites discover it
  through registration without new shared version branches.
- [ ] Review direct `DastV4` and `AssetPackageV4FormatVersion` references;
  retain only v4 codec/fixture ownership and deliberate central policy entries.
- [ ] Run focused AssetCore, Core object, compatibility, migration, cache, cook,
  and tool tests through the repository build/test entrypoint.
- [ ] Run the asset baseline and tracked-package hash checks and prove this plan
  changed no authored `.dasset` bytes.
- [ ] Run the required full `all` build and documentation validation.
- [ ] Move lasting format-registry, codec-identity, mutation-policy, and exact
  migration contracts into Runtime documentation and complete this plan.

#### Acceptance Gate

- The synthetic next version stays within the frozen change budget, all focused
  and full validation passes, the tracked corpus remains byte-identical v4, and
  Runtime documentation owns every lasting implemented invariant.

## Validation Matrix

| Area | Required evidence |
| --- | --- |
| Codec identity | V4 writer observes v4 regardless of ordinary-writer policy; frozen golden bytes remain identical |
| Policy completeness | Every supported reader and selected writer resolves uniquely; cache policy identity changes with the supported contract |
| Read dispatch | Header, validation, inspection, compatibility, references, and live load pass for every registered codec and fail closed for unsupported input |
| Writer dispatch | Ordinary and bundle saves use only the selected writer; older writers remain stable and independently callable where required |
| Mutation dispatch | Fixup, relocation, redirector, and cook rewrites use declared capabilities and reject non-ordinary formats before mutation |
| Migration authorization | Source/target/edge/risk/policy are revalidated at apply; steps are executable rather than report-only |
| Transaction safety | Stale input, dependency omissions, cancellation, injected failures, rollback, recovery, post-audit, and registry publication preserve existing guarantees |
| Permanent regression | Test-only source/target codecs keep generic migration apply and recovery covered after production legacy retirement |
| Content invariance | Baseline and hashes prove no tracked `.dasset` changed during architecture hardening |
| Qualification | Focused suites, DurinDevTool checks, documentation validation, and the full `all` build pass |

## Definition of Done

- Shared AssetCore orchestration contains one codec lookup boundary and no
  version-specific reader/writer control-flow branches.
- The frozen v4 writer uses v4 Archive semantics independent of mutable policy
  and continues to emit byte-identical qualified output.
- Supported readers, selected writers, and cache policy identity are complete,
  unique, and validated from one owned declaration.
- Migration planning and apply use one exact executable edge, and apply
  revalidates authorization and source bytes under the writer lock.
- Generic apply, rollback, recovery, and post-audit tests remain active without
  retaining a production legacy codec.
- A synthetic next version demonstrates that future format work is isolated to
  codec/adapter files, policy registration, an exact edge, fixtures, and docs.
- No tracked authored package changes, and lasting contracts are published in
  Runtime documentation.

## Deferred Follow-ups

- The actual DAST v5 wire contract, reader, writer, measurement, migration,
  qualification, content rollout, and v4 retirement.
- External-project compatibility windows and downloadable migration bundles.
- Multi-hop migration graphs and retained historical writers beyond the bounded
  early-development source-to-current window.
- Dynamic/plugin codec registration.
- Schema-keyed migration infrastructure or custom struct codecs unless a
  production asset audit provides concrete durable state that current custom
  versions and upgrade hooks cannot represent.

## Related Documentation

- [Versioning](../Runtime/Assets/Versioning.md)
- [Asset Packages](../Runtime/Assets/AssetPackages.md)
- [Build and Run](../Development/Build/BuildAndRun.md)
- [DAST V4 Mixed-Version Migration](Archive/2026-08/DASTV4MixedVersionMigration.md)
- [DAST V4 Qualification and Rollout](Archive/2026-08/DASTV4QualificationAndRollout.md)
- [Compact Asset Serialization Roadmap](../Roadmaps/Archive/2026-08/CompactAssetSerialization.md)

## Related Code

- `Engine/Source/Runtime/AssetCore/Public/AssetPackageVersionPolicy.h`
- `Engine/Source/Runtime/AssetCore/Public/AssetPackageV4Reader.h`
- `Engine/Source/Runtime/AssetCore/Public/AssetPackageV4Writer.h`
- `Engine/Source/Runtime/AssetCore/Public/AssetMigration.h`
- `Engine/Source/Runtime/AssetCore/Private/AssetPackageArchive.cpp`
- `Engine/Source/Runtime/AssetCore/Private/AssetSystem.cpp`
- `Engine/Source/Runtime/AssetCore/Private/AssetCompatibility.cpp`
- `Engine/Source/Runtime/AssetCore/Private/AssetMigration.cpp`
- `Engine/Tests/Native/AssetCoreTests/Private/AssetPackageVersionPolicyTests.cpp`
- `Engine/Tests/Native/AssetCoreTests/Private/PackageTests.cpp`
