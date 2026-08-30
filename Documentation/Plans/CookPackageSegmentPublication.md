# Cook Package Segment Publication Plan

Summary: Turn per-family Cook contributions into one deterministic project Cook schedule, immutable save-plan batch, incremental state, and transactional package-segment publication boundary.

Last reviewed: 2026-08-30

Status: Active
Completed:

## Current Status

The Package Bulk Data roadmap M1 and M2 exit gates have passed. Every supported
asset family can produce one DAST v7 cooked package whose large PlatformData
fields use an optional raw `.dbulk` segment, and cooked runtime load is lazy and
source/DDC independent. The checked-in corpus is also canonical DAST v7, so M3
does not need to preserve an authored migration workflow.

Cook is not yet a project operation. Callers construct `FCookContext`, invoke a
family-specific `AddToCook`, and call `Publish` directly. Reachability discovery
exists, but no production coordinator owns root selection, deterministic
loading/preparation, cancellation, incremental state, or a complete run report.
`FCookContext` also combines live-object capture, package-segment layout,
filesystem publication, CMNF generation, and stale cleanup in one mutable
object. Its manifest-last ordering is covered, but overwriting an existing
generation does not yet have a store transaction that can restore every prior
file after a mid-commit failure.

This plan selects a synchronous offline project Cook hosted by
`DurinAssetTool` and exposed as the top-level `DevTool cook` workflow. Engine
owns the reusable coordinator and narrow output-store contracts; the program
owns project/module bootstrap and process policy; DurinDevTool owns command-line
selection and stable human/JSON reporting. Initial scheduling may use bounded
parallel preparation only after family thread-safety is proven; deterministic
single-process execution is the required baseline.

## Goal

Given a project, explicit target/profile, output root, and optional explicit
roots, one command discovers the complete runtime package closure, prepares each
package through a registered family contributor, captures immutable cooked
package and raw-segment bytes, reuses valid unchanged results, and commits one
recoverable output generation. Success publishes a self-consistent
`CookManifest.bin` last; failure or cancellation leaves the previous valid
generation authoritative and returns package-qualified diagnostics.

## Scope

- Project-level Cook root selection, reachability, deterministic scheduling,
  package loading, preparation, capture, cancellation, and reporting.
- A class-keyed Cook contributor registry for family readiness/build policy
  without adding Cook virtuals to `DObject` or teaching the coordinator family
  schemas.
- Immutable per-package save plans containing canonical DAST bytes, optional
  raw segment bytes, integrity facts, target/profile, and build provenance.
- A narrow transactional output-store API and a local loose-file
  implementation with validation, rollback, manifest-last commit, and
  manifest-owned stale cleanup.
- Editor-only incremental Cook state distinct from runtime CMNF, including
  input fingerprints, output digests, producer revisions, and Cook-hit reasons.
- A native `DurinAssetTool cook` operation and top-level `DevTool cook` command
  with stable JSON and human result contracts.
- Integration of ordinary reflected packages and Terrain World opaque raw
  package contributions through the same run transaction.

## Non-Goals

- Changing family PlatformData schemas, DDC envelopes, DAST v7 field metadata,
  raw `.dbulk` layout, package-resource loading, or authored save behavior.
- Removing DAST v6/DABK or DBLK compatibility readers and fixtures; the M4
  legacy-retirement plan owns those deletions.
- Cook On The Fly, distributed Cook, remote stores, archive/install-chunk
  layout, patch generation, packaging/signing, or deployment.
- Selecting optional, memory-mapped, virtualized, compressed, or deduplicated
  package storage; those remain evidence-gated in M5.
- Making CMNF an incremental build database or exposing editor-only Cook state
  to runtime.
- Guaranteeing parallel family preparation before ownership and determinism
  tests establish that it is safe.

## Selected Decisions and Invariants

- `FCookCoordinator` in Engine owns one run from immutable request capture to a
  terminal result. It receives already-bootstrapped project services and never
  initializes an editor loop or UI.
- `DurinAssetTool` is the native process host because it already resolves a
  project, loads the Editor-capable module closure, scans assets, and supports
  bounded non-interactive execution. DurinDevTool exposes this as a distinct
  top-level `cook` command rather than expanding authored `asset` semantics.
