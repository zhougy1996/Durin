# Rendering Structured Diagnostics Plan

Summary: Complete structured error propagation through renderer resource creation and RDG, building on the migrated Material, RHI, and Shader contracts.

Last reviewed: 2026-09-17

Status: Active
Completed:

## Current Status

Shader migration is complete. Remaining implementation starts at Stage 1;
Stage 1 first simplifies Shader error construction before extending the pattern.
The working baseline contains:

- `6a42c408a`: structured ShaderMap, material shader map, reflection, and payload errors.
- `b4bb1e8d3`: typed compiler, provider, cooked request, and library failures,
  with shared consumers and tests migrated.
- Validation on 2026-09-17: workspace `all` build passed; 58/58 affected
  test targets and 6/6 shader-domain targets passed; documentation validation passed.
  Logs under `Build/.agent-state/logs`:
  `20260917-204324-408502-5132-cmake.log`,
  `20260917-204130-264308-28036-ctest.log`, and
  `20260917-204359-414393-33328-ctest.log`.

Verified remaining seams:

| Owner | Current transport | Required change |
| --- | --- | --- |
| RenderCore `RenderResourceCreation.h` | `FRenderResourceCreateError.Message`; `GetFingerprint()` hashes text | Structured causes and semantic fingerprinting |
| Renderer resource producers and `RendererResourceDiagnostics` | Helpers accept prose and reporters read `Message` | Preserve causes until logging or UI presentation |
| RenderCore `RDG.h` / `RDG.cpp` | `FRDGResult` combines `ERDGError` with `Message` | Structured reasons and graph context under the existing result semantics |
| `FRDGAllocator` / `RendererRDGAllocator` | `Allocate` returns `bool` with `std::string& OutError` | Typed allocation results with lower-layer causes |

## Goal

Engine-owned rendering failures carry stable codes and owned context through
validation, allocation, creation, and recovery. Logs, assertions, console output,
and editor presentation format text at their boundaries. Callers and tests can
classify failures without parsing English diagnostics.

## Scope and Selected Decisions

- Keep domain-specific errors. Reuse the existing Material, RHI, and Shader
  contracts rather than introducing a universal error hierarchy.
- Typed errors must also simplify their call sites. Use small named construction
  helpers for recurring shapes, direct result forwarding, and ordinary control
  flow. Do not introduce propagation macros, generic builders, implicit success
  conversions, or a new result framework merely to shorten syntax.
- Preserve `FShaderError` and `FRHICreationError` as typed nested causes where
  resource creation or RDG allocation crosses those domains. Use explicit
  alternatives for mutually exclusive causes; avoid preformatted cause strings.
- Extend existing resource categories/reasons and RDG categories as needed.
  Success has one authoritative representation, independent of text or payload.
- Retain useful identities, pass/resource/parameter indices, ranges, expected
  and actual values. Stored diagnostics must own data that outlives temporary
  builders, allocator attempts, and compiler outputs.
- Preserve bounded opaque external diagnostics only at existing external
  adapters. They do not determine classification, retry eligibility, or deduplication.
- Resource failure fingerprints use selected semantic fields and nested cause
  identities, excluding formatted prose. Preserve generation gating, retry
  dependencies, retained fallback, failure suppression, and recovery notification.
- Preserve RDG declaration, compilation, scheduling, rollback, resource
  retirement, and GPU submission behavior. This is not a graph execution redesign.
- Keep existing presentation strings where practical, but semantic tests assert
  codes and context. Formatting tests own wording assertions.
- Exclude unrelated asset/Core error APIs, successful console messages, and
  general telemetry. Record any remaining string adapter with its owner and reason.

Required references:
[Shader diagnostics](../Runtime/Rendering/ShaderDiagnostics.md),
[RHI diagnostics](../Runtime/Rendering/RHIDiagnostics.md),
[Material diagnostics](../Runtime/Rendering/MaterialDiagnostics.md),
[Renderer resource recovery](../Runtime/Rendering/RendererResourceRecovery.md),
[Render Graph](../Runtime/Rendering/RenderGraph.md),
[build workflow](../Agents/BuildAndRun.md),
[test workflow](../Agents/Testing.md), and
[documentation workflow](../Agents/Documentation.md).

## Implementation Stages

### Stage 0: Establish the Shader baseline

Dependency: completed Material and RHI typed-error migrations.
Outcome: Shader operations supply typed causes for the remaining rendering layers.

- [x] Migrate ShaderMap, reflection, compiled payload, compiler, provider,
  dependency capture, and cooked library results.
- [x] Migrate shared consumers and document presentation boundaries.
- [x] Validate workspace build, affected consumers, and shader-domain tests;
  record commits and evidence in Current Status.

### Stage 1: Simplify Shader error construction and propagation

Dependency: Stage 0. Outcome: typed diagnostics remain useful while error
plumbing stops obscuring normal compilation and source-capture control flow.

The motivating case is `ShaderBuildModule.cpp` source capture: cancellation,
directory limits, and filesystem failures repeatedly expand nested
`FShaderOperationResult` / `FShaderError` initializers. Filesystem status checks
also use the mount root even when the failing operation concerns one entry.
Keep required error checks; simplify construction and improve context accuracy.

- [ ] Audit repeated error construction and propagation in ShaderBuild and
  RenderCore Shader code. Distinguish justified detail from wrapper-only syntax,
  overloaded generic fields, unused temporaries, and redundant copies.
