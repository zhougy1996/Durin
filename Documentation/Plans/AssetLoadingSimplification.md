# Asset Loading and Error Simplification Plan

Summary: Replace recursive asset error transport and closure-wide ordinary-load rollback with compact diagnostics, explicit load readiness, and scoped cleanup of incomplete graphs.

Last reviewed: 2026-09-18

Status: Active
Completed:

## Current Status

This plan replaces `Documentation/Plans/AssetObjectTypedErrors.md` by explicit
design choice. The former plan is superseded, not completed. Its remaining
cause-retention and rollback-preservation gates are withdrawn. No runtime
behavior has changed as part of this replacement; Stage 0 is the next work.

The former work remains in Git history: baseline `12beed739`, subsequent
`6d317f0b5` and `d569de006`, and branch
`codex/backup-asset-typed-errors-before-squash-20260918`. Existing typed APIs
are inputs to an audit, not requirements to preserve or changes to blindly undo.
Prior test/build passes do not validate the newly selected behavior.

Code inspection identifies three coupled problems. `LoadAuthoredObject`
assembles Archive state, reflected-value causes, and a package-specific error,
then the linker embeds it in `FAssetResult` alongside formatted text. Root load
failure retires all `TransactionPackages`, including successful nested loads.
`FindResidentPackage` does not exclude loading skeletons, and ordinary packages
are Standalone: neither a resident lookup nor loss of the requesting scope is
proof of readiness or eligibility for GC.

## Goal

Ordinary loading guarantees valid completed results and cleanup of incomplete
work. It does not promise restoration of the pre-request residency set or
reversal of arbitrary callback side effects. Return actionable diagnostics
without preserving a recursively owned tree of operation results. Keep stronger
replacement and persistence guarantees only at operations that require them.

This is a deliberate contract change across Engine and CoreDObject boundaries,
not a compatibility-adapter exercise. Migrate all workspace consumers, tests,
and owning documentation as each boundary changes.

## Selected Design

### Errors and diagnostics

- Ordinary load results carry a stable error classification and an owned,
  complete diagnostic message. Success has one authoritative representation.
  Reuse an existing suitable result rather than adding a parallel framework.
- Capture object/package identity, field route, dependency context, and the
  concrete reason while they are available, before candidate cleanup. A message
  surviving cleanup is sufficient; retaining typed nested causes is not a goal.
- Remove `FPackageObjectLoadError`, its result wrapper, and the corresponding
  `FAssetResult` cause link after migrating their producers and consumers.
  Serializers continue to return void and record the first Archive failure.
- Stop carrying a complete resolver operation result inside a load diagnostic.
  Preserve classification and useful text; actionable recovery or operation
  state belongs to an explicit owning-operation channel when actually needed.
- Audit the former plan's asset/object wrappers, including Capture, codecs,
  graph operations, property operations, Cook, and editor adapters. Keep typed
  fields only where a real consumer needs machine-readable distinctions.
  Formatting may occur at a module boundary; delaying all formatting to UI is
  no longer an invariant. Do not replace every error with an unclassified string.
- Separate load diagnostics from write disposition, recovery locations,
  transaction identity, and affected files. Do not remove recovery semantics
  from save, import, Cook publication, or replacement to simplify a load API.

### Load readiness and cyclic dependencies

- Distinguish internal skeleton availability, value restoration/validation,
  PostLoad execution, completed readiness, and failure. The exact representation
  must use one authoritative load state rather than competing residency maps.
- Internal reference fix-up may resolve a loading skeleton. Public load success
  and ordinary asset consumption require completed readiness. Define explicit
  reentrancy behavior instead of returning a skeleton as a completed package.
- Preserve supported cyclic hard references. Mutually dependent incomplete
  packages form a completion group; one member cannot be retained as an
  independent success while it references a failed member. Audit existing cycle
  support and select the smallest correct group implementation in Stage 0.
- A completed dependency outside the failing group remains valid and may stay
  resident. Root-request failure does not reverse all successful nested loads.

### Cleanup, callbacks, and residency

- A scoped owner retains incomplete objects, temporary registrations, resource
  registrations, and required references. On failure it removes discoverability
  and releases those owned items exactly once. It must survive early returns and
  callback exceptions without masking the original failure.
- Centralize cleanup; remove overlapping manual rollback calls, global
  transaction sweeps, and dependency rollback callbacks that exist solely to
  restore ordinary-load residency. Keep specialized ownership where required.
- Construction, serialization, and validation must not publish candidate
  references or perform external mutations requiring general compensation.
  Serialization modifies its target; dependency resolution uses loader bindings.
  Audit callbacks before relying on this restriction; it is not automatically
  enforced by C++ or by RAII.
- Complete recoverable data rejection and fallible publication preparation
  before PostLoad. PostLoad is a void initialization notification, not a
  recoverable transaction participant. Resource readiness may remain separate
  from successful object loading. Define reentrant loads and exceptional failure
  handling without promising rollback of already executed external side effects.
- Preserve explicit Standalone cache residency for completed ordinary packages
  initially. Successful independent dependencies retained after a failed request
  remain subject to ordinary unload/reference protection; do not claim that GC
  alone will reclaim them. No new automatic eviction subsystem is required.
- Make incomplete graphs inaccessible promptly; physical destruction may follow
  the object/resource retirement rules. Remove forced GC calls only after proving
  safe retry, path reuse, references, and deferred resource release.
- Hot reload, private graph replacement, and persistent writes retain their
  own candidate ownership and commit/abort guarantees. Ordinary dependency
  loading and operation-owned candidate cleanup must have distinct contracts.

## UE Reference Constraints

