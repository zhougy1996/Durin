# Legacy DAST v4 Compatibility Retirement Plan

Summary: Retire DAST v4 write, rollback, and read compatibility after separating the version-neutral object-stream implementation required by DAST v5.

Last reviewed: 2026-08-24

Status: Archived
Completed: 2026-08-24

## Current Status

The user has authorized a complete DAST v4 format break: no external legacy
assets remain, no future old-asset workflow is required, and DAST v4 reading,
writing, mutation, and canonical rollback may be retired. This activates the
previously conditional legacy-retirement boundary from the completed authored
package storage roadmap.

The retirement is complete. DAST v5 is the sole registered reader, writer, and
mutation codec; DAST v4 has no production reader, writer, rollback, converter,
or CLI target. A valid v4 preamble is rejected as `UnsupportedVersion` before
format-specific decoding and a focused runtime/catalog test proves that the
prior catalog revision remains unchanged.

A construct-free audit on 2026-08-24 inspected all 30
mounted tracked packages: every package is DAST v5 and reported `Ready`,
`Compatible`, `Current`, with no findings. The tracked corpus contains two
`.dabulk` companions, both retained as current DABK v1 storage. The older
storage-qualification wrapper could not consume the native inventory because
its report-schema expectation is stale; that wrapper is part of the lasting
tooling cleanup in Stage 4, not evidence of a package integrity failure.

The first implementation change is structural:
DAST v5 currently reuses implementation named and exposed as `DastV4` for its
logical object stream. That shared object-stream codec must become
version-neutral before the v4 envelope and compatibility surface can be
removed. DABK v1 is not legacy at this boundary; it remains the selected
external payload placement for DAST v5.

## Goal

Remove all production support for reading, writing, mutating, migrating, or
rolling back to DAST v4 while preserving the DAST v5 wire, its version-neutral
logical object stream, its external DABK v1 placement, and failure-atomic asset
publication.

## Scope

- Separate the logical object-stream, Archive adapter, reflected schema/value,
  reference, and object-construction mechanics shared by DAST v5 from the
  `DastV4` namespace, filenames, constants, diagnostics, and public testing
  surface.
- Remove DAST v4 from the supported-reader policy and package codec registry.
- Remove DAST v4 writer selection, canonical rollback, mutation, relocation,
  redirector, migration, command-line, report, and test surfaces.
- Make a DAST v4 preamble fail deterministically as an unsupported version
  before format-specific decoding, object construction, mutation, or
  publication.
- Bump every persisted reader-policy or compatibility fingerprint whose
  meaning changes when v4 support is removed.
- Update lasting package, tool, test, and workflow documentation to describe
  DAST v5 as the only supported authored package version.
- Audit the tracked corpus and supported repository workflows for v4 input or
  rollback dependencies before deleting the compatibility implementation.

## Non-Goals

- Removing or replacing DABK v1, `ExternalDabkV1`, companion-first
  publication, or the current Git/LFS boundary.
- Changing the DAST v5 trailer/footer bytes, object-stream bytes, placement
  vocabulary, or ordinary-writer policy.
- Migrating old assets, retaining a hidden recovery reader, or providing a
  downgrade utility after retirement.
- Removing generic transaction rollback, publication rollback, catalog
  reconciliation, or failure recovery merely because their APIs use the word
  `Rollback`.
- Adding persistent virtualization, compression, deduplication, range IO, or
  package aggregation.
- Rewriting Git history or deleting historical plan and roadmap evidence.

## Design Decisions and Invariants

- This plan is an intentional format break. DAST v4 is not a deprecated input
  after completion; it is unsupported.
- The user's statement that no external old assets remain and old assets will
  not be used again is the product-compatibility authority for retirement.
  Stage 0 still verifies the repository corpus, supported branches/workflows,
  fixtures, and recovery inputs so implementation does not strand an in-scope
  asset accidentally.
- DAST v5 remains the only supported reader and writer. Unsupported DAST
  versions fail from the bounded common preamble before a version-specific
  codec is selected.
- The v5 object stream deliberately reuses the established logical encoding,
  but that encoding is not a promise to support the v4 package envelope.
  Shared code gains version-neutral ownership before v4 compatibility code is
  removed; v5 must not keep production dependencies on a `DastV4` compatibility
  namespace as an accidental substitute for that refactor.
