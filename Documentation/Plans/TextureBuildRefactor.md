# TextureBuild Refactor Plan

Summary: Clarify texture recipe inputs, shared mip processing, and format encoding while preserving the TextureBuild module and existing output behavior.

Last reviewed: 2026-10-10

Status: Completed
Completed: 2026-10-10

## Current Status

Stages 0-4 are complete. At completion, the module remained named `TextureBuild`.
A subsequent rename changed the module and its interface to `TextureCompressor`
and `ITextureCompressorModule`; the decisions and validation below retain their
historical names. Shared mip
requests/results, private BC encoding, one metrics value type, and distinct
LDR/HDR Cube alternatives are implemented. Module registration, source/payload
representations, builder/projection versions, and DDC key definitions are unchanged.

Final validation on Win64-Debug-DurinEditor:

- TextureTests: 136/136 passed, including the new prepared-HDR alternative
  rejection case; it also passed alone after its target settings were corrected.
- TextureImportWorkflowTests: 23/23 passed.
- DerivedDataTextureQualificationTests: 5/5 passed after completing the private
  session dependency in the existing same-owner qualification seam.
- TextureCompressionQualificationTests: passed. All three output hashes and
  generated intermediate storage exactly match the baseline below. Final
  serial/parallel medians in milliseconds were Color 1712.28/684.528,
  Normal 7767.55/3073.18, and DataMask 5504.72/2169.9. Parallel changes were
  approximately +0.3%, +0.5%, and -2.2%; no material throughput regression was observed.
- The extracted BC encoder function matched the baseline token-for-token,
  ignoring whitespace and its enclosing namespace.
- Final `all` build passed for the workspace, including Engine, Sandbox,
  RoadWeaver, and StudioCubeGenerate. VulkanRHIIntegrationTests compiled the
  migrated VulkanTextureSamplingTests fixture successfully; GPU execution was
  not required because GPU behavior is unchanged.
