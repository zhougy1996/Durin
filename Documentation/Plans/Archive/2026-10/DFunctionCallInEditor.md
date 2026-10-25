# DFUNCTION CallInEditor Plan

Summary: Add generated no-argument void member-function reflection and transaction-aware Details actions.

Last reviewed: 2026-10-16

Status: Archived
Completed: 2026-10-16

## Current Status

Stages 0-3 are complete. The two outstanding regression failures were resolved
and the final broad regression passed after the implementation-stage handoff.
The first consumer is
`DPointLightComponent::NormalizeSettings`: it canonicalizes the component's own
authored color, intensity, and range, with render-state reconstruction through
the light component's edit/Undo/Redo hooks. It does not create or edit other
objects. All APIs below describe the selected implementation contract.

Validation exposed an existing property-session boundary issue: history-budget
discard was treated as successful completion. Stage 2 additionally classifies
that discard as failure, retains the cancelable preview, and rejects later
commits through an already-closed scope. This is required for action admission
to honor failed property-edit completion without silently losing history.

Inheritance validation also requires registration-order independence. The
runtime resolves the root descriptor through the owning class's parent chain
after metadata publication instead of caching a parent-function pointer during
local registration. This covers derived-before-base registration without adding
eager type-helper calls and potential registration dependency cycles.

### Validation Receipt

Validated the final implementation worktree based on revision `8f5a3b5c1` before
the implementation commit. All commands used `Win64-Debug-DurinEditor`.

- DHT suite: 247 passed, 1 skipped. Receipt: `Build/DFunctionDHTSuite04.log`.
- New action acceptance filter: 12 passed, including derived-before-base
  registration and an actual shared-panel ImGui click. Receipt:
  `Build/.agent-state/logs/20261015-205605-414225-40724-EditorPropertyTests.log`.
- Final `all` build passed, including Engine, Sandbox, RoadWeaver, and
  RoadWeaverEditor. Receipt:
  `Build/.agent-state/logs/20261015-205652-297896-35228-cmake.log`.
- Final complete focused targets passed: EditorPropertyTests 61,
  EditorOperationTests 61, CoreObjectTests 105. Receipts are respectively
  `Build/.agent-state/logs/20261015-205854-314389-43628-EditorPropertyTests.log`,
  `Build/.agent-state/logs/20261015-205904-186176-17172-EditorOperationTests.log`,
  and `Build/.agent-state/logs/20261015-205915-526789-31580-CoreObjectTests.log`.
- An earlier broad affected run passed 105/107 target groups. Its remaining
  failures were TextureCookIntegrationTests (duplicate `Durin.Material` asset
  compiler registration) and SandboxGameplayTests (process segmentation fault).
  Receipt: `Build/NativeTestResults/Win64-Debug-DurinEditor/affected.xml` and
  `Build/.agent-state/logs/20261015-204927-959531-12388-ctest.log`. This broad
  run predates the final registration-order refinement and was not repeated at
  this handoff. Neither failure is classified as pre-existing without a baseline
  reproduction; Stage 3 acceptance remains open.
- A corrupt CoreUtilityTests compiler PDB initially blocked the broad build;
  recompiling only that target's generated objects/PDB repaired the link. No
  repository source change was needed for that environment repair.

Follow-up validation on 2026-10-16 started from revision `810bfe97b`:

- TextureCookIntegrationTests already contains the compiler-lifetime repair in
  `af10c798d` and the resource-update suite naming repair in `02b064c20`. The
  complete target passed 3/3 in Debug. Receipt:
  `Build/.agent-state/logs/20261016-104245-745967-31536-TextureCookIntegrationTests.log`.
- SandboxGameplayTests passed all assertions but crashed during process-static
  destruction. A debugger reproduced the access violation in
  `UnregisterCookContributor`, reached from the retained Engine module's Cook
  registration scope after the Cook registry had already been destroyed. This
  target loaded modules without closing their lifecycle. A target-owned GoogleTest
  environment now calls `ShutdownModulesAtExit` after its worlds are retired and
  before the scheduler and registries close; runtime lifecycle APIs are unchanged.
  Diagnostic receipt: `Build/SandboxGameplayCdb.log`.