- Explicit roots and registered `bCookRoot` reference stores feed the existing
  `BuildCookReachability` snapshot. Redirectors resolve to final identities and
  are never emitted. The normalized package list is sorted before any load or
  contribution.
- A contributor registry maps reflected asset classes to owner-gated
  preparation/capture adapters. Resolution is deterministic and inheritance
  aware; duplicate or ambiguous registrations fail before loading the first
  package. Metadata-only packages use an explicit generic contributor.
- Contributors may ensure derived PlatformData is current, but may not save
  authored packages, set package dirty state, rewrite reflected authored data,
  or publish files. They return a detached contribution or a qualified error.
- Capture produces an immutable `FCookSavePlan` per virtual package. A plan
  owns canonical package bytes, optional segment bytes, exact extent/digest,
  output identities, input fingerprint, producer/schema revisions, and build
  provenance. No live `DObject`, `DPackage`, DDC handle, or physical output path
  escapes capture.
- All plans are captured and validated before the output transaction begins.
  Layout and CMNF entry order are canonical by normalized virtual path; worker
  completion order cannot affect bytes, diagnostics, or publication order.
- `ICookOutputStore` exposes only bounded read/validate, transaction staging,
  commit, rollback, and manifest-owned retirement operations. The first
  implementation is a local loose store. Remote/archive capabilities are not
  anticipated through asset-family APIs.
- Publication stages every changed file, validates staged bytes, backs up or
  otherwise retains every overwritten prior-generation file, commits segments
  before referencing packages, and commits `CookManifest.bin` last. Any failure
  before manifest commit restores the prior manifest closure; cleanup after
  manifest commit removes only paths owned by the previous valid manifest.
- CMNF remains the runtime installation authority: target/profile, output kind,
  required flag, size, and digest. Versioned `CookState.bin` is editor/tool-only
  and records incremental inputs, producer revisions, output mapping, and
  provenance. Runtime never requires or reads it.
- A Cook hit requires matching target/profile, input fingerprint and producer
  revisions plus valid bytes in the selected output store. Missing, truncated,
  corrupt, or mismatched output downgrades to capture/publication rather than a
  hit. DDC hits and Cook hits are reported separately.
- Cancellation is checked between discovery, package preparation, capture,
  staging, and commit boundaries. Cancellation before commit publishes nothing;
  interruption during commit executes the same rollback protocol as an I/O
  failure and returns a deterministic terminal result.
- One invocation owns one project, target/profile, and output root. Concurrent
  writers to the same root are rejected with an ownership/lock diagnostic.

## Implementation Stages

### Stage 0: Freeze orchestration, state, and failure contracts

- [ ] Inventory every production and test-only `FCookContext`, `AddToCook`,
  `AddRawPackage`, `BuildCookReachability`, CMNF, stale-cleanup, and cooked-root
  consumer; classify family, generic-package, Terrain opaque-stream, and legacy
  DBLK callers.
- [ ] Specify the `FCookRequest`, `FCookRunResult`, package result, progress,
  cancellation, contributor, immutable save-plan, output-store transaction,
  CMNF, and incremental-state contracts without implementation-only pointers or
  paths crossing their owners.
- [ ] Freeze root precedence: explicit roots augment registered project/runtime
  roots; an empty final root set is an error; redirects are resolved once from
  the captured registry snapshot.
- [ ] Freeze input fingerprint components and versioning. Include canonical
  source package identity, dependency/reference closure facts, target/profile,
  contributor and family producer versions, and relevant project Cook settings;
  exclude timestamps, output paths, scheduling order, and DDC storage location.
- [ ] Define exact Cook-hit, DDC-hit, rebuilt, reused-output, failed, cancelled,
  and unsupported diagnostics and their stable JSON names.
- [ ] Add failure-injection seams for contributor preparation, package capture,
  store staging, each commit boundary, rollback, stale cleanup, cancellation,
  and competing-writer rejection.
- [ ] Record pre-change baseline coverage and output fixtures for ordinary
  package-only, package-plus-segment, Terrain opaque segment, redirect,
  dependency closure, and prior-manifest scenarios.

Completion condition: the public ownership and wire/state contracts are
reviewable in headers/tests, every current caller has a destination, and no
unresolved choice remains about host, roots, fingerprinting, transaction order,
or failure policy.

### Stage 1: Separate immutable Cook capture from publication

