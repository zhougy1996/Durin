# Modern Derived Data Cache Plan

Summary: Evolve Durin's structured cache toward independently stored values, selective reads, and asynchronous cache requests.

Last reviewed: 2026-10-11

Status: Active
Completed:

## Current Status

Stages 0 through 2 are complete. Stage 3 is next: selective value queries and a
Texture2D consumer. Independent compressed values and content-addressed blob
storage now use
`records/v3` manifests and globally shared `values/v1/Content` blobs. Full-record
queries remain synchronous and resolve every required value.
Conversion copies bounded descriptor tables but shares immutable payload and CB
allocations; it does not copy or rehash payload bytes.

Stage 2 validation on 2026-10-11: the final Win64-Debug-DurinEditor all build
passed, and `@module=derived-data-cache` passed all four ordinary targets:
DerivedDataBuildTests, DerivedDataCacheTests, RenderShaderBuilderTests, and
RenderShaderCacheTests. The final report is
`Build/NativeTestResults/Win64-Debug-DurinEditor/module-derived-data-cache.xml`.
TextureTests passed 140 cases and StaticMeshTests passed 169 cases. The existing
symlink cache test remains skipped when the host cannot create symlinks.
TextureCookIntegrationTests compiled with the migrated cold-cache fixture;
GPU execution was not run because storage changes do not alter GPU behavior.
Changed-document and all-plan validation passed.

Shared API and storage-layout consumers were searched across Engine, Sandbox,
and RoadWeaver source/test roots. Shader, Texture, StaticMesh, and collision test
fixtures now address the new manifest namespace; the latter two projects have
no direct consumers requiring migration. Initial affected execution identified
two stale Shader path assertions, corrected before final validation. A shared
test-support header causes `affected` to select all Engine targets, so the
bounded module selection plus Texture/StaticMesh targets supplies task-specific
coverage. Builds/tests use the host toolchain outside the sandbox because Stage 1
confirmed the sandbox's MSVC PDB service restriction.

## Goal

Align with modern UE DDC capabilities, without promising UE wire compatibility:
independent cache records, content-addressed compressed values, selective queries,
and bounded asynchronous I/O. Preserve the [build protocol](../Runtime/Assets/DerivedDataBuild.md)
ownership boundaries and complete typed build results.

## Selected Decisions

- A cache value can be a descriptor without resident data. A build output must
  contain every value required by its complete output contract. Partial cache
  records must never become successful complete build outputs.
- Keep cold producer allocations raw and shared; persistence must not introduce
  a compulsory compress/decompress round trip into cold assembly.
- Preserve canonical action encoding, 12-byte value IDs, asset payload codecs,
  deterministic messages, and transient-log exclusion during the initial migration.
- Start with Zstd and XXH3-128. Stronger blob identity is a separate versioned
  decision before introducing untrusted/shared storage, not a global identity migration.
- Publish blobs before atomically publishing their manifest. Readers validate
  required references; missing required blobs fall through to the existing rebuild path.
- Separate storage-format versions from action and family semantic versions.
  Introduce new storage namespaces in Stage 2; old schema-2 cache entries may
  cold-invalidate without an online conversion service.
- Selection policy belongs to cache requests. Asset consumers determine required
  value IDs; DDC retains generic validation and no asset policy.
- Stage 2 selects retain-only orphan safety, with automatic reclamation disabled.
  Failure and concurrent-reader tests verify requests never delete content.
  A future collector must introduce and validate exclusive ownership or epochs/leases
  before any automatic deletion; reference scanning alone cannot cover the
  blob-before-manifest publication interval. This avoids introducing unrequested
  eviction policy into the storage migration.

## Implementation Stages

### Stage 0: Select boundaries and acceptance gates

- [x] Inspect current cache, codec, execution, and asset value consumers.
- [x] Select the record/value separation, complete-output invariant, and rollout order.
- [x] Validate this plan with the all-plan validator (7 active, 5 completed,
  and 402 archived plans on 2026-10-11).

### Stage 1: Separate cache records from complete build outputs

Depends on Stage 0. Outcome: cache records own immutable descriptor tables rather
than wrapping a build output, while existing full-record persistence remains usable.

- [x] Make value data presence explicit; support descriptor-only references,
  including zero-length values without conflating them with absent data.