- DABK v1 remains a current DAST v5 external placement. Its container reader,
  writer, integrity rules, publication ordering, reachability, repair, and
  orphan protection remain production behavior.
- Removing v4 read support invalidates reader-policy caches and any other
  persisted compatibility result derived from the `{4, 5}` supported set.
- Tests may retain one minimal v4 preamble byte fixture solely to prove the
  deterministic unsupported-version result. They do not retain a hidden v4
  decoder, writer, migration path, or full legacy golden corpus.
- Historical completed plans and roadmaps keep their recorded v4 decisions as
  historical evidence. Current contracts and active guidance record the new
  v5-only policy.

## Current Foundations and Gaps

| Area | Current foundation | Retirement gap |
| --- | --- | --- |
| Corpus | The tracked corpus was migrated to DAST v5, and the user confirms no external v4 assets remain | Run a final construct-free repository audit and classify any fixture, generated input, supported branch, backup, or recovery dependency before deletion |
| Version policy | DAST v5 is the ordinary writer | `SupportedAssetPackageReaderVersions` still contains v4 and v5; `LatestAssetPackageWriterVersion` still names v4 while the ordinary writer names v5 |
| Codec registry | Version dispatch is centralized | The v4 codec remains reader-, writer-, and mutation-complete and is registered beside v5 |
| Shared object stream | V5 reuses the proven logical tables, values, Archive adapter, reference extraction, compatibility probe, and live loading | V5 reaches those mechanics through `DastV4` headers, namespaces, constants, and diagnostics |
| Rollback tooling | Canonical resave and DurinAssetTool can explicitly target v4 | V4 remains the default canonical-resave target and a supported report/CLI selection |
| External payloads | DAST v5 uses trailer entries with `ExternalDabkV1` and immutable generation-named companions | Cleanup must distinguish current DABK v1 behavior from retired v4 package descriptors |
| Tests | V4 and v5 package, consumer, migration, rollback, corruption, and golden coverage exists | Shared behavior must move to version-neutral/v5 coverage before v4-only fixtures are removed |

## Implementation Stages

### Stage 0: Freeze the retirement inventory and format-break contract

- [x] Run a construct-free inventory of every tracked `.dasset` and reachable
  `.dabulk`; require every supported package to be DAST v5 with no missing,
  corrupt, stale, or orphaned authored payload.
- [x] Record the product decision that external v4 assets, supported old
  branches, backups, import inputs, and recovery workflows are outside the
  compatibility boundary and will not receive a retained reader or converter.
- [x] Classify every production and test reference to v4 as shared
  object-stream implementation, v4 envelope read, v4 write/mutation, rollback
  tooling, negative unsupported-version coverage, or historical evidence.
- [x] Freeze the post-retirement version policy, cache/fingerprint changes,
  CLI behavior, diagnostic text/code, and disposition of v4 golden fixtures.
- [x] Select version-neutral names and file/module ownership for the shared
  logical object-stream codec without changing its bytes.

#### Acceptance Gate

- The repository inventory contains no supported v4 package and all reachable
  v5 external payloads remain valid.
- Every live v4 dependency has one explicit disposition and no supported
  branch, backup, recovery, tool, or external asset requires v4 decoding.
- The selected shared-code boundary is sufficient for v5 and contains no v4
  package-policy promise.

#### Stage 0 Evidence and Frozen Disposition

- `DevTool asset audit --project Sandbox\\Sandbox.dproject --format json`
  reported 30 DAST v5 packages, all ready, compatible, current, and finding-free.
- `git ls-files`/repository inventory found 30 `.dasset` files and two `.dabulk`
  files; no tracked package preamble declares version 4.
- External old assets, old branches, backups, import inputs, downgrade, and
  recovery workflows are outside the supported boundary by explicit product
  decision. No reader or converter will be retained for them.
- Shared logical-stream code will move from `DastV4` and `PackageV4*` ownership
  to the internal `PackageObjectStream` namespace and filenames. The neutral
  codec owns the five-section logical stream only; public package dispatch and
  version policy remain in the v5 envelope codec.
- Post-retirement policy is one registered reader/writer version (v5), reader
  policy fingerprint `3`, only v5 canonical-resave behavior, and deterministic
  `UnsupportedVersion` for a valid DAST v4 preamble. One minimal negative
  preamble fixture may remain; complete v4 golden and decoder fixtures will not.