- The repaired Sandbox target passed 13/13 in one process and 13/13 in serial
  isolated processes, including normal exit. Receipts:
  `Build/.agent-state/logs/20261016-104513-077921-28188-SandboxGameplayTests.log`
  and `Build/NativeTestResults/SandboxGameplayIsolated.xml`.
- Final `test all --test-jobs 4` passed 108/108 ordinary native target groups,
  including both previously failing targets, after building `DurinNativeTests`.
  This excludes characterization and qualification under the ordinary test
  policy. Receipts: `Build/NativeTestResults/DFunctionAcceptanceFinal.xml` and
  `Build/.agent-state/logs/20261016-104915-151344-1448-ctest.log`.

The shared Details layout is rebuilt every frame in LevelEditor and
TextureEditor; their existing object edit notifications refresh derived state.
The retained material-collection diagnostics also consume the action refresh
result. Contract documentation and the plan are validated with the repository
documentation commands before commit.

## Goal

Expose explicitly annotated native object actions through the shared Details
panel. The first release supports one live inspected object, synchronous
game-thread execution, and non-static, non-const `void Method()` declarations.
An inherited action appears once, native virtual dispatch remains intact, and
ordinary object edits use the editor's existing history and package revisions.

The first release excludes parameters (including defaulted parameters), return
values, static/free functions, templates, variadics, ref-qualified methods,
pure-virtual annotated declarations, overload sets containing an annotated
method in the declaring class, struct functions, asynchronous actions,
multi-selection, persistent function references, scripting, and RPC. Ordinary
virtual methods, concrete overrides, `final`, and `noexcept` are supported.
The annotated method must have a callable definition when its module links.

## Existing Integration Points

- [Reflection generation](../../../Development/Tooling/ReflectionGeneration.md):
  hermetic declaration extraction, thin exports, generated friends, and output
  invalidation. Function bodies remain outside DHT's semantic inputs.
- [Generated reflection](../../../Runtime/ObjectSystem/ReflectionSystem.md): `DClass`, type
  registration, class defaults, and property ownership. The empty `DFUNCTION`
  macro is not an implemented function-reflection contract.
- [Reflected property editing](../../../Editor/Architecture/ReflectedPropertyEditing.md):
  shared layouts, search, customizations, active property edits, and object hooks.
- [Transactors](../../../Editor/Architecture/Transactors.md): checked recording results,
  scoped capture, history replay, package checkpoints, and cancellation semantics.

## Authoring Contract

```cpp
// Inside a reflected DCLASS with GENERATED_BODY().
DFUNCTION(CallInEditor, Category = "Settings", DisplayName = "Normalize Settings",
    ToolTip = "Normalize this object's authored settings.")
void NormalizeSettings();
```

`DFUNCTION()` registers a callable without exposing a Details button.
`CallInEditor` opts into the editor invocation policy below. Initial typed
presentation metadata comprises `Category`, `DisplayName`, and `ToolTip`.
Unspecified category is `Actions`; display name uses the existing CamelCase
presentation convention. Unknown/duplicate specifiers and malformed values are
errors, rather than silently ignored future promises. Metadata uses the shown
flat syntax; do not introduce a second `meta=(...)` grammar in this release.

Public, protected, and private methods may be annotated. Generated invokers
live inside the already-friended class statics helper, so access is granted
explicitly without changing method visibility. `CallInEditor` authorizes the
editor adapter only; it grants no remote, console, script, or network exposure.
It does not compile a runtime declaration out of game builds. Editor-module
actions belong in editor modules; the flag alone introduces no editor link
dependency into CoreDObject or Engine.

## Runtime Metadata And Invocation