- Changed-document and all-plan validation passed; final completion metadata is
  checked again before commit. Lasting recipe boundaries are documented in the
  [build protocol](../Runtime/Assets/DerivedDataBuild.md#family-boundaries).

Consumer inventory: TextureBuild's 2D and LDR Cube entrypoints, TextureTests,
TextureCompressionQualificationTests, and Vulkan texture sampling fixtures call
the shared mip recipe. Cube canonical values additionally cross Engine's build
adapter/resolver/application and AssetForgeBuiltins import validation. Searches
of Engine, Sandbox, and RoadWeaver found no other direct recipe consumers.

Frozen behavior: a single source mip generates to 1x1; supplied chains are kept
as supplied. Color selects BC1/BC3 using alpha across all supplied mips, normals
select BC5, and data masks select BC7. Cube uses one alpha decision for six faces.
MaxResolution selects a mip-aligned retained base. Filtering retains linear-light
sRGB handling, normal renormalization, and alpha coverage. Cancellation checks
remain at 64 blocks and 8 scanlines, predicates are serialized during compression,
and all tasks drain before borrowed storage is released. Validation precedes
cancellation. Failed recipes return no usable partial platform product.

Baseline: TextureTests passed 135/135 on Win64-Debug-DurinEditor.
TextureCompressionQualificationTests passed with generated intermediate storage
of 1,398,100 bytes for every usage. The deterministic 1024x1024 output hashes were
Color `0ad3be03b506f8e86ab773d0f0f042bc`, Normal
`05f144218335d14bde46d2eb51883234`, and DataMask
`323fd237eaaca1d8575aa82bf3c6fb1f`. Serial/parallel medians in milliseconds were
1903.32/682.666, 8446.77/3057.85, and 5908.09/2217.99 respectively.
The compression-cancellation fixture observes mip completion through timing;
retain that observation through an explicitly diagnostic metrics sink of the
same type as returned production metrics.

The initial sandbox build reported MSVC C1902 (PDB manager mismatch); rebuilding
through DevTool outside the sandbox passed. Qualification requires the explicit
`--mode qualification` selection on this checkout.

Validation adjustment: DerivedDataTextureQualificationTests directly compiles
the private Cube source entrypoint, but omitted its non-exported texture session
dependency and failed to link. Include that specific private implementation in
the same-owner qualification seam and update its rationale rather than exporting
a production symbol solely for the test. No production session policy changes.

## Goal

Make recipe ownership and data flow explicit without changing texture quality,
encoded bytes, cache identity, or asset publication behavior. Keep a small set
of typed family entrypoints and share algorithms where their semantics match.

## Scope and Selected Decisions

- Keep `TextureBuild`, its module registration, dependency direction, and
  editor-lifetime `ITextureBuildModule` contract. A future rename is separate work.
- Preserve the ownership boundary in the
  [derived data build protocol](../Runtime/Assets/DerivedDataBuild.md#ownership):
  Engine owns DDC, scheduling policy, input identity, codecs, and application;
  TextureBuild owns detached CPU recipes and uses the existing Core task system.
- Treat the module as a texture processing pipeline. Mip generation, Cube
  projection, HDR processing, and Volume recipes remain valid responsibilities;
  they do not need separate modules merely because they do not compress data.
- Keep 2D, LDR Cube, HDR Cube, and Volume family policies explicit. Share RGBA8
  mip processing and BC encoding between 2D and LDR Cube. Preserve HDR angular
  filtering and Volume voxel filtering as distinct algorithms.
- Replace the shared positional mip API with an internal request containing
  source mips, resolved mip settings, and a selected output pixel format.
  Resolve optional sRGB policy at the family entrypoint. Retain execution
  control separately from deterministic recipe settings.
- Select formats at family entrypoints. LDR Cube selects one format for all six
  faces; the shared processing layer no longer accepts `TransparencyOverride`.
  Retain a cancellable transparency-analysis helper where needed.
- Return the completed platform data and metrics through `std::expected`.
  Build candidates locally so failed operations cannot expose partial products.
  Preserve any required legacy output-parameter contract during consumer migration.
- Use one metrics value type instead of copying identical metric structures.
  Keep a diagnostic observer only where existing qualification needs in-progress
  facts; production cancellation control must not depend on elapsed-time metrics.
- Separate BC encoder implementation from mip filtering through private functions
  and files. Do not add a codec registry, virtual encoder hierarchy, worker pool,
  or independently loadable format modules for the current single backend.
- Represent LDR-face and HDR-panorama Cube recipe inputs as distinct variant
  alternatives. Normalization output must also expose an unambiguous alternative
  while retaining authoring metadata required by Engine application and PostLoad.
- Consolidate duplicated validation within recipe layers, but retain validation
  at untrusted input, build-service, and module-output boundaries. Private helper
  preconditions must be explicit; removing checks must not change error precedence.

## Implementation Stages

### Stage 0: Freeze Behavior and Consumer Inventory

Dependency: none.

- [x] Inventory recipe APIs and direct helper consumers in the source and test
  roots of Engine, Sandbox, and RoadWeaver declared by `Durin.dworkspace`.
- [x] Record current single-source mip generation versus supplied-chain behavior,
  including that supplied chains are retained rather than automatically completed.
- [x] Record format selection, whole-Cube alpha handling, MaxResolution selection,
  sRGB filtering, normal normalization, and alpha-coverage behavior.
- [x] Record failure-output guarantees, validation-before-cancellation precedence,
  callback serialization, task draining, and frozen cancellation checkpoint intervals.
- [x] Identify existing CPU correctness and compression qualification coverage;
  add characterization only for behavior needed by this refactor and not covered.
- [x] Confirm how diagnostic tests observe metrics while work is running and
  select the minimal replacement for those observations before changing the API.

Completion: a bounded consumer and invariant inventory is recorded here; relevant
existing baseline tests pass or pre-existing failures are documented separately.

### Stage 1: Introduce Explicit Shared Recipe Inputs and Results

Dependency: Stage 0.

- [x] Introduce the internal resolved mip request and expected result; avoid
  introducing a second complete copy of public family settings.
- [x] Move 2D and LDR Cube format selection into their entrypoints and replace
  the transparency override with an explicit selected format.
- [x] Use local candidate output and migrate direct consumers, including native
  tests and Vulkan sampling fixtures; remove the positional API after migration.
- [x] Consolidate metrics and control adaptation while preserving required
  diagnostic observations, cancellation cadence, and failure classification.
- [x] Remove repeated validation only where a validated private call path and
  explicit preconditions replace it; keep public entrypoints safe to invoke directly.

Completion: all discovered consumers compile; affected correctness tests pass;
serial and parallel output bytes, mip layouts, and failure guarantees match baseline.

### Stage 2: Separate Mip Processing from BC Encoding

Dependency: Stage 1.

- [x] Extract BC initialization, quality parameters, block gathering, and row
  encoding into a private encoder implementation with a narrow image-view API.
- [x] Keep mip filtering, normal handling, and alpha coverage together in the
  processing layer; make the top-level recipe read as a short sequence of stages.
- [x] Keep bounded Core task parallelism and serialized cancellation predicates
  intact; drain compression work before returning or releasing borrowed storage.
- [x] Retain shared immutable source storage and avoid new source-buffer copies.
- [x] Keep Operations entrypoints where they own target validation or family
  policy; collapse forwarding code and duplicate adapters that add no contract.

Completion: encoded bytes remain identical across supported usage, quality, and
alpha combinations; existing cancellation and source-immutability tests pass.
Qualification shows no unexplained throughput or intermediate-memory regression.

### Stage 3: Make Cube Input Alternatives Explicit

Dependency: Stage 2.

- [x] Replace the required Faces plus optional HDR pointer recipe input with
  alternatives that contain only the data required by each path.
- [x] Make normalization results expose their LDR/HDR alternative explicitly,
  retaining source layout, dimensions, exposure, and authored panorama metadata.
- [x] Migrate module contracts, Engine build adapters, result application,
  PostLoad paths, and all discovered test doubles and consumers.
- [x] Preserve source representations, serialized payloads, build constants,
  projection versions, builder versions, and DDC identity.
- [x] Verify six-face consistency, LDR panorama projection, HDR exposure and
  radiance limits, angular mip behavior, and normalization failure handling.

Completion: invalid cross-path states are excluded by the recipe types; existing
Cube integration and cache tests pass; a shared Engine API migration receives an
`all` build and validation of affected project targets before handoff.

### Stage 4: Final Validation and Contract Handoff

Dependency: Stages 1-3.

- [x] Validate affected CPU recipe, Cube, Volume, and DDC integration coverage
  using the registered selections described in [Testing](../Agents/Testing.md).
- [x] Compile changed Vulkan sampling consumers; GPU execution is required only
  if GPU behavior changes, following the testing workflow.
- [x] Follow [Build And Run](../Agents/BuildAndRun.md), validate affected project
  targets, and complete the required `all` build for shared Engine API changes.
- [x] Review deterministic outputs and key identities against Stage 0 evidence.
  Do not bump recipe versions for an output-preserving structural refactor.
  Any intended output change requires a recorded plan revision and invalidation policy.
- [x] Update lasting ownership or recipe contracts in their authoritative
  documentation, including [Volume textures](../Runtime/Assets/VolumeTextures.md)
  only where its implemented contract changes.
- [x] Record validation evidence and update lifecycle metadata/checklists through
  completion, following [plan lifecycle rules](AGENTS.md#status-maintenance).

Completion: all required gates pass, final evidence is recorded, and no obsolete
helper API, duplicated metrics type, or contradictory Cube input state remains.

## Deferred Work

- Streaming mip generation/compression with only current and next levels retained.
  This is a separate memory optimization because it changes timing observations,
  peak-memory accounting, cancellation sequencing, and allocation lifetimes.
- New codecs, HDR compression, quality changes, seam filtering changes, and
  correction of existing filtering behavior are separate feature or bug-fix work.
- Renaming the module to `TextureCompressor` is outside this plan.