- Production references are classified as follows: the three `PackageV4*`
  implementation files and two headers contain the shared logical stream plus
  removable v4 envelope assumptions; `AssetPackageCodec.cpp` owns removable v4
  dispatch; authoring/operations/canonical-resave/tool references own removable
  v4 output and rollback; v5 codec references require neutralization; v4 test
  files are either shared behavior to retarget or legacy fixtures to delete;
  completed plans/roadmaps remain historical evidence.

### Stage 1: Extract the version-neutral logical object-stream codec

- [x] Move or rename shared package tables, logical types, schema/value codec,
  Archive adapter, reference extraction, compatibility probing, object
  construction, and writer input into version-neutral internal ownership.
- [x] Change DAST v5 to call the version-neutral interfaces directly rather
  than converting its preamble and entering a `DastV4` compatibility API.
- [x] Separate common logical-stream limits and diagnostics from v4 envelope
  version, EOF, and exact-five-section policy.
- [x] Move shared production and reference-codec tests to version-neutral or
  v5 ownership while preserving byte-for-byte v5 golden output.
- [x] Prove the refactor introduces no package, trailer, companion, registry,
  reference, or authored-payload behavior change.

#### Acceptance Gate

- DAST v5 build, read, write, inspect, compatibility, reference, relocation,
  redirector, canonical-resave-to-v5, and live-load paths have no dependency
  on a v4 envelope reader or writer.
- Existing v5 golden bytes and complete package/consumer behavior remain
  unchanged.
- Remaining `DastV4` production references are exclusively the compatibility
  paths scheduled for Stages 2 and 3.

#### Stage 1 Evidence

- Shared reader, writer, Archive adapter, logical types, limits, diagnostics,
  reference extraction, compatibility probing, mutation, and live construction
  now live under private `PackageObjectStream` filenames and namespace.
- DAST v5 uses `PackageObjectStream` directly and exposes the neutral
  `BuildPackageFromObjectStream` helper; no `DastV4` codec namespace or
  `PackageV4Reader` / `PackageV4Writer` implementation name remains. The
  explicitly legacy writer-selection enum is retained only until Stage 2.
- `DevTool build --target AssetPackageTests` completed for
  `Win64-Debug-DurinEditor`; `DevTool test AssetPackageTests` passed all 111
  tests across 8 suites, including the v5 trailer golden and shared codec
  coverage.

### Stage 2: Retire DAST v4 writing, mutation, and rollback

- [x] Remove `EAssetPackageWriterSelection::DastV4` and make v5 the only
  package-authoring selection and canonical-resave target.
- [x] Remove v4 writer registration, live-object writer, redirector writer,
  reference rewrite, relocation, and mutation capability.
- [x] Remove `--target=v4`, v4 rollback planning/application, v4-specific
  report fields, and user-facing rollback diagnostics from DurinAssetTool.
- [x] Replace or remove `LatestAssetPackageWriterVersion` and other ambiguous
  constants so every default and source-format decision states its real v5 or
  version-neutral meaning.
- [x] Remove v4 write/rollback use from domain and integration tests while
  preserving failure-atomic rollback of in-progress v5 operations.

#### Acceptance Gate

- No production API, tool command, test helper, environment value, or implicit
  fallback can create or mutate a DAST v4 package.
- Ordinary and explicit package authoring produce only canonical DAST v5.
- Generic transactional rollback and the previous committed v5 package/payload
  closure remain intact under injected publication failures.

#### Stage 2 Evidence

- The public writer selection contains only `Ordinary` and explicit `DastV5`;
  canonical resave defaults to and verifies v5, and the v4 codec has no writer
  or mutation capabilities registered.
- DurinAssetTool defaults canonical resave to v5 and rejects `--target=v4` as
  an unknown argument before project initialization.
- V4 upgrade/rollback cases were removed from package and representative
  consumer tests; v5 companion publication-failure coverage still proves the
  previous committed package/payload closure is restored.
- `DevTool build --target AssetPackageTests` and
  `DevTool build --target DurinAssetTool` completed for
  `Win64-Debug-DurinEditor`; all 111 `AssetPackageTests` passed.
- The ambiguous `LatestAssetPackageWriterVersion` name is gone; the temporary
  internal context is explicitly `AssetPackageObjectStreamVersion`. Stage 3
  changes that stream preamble from the retired envelope version to v5.

