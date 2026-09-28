# Derived Data Outcome and Diagnostics Plan

Summary: Separate cancellation, build failures, cache outcomes and execution diagnostics, then simplify all derived-data callers without weakening validation or publication boundaries.

Last reviewed: 2026-09-28

Status: Active
Completed:

## Current Status

Stage 0 records the selected architecture from the code review and owner
discussion. Implementation has not started. No native build or test result is
claimed by this plan. Stages 1-5 remain open.

The existing [build protocol](../Runtime/Assets/DerivedDataBuild.md) remains the
implemented contract until the corresponding migration lands. This plan follows
the completed [build sessions plan](DerivedDataBuildSessions.md); it changes
outcome and diagnostic contracts rather than reintroducing scheduling or typed
products into DDC.

## Goal

Make request completion explicitly success, cancellation or failure. Use
`std::expected` for genuinely binary operations, preserve actionable failure
causes, and give cache recovery and reporting one owner. Family callers should
translate failures once instead of interpreting execution phases repeatedly.

## Scope and Evidence

The migration covers DerivedDataCache storage, codec, execution and sessions;
Engine Texture2D, TextureCube, VolumeTexture, StaticMesh render and physics
collision adapters; and ShaderBuild. Search source and test roots for Engine,
Sandbox and RoadWeaver as declared in `Durin.dworkspace` before changing shared
symbols. Include import, synchronous build, compiling-manager, PostLoad and Cook
consumers where they receive the affected results.

Observed problems in the current implementation:

- `FBuildResult` is `expected<FBuildOutput, FBuildError>` and cancellation is an
  `EBuildErrorCategory`. Callers repeatedly separate cancellation before handling
  errors. Several assembly helpers return strings for cancellation and require
  callers to query cancellation again to recover the missing distinction.
- Cube and Volume duplicate phase/category translation, including ordinal
  comparison with `Resolve`. Execution-time `Unavailable` can become
  `InvalidBuilderOutput`. Texture2D also defaults some failures to invalid output.
- A missing function is default `InvalidInput` in `ExecuteBuildRequest`, but
  `Unavailable` in session admission. Shader's generic fallback maps failures
  without producer details to `InvalidCompileRequest`.
- `OnCacheIssue` replaces default logging. Shader's observer handles persistence
  failures and corrupt/oversized reads but omits read `StorageFailure` reporting.
- Executor exception handling and session catch-all behavior differ. Escaping
  exceptions are reported by the session as `Build / ProducerFailure`, losing
  their actual execution phase. Optional persistence explicitly handles
  `bad_alloc`, but other escaping exceptions can invalidate successful work.
- Cache-hit validation converts every non-cancellation failure to `Corrupt`;
  environmental failures need distinct handling or a narrower validator contract.
- `ECacheError::Miss` models a normal lookup outcome as an error.
- StaticMesh production validates in `MakeSharedOutput`, executor validation and
  assembly `ReadLayout`. Shader validation and assembly repeat layout, code hash
  and reflection processing. Physics already avoids one duplicate array scan
  inside assembly, so it must not be changed by analogy without tracing its path.

Implementation anchors are `DerivedDataBuildFunction.h`,
`DerivedDataBuildExecution.cpp`, `DerivedDataBuildSession.cpp`,
`TextureCubeSourceBuild.cpp`, `VolumeTextureBuild.cpp`, `Texture2DBuild.cpp`,
`StaticMeshSharedOutput.cpp`, `PhysicsSharedOutput.cpp`, `ShaderSharedOutput.cpp`,
`ShaderBuildSession.cpp` and `ShaderBuilder.cpp` in their owning modules.

## Selected Architecture

### Outcome and failure are different concepts

Use a constrained sum type for success, cancellation and failure. The following
names are design sketches; do not expose independent mutable status, optional
output and optional error fields that permit contradictory combinations.

