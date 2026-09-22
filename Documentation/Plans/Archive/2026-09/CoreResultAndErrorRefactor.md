# Core Result and Error Refactor Plan

Summary: Simplify CoreDObject and Core operation results and preserve typed errors until presentation boundaries.

Last reviewed: 2026-09-22

Status: Archived
Completed: 2026-09-22

## Current Status

All stages are complete. CoreDObject and Core now use the selected expected
contracts with operation-scoped errors and formatter implementations in their
owning `.cpp` files. The final source/test audit covered Engine, Sandbox and
RoadWeaver and found no stale selected wrappers or formatter names. Existing
JSON/YAML expected APIs, stateful archive failure storage, startup configuration
text adapters, and package effect reports remain intentional exceptions.

Final validation reuses the passing batch evidence below. The latest all build
passed after the image changes; SceneImportTests passed on the final code.
MaterialQualificationTests and BinaryEnvelopeQualificationTests compiled;
GPU execution and timing qualification were not needed for this API migration.
Changed-document validation and all-plan lifecycle validation passed. The
implementation batches were committed with this plan's exact stage provenance.

Stage 1 is complete. All 13 selected error-only result wrappers are removed,
including callbacks, fixtures, and Engine/editor/RoadWeaver consumers. Formatter
implementations remain in their owning `.cpp` files. The inventory covered all
six workspace source/test roots. Existing output, partial-mutation, rollback,
and validation-hook semantics are preserved. Error payload sentinels remain
compatible with embedded error construction; expected success never reads them.
Scalar metadata validation returns success explicitly; admission failures
construct unexpected values before enriching context.

Validation: the final Win64-Debug-DurinEditor `all` build passed. The registry
selection `@domain=reflection+property-editor+asset-package+road-scene+material-runtime+material-function+material-compiler+world`
passed all 18 targets, and `@domain=editor-shell+spline` passed all 5 targets
(22 distinct targets total). These include every changed native-test target.
JUnit reports are under `Build/NativeTestResults/Win64-Debug-DurinEditor/`.
The initial narrow linker check also passed all 20 cases. The final source/test
scan found no selected wrapper or legacy formatter names. Changed-document and
all-plan validators passed. Initial compilation found legacy aggregate
initializers and ambiguous same-name methods; the final batch corrected them
without changing those unrelated result contracts.

Stage 2 is complete. Path and soft-object errors now belong to AssetPath.h
and SoftObjectPtr.h, with formatters in their matching `.cpp` files. Soft-object
admission retains an invalid-path cause only when path construction fails.
Graph saves return expected void; loads and duplication return expected object
pointers. Error-only consumers and failed-load assertions are migrated together.
Snapshots, graph operations, overrides, capture and replacement retain bounded
operation-specific causes instead of composing internal strings. The legacy
FArchive string diagnostic projection remains an explicit adapter; downstream
typed results retain its structured cause when available and copy opaque text
only when there is no typed cause.

Stage 2 validation: the final `all` build and MaterialQualificationTests compile passed.
The recorded 31-target contract/feature batch and sky-box, volumetric-cloud,
asset-reference, asset-workflow and texture supplement covered all migrated
consumers. SceneImportTests passed all 10 integration cases. Supplementary
coverage exposed six success-path error reads in package byte decoding; these
were guarded and EditorAssetWorkflowTests and TextureTests then passed.
A content-browser asynchronous start gate timed out once in the supplementary
batch; its isolated case passed, as did the preceding whole-target run.
No selected stale API names remain in workspace source/test roots.

Stage 3 is complete. Mounted path values and errors are
separate; failure reports retain discovered mount identity and native errors.
Envelope operations use expected void while preserving success-only writes to
caller outputs. Redundant decode categories are removed in favor of archive
failures. Persistence reports retain typed file/capture causes and their
external diagnostic snapshot. Startup mount configuration remains an explicit
text adapter: inspection found no machine-classification consumer requiring a
new registration error domain. Source/test inventory covered all six roots.
The final all build passed. All 35 targets passed in the recorded batch
`@kind=contract+feature,domain=core+reflection+asset-package+asset-workflow+texture+static-mesh+material`.
These checks cover core, reflection, asset-package, asset-workflow,
texture, static-mesh, and material consumers; the broad affected resolver
selects all because a shared legacy header was removed, so bounded semantic
coverage is used instead of unrelated GPU/integration execution.

Stage 4 is complete. Grayscale PNG and Radiance HDR now return owning
expected image values, with separate error enums/context and local ToString
overloads. HDR scanline helpers propagate typed packet errors and parent parse
offsets. File overloads retain native causes. Existing HDR fixture assertions
now inspect codes instead of matching strings; new grayscale fixtures verify
exact 16-bit samples and rejection before publication. All six source/test
roots were inventoried. Validation selects image-codec, texture, thumbnail,
asset-workflow, core, and the bounded scene import integration target.
The final all build passed; all 15 targets in
`@kind=contract+feature,domain=image-codec+texture+thumbnail+asset-workflow+core`
passed. SceneImportTests passed all 10 integration cases.

