# Material API Simplification Plan

Summary: Consolidate material graph command entry points and simplify synchronous compiler result and snapshot contracts.

Last reviewed: 2026-09-19

Status: Archived
Completed: 2026-09-19

## Current Status

All stages completed. Common graph commands now belong to Document, connection
consumers use pin addresses, synchronous results use explicit completed outcomes,
and compiler capture returns one owned optional snapshot with dependency stamps.
The obsolete facades and snapshot output-parameter overload are removed.

Validation on `Win64-Debug-DurinEditor`:

- Stage 1 and 2: the three `@material-editing` targets passed; Stage 2 also
  passed MaterialFunctionTests (28 cases). Connection regression assertions cover
  replacement/disconnect IDs, no-change, retained defaults and undo.
- Stage 3: `build --target all` and the five compiler/function/runtime/cook/
  compilation-lifecycle targets passed. New tests cover default failure, valid
  empty builds, explicit bool conversion and diagnostic/source independence.
- Stage 4: `build --target all` passed, including Sandbox, RoadWeaver and
  RoadWeaverEditor. All eight targets selected by
  `@domain=material-compiler+material-function+material-cook+material-runtime+material-compilation+material-editing`
  passed with XML report output. AssetPackageReloadTests (14 cases) and
  SceneImportTests (10 cases) passed. MaterialQualificationTests compiled;
  no GPU qualification or application smoke was required or run.
- Both new result/capture contract cases passed standalone through `--isolate`.
  Snapshot coverage includes absent root, failure without payload, retry,
  detached input, instance resolution, function stamps and reload freshness.
- Lasting contracts are recorded in graph operations, material diagnostics and
  expression building. Changed-document and all-plan validation passed.

### Migration inventory and decisions

- Common graph consumers are MaterialEditor widgets, creation, canvas and native
  graph/editing/function/parameter-panel tests. Sandbox and RoadWeaver have no
  direct uses of the selected APIs. Catalog and parameter operations remain.
- Inspect, remove, move, layout calculation/application, copy, paste, duplicate
  and cut facades forward without changing results. MoveMaterialOutput resolves
  the terminal ID and moves it; retain that convenience on Document.
- Connection facades overwrite affected IDs: connect reports source and target,
  disconnect reports target; surface assignment reports source and surface
  disconnect reports the previous source. Document will own affected endpoint
  reporting, including the actual output terminal. Generated IDs remain reserved
  for creation/paste; parameter IDs continue to come from committed deltas.
- Engine producers/consumers include expression building and validation,
  normalization, generation, MaterialInterface, reachability, compile lifecycle,
  cooked-program decoding, AssetForgeBuiltins, renderer and material/function/
  compiler/lifecycle/scene-import/package-reload tests. Qualification consumers
  require compilation only; GPU behavior is unchanged.
- Synchronous results use the existing concrete `bSucceeded` convention with
  explicit bool conversion. Default construction is unsuccessful; producers set
  success only after completing their stage. Diagnostics and source emptiness
  never define bool conversion. Payload is consumable only on success; failed
  building clears emitted payload, while normalization/compiler failures may
  retain diagnostic context and generated source. No new result framework or
  severity taxonomy is introduced. Valid empty graph builds remain successful.
- Snapshot capture will return an optional owned payload (input plus function
  stamps) and diagnostics. Payload presence is its sole outcome, so capture
  failure cannot expose a usable input independently of its status.

## Goal

Reduce the number of public concepts a material-system caller must understand:
one owner for common graph commands, one pin-address vocabulary for connections,
consistent synchronous result semantics, and one owned result for compiler input
capture. Prefer deleting forwarding layers over adding abstractions.

## Scope and Decisions

- `FMaterialGraphDocument` owns common material/function graph mutations.
  Retire equivalent `FMaterialGraphOperations` forwarding methods after migrating
  consumers. Keep parameter-specific operations and catalog queries where they
  have independent responsibilities; do not move everything into a larger class.
- Connection APIs use `FMaterialGraphPinAddress` for both endpoints. Named address
  constructors may improve readability without adding parallel command APIs.
- Each synchronous compiler stage keeps its own payload type but follows one
  success/failure convention. Diagnostics explain outcomes; text and payload
  emptiness must not independently define success. Do not add a general result
  framework, new error hierarchy, or diagnostic severity model for this work.
- Compiler snapshots return their input, function-owner stamps and validation
  outcome together. Prioritize this multi-output API; simple lookup output
  parameters are outside scope.
- Preserve Engine typed errors, node/port/function-call diagnostic locations,
  transaction rollback, undo/redo, gesture cancellation and retry behavior.
  Asynchronous cancellation, supersession and result admission remain separate
  lifecycle concepts. Preserve shader identities, cooked formats and cache keys.

## Implementation Stages

### Stage 0: Establish the migration contract