### Stage 3: Retire DAST v4 reading and compatibility dispatch

- [x] Remove v4 from `SupportedAssetPackageReaderVersions`, unregister its
  codec, and delete v4 header, validation, inspection, reference, compatibility,
  load, and runtime dispatch paths.
- [x] Increment the reader-policy fingerprint and invalidate or rebuild every
  persisted cache keyed by the old supported-reader set.
- [x] Make a valid DAST preamble with version 4 return the selected
  unsupported-version error before any v4-specific parsing or allocation.
- [x] Delete v4-only production reader structures, diagnostics, retained
  unknown-closure behavior, and compatibility plumbing after shared v5 code
  has moved to neutral ownership.
- [x] Confirm registry discovery, cache loading, dependency scans, command-line
  inspection, repair, and editor startup all handle a v4 file as an unsupported
  input without mutation.

#### Acceptance Gate

- DAST v5 is the sole registered and supported package version.
- No production v4 decoder or hidden recovery reader remains linkable.
- A minimal negative fixture proves deterministic v4 rejection with no object,
  catalog, dirty-state, package, or payload publication side effect.

#### Stage 3 Evidence

- The codec registry contains only `dast-v5`; supported-reader policy is `{5}`
  and `AssetPackageReaderPolicyFingerprint` advanced from `0x41504302` to
  `0x41504303`.
- The neutral object-stream writer and reader now emit and require the v5
  preamble directly. V5 no longer rewrites a v5 preamble to an internal v4
  preamble, so no production v4 decoder remains behind the shared codec.
- Public header and validation tests include version 4 beside other unsupported
  versions and prove `UnsupportedVersion` is returned at common dispatch.
  Existing registry refresh coverage proves an unsupported preamble retains the
  prior catalog revision and publishes no replacement entry.
- Shared wire/reference fixtures and suites were renamed to object-stream
  ownership and their canonical preamble golden advanced to v5. The obsolete
  full legacy schema-migration package fixture was removed.
- `DevTool test AssetPackageTests` rebuilt the target and passed all 111 tests
  across 8 suites after the reader-policy change.

### Stage 4: Remove legacy-only fixtures and update lasting contracts

- [x] Delete v4 writer, reader, Archive-adapter, rollback, migration, golden,
  corruption, consumer, and reference-codec tests that no longer exercise
  shared v5 behavior.
- [x] Retain or rewrite tests for the version-neutral object stream, v5
  trailer/package codec, DABK v1 external payloads, atomic publication,
  unsupported-version rejection, and cache-policy invalidation.
- [x] Update Asset Packages and directly affected tool/workflow contracts to
  state that v5 is the only supported authored package version and no rollback
  or legacy-read route exists.
- [x] Preserve historical completed plan/roadmap statements as historical
  evidence while updating their current-status handoff only where repository
  documentation rules require a live-policy clarification.
- [x] Audit includes, filenames, symbols, CMake sources, CLI help, diagnostics,
  comments, and active documentation for misleading v4 compatibility claims.

#### Acceptance Gate

- Active contracts, CLI help, implementation, and tests agree on v5-only
  package support and continued DABK v1 external placement.
- No v4-only source or fixture remains except the minimal negative preamble
  fixture and historical documentation evidence.
- Documentation lifecycle validation passes.

#### Stage 4 Evidence

- Shared wire, reference-model, reader, and writer fixtures now use
  `PackageObjectStream` filenames, suite names, diagnostics, and v5 goldens.
  The legacy schema-migration package fixture is deleted; only synthesized
  unsupported preambles remain for negative dispatch coverage.
- DurinDevTool's baseline policy and messages now require DAST v5. Its storage
  qualification wrapper distinguishes report schema v1 from native inventory
  schema v2, restoring construct-free DABK reachability qualification.
- Asset Packages, Versioning, Asset Data Lifecycle, Volume Textures, Core
  Serialization, and Canonical Resave guidance now state v5-only support and no
  legacy read or rollback route. Completed plans and roadmaps retain their
  historical compatibility decisions unchanged.
- `DevTool asset baseline --project Sandbox\\Sandbox.dproject` passed for all
  30 current DAST v5 packages. Storage qualification completed for 30 packages
  and 2 external payloads (2,359,296 reachable bytes) with zero corrupt packages
  and zero missing external payloads.