Add a class-owned native `FFunction` descriptor, separate from `FProperty` and
the `DStructBase::ChildProperties` chain. It is not a managed `DFunction` object
and does not enter property serialization, default comparison, or GC schemas.
`DClass` owns immutable local descriptors; inherited descriptors are traversed,
not copied into derived classes. Descriptor lifetime follows its owning class
and module registration lifetime. Presentation strings are owned at runtime.

The initial descriptor contains declaring class, name, function flags, typed
presentation metadata, the fixed signature kind `VoidNoArgs`, and a generated
`void (*)(DObject&)` invoker. Root identity is resolved through the parent class
chain after publication. The reflected slot uses its root declaring qualified class
name and method name; a local redeclaration retains its own declaring identity.
These identities are session lookup keys, not a new serialized ABI.

Extend `DurinCodeGen::FClassParams` with a function-array pointer/count using
trailing defaults, and validate all records before publishing registration.
Keep function registration out of property construction and serialization.
Implemented public operations are:

```cpp
DClass::ForEachFunction(Visitor, bIncludeSuper);
DClass::FindFunctionByName(Name, bIncludeSuper);
FFunction::InvokeVoidNoArgs(DObject&) -> EFunctionInvokeError;
```

Enumeration and lookup return the effective most-derived descriptor once per
slot, with a documented deterministic order. Sort effective slots by root owner
qualified name and method name; presentation can additionally group categories.
`bIncludeSuper = false` returns local descriptors only. Invocation validates live
target identity, compatible class, signature kind, and invoker before entering
native code. The editor additionally rejects default/template objects. Runtime
invocation does not open transactions or read editor state.

The generated body casts to the declaring C++ type and makes an ordinary member
call. Do not encode a pointer-to-member as `void*`, use `reinterpret_cast` to
invent an ABI, or qualify the call in a way that bypasses virtual dispatch.
Invocation results describe admission and whether native code ran; they cannot
represent a domain failure returned by a `void` function. Native exceptions are
not a supported recoverable result or rollback mechanism.

## Inheritance And Name Rules

1. A base action is available on compatible derived instances without another
   annotation. It appears once, including when several ancestors are visited.
2. An unannotated C++ virtual override executes through the base invoker's normal
   virtual dispatch. It inherits the reflected slot and all presentation/policy.
3. A derived annotated declaration with an inherited reflected name is accepted
   only as a genuine compatible virtual override. It replaces the effective
   descriptor for that slot. Its annotation is complete: omitted metadata uses
   ordinary defaults and omitted `CallInEditor` removes the button for that
   class and descendants. This gives an explicit opt-out without another flag.
4. A reflected same-name non-virtual hiding declaration, an incompatible
   redeclaration, or an annotated overload is a DHT error with both owners and
   source locations. Multiple independent callable slots with the same name in
   one class hierarchy are not supported in the first release.
5. An unannotated non-virtual method that hides a reflected base name does not
   replace the reflected action. Reflection still invokes the base declaration.
   DHT reports this as an authoring error in configured reflected headers, rather
   than allowing a button whose behavior disagrees with the visible declaration.
   Overloads hiding an inherited reflected name are rejected for the same reason.

DHT must implement these checks from the current header's declarations and
exported reflected ancestor summaries, not from ordinary included headers or
function bodies. Thin class exports gain only reflected method names, fixed
signature/virtual facts, declaring identities and diagnostic locations needed
for ancestry validation. Unannotated overrides need not become exported callable
descriptors: root virtual facts suffice for later descendant validation.
Native compilation remains the final authority for C++ override legality.
Validate equivalent cases across headers, modules, and all workspace projects.

## Details Integration And Click Flow

Implement discovery/layout and execution in DurinEd `PropertyEditor/`, with no
Actor, asset-type, or RoadWeaver branches. Extend `IDetailLayoutBuilder` and
`FDetailLayoutBuilder` with default function rows and explicit hide/place
operations keyed by reflected slot identity. Registered customizations run
before default functions are appended. `HideCategory` hides both kinds of rows;
`ReplaceDefaultProperties` retains its property-only meaning. Explicitly placed
function rows suppress their default duplicate.