- [x] Give cache records independent schema, value, metadata, and message state.
- [x] Support stripping resident value data without modifying the source record.
- [x] Reject descriptor-only records at complete-output conversion and schema-2 encoding.
- [x] Preserve shared payload allocations and avoid rehashing immutable values during conversion.
- [x] Add focused reference, conversion, lifetime, and incomplete-record tests,
  including execution fallback and replacement of an incomplete cache record.
- [x] Update the implemented build protocol and pass relevant native tests and an all build.

### Stage 2: Store independent compressed values and manifests

Depends on Stage 1. Outcome: action-key manifests reference reusable content blobs.

- [x] Add an immutable compressed-buffer codec with explicit raw identity, size,
  codec/version, bounded validation, and a raw fallback for incompressible data.
- [x] Replace whole-record compression with versioned manifest and blob namespaces.
- [x] Implement atomic blob publication followed by manifest publication;
  handle concurrent same-content writers and corruption repair.
- [x] Preserve successful local results on persistence failures; bound total raw,
  encoded, and compression working-set memory separately.
- [x] Verify reuse across records, missing/damaged blobs, interrupted publication,
  empty values, concurrent writers, and full cold/warm output equivalence.
- [x] Specify and test retain-only orphan handling with automatic reclamation
  disabled; document collector prerequisites covering live manifests, active
  readers/writers, and publication races in the owning build protocol.
- [x] Pass the bounded affected-consumer selection and an all build; document the
  new storage contract. Tests additionally cover aggregate encoded-byte limits,
  retained backing-capacity limits, and failed replacement preserving a manifest.

### Stage 3: Add selective value queries and a Texture2D consumer

Depends on Stage 2. Outcome: selected mip queries avoid fetching unrelated blobs.

- [ ] Add metadata-only, selected-value, and full-record request modes with explicit
  required-value and missing-data results.
- [ ] Preserve full-output build-session behavior; introduce an asset-level path
  that explicitly requests and assembles a selected Texture2D mip set.
- [ ] Measure actual read bytes, decoded bytes, peak memory, and full-hit latency;
  verify a single-mip query never reads or inflates other mip payloads.
- [ ] Add independently compressed blocks and range reads only after value-level
  selection works; test offsets, bounds, block integrity, and cancellation.
- [ ] Extend selection to StaticMesh LOD/streams where a concrete consumer needs it;
  leave Shader and Physics rollout driven by actual requirements.
- [ ] Pass affected native tests and an all build; record comparative measurements.

### Stage 4: Introduce asynchronous cache I/O

Depends on Stage 3. Outcome: slow cache I/O does not occupy producer execution slots.

- [ ] Add batched asynchronous cache requests using request owners and Core scheduling.
- [ ] Convert build execution to continuations; define exactly-once completion,
  cancellation, priority, shutdown draining, and buffer lifetime.
- [ ] Coalesce blob fetches and bound I/O, compression, and decompression concurrency.
- [ ] Verify slow backend behavior, shutdown, cancellation, and local-result preservation.
- [ ] Pass affected native tests and an all build before adding remote backends.

### Stage 5: Complete structured metadata and identity evolution

Depends on Stage 2; may proceed alongside later consumer work after Stage 2 acceptance.

- [ ] Express manifests as CB objects with attachment references; specify the
  Durin format explicitly rather than claiming compatibility from matching API names.
- [ ] Migrate binary metadata wrappers to structured fields where consumers benefit.
- [ ] Separate deterministic assembly metadata from optional observational metadata.
- [ ] Decide whether shared/untrusted blob storage requires a stronger hash and
  introduce a versioned blob identity without silently changing action/source identity.
- [ ] Update lasting contracts, pass affected tests and an all build, and complete
  the plan only after every required gate above is satisfied.

## Validation and Handoff

Follow [Testing](../Agents/Testing.md), [Build and Run](../Agents/BuildAndRun.md),
and [Documentation](../Agents/Documentation.md). Validate changed documents and
all plans on creation/lifecycle changes. Each implementation commit updates this
plan with observed evidence and exact Plan/Stage trailers. Shared API changes
require consumer searches across every project in `Durin.dworkspace` and an all
build before handoff. Performance claims require measurements; no numerical
improvement is assumed in advance.