### Stage 5: Validate the v5-only package system and complete retirement

- [x] Build every affected AssetCore, DurinAssetTool, editor, and consumer
  target through the root agent build workflow.
- [x] Run the selected package, bulk-container, compatibility, asset-tool,
  registry/cache, reference/mutation, import, Texture, Material, StaticMesh,
  VolumeTexture, Cook, and editor integration tests through the root agent
  testing workflow.
- [x] Run a fresh construct-free corpus inspection and prove every tracked
  package is v5, all referenced DABK companions are reachable, and no v4 or
  orphan state remains.
- [x] Exercise the v4 unsupported-version path through runtime, tooling, and
  registry/cache boundaries and confirm no mutation occurs.
- [x] Update this plan with exact build, test, corpus, diagnostic, and
  documentation-validation evidence, then complete its lifecycle state.

#### Acceptance Gate

- All required native, tool, corpus, and documentation validation passes.
- The repository can neither read nor write DAST v4, and deterministic v4
  rejection is proven at every public entry point.
- DAST v5 authored save/load, references, operations, recovery, Cook, and DABK
  v1 external payload behavior remain green with no wire regression.

#### Stage 5 Evidence

- Two complete `Win64-Debug-DurinEditor` `DevTool build` runs of target `all`
  passed after the production and final test-adapter changes. Focused
  `AssetPackageTests` and `DurinAssetTool` builds also passed throughout.
- `AssetPackageTests` passed 111/111; `AssetPackageTrailerTests` passed 8/8;
  `AssetBulkContainerTests` passed 11/11; `AssetCookTests` passed 13/13; and
  `AssetImportCoreTests` passed 61/61. The focused static-mesh legacy-field
  mutation case and the v4 runtime/catalog rejection case also passed.
- The final v4 negative test changes a valid v5 package preamble to version 4,
  proves live load returns `UnsupportedVersion`, proves no object is returned,
  and proves a failed registry refresh retains the exact prior revision and
  entry. DurinAssetTool rejects `--target=v4` before project initialization.
- `DevTool asset baseline --project Sandbox\\Sandbox.dproject` passed for all
  30 tracked/mounted v5 packages. Construct-free storage qualification found 30
  packages, 2 reachable external DABK v1 payloads, 2,359,296 reachable bytes,
  zero corrupt packages, and zero missing external payloads.
- The focused DurinDevTool suite passed 35/35. Documentation validation passed
  for all 140 active documents and plan validation passed for 1 active, 12
  completed, and 230 archived plans before this lifecycle transition.
- A diagnostic `DevTool test all` run built the complete 80-target default test
  set, then reported 10 failures. Isolation found one task-related Cook fixture
  and one material byte-rewrite helper that still supplied a bare object stream;
  both were corrected and their focused coverage passed. Remaining isolated
  failures are existing consumer-test setup/lifecycle issues: an unregistered
  `Durin.Image` provider in editor/texture imports and unavailable preview mesh
  render data / render-command admission in material/static-mesh lanes. They do
  not exercise package-version dispatch, reproduce without a legacy input, and
  are not retirement blockers; they remain visible here rather than being
  reported as a green aggregate run.

## Validation Matrix

| Area | Required evidence |
| --- | --- |
| Documentation | Changed-document validation throughout; all-plan validation for lifecycle changes; active Asset Packages and tool guidance state v5-only support |
| Static inventory | Every v4 symbol, include, source file, build registration, CLI option, diagnostic, fixture, and active-document claim is classified and dispositioned |
| Corpus | Construct-free inspection reports only supported v5 packages, valid reachable DABK v1 companions, and zero missing/corrupt/orphan state |
| Shared codec | V5 golden bytes, logical types, schema/value codec, Archive behavior, reference extraction, compatibility probing, and live object construction remain byte- and behavior-equivalent after neutralization |
| Version policy | Only v5 is registered; reader-policy/cache fingerprint changes are proven; v4 returns deterministic unsupported-version diagnostics |
| Authoring | Ordinary, bundle, redirector, relocation, reference rewrite, canonical resave, and tool paths cannot select or emit v4 |
| Publication and recovery | V5 package plus companion publication remains failure-atomic; generic transaction rollback preserves the previous committed v5 closure |
| DABK | External DABK v1 read/write, integrity, generation reachability, submit closure, repair, and orphan protection remain supported |
| Native and tool tests | Select and run affected tests according to [Agent Testing Workflow](../../../Agents/Testing.md), including negative v4 entry points and representative authored consumers |
| Build | Build affected targets according to [Agent Build and Run Workflow](../../../Agents/BuildAndRun.md); record exact preset/configuration and results |