Buttons participate in category ordering, search, tooltips, visible-row counts,
and the shared panel's empty-state wording. Search includes method name,
declaring type, display name, category, and tooltip. Stable widget IDs use slot
and object identity, never the translated display label. Default ordering inside
a category follows the deterministic effective-slot order above.

Use a shared editor action executor that is callable without ImGui for tests:

1. Record a click request while drawing; finish the table/layout traversal before
   executing it. Do not mutate the target while property rows are traversing it.
2. Finish the current `FPropertyView` edit via its checked terminal operation.
   If it fails, retain that edit and reject the action. A completed property edit
   and the button action are separate history entries.
3. Resolve and validate the object key and effective slot again, then retain the
   target for the synchronous invocation through the existing collector/root
   mechanism. Never retain a raw captured object pointer across frames.
4. Reject read-only context, invalid/template/default target, missing flag,
   non-game-thread execution, busy/non-idle transactor, or recursive action
   execution. Disable known-unavailable buttons with a reason; repeat checks on
   invocation because UI state is not admission authority.
5. Prepare checked transaction capture, invoke once, notify/finalize as described
   below, report errors through `FPropertyViewContext::ReportError`, and request
   a fresh layout. Do not reuse property addresses or rows after the call.

The executor consumes the host's `Context.Transactor`, not a separate history or
an implicit fallback to another editor instance. Add a distinct action-invoked /
refresh result instead of setting `bChanged` merely because the button was
pressed; committed authored changes retain their existing meaning. All hosts
of `DrawDetailPanel` must be migrated for the new refresh/result semantics.

## Transaction And Mutation Contract

The initial `CallInEditor` policy is automatic capture of the inspected object's
non-transient reflected members. A click requires an available idle transactor,
opens one named scope, captures before-state, invokes, then explicitly ends the
scope and handles its result. Description uses the display name; context retains
the slot identity and primary target. Unchanged before/after state creates no
history entry and does not dirty a package.

Expose checked scoped begin/capture operations returning existing structured
`FTransactorResult` causes. Preserve the existing best-effort `Modify()` wrapper
for its current consumers. A failed begin or before-capture prevents invocation;
inspect cleanup failures as well. Merely checking `IsActive()` after `Modify()`
is not a replacement for a result-bearing API.

After a normally returned invocation, send one full-object `PostEditChange()`
notification before final capture, so derived runtime state can be refreshed and
any normalization is included in the final snapshot. Do not fabricate a changed
leaf path for an arbitrary method. The action must preserve object invariants
itself; this path cannot provide detached pre-write validation for arbitrary C++
code. Functions intended for this adapter should not duplicate the adapter's
terminal notification. Existing Undo/Redo property notifications must rebuild
equivalent derived state; verify this with the first real consumer.

Undo restores captured before-values; Redo restores captured after-values.
Neither calls the function again. Target member capture is not recursive graph
capture: component fields, referenced objects, object creation/destruction,
package membership, files, and external services are outside this policy.
CallInEditor methods in this release must not perform those authored mutations,
destroy the target, open modal loops, pump events, or start deferred authored
work. Transient derived caches are allowed when reconstructible from captured
state through edit/Undo/Redo hooks. This is an authoring contract, not something
the no-argument signature proves.

`Cancel()` discards records and never implies rollback. An after-capture or
history-finalization failure can occur after native mutation. Return distinct
facts for native invocation and history outcome, surface the structured failure,
refresh the view, and conservatively mark the target package dirty and invalidate
its saved checkpoint through existing services. Do not report successful Undo
coverage, automatically retry the action, or claim atomic rollback. No-op actions
do not take this failure fallback unless finalization actually fails. Strongly
atomic operations must use a domain custom change in a later explicit policy.

Successful recorded changes use existing transaction package revisions and dirty
tracking; do not add an unconditional `MarkPackageDirty()` on every click.
Nested UI action execution is rejected. Ordinary nested scopes opened by a
method must obey current LIFO rules, but they do not extend the automatic
single-object mutation contract or permit overlapping custom records.