- [x] Record the selected scope, ordering and exclusions.
- [x] Audit affected symbols in source and test roots of every project declared
  by `Durin.dworkspace`; identify runtime, editor, cook and test consumers.
- [x] Record which facade methods are pure forwarding and which change output
  semantics, especially affected/generated node and parameter IDs.
- [x] Decide the minimal concrete synchronous result representation after checking
  default construction, failure publication and diagnostic consumers. Record the
  decision here before Stage 3; avoid publicly contradictory success/payload states.

Acceptance: a bounded migration inventory and explicit result invariants exist;
no implementation decision depends on assumptions about unsearched consumers.

### Stage 1: Consolidate graph command ownership

Depends on Stage 0.

- [x] Route common inspect, remove, move, layout, copy, paste, cut and duplicate
  operations through `FMaterialGraphDocument`.
- [x] Generate command status and changed IDs at the mutation owner. Resolve facade
  differences explicitly, preserving selection and refresh behavior for consumers.
- [x] Migrate callers and delete redundant facade declarations/definitions. Keep
  convenience methods only where they add a documented caller-facing operation.
- [x] Update graph operation documentation and regression coverage for no-change,
  changed IDs, rejected edits and undo/redo.

Acceptance: common operations have one implementation and one public owner;
material-editing regression targets and affected consumers compile and pass.

### Stage 2: Unify graph connection interfaces

Depends on Stage 1.

- [x] Use `Connect(Target, Source, ...)` and `Disconnect(Target, ...)` as the common
  connection boundary; migrate ordinary inputs, function ports and surface inputs.
- [x] Supply clear pin-address constructors where needed and retire equivalent
  public `ConnectInput`, call-input and surface-assignment forwarding APIs.
- [x] Keep semantic operations such as changing material-output mode distinct from
  connecting a pin. Preserve output-mode behavior and stable function-port GUIDs.
- [x] Validate occupied-input replacement, invalid selectors, disconnect defaults,
  aggregate/per-property outputs, function pins, changed IDs and undo/redo.

Acceptance: callers use one endpoint vocabulary and command path without losing
the distinction between connection edits and material-output mode changes.

### Stage 3: Normalize synchronous compiler result semantics

Depends on Stage 0; execute after Stage 2 to keep migrations isolated.

- [x] Apply the selected convention to `MIR::FBuildResult`,
  `FMaterialProgramValidationResult`, `MIR::FNormalizationResult`,
  `FMaterialSourceGenerationResult` and `FMaterialCompilerResult`.
- [x] Make boolean conversion explicit and consistent. Establish what default
  construction means and ensure completed success/failure is constructed deliberately.
- [x] Retain stage-specific payloads and diagnostics. Preserve each stage's required
  failed-output publication behavior; do not infer success solely from an empty
  diagnostic list or nonempty generated source.
- [x] Migrate all producers and consumers, including lifecycle adapters and cooking.
- [x] Verify failure propagation, valid empty data where supported, retained
  diagnostics, compiler identity stability and rejection of unusable results.

Acceptance: synchronous stages share a documented outcome convention; affected
compiler/function/runtime/cook/lifecycle tests pass and an `all` build succeeds.

### Stage 4: Return an owned compiler snapshot

Depends on Stage 3.

- [x] Replace `SnapshotMaterialCompilerInput`'s `OutInput` and optional `OutOwners`
  with one result containing the detached input and dependency stamps on success.
- [x] Keep failure diagnostics in that result and prevent failed capture from
  publishing a usable snapshot. Preserve owning-thread and object-lifetime rules.
- [x] Migrate compilation, reachability, cook and test consumers; remove the old
  output-parameter overload once all callers have moved.
- [x] Validate capture failure, function dependency freshness, instance/root
  resolution and graph reachability behavior.

Acceptance: callers receive one snapshot outcome with no separately managed output
variables; affected tests pass and an `all` build succeeds.

## Validation and Handoff

Follow the [build](../../../Agents/BuildAndRun.md), [test](../../../Agents/Testing.md) and
[documentation](../../../Agents/Documentation.md) workflows. Use the registered
material-editing domain for editor stages and select affected native targets for
Engine stages. Shared Engine API migrations require an `all` build before handoff;
also validate any affected Sandbox and RoadWeaver targets found by the audit.
No GPU qualification or application smoke is implied by these API-only changes.

Commit each validated stage with this plan and the exact stage title as provenance.
Update checklists with evidence, document lasting contracts in
[graph operations](../../../Editor/Architecture/MaterialGraphOperations.md),
[material diagnostics](../../../Runtime/Rendering/MaterialSystem.md#results-and-diagnostics) and
[expression building](../../../Runtime/Rendering/MaterialExpressionBuilding.md) as relevant.
Do not mark later stages complete from compilation alone. Complete the plan only
after all migrations, behavioral gates and documentation validation pass.