```cpp
struct FBuildSucceeded { FBuildOutput Output; };
struct FBuildCancelled { EBuildSessionPhase Phase; };
struct FBuildFailed { FBuildFailure Failure; };
using FBuildOutcome = std::variant<
    FBuildSucceeded, FBuildCancelled, FBuildFailed>;

struct FBuildCompletion
{
    FBuildOutcome Outcome;
    FBuildExecutionReport Report;
};
```

Success guarantees a validated immutable output. Cancellation and failure expose
no publishable product. Enforce this through constructors/factories or a wrapper;
do not allow default construction to imply success with an invalid output.

`FBuildFailure` contains a stable reason, diagnostic phase, bounded description
and optional producer diagnostic. Initial reason families are invalid input,
input unavailable, producer failure, invalid output, service unavailable,
resource exhaustion and internal failure. Admission has its own reasons such as
closed session, capacity exceeded, missing function and invalid bindings. Use
explicit mappings to completion failures when synchronous APIs combine admission
and execution. Do not turn every admission failure into module absence.

Phase describes where something happened; it does not determine recovery or
business error class. Remove ordinal phase comparisons. Preserve producer codes
only when consumers need them for behavior or diagnostic identity; qualify them
with a stable producer domain and validate conversions. DDC must not interpret
Texture or Shader enums. Do not introduce arbitrary typed product/error sidecars.

### Use expected at binary boundaries

| Operation | Selected result shape |
| --- | --- |
| Definition/action creation | `expected<T, ValidationError>` |
| Codec decode/encode | `expected<T, CodecFailure>` |
| Cache lookup | `expected<optional<Bytes>, CacheFailure>`: present is hit, empty is miss |
| Cache put | `expected<void, CacheFailure>` |
| Cancellable Describe/Resolve/Build/Validate or assembly | Success/Cancelled/Failed sum type with the operation's success value |
| Session submission | `expected<RequestHandle, AdmissionError>` |
| Session completion | `FBuildCompletion` |

A small reusable cancellable-result type within the owning build interfaces may
avoid repeated variants. Do not impose a new global Core result framework or
rewrite unrelated `expected` APIs. Pure non-cancellable recipe and codec APIs
remain binary. Cancellable void operations need an explicit success alternative.

### Session admission and cancellation

- Rejected submission returns an admission failure and never invokes completion.
- Accepted submission returns a handle and produces exactly one terminal
  completion, including dropped dispatched work and shutdown cancellation.
- An already-cancelled request that passes admission completes as Cancelled;
  cancellation does not make closed or invalid sessions accept work.
- Completion may run inline before Submit returns. Dispatchers must either reject
  without executing/retaining work or accept work for at-most-once execution.
- Cancel requests cooperative cancellation; it is not itself a terminal result.
  Session state arbitrates the race under a defined completion transition. A
  cancellation accepted before terminal commitment produces Cancelled; a cancel
  after that commitment cannot rewrite success or failure.
- Preserve external predicate support. Observe it at defined execution and final
  completion boundaries; arbitrary external predicate changes are not atomic
  with session state. Document this distinction from handle cancellation.
- Keep cancellation checks during expensive resolution, validation and assembly.
  Manager cancellation, latest-wins and publication checks remain necessary
  after DDC completion because they protect a different lifetime boundary.
- All cancellation propagation is typed. Do not identify cancellation by message
  text or by converting it into invalid input/output first.

### Cache recovery and execution reporting

Cache backend/codec report facts; the executor owns best-effort cache policy.
Missing entries are normal misses. Corrupt entries and cache service failures
allow reconstruction when the request permits it. A valid built output survives
record, encode, compression and store failures. Request cancellation still wins
at its defined terminal boundary even if optional persistence has begun.

The execution report records action/key availability, lookup outcome, whether
construction ran, persistence outcome and bounded transient diagnostics. It is
available on success, cancellation and failure, and never enters output identity
or serialized records. Metrics must not infer store failure by enumerating phases.

Distinguish cache validation rejection from validator execution failure. Semantic
data rejection makes the entry unusable and permits a rebuild. Resource exhaustion
or an internal exception is not evidence of corrupt data; report its actual cause
and terminate the request rather than silently misclassifying it as corruption.