- [ ] Introduce owner-gated contributor registration and deterministic
  class/inheritance resolution with duplicate, ambiguity, owner retirement, and
  unsupported-class tests.
- [ ] Extract package serialization and raw-segment layout from the mutable
  publication loop into an immutable `FCookSavePlan` builder. Preserve DAST v7
  and raw `.dbulk` golden bytes.
- [ ] Adapt every supported Texture2D, TextureCube, VolumeTexture, StaticMesh,
  collision, SkeletalMesh, Skeleton, AnimationClip, TerrainHeightmap, Material,
  EnvironmentLighting, Level/metadata, and Terrain World contribution path.
- [ ] Prove contributors publish no files and do not change authored package
  bytes, dirty state, object topology, or reflected source values while
  preparing derived state and capture overrides.
- [ ] Retire public per-family orchestration entrypoints after all callers use
  the registry/save-plan boundary; retain only bounded compatibility helpers
  explicitly assigned to M4.
- [ ] Keep one canonical segment builder and package serializer for direct
  tests, project Cook, and future stores; reject duplicate virtual paths and
  target/profile disagreement before returning a batch.

Completion condition: a deterministic batch of detached save plans can be
created without a Cook root, and two captures of unchanged inputs produce
identical plans independent of load or completion order.

### Stage 2: Add project scheduling and the offline Cook command

- [ ] Implement `FCookCoordinator` discovery, sorted loading, contributor
  preparation, save-plan capture, bounded cancellation, and aggregate result
  flow over one registry/reference snapshot.
- [ ] Select project roots from explicit command arguments and registered Cook
  root stores, including the configured default Level route, then exercise
  missing roots, cycles, type mismatches, stale reference indices, duplicate
  output identities, redirect omission, and external module roots.
- [ ] Extend `DurinAssetTool` with a `cook` operation that bootstraps the
  required developer/editor modules without an application loop and accepts a
  project, output root, target/profile, repeated roots, incremental policy, and
  JSON output.
- [ ] Add top-level `DevTool cook` selection, runtime resolution, interruption,
  heartbeat, exit-code, human summary, and versioned JSON-contract handling.
  Preview/dry-run captures and reports plans but never opens a store
  transaction.
- [ ] Make diagnostics identify the requested root, final package, contributor,
  operation stage, target/profile, and causal error while preserving a stable
  machine-readable code.
- [ ] Start with deterministic serial preparation. Enable a bounded worker
  schedule only if race, module-owner, cancellation, and byte-equivalence tests
  prove it; otherwise record serial execution as the qualified M3 policy.

Completion condition: one non-interactive command can capture the complete
Sandbox runtime closure with stable ordering and reporting, and discovery or
capture failure produces no output mutation.

### Stage 3: Publish transactionally and add incremental Cook state

- [ ] Introduce `ICookOutputStore` and the local loose implementation without
  exposing store paths or handles to contributors, save overrides, PlatformData,
  `FBulkData`, or CMNF consumers.
- [ ] Stage and revalidate all changed segments, packages, CMNF, and Cook state;
  implement complete rollback for new and overwritten outputs at every injected
  commit boundary.
- [ ] Preserve segment-before-package-before-manifest ordering while making the
  prior valid manifest closure readable until the new manifest commit succeeds.
- [ ] Version and encode `CookState.bin` canonically. Reject unsupported or
  corrupt state as an incremental miss without weakening validation of an
  existing CMNF/output closure.
- [ ] Reuse unchanged valid outputs and report Cook hits without rewriting their
  timestamps or bytes. Distinguish capture requiring a DDC hit, capture requiring
  a rebuild, and publication-only replacement.
- [ ] Remove stale files only from the previous valid CMNF after successful new
  manifest publication; never delete unowned files, current shared outputs,
  rollback material, or a prior generation still needed for recovery.
- [ ] Enforce one writer per output root and prove retry after failure,
  cancellation, or process interruption converges to the same canonical result.

Completion condition: no injected failure or cancellation exposes a mixed
package/segment/manifest generation, unchanged second Cook is a validated Cook
hit, changed dependency recooks only its affected closure, and corrupt output is
repaired rather than reused.

### Stage 4: Integrate families and qualify the M3 boundary