These sources motivate selected constraints; they do not establish that every
UE loader path is transaction-free or that Durin should copy UE internals.

- [Async loading guidance](https://dev.epicgames.com/documentation/unreal-engine/asynchronous-level-loading-in-unreal-engine)
  restricts custom serialization to its target and recommends deferring global
  interactions, delegate registration, and synchronous loads to PostLoad or later.
- [Object flags](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/CoreUObject/EObjectFlags)
  distinguish load and PostLoad stages. Existence alone is not readiness.
- [PostLoad](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/CoreUObject/UObject/PostLoad)
  is void. [Rendering lifecycle](https://dev.epicgames.com/documentation/unreal-engine/threaded-rendering-in-unreal-engine)
  shows resource initialization and deferred destruction requiring separate care.
- [Streamable handles](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/Engine/FStreamableHandle)
  distinguish request completion, per-entry failure, and reference retention.
  Releasing a handle stops its GC retention; it is not a general undo operation.
- [FIoStatus](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/Core/FIoStatus)
  demonstrates compact code/message status. UE's public contracts do not require
  the recursive asset/object cause structure introduced by the former plan.

## Implementation Stages

### Stage 0: Audit consumers and fix the loading contract

Outcome: an implementation-ready ownership and state model grounded in current
code. This stage precedes removal of any rollback guarantees.

- [ ] Trace ordinary load, cyclic references, reentrancy, custom serializers,
  PostLoad, resource registration, unload, private preparation, and reload.
- [ ] Inventory error producers and real code/context consumers across all
  projects in `Durin.dworkspace`; distinguish behavior from tests of old layout.
- [ ] Record the state transitions, completion-group algorithm, public/internal
  lookup rules, owner of each cleanup action, and callback restrictions here.
- [ ] Resolve PostLoad reentrant-load ordering and exceptional-failure behavior;
  identify existing callbacks that must change before the boundary moves.
- [ ] Map each former error boundary to retain, flatten, or remove, with a
  consumer-based reason; identify recovery metadata that needs a separate result.

Completion: every selected semantic change has affected consumers and concrete
acceptance cases; no unresolved ownership decision blocks Stages 1 or 2.

### Stage 1: Flatten ordinary load errors

Depends on Stage 0. Outcome: the full live/private field-load path uses compact
diagnostics without recursively embedding operation results.

- [ ] Replace the package object error/result wrappers and recursive asset cause
  adapter across producers, private preparation, live loading, and callers.
- [ ] Preserve first failure, codes, object/field/dependency context and original
  Archive messages; include useful expected/actual details at failure creation.
- [ ] Remove obsolete formatter and retention-only tests. Test actionable errors
  after cleanup, retry, and independent requests without asserting full prose.
- [ ] Pass affected tests and an all build; update authoritative contracts.

### Stage 2: Replace ordinary load transactions with scoped completion

Depends on Stage 0; integrate with Stage 1's diagnostics. Outcome: readiness and
cleanup are correct without reverting successful independent dependencies.

- [ ] Introduce authoritative load phases and internal skeleton resolution;
  prevent public success for an incomplete object or package.
- [ ] Implement completion/failure propagation for supported cyclic groups.
- [ ] Move recoverable gates before PostLoad and migrate side-effecting callbacks;
  define and test reentrant requests and exception cleanup.
- [ ] Replace root `TransactionPackages` rollback and redundant cleanup paths
  with explicit incomplete-group ownership; preserve existing resident packages.
- [ ] Verify failure removes temporary discoverability/resources and allows retry;
  keep independent successful dependencies usable and normally unloadable.
- [ ] Preserve private replacement abort and old live graph validity; update
  explicit dependency scopes without silently changing their operation contracts.
- [ ] Pass affected tests and an all build; update load/lifetime contracts.

### Stage 3: Consolidate asset and object result boundaries

Depends on Stages 1 and 2. Outcome: obsolete typed-error architecture and
compatibility adapters no longer dictate asset subsystem design.

- [ ] Execute Stage 0's retain/flatten/remove inventory across codec, Capture,
  property/graph operations, asset services, Cook, compilation, import, and UI.
- [ ] Separate actionable write/recovery outcomes from ordinary diagnostics;
  preserve cancellation, async completion, and external provider semantics.
- [ ] Remove obsolete cause members, headers, formatters, adapters, and old
  contract prose. Keep genuinely consumed typed status without a universal
  error registry, recursive result hierarchy, or mandatory formatting framework.
- [ ] Migrate all workspace consumers and pass affected tests and an all build.
- [ ] Document implemented contracts and close this plan only after all gates pass.

## Validation and Handoff

Follow [native testing](../Agents/Testing.md) and
[build guidance](../Agents/BuildAndRun.md). Shared Engine API changes require an
all build and affected targets across Engine, Sandbox, and RoadWeaver. Run builds
serially and retain evidence per completed stage.

Required behavioral cases include truncated/invalid fields with useful context;
external resolution failure; failure followed by retry at the same path;
independent dependency success followed by root failure; a failing cyclic group;
pre-existing resident dependency preservation; rejection of premature public
success; PostLoad ordering/reentrancy; exception-safe cleanup; resource retirement;
normal unload of retained dependencies; and failed replacement preserving the
old live graph. Test semantics rather than the shape of diagnostic wrappers.

Update [asset packages](../Runtime/Assets/AssetPackages.md),
[serialization](../Runtime/Core/Serialization.md), and other affected owning
contracts only as behavior is implemented. Update this plan in the same commit
as each validated implementation batch, using its exact Plan and Stage trailers.
Plan replacement alone requires changed-document and all-plan validation, not
native builds; it must not mark implementation stages complete.