## Goal

Apply the operation-boundary principles of [RHI diagnostics](../../../Runtime/Rendering/RHICommandExecution.md#results-and-diagnostics)
without copying its implementation layout. Declare errors and `ToString`
overloads near their owning interfaces and implement them in the respective
existing `.cpp` files. Do not introduce module-wide error string files.

Use `std::expected<T, E>` directly for fallible operations and
`std::expected<void, E>` when there is no success value. Keep errors scoped to
the operation and retain only context required by consumers. Format internal
errors at logs, assertions, UI, or explicit external adapters. Opaque parser,
callback, and exception diagnostics may retain owned text.

Preserve publication behavior, partial mutation, archive first-failure state,
recovery classifications, ownership, and asynchronous lifetimes. Expected does
not imply rollback. Keep report types carrying meaningful observations on both
success and failure, including package commit/recovery effects and module
retirement evidence. Existing JSON/YAML expected APIs need no structural rewrite.

## Implementation Stages

### Stage 0: Establish migration boundaries

- [x] Inspect the RHI reference, selected Core/CoreDObject declarations, and
  workspace project manifests.
- [x] Record local formatter ownership and retained state/report exceptions.
- [x] Inventory producers and consumers in all Engine, Sandbox, and RoadWeaver
  source/test roots for each batch before changing its shared APIs.
- [x] Record output publication and embedded-error constraints for each batch;
  discover semantic test targets through the configured registry.

Completion: each implementation batch has an explicit success/error mapping
and selected validation before its API changes.

### Stage 1: Replace CoreDObject error-only wrappers

Dependency: Stage 0 checks for each selected batch.

- [x] Migrate canonical Map keys and package linker operations.
- [x] Migrate property value, reflected Map key, snapshot, copy, edit, and
  container results, preserving partial mutation and rollback details.
- [x] Migrate object validation, save overrides, package capture, and object
  replacement results, including callbacks and test helpers.
- [x] Use local `ToString` overloads for the migrated error types.

Completion: selected wrapper structs are removed, all consumers compile, and
semantic tests cover error codes/context and existing publication guarantees.

### Stage 2: Scope object errors and graph values

Dependency: Stage 1.

- [x] Separate object-path and soft-object operation errors where consumers
  need different domains; retain necessary path causes in archive adapters.
- [x] Replace graph pointer/error records with expected values and define
  save/load/duplication success payloads independently.
- [x] Replace internal formatted causes in capture, replacement, snapshot,
  overrides, and graph propagation with bounded typed context or deliberate
  operation-level classification. Avoid a universal error tree.

Completion: no selected internal propagation site relies on generated prose;
failed loads retire candidates and existing save/output guarantees remain tested.

### Stage 3: Refactor Core path and serialization results

Dependency: Stage 2.

- [x] Separate mount lookup/path/policy success data from error context and
  migrate registration diagnostics where callers need typed failures.
- [x] Replace redundant decode results and envelope bool/diagnostic interfaces
  with operation-specific expected contracts.
- [x] Preserve package writer/save effect reports; replace internally generated
  diagnostic prose with typed causes where useful to their consumers.
- [x] Keep formatter implementations in their existing owning `.cpp` files.

Completion: affected source and test consumers in every workspace project use
the new contracts; path/codec and persistence semantic tests pass.

### Stage 4: Complete image decoding consistency

Dependency: Stage 3.

- [x] Return grayscale PNG and Radiance HDR decoded values through expected,
  with errors retaining format-specific limits and file causes.
- [x] Migrate callers and local formatters; preserve image allocation limits
  and failure publication behavior.

Completion: ordinary, grayscale, and HDR decode contracts consistently separate
successful images from typed failures; relevant malformed-input tests pass.

### Stage 5: Validate and update contracts

Dependency: Stages 1–4. Each earlier batch must also pass its applicable gates.

- [x] Update implemented contracts in their owning Runtime documents, including
  [Serialization](../../../Runtime/Core/Serialization.md) and
  [Package persistence](../../../Runtime/Core/PackagePersistence.md).
- [x] Search all workspace source/test roots for stale selected API names,
  result-member access, and premature formatting.
- [x] Complete the shared Engine API `all` build and affected project targets.
- [x] Run the registry-selected affected semantic tests and record exact results.
- [x] Validate changed documentation and all plan lifecycle metadata.
- [x] Commit isolated validated batches with this plan and exact stage trailers.

Validation follows [Build and run](../../../Agents/BuildAndRun.md),
[Testing](../../../Agents/Testing.md), and [Documentation](../../../Agents/Documentation.md).
No GPU execution is required solely for these result-contract migrations.

Completion: all required checks pass, lasting contracts reflect the final APIs,
and this plan records completed status with validation evidence.