## Extension Boundaries

- Parameter support can add signature descriptors, parameter direction and
  lifecycle/layout operations, return storage, and a checked call frame. Reuse
  supported property value machinery where suitable without attaching parameter
  nodes to an object's serialized fields. Keep `VoidNoArgs` as a typed fast path;
  do not add an unused VM, argument blob, or `ProcessEvent` now.
- A future explicit domain-managed editor policy can omit automatic target
  capture and delegate history, compensation, multi-object changes, files, or
  async work to existing custom-change services. It needs a result-bearing
  adapter before adoption; a `void` callback alone cannot prove command success.
  Do not ship a silent non-transactional fallback in the first release.
- Script, console, automation, and remote adapters require their own exposure
  flags and admission rules. Multi-selection needs an explicit batching,
  deduplication, mixed-type, and partial-failure policy before enabling it.
- Function references are not serialized. Renames need no asset migration in
  this release. Persistent bindings and module reload require separate stable
  identity/version/lifetime contracts; UI rows must not outlive module code.

## Implementation Stages

### Stage 0: DHT Declaration And Inheritance Model

Outcome: supported declarations produce deterministic, validated function data.

- [x] Add function model, annotation parsing, strict diagnostics, hermetic lookup,
  and exported ancestry summaries; version relevant schemas and caches.
- [x] Cover access levels, virtual overrides, metadata replacement/opt-out,
  cross-header/module ancestry, hiding, unsupported signatures, and stale cache
  repair in DHT tests. Do not add function bodies to extraction.
- [x] Verify errors identify the annotated method and relevant ancestor.

### Stage 1: Runtime Registration And Native Invocation

Depends on Stage 0. Outcome: class lookup invokes one correct native function.

- [x] Add immutable function descriptors, generated invokers, class registration,
  effective enumeration/lookup, and structured invocation admission results.
- [x] Verify inherited/private calls, native virtual dispatch, effective flags,
  wrong-class/dead-target rejection, deterministic enumeration, registration
  validation, and separation from serialization/GC property schemas.

### Stage 2: Checked Transactions And Shared Details Actions

Depends on Stage 1. Outcome: a button invokes once with defined history behavior.

- [x] Add result-bearing scoped begin/capture without weakening existing callers;
  migrate all affected shared API consumers across the workspace.
- [x] Add a non-UI executor, deferred-until-after-layout clicks, target retention,
  property-edit completion, admission/reentrancy gates, and result reporting.
- [x] Add searchable categorized rows, customization hide/place operations,
  duplicate suppression, tooltips, disabled reasons, and refresh integration.
- [x] Verify one Undo entry, before/after replay without reinvocation, inherited
  flags, no-op dirty behavior, notification-driven derived-state restoration,
  missing/busy transactor, read-only/default targets, rejected capture before
  invocation, and failed finalization after mutation with dirty/checkpoint repair.
- [x] Verify a pending property edit finishes separately, failed edit completion
  blocks invocation, and click execution does not invalidate row traversal.

### Stage 3: Consumer And Acceptance

Depends on Stage 2. Outcome: the feature is demonstrated and documented.

- [x] Select one real single-object settings-normalization or derived-preview
  action whose effects satisfy the automatic capture contract; do not force a
  structural road/asset generation command into this policy. Record the exact
  consumer and affected targets here before implementation.
- [x] Validate actual Details behavior, Undo/Redo-derived state, and save/dirty
  state in the owning host. Use test fixtures for failure and inheritance cases.
- [x] Follow [native testing guidance](../../../Agents/Testing.md) and
  [build guidance](../../../Agents/BuildAndRun.md); validate affected project targets
  and complete an `all` build for the shared Engine API migration before handoff.
- [x] Publish implemented contracts in reflection generation, runtime reflection,
  reflected property editing, and transactor documentation; validate documents
  and record evidence here before completing the plan.
- [x] Diagnose/classify the two recorded broad-suite failures and obtain a final
  broad regression receipt before marking Stage 3 and this plan complete.