Separate diagnostic delivery from observation. One configured reporting sink
owns cache logging; metrics observers do not replace it. Family/coordinator
boundaries own terminal business failure reporting once. Cancellation and normal
misses do not emit error logs. Optional diagnostic callbacks cannot change the
outcome; bound diagnostic storage and define best-effort behavior under allocation
failure rather than promising allocation-free recovery without evidence.

Catch recoverable C++ exceptions at the relevant operation boundary, preserving
phase and distinguishing allocation failure from producer/internal exceptions.
The session catch-all remains a lifecycle backstop, not the primary translator.
Do not turn observer exceptions into producer failures. No recovery is promised
for process-fatal assertions, memory corruption or platform faults.

### Family adapters and validation ownership

Each family adapter translates actual build failure once. Cube and Volume share
common texture mappings; Texture2D keeps only necessary domain-specific details.
Shader preserves compiler diagnostics without calling unrelated infrastructure
failures invalid compile requests. StaticMesh and Physics preserve their own
assembly/application errors and stages. No caller decodes DDC phase ordering.

Audit repeated validation separately from result migration. Generic envelope,
limits and schema checks belong to DDC; family semantic validation belongs to
registered functions; runtime construction and publication checks stay with the
typed owner. Captured metadata checks and materialized content identity checks
protect different boundaries and must both remain.

Remove repeated full scans only after proving equivalent inputs and conditions.
If a trusted assembly entry or validation receipt is introduced, bind it to the
exact immutable output, action, validator contract and relevant limits. Do not
use a forgeable boolean, cache a request-local typed product inside a stateless
function, or allow unvalidated cache bytes onto the trusted path. Preserve the
fully checked entry for independent callers. Allocation, conversion, cancellation
and runtime publication can still fail after semantic validation.

### UE reference and deliberate adaptation