- [ ] Introduce the smallest domain-local helper set supported by actual reuse:
  code-only failure, filesystem failure with path and native error, and repeated
  limit/context shapes. Prefer local helpers for capture-only cases and shared
  factories only when multiple owners need the same semantics. Do not create
  one factory per enum value or require one-off errors to use a builder.
- [ ] Reduce double-brace construction at routine failure sites and forward
  existing typed results directly. Use scoped result variables where inspection
  is needed; remove assignment-heavy propagation and misleading text-era names.
- [ ] Audit `FShaderError` payloads against callers, formatting, and tests.
  Retain fields that support classification, recovery, or actionable diagnosis;
  remove accidental duplication. Use meaningful context names for recurring
  shapes rather than growing an unstructured bag of optional fields.
- [ ] Replace repeated capture-budget literals with named constants. Preserve
  iterator-construction, increment, metadata-query, and post-loop error checks;
  report the actual failing entry when known without dereferencing an invalid
  iterator. Keep cancellation, limits, symlink policy, and output publication.
- [ ] Ensure formatting exposes retained useful context, including relevant
  paths and limits. Preserve bounded external diagnostics and native error codes.
- [ ] Add or adapt focused semantic tests where construction or context changes;
  review representative before/after capture and compile paths for readability.
  Pass Shader tests, affected consumers, the workspace `all` build when shared
  APIs change, and documentation validation; record evidence in Current Status.

Completion gate: routine failures read as short, explicit operations; typed
causes and useful context survive unchanged or become more accurate. No hidden
control flow or general error framework is introduced. Apply this construction
style to Stages 2 and 3 instead of repeating the verbose baseline pattern.

### Stage 2: Migrate renderer resource creation and recovery

Dependency: Stage 1. Outcome: resource creation preserves typed causes and
recovery behavior no longer depends on diagnostic wording.

- [ ] Inventory `FRenderResourceCreateError`, `MakeRendererResourceCreateError`,
  fingerprinting, and all producers/reporters across Engine, Sandbox, and
  RoadWeaver source and test roots declared by `Durin.dworkspace`.
- [ ] Record a mapping of existing failure sites to reason, owned context,
  nested cause, retry dependencies, and fingerprint fields before editing APIs.
- [ ] Replace resource `Message` storage and string-producing helpers, including
  GlobalShader and Renderer material/pipeline/resource adapters, with typed data.
- [ ] Add one resource formatter and migrate logs, assertions, console/UI
  adapters, and diagnostic callbacks without formatting during propagation.
- [ ] Test distinct semantic causes, wording-independent deduplication,
  unchanged-generation suppression, generation-triggered retry, retained
  fallback, recovery notification, and error lifetime after producer teardown.
- [ ] Update the resource recovery contract; pass the workspace `all` build,
  selected resource/recovery tests, affected consumer tests, and documentation
  validation. Record exact targets, results, and any skipped GPU coverage.

Completion gate: no engine-owned resource failure requires prose for identity,
classification, or propagation; typed Shader/RHI causes reach presentation intact.

### Stage 3: Migrate RDG validation and allocation results

Dependency: Stage 2. Outcome: RDG validation and allocator failures expose
actionable structured graph context without string error outputs.

- [ ] Inventory every `FRDGResult` construction/consumer and allocator
  implementation, including test allocators and graph diagnostic/capture adapters.
- [ ] Map existing failure sites to RDG category, specific reason, graph
  identities/indices, ranges, and expected/actual values; record the selected
  payload shape before changing the shared interface.
- [ ] Replace `FRDGResult.Message` with structured diagnostics while preserving
  existing success and graph-state semantics.
- [ ] Replace `FRDGAllocator::Allocate` and Renderer/test implementations'
  `bool` plus `OutError` transport. Retain typed resource/RHI causes and preserve
  failed-output, partial-allocation rollback, and retained-resource contracts.
- [ ] Format only at graph logging, assertion, capture presentation, and UI
  boundaries. Audit stored diagnostic ownership after graph reset/destruction.
- [ ] Test invalid declarations/metadata/dependencies, missing producers,
  safety limits, allocation failure, missing/incompatible allocations, and
  failure-path cleanup using codes and context rather than substrings.
- [ ] Update Render Graph contracts; pass the workspace `all` build, RDG and
  allocator tests, affected consumers, and relevant Vulkan integration coverage.

Completion gate: all allocator implementations and RDG callers compile against
typed results; rollback, graph execution, and resource lifetimes remain covered.

### Stage 4: Close cross-layer migration and document residual adapters

Dependency: Stages 1 through 3. Outcome: the rendering error path is consistently
typed and its remaining external/presentation boundaries are explicit.

- [ ] Search all project source/test roots for removed signatures, prose-based
  error decisions, early formatting, and lost nested causes. Classify residual
  `Message`/`OutError` matches by ownership instead of renaming unrelated fields.
- [ ] Verify representative Shader-to-resource and RHI-to-allocator-to-RDG
  failures preserve original codes/context through their final presentation.
- [ ] Complete final workspace `all` build and affected-target regression;
  document GPU environment and any unavailable coverage without marking it passed.
- [ ] Reconcile lasting contracts, run changed-document and all-plan validation,
  and record validation receipts and implementation commits in Current Status.
- [ ] Mark this plan Completed only when every required gate is satisfied.

Each implementation commit updates this plan's status/checklists and includes
the repository-required `Plan` and exact `Stage` trailers. If code inspection
changes a selected decision, update the decision and rationale before proceeding.