- [ ] Run the project coordinator across every supported family plus package-only
  assets and Terrain World opaque regions; verify runtime loading uses only the
  published package/segment closure.
- [ ] Prove DDC hit, DDC miss/rebuild, Cook hit, selective invalidation, full
  recapture, dry-run, cancellation, competing writer, missing/corrupt output,
  rollback, retry, stale cleanup, and unsupported target/profile behavior.
- [ ] Record deterministic output hashes across repeated runs and configured
  worker counts, plus package count, changed/reused bytes, peak captured bytes,
  range count, build/cache provenance, wall time, and commit/rollback timings.
- [ ] Run focused `AssetCookTests`, `AssetPackageTests`, affected family tests,
  DurinDevTool command-contract tests, the registered `fast-all` matrix, and a
  bounded Sandbox command/application smoke following repository test guidance.
- [ ] Move lasting project Cook, save-plan, incremental-state, output-store,
  manifest, and failure rules into their owning Runtime/Development documents.
- [ ] Update the Package Bulk Data roadmap with M3 completion evidence and exact
  M4 entry inventory; create the M4 legacy-retirement plan only after every
  remaining compatibility reader, adapter, fixture, and Content Browser route
  has a named deletion test.

Completion condition: M3 exit gates and the acceptance matrix pass, the
project-level command is the only production orchestration path, lasting
contracts own the behavior, and the roadmap can advance to legacy retirement
without reopening family schemas or publication design.

## Acceptance Matrix

| Area | Required evidence |
| --- | --- |
| Discovery | Explicit/default/external roots, hard and typed soft references, deterministic redirect resolution, complete closure, stale index and missing/type/cycle failures |
| Contribution | Exact/derived class dispatch, owner retirement, unsupported class, every supported family, metadata-only package, Terrain opaque package, no authored mutation |
| Capture | Stable DAST v7 and `.dbulk` bytes, immutable plans, no physical path/DDC handle/live object escape, target/profile validation, duplicate rejection |
| Incremental | First Cook, clean Cook hit, DDC hit distinct from Cook hit, producer/settings/dependency invalidation, missing/corrupt output repair, unsupported state version |
| Transaction | Stage validation, competing writer, segment/package/manifest failure injection, rollback, retry, prior-generation retention, manifest-owned stale cleanup |
| Command | Project/output/target/profile/root validation, dry-run, JSON schema/order, human summary, progress heartbeat, cancellation and exit codes |
| Runtime | Package-only and external-field load, Terrain region load, lazy field reads, no source/DDC fallback, missing/truncated/mismatched segment failures |
| Scale | Deterministic hashes and reports, bounded captured memory and handles, changed/reused bytes, package throughput, shutdown conservation |

## Related Documentation

- [Package Bulk Data System Roadmap](../Roadmaps/PackageBulkDataSystem.md)
- [Asset Data Lifecycle and Storage](../Runtime/Assets/AssetDataLifecycle.md)
- [Package Bulk Data](../Runtime/Assets/BulkData.md)
- [Asset Packages](../Runtime/Assets/AssetPackages.md)
- [Workspace And Projects](../Workspace/WorkspaceProjects.md)
- [Build And Run](../Development/Build/BuildAndRun.md)
- [Agent Build and Run Workflow](../Agents/BuildAndRun.md)
- [Agent Testing Workflow](../Agents/Testing.md)

## Related Code

- `Engine/Source/Runtime/Engine/Public/Asset/Cook.h`
- `Engine/Source/Runtime/Engine/Public/Asset/References.h`
- `Engine/Source/Runtime/Engine/Private/Asset/CookedAsset.cpp`
- `Engine/Source/Runtime/Engine/Private/Asset/AssetPublicationCoordinator.cpp`
- `Engine/Source/Runtime/Engine/Private/Asset/AssetPackageObjectStreamArchiveAdapter.cpp`
- `Engine/Source/Programs/DurinAssetTool/Private/AssetToolMain.cpp`
- `Engine/Source/Developer/TerrainBuild/Private/Terrain/TerrainWorldCook.cpp`
- `Tools/DurinDevTool/durin_dev_tool/`
- `Tools/DurinDevTool/tests/`
- `Engine/Tests/Native/AssetCoreTests/Private/CookedAssetTests.cpp`
- `Engine/Tests/Native/AssetCoreTests/Private/PackageTests.cpp`