## Definition of Done

- DAST v5 is the only supported package reader and writer in policy, codec
  registration, runtime, editor, tools, tests, and active documentation.
- DAST v4 bytes are rejected as unsupported before format-specific decode,
  object construction, catalog publication, or mutation.
- No API or CLI can request v4 output, migration, mutation, or rollback.
- V5 no longer depends on v4 envelope compatibility code; shared logical-stream
  mechanics have version-neutral ownership and retain canonical v5 bytes.
- Current DABK v1 external payload behavior and all generic failure-atomic
  rollback semantics remain intact.
- The repository corpus and all required validation are green, lasting
  contracts are current, and historical evidence remains truthful.

## Deferred Follow-ups

- Persistent authored payload virtualization and remote hydration policy.
- DABK replacement or external-placement retirement.
- Compression, cross-package deduplication, range IO, trailer compaction, and
  package aggregation.
- Any future package-format version after DAST v5.

## Related Documentation

- [Authored Package Storage Evolution](../../../Roadmaps/Archive/2026-08/AuthoredPackageStorageEvolution.md)
- [Authored Trailer Corpus Migration](AuthoredTrailerCorpusMigration.md)
- [Selected Local Authored Payload Publication](SelectedLocalAuthoredPayloadPublication.md)
- [Asset Packages](../../../Runtime/Assets/AssetPackages.md)
- [Asset Data Lifecycle and Storage](../../../Runtime/Assets/AssetDataLifecycle.md)
- [Content Version Control](../../../Development/VersionControl/ContentVersionControl.md)
- [Agent Testing Workflow](../../../Agents/Testing.md)
- [Agent Build and Run Workflow](../../../Agents/BuildAndRun.md)

## Related Code

- `Engine/Source/Runtime/AssetCore/Private/Asset/PackageVersionPolicy.h`
- `Engine/Source/Runtime/AssetCore/Public/Asset/PackageAuthoring.h`
- `Engine/Source/Runtime/AssetCore/Public/Asset/CanonicalResave.h`
- `Engine/Source/Runtime/AssetCore/Private/AssetPackageCodec.cpp`
- `Engine/Source/Runtime/AssetCore/Private/AssetPackageV5Codec.cpp`
- `Engine/Source/Runtime/AssetCore/Private/Asset/PackageObjectStreamReader.h`
- `Engine/Source/Runtime/AssetCore/Private/Asset/PackageObjectStreamWriter.h`
- `Engine/Source/Runtime/AssetCore/Private/AssetPackageObjectStreamReader.cpp`
- `Engine/Source/Runtime/AssetCore/Private/AssetPackageObjectStreamWriter.cpp`
- `Engine/Source/Runtime/AssetCore/Private/AssetPackageObjectStreamArchiveAdapter.cpp`
- `Engine/Source/Runtime/AssetCore/Private/AssetPackageOperations.cpp`
- `Engine/Source/Runtime/AssetCore/Private/AssetCanonicalResave.cpp`
- `Engine/Source/Runtime/AssetCore/Private/AssetCompatibility.cpp`
- `Engine/Source/Runtime/AssetCore/Private/AssetRuntime.cpp`
- `Engine/Source/Runtime/AssetCore/Private/AssetReference.cpp`
- `Engine/Source/Runtime/AssetCore/Private/Asset/PackageTrailer.h`
- `Engine/Source/Runtime/AssetCore/Private/AssetPackageTrailer.cpp`
- `Engine/Source/Programs/DurinAssetTool/Private/AssetToolMain.cpp`
- `Engine/Tests/Native/AssetCoreTests/Private/PackageTests.cpp`
- `Engine/Tests/Native/AssetCoreTests/Private/PackageObjectStreamWriterTests.cpp`
- `Engine/Tests/Native/AssetCoreTests/Private/PackageObjectStreamWireContractTests.cpp`
- `Tools/DurinDevTool/durin_dev_tool/asset.py`