Epic's public [FBuildCompleteParams](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Developer/DerivedDataCache/FBuildCompleteParams)
separates basic status, detailed build status, cache key and output.
[EStatus](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Developer/DerivedDataCache/UE__DerivedData__EStatus)
distinguishes Ok, Error and Canceled.
[FBuildOutput](https://dev.epicgames.com/documentation/unreal-engine/API/Developer/DerivedDataCache/FBuildOutput)
owns values/messages/logs and has no values when it contains errors.

These references support separating request state, execution facts and producer
diagnostics. Durin selects a sum type and keeps success-only immutable outputs;
this is an adaptation, not a claim that UE uses the proposed C++ types. Do not add
negative-result caching, remote workers, a new scheduler, universal asset states,
or a separate DDC module. Preserve keys, recipe versions and payload formats unless
a separately justified semantic change requires invalidation.

## Implementation Stages

### Stage 0: Record outcome and ownership decisions

- [x] Record code-review evidence and the selected three-way completion model.
- [x] Define binary-operation boundaries, cache recovery and reporting ownership.
- [x] Identify caller simplification, protected validation boundaries and gates.

Outcome: an executable proposal; this stage does not claim implementation or
native validation. Subsequent stages depend on these decisions.

### Stage 1: Introduce typed outcomes and stable failures

- [ ] Inventory affected symbols and consumers across all workspace source/test roots.
- [ ] Add constrained success/cancelled/failed outcomes and stable failure reasons.
- [ ] Migrate cancellable resolver/function/validator contracts and all overrides.
- [ ] Separate normal cache miss from cache failures; preserve binary codec APIs.
- [ ] Mechanically migrate consumers so the stage builds without a cancellation-as-error compatibility path.

Depends on Stage 0. Complete when cancellation is absent from migrated error
enums, invalid success construction is prevented, and focused result/cache tests
pass. Keep policy cleanup in later stages rather than silently changing behavior.

### Stage 2: Unify execution, completion and diagnostics

- [ ] Implement admission/completion mapping and terminal cancellation arbitration.
- [ ] Return execution reports for every terminal outcome, including early failure.
- [ ] Localize exception translation and distinguish cache rejection from environmental failure.
- [ ] Separate reporting sinks from observers and centralize cache recovery policy.
- [ ] Preserve inline completion, dispatcher rejection, dropped work, drain and ownership guarantees.

Depends on Stage 1. Complete when execution/session fault-injection and race tests
prove the behavior matrix below, with equivalent direct and session execution
semantics where their lifecycle contracts overlap.

### Stage 3: Simplify family callers

- [ ] Centralize Texture2D/Cube/Volume failure mapping and remove phase-order logic.
- [ ] Migrate StaticMesh, Physics and Shader translation without losing domain diagnostics.
- [ ] Replace Shader cache phase classification with report-driven counters.
- [ ] Propagate typed cancellation through affected assembly helpers and outer consumers.
- [ ] Remove obsolete translation helpers and redundant logs; retain publication checks.

Depends on Stage 2. Complete when all six families use the new contracts and
targeted regression tests cover hit, miss, cancellation, infrastructure failure,
producer failure and assembly failure without misleading error classifications.

### Stage 4: Remove proven duplicate validation

- [ ] Trace cold/hit validation passes in each family and record their distinct guarantees.
- [ ] Remove equivalent repeated scans, starting with StaticMesh and Shader.
- [ ] Add constrained trusted assembly only where it demonstrably removes work safely.
- [ ] Test corrupt output rejection and unchanged assembly/publication protections.

Depends on Stage 3. Complete when scan-count or equivalent targeted evidence
demonstrates the intended reduction and untrusted paths still receive full
validation. Document retained checks and their distinct boundaries.

### Stage 5: Qualify and publish the contract

- [ ] Validate affected project targets and complete the required `all` build for shared Engine API migrations.
- [ ] Run relevant native DDC, session, texture, mesh, physics, shader and Cook tests.
- [ ] Update the owning build protocol and any changed family contracts.
- [ ] Search all workspace consumers for obsolete cancellation errors, phase-order mapping and bypass paths.
- [ ] Run changed-document and all-plan validation; record exact evidence and remaining limitations.
- [ ] Commit isolated changes with exact plan/stage trailers and mark completion only after all gates pass.

Depends on Stage 4. Follow [build guidance](../Agents/BuildAndRun.md),
[native testing guidance](../Agents/Testing.md) and
[documentation guidance](../Agents/Documentation.md); select actual targets via
those workflows. Do not overlap build process trees or infer platform coverage
from a different host's result.

## Behavioral Acceptance Matrix

| Scenario | Required observation |
| --- | --- |
| Valid cache hit | Success; no source-byte resolution or producer invocation |
| Normal miss | Build allowed; no error diagnostic for the miss |
| Corrupt cached record or semantic output | Rebuild; cache rejection recorded once |
| Cache read storage failure | Rebuild; storage diagnostic remains visible with metrics observer installed |
| Validator resource exhaustion/internal failure | Failed with actual reason; not counted as corrupt cache |
| Producer failure or invalid cold output | Failed; no persistence or publication of invalid output |
| Record/encode/compress/store failure or supported injected exception | Success with usable output and persistence diagnostic, unless cancelled |
| Cancellation during each execution phase or typed assembly | Cancelled; no business error log or publishable completion product |
| Accepted handle cancellation racing completion | One terminal callback; documented commitment rule holds |
| Closed/full/unbound session or dispatcher rejection | Admission failure; no completion callback for rejection |
| Accepted dispatched work abandoned | One Cancelled completion; ownership and drain accounting released |
| Observer/reporting callback throws | Outcome unchanged; completion accounting remains correct |
| Missing function through inline/session entry | Consistent reason after explicit admission translation |
| Equivalent non-allocation exception through direct/session execution | Same execution cause and phase; session still completes exactly once |
| Typed assembly/runtime application fails after validated build | Family failure retained; no false claim that validation makes application infallible |
| Trusted assembly optimization | Reduced duplicate scans; invalid or mismatched validation proof cannot bypass checks |
