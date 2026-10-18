# Editor Execution Foundation Plan

Summary: Unify native tool completion, editor transaction lifetime and cooperative Python execution on existing services before adding domain tools.

Last reviewed: 2026-10-18

Status: Completed
Completed: 2026-10-18

## Current Status

All five stages are complete and accepted on macOS arm64. Contract freeze:
`922e3942f`; native completion: `c77edb0cb`; transaction validity/host binding:
`1025cee18`; cooperative Python and shared result views: `cca6f4fb1`; final
integration/lifetime correction and acceptance are in this commit. The final
matrix passes 1,275 cases across editor, non-editor and Python-disabled settings,
with one separately driven SDK case explicitly skipped. Workspace all and
non-editor Engine builds pass. The detailed receipt below owns selections,
reproduction settings and remaining reference boundaries.
It supersedes `Documentation/Plans/EditorTransactionUEAlignment.md` and
`Documentation/Plans/EditorAgentToolsetExpansion.md` at `aff860306`. Their separate
active checklists are retired, not marked implemented or successfully completed.
The original transaction plan's Stages 0-3 were accepted in `c2a2fad14`, and its
synchronous Python Stage 4 in `7527b2715`; their evidence is retained below.
New stage numbers belong to this consolidated plan, not those historical stages.

The user selected mechanism work first. Native history Toolsets, observation and
property/scene/asset/material tools, and new MCP transaction/transport features
are outside this plan. Their former feature gates are deferred by this scope
change, not waived as passed. Existing Console/MCP/Python consumers still need
compatibility coverage. This plan can finish without delivering new domain tools
or resuming MCP feature work.

There is no remaining implementation stage in this plan. Lasting contracts are in
Core Tools/TaskSystem and editor Tools/Transactors/PythonTools, with Python usage and
MCP compatibility updated. Deferred domain tools and protocol features remain
outside this completed mechanism scope.

## Goal And Scope

Complete a small shared foundation for synchronous and asynchronous tool calls,
Python steps that return to the editor loop, and correct global transaction
recording across those steps. Preserve existing synchronous callers and the
accepted UE-shaped Modify/Begin/End/Cancel/Undo/Redo model.

Reuse one native Toolset registry, one invocation service per editor, the existing
Python runtime/script pump, and one transactor/history per editor. Keep Core free
of editor and transport types. Use deterministic native fixtures and real embedded
Python to exercise the mechanisms; no new production tool catalog is needed.

Deferred features include context/log/asset discovery, property and scene edits,
asset creation/save/import, material graph operations, native history providers,
cross-RPC Begin/End/Cancel, and new protocol endpoints. Future tool providers must
call their owning domain services, use explicit target identity and revalidate
on GameThread; they must not create another scheduler, history or result store.
Their schemas, domain policies and visible-editor feature acceptance belong to
future work, not this foundation's completion gate.

## Architecture And Selected Contracts

### Ownership

| Owner | Responsibility | Reuse and simplification |
| --- | --- | --- |
| Core IToolset/FNativeToolset | Descriptor, arguments and Future of terminal FToolResult | One registry and completion contract; ready-Future adapter for synchronous handlers |
| FEditorToolService | Invocation identity, admission, final validation, lookup/cancel and bounded retention | Extend existing queue/receipts; one authoritative client-visible outcome; remove reserved OperationId |
| Python runtime / IEditorScriptPump | Interpreter, job state/output, references and continuation dispatch | One continuation queue; callback is a one-step continuation; Python handles view native results |
| Per-editor transactor | Recording, Modify/custom records and synchronous Undo/Redo | Existing history and transaction identity; internal scope validity and execution-bound host recorder |
| Console / MCP | Existing entry, result formatting and client lifetime | Thin adapters; no domain logic, new protocol or separate async manager |

A script job may own several tool invocations, so those identities remain distinct.
An invocation's Future, service receipt and Python handle are views/ownership of
one completion path, not three independently mutable copies of its result.
Provider outcomes are validated before client-visible terminal publication.

### Future completion and admission

- IToolset/FNativeToolset invocation returns Future<FToolResult> semantics, without
  renaming these types to UE's FToolset. Synchronous native handlers use a ready
  Future adapter; asynchronous handlers return pending Futures. Reuse an existing
  suitable primitive first; add only demonstrated missing completion capabilities,
  not another scheduler. Stage 0 selects the exact C++ type.
- Support nonblocking readiness/subscription, owned result retention, inline or
  later completion, and exactly one terminal outcome. Final values and errors
  pass the same schema/size validation. A submission acknowledgement is not final
  success. Transport types and editor pointers do not enter Core result data.
- Descriptors declare whether execution may pend. Native CallNow and Python
  tools.call() retain direct synchronous results and reject may-pend tools before
  provider callbacks can cause effects. A synchronous provider returning pending
  is a contract error; never retry after effects or claim rollback. No blocking
  Future wait/get or nested editor tick is allowed on GameThread.
- tools.submit() accepts either kind and returns a handle over the same invocation.
  result() returns/raises when ready and reports NotReady otherwise. yield handle
  waits cooperatively, then sends converted data or raises the existing tool error
  into the generator. Bare yield resumes on a later tick.
- Completion marks work runnable; it never reenters Python inline. Editor-related
  publication uses the owning GameThread boundary. Detached producers deliver
  owned data only; copied tool contexts do not authorize later DObject access.
- Extend current accounting to cover queued, pending and retained results,
  including consumer-held results after receipt TTL eviction. Release each charge
  with its actual ownership and apply backpressure before admitting unbounded work.
  Configure must not treat pending work as idle. No second cache or budget manager.

### Python scheduling and cancellation

- Keep the existing single embedded interpreter on GameThread. Plain scripts run
  synchronously; long work explicitly uses callbacks/generators through the same
  pump. A callback is one step in the continuation model, not a separate executor.
  No worker interpreter, general asyncio integration, automatic bytecode slicing,
  or retained provider callback stack is introduced.
- Steps run outside ToolService provider dispatch. Preserve registration order
  within a job; dynamically registered callbacks append without recursive entry.
  Ready jobs interleave fairly between steps. Bound continuations/resumes and use
  a soft per-tick budget between callbacks; one callback still runs until return.
- Retain Python references while suspended. Rebind job globals, host, tools,
  stdout/stderr, trace and cancellation context on every entry. Jobs remain live
  until their continuations and owned invocations settle or safely detach, followed
  by scope/reference cleanup before terminal publication. No implicit fire-and-forget.
- Distinguish observation unsubscribe, cooperative domain cancellation, job
  cancellation and transaction Cancel. Reuse existing tokens and runtime state;
  different meanings do not require separate cancellation frameworks. Job cancel
  stops normal resumes, signals owned work and runs generator close/finally on the
  owner thread. Late results cannot revive cancelled jobs or access a closed host.
- Use one optional runtime-owned job deadline measured by total elapsed time,
  including suspension; execution time is diagnostic, not a second timeout policy.
  Remove the fixed mandatory 30-second runtime deadline and inert per-call timeout
  compatibility. Native execution remains cooperative and cannot be preempted by
  that deadline. Existing transport deadlines remain adapter policy.
- Provider/module owners stop and drain their producers or safely detach data-only
  completion before unload. A Future or cancellation signal does not pin provider
  code. Finalize CPython only after in-use callbacks and Python cleanup finish;
  report cleanup failures without claiming restored effects or safe unload.

### Transactions and host binding

- Do not automatically wrap a job, tool call or Future lifetime in a transaction.
  Prefer preparation/waiting outside recording, then revalidate target identity and
  revision and apply within a short synchronous ScopedEditorTransaction. Existing
  domain scopes compose with ordinary recording; replay remains synchronous.
- Explicit scopes may span yield. They keep that editor's global recording open;
  same-host UI/native edits and other jobs may join it. Undo/Redo availability
  follows the native transactor. There is no independent Undo for unrelated edits
  within that recording, client isolation, silent scope splitting, automatic
  close/reopen, yield prohibition or UI lockout added to simulate isolation.
- Exceptions/job cancellation normally End still-valid scopes. Explicit transaction
  Cancel discards the entire active recording, including inner Cancel, under the
  accepted native rules. Neither operation automatically restores data; committed
  short transactions stay in history. Unwind scopes before publishing the job's
  terminal outcome and report incomplete cleanup honestly.
- Scope helpers must recognize invalidated recordings and destroyed hosts before
  End/Cancel/destruction. Prefer existing transaction identity plus host lifetime;
  introduce a new generation only if necessary. Begin indices are nesting counts,
  not validity identities. Old helpers must never affect a replacement recording.
  This is internal validity, not a public owner token, lease or per-job history.
- Bind the runtime-neutral recorder to the executing editor at existing host
  boundaries, covering Python, tool, UI and ordinary native entry. Same-host jobs
  share recording; different hosts keep distinct histories. Suspension exits the
  execution binding without ending recording. Restore only a valid prior binding;
  cross-frame correctness cannot depend on a PreviousRecorder pointer stack ending
  in LIFO order. Define unbound entry without breaking existing single-host use.
- Retain necessary transactor/object references through GC-visible ownership and
  existing work gates. Callback guards cover only the active callback; pending work
  cannot use a borrowed editor pointer after return or outlive code it still needs.

## Stage 0 Contract Freeze

This section selects implementation work; it does not describe shipped APIs.
Inspection baseline: `fd35640525d4fd89501e077c03aa3b887d54e1fc`, macOS arm64,
`MacOS-arm64-Debug-DurinEditor`. The checkout was clean. Raw symbol inventories
and resolved test registrations are under `Build/EditorExecutionFoundation/Stage0/`.

### Completion reuse and API

Use `Tasks::TSharedTask<FToolResult>` as `FToolFuture`, with
`Tasks::TCompletionSource<FToolResult>` as the producer. Retain immutable results
through `GetResultShared()`; never use the blocking `GetResult`, `TakeResult` or
Wait APIs in tool dispatch or Python. Existing shared-task storage, external
completion, producer-abandonment failure and terminal hooks already implement the
necessary single-assignment and publication ordering. Add only these missing
capabilities to the existing task implementation:

- A scheduler-independent external completion source and ready-value factory,
  using the same task node/result storage and terminal hooks. Synchronous tools
  must still work before scheduler startup and after scheduler shutdown. This
  source creates no scheduled body or execution queue. Do not initialize an
  implicit scheduler to create a ready result.
- A nonblocking, removable terminal observation backed by the existing
  `FTaskTerminalHook`. Binding races with completion under the existing terminal
  publication lock. A subscription made after completion observes it once;
  completion during subscription cannot lose or duplicate the notification.
  Unsubscribe drops observation, not domain work. In-flight notification may
  finish, so service observers capture only detached shared/weak state.
- A shared-task lifetime attachment propagated to `GetResultShared()` aliases,
  so result accounting survives receipt eviction and the endpoint itself.

`IToolset::Call` returns `FToolFuture`. Increment the modular feature interface
version for its changed ABI. `FToolBinding` accepts a Future handler and retains
an explicit synchronous handler adapter; `BindTool` uses the ready adapter.
`FToolDescriptor::bMayPend` defaults false. CallNow rejects true before availability
or execution callbacks. A false descriptor returning pending is InvalidOutput
without retry, cancellation-as-rollback, or a nested tick. Submit accepts both.
Remove only `FToolResult::OperationId`; asset staging and notification operation
identities are unrelated and stay.

A provider Future is an untrusted producer outcome. ToolService validates the
ready/delayed terminal value at the same finalization boundary, including status,
diagnostics, failure data, successful output schema and allocated size. Task
failure/abandonment becomes bounded InternalError; cancellation acknowledgement
becomes Cancelled. Never expose a raw provider task as a validated client result.
The existing invocation record owns the final completion source. Receipt lookup
and a new retained-invocation handle observe that same final Future. Temporary
raw and validated storage are two phases of one path, not independent result
caches. Release raw storage immediately after finalization.

Terminal observers only mark an invocation ready. Drain finalizes on the owning
GameThread, without entering Python. Inline-ready completion may finalize during
the admitting native boundary. Pending work keeps the exact admitted descriptor
and registration identity; replacement never reroutes it. Completion after provider
removal is allowed only from a safely detached data-only producer. Feature callback
scopes end at provider return and do not pin pending producer code.

### Invocation transitions and bounds

Reuse session/sequence IDs and `FImpl::Invocations`; a script job ID can own many
of them. Public Running covers dispatched pending work, preserving existing
Console/MCP state encoding. Internal phases are queued, dispatching, pending,
ready-to-finalize and terminal. Each phase can publish at most one terminal result.
Queued cancellation prevents dispatch. Dispatched cancellation only signals the
existing `FToolCancellationSource`; a provider's successful applied result can
still win. Observation unsubscribe does not request cancellation. Closure signals
owned work and publishes a bounded closed/cancelled outcome after detaching its
data-only completion observation. Late completion cannot reach the editor.

Extend `FEditorToolLimits` and existing accounting, not a second manager:

- Bound pending count and retained input/route bytes independently of queued
  capacity. Reserve pending admission before invoking a provider; saturation
  cannot occur after effects. Retain that charge until finalization/detach.
- Bound live result count and allocated bytes, including consumer-held aliases.
  Reserve a terminal slot and bounded diagnostic allowance at admission, then
  charge the validated result before publication. If the final value cannot fit,
  publish the reserved bounded LimitExceeded result. Raw producers must honor Core
  value bounds; arbitrary producer allocations are not a service-owned cache.
- Pruning TTL/count removes receipt ownership only. Accounting attaches to shared
  terminal storage and releases after the last receipt/consumer reference. Under
  pressure prune eligible receipts before refusing new admission. Live consumer
  pins cannot be silently evicted. Configure rejects queued, dispatching or pending
  work, and rejects limits below still-live consumer charges.

Use existing work gates and cooperative tokens. Modules must stop/drain their
producers before unload or prove a detached completion uses Core-owned data/code
only. Do not advertise a Future, endpoint or cancellation token as an unload pin.

### Recording validity and execution binding

Reuse `FTransaction::GetId()`/`NextTransactionId` for active-recording identity;
expose an internal query on DTransactor with an inert default for unsupported
implementations. Scope helpers retain a GC-visible strong transactor reference
and the ID obtained after Begin. End/Cancel/destruction check the exact live
object, active recording ID and host work gate where applicable. Invalid scopes
become inert before touching a replacement recording. Begin indices continue to
be nesting counts, and inner Cancel still discards the whole active recording.
No public transaction owner token or new generation is needed.

Bind the runtime-neutral `FObjectTransaction` bridge during host operations,
subsystem callbacks, tool callbacks and Python steps. The host-operation mechanism
must expose a runtime-neutral entry/exit seam without an Engine-to-DurinEd
include dependency. Restore a prior execution binding only if its object/lifetime
is valid. A host with no active recording binds its own recorder (SaveObject
returns false), preventing capture by another host's suspended recording.
Suspension exits the binding and keeps recording open. Remove PreviousRecorder
from cross-frame recording ownership. For ordinary unbound native Begin/Modify,
resolve the explicitly begun live recording; with multiple candidates require an
explicit host execution binding rather than choosing an arbitrary history.
Retire candidates on End/Cancel/destruction. Existing single-host native use
remains valid. A scope alone must not retain a destroyed host or reopen its gate.

### One Python continuation model

Keep `FPythonRuntime::FImpl`, `FJob`, its outstanding queue and IEditorScriptPump.
A job owns fresh globals, Python references, ordered continuations, entered native
scopes and native invocation handles. Replace `FJob::Invocations`' copied
FToolResults with retained native invocation handles. `tools.call` uses CallNow;
`tools.submit` admits an invocation and returns its handle. `result()` converts
only a ready validated result, otherwise raises `durin.NotReady`. Remove the
inert per-call/result timeout parameter from the facade and binding.

Expose ordered callback/generator registration in the durin facade. A callback
is one continuation step; a generator remains at its position until finished.
Dynamic registrations append, never recurse. Each ready job gets one step before
another round; bare yield cannot resume until a later editor tick. Yielding a
job-owned invocation waits until terminal, then sends converted Data or throws
ToolError into the generator. Foreign handles and unsupported yielded values
fail explicitly. Completion marks readiness and never enters CPython inline.
Bound live continuations and resumes per tick, with a soft time budget checked
between callbacks. An individual step still runs until return.

On every entry rebind globals/tools, host, stdout/stderr, trace and cancellation
context, restoring interpreter globals afterward. Preserve suspended references
on GameThread; temporary provider/host guards end after each step. A job remains
Running while continuations or owned invocations remain unsettled. No implicit
fire-and-forget: terminal publication follows settlement or explicit safe detach,
reverse scope unwind and Python reference cleanup.

CancelJob/session/host close stop normal resumes, signal all owned native work,
and close generators on GameThread so finally blocks run. Cleanup runs with a
closed-work context: scope End/Cancel is permitted, new tools/continuations are
not. Failure to close is reported and cannot be called rollback or safe unload.
Stop drains suspended Python cleanup before Py_FinalizeEx; callback Stop defers
until the active step returns. Late native outcomes cannot revive jobs.

Add one optional runtime option for a positive finite total-elapsed job deadline,
starting at job admission and including queue/suspension. Default is no deadline.
The trace and pump share it; native work remains cooperative. Remove the mandatory
30-second runtime limit and update its old integration test to explicitly configure
a short deadline. Existing MCP transport deadlines stay unchanged.

### Consumer inventory and gate selection

Search covered all source and native-test roots of Engine, Sandbox and RoadWeaver
listed by Durin.dworkspace. Direct IToolset implementations are FNativeToolset and
test fixtures in ToolTests/EditorToolTests. Production bindings are LevelEditor's
PIEToolset and DurinPython's PythonToolset. ToolService is the sole production
IToolset invocation boundary. Console and MCP consume service results; Python
currently calls CallNow and caches copies. Sandbox has no matching consumer.
RoadWeaver's RoadEditorIntegrationTests directly exercises DTransBuffer/Modify and
must retain coverage after binding changes. No new production providers are needed.

Stage 1 selects CoreToolTests, CoreConcurrencyTests, EditorOperationTests,
ExternalToolTests, EditorShellTests, MCPCodecTests, MCPIntegrationTests,
MCPPythonJobTests and PythonToolIntegrationTests, adding deterministic completion,
capacity and detached-lifetime cases to their owning fixtures. Stage 2 adds
CoreObjectTests, EditorPropertyTests, LevelMutationTests, AssetPackageReloadTests,
WorldTests, MaterialGraphEditingTests and RoadSceneIntegrationTests, including
isolated stale-scope and A/B/A regressions. Stage 3 uses real deployed CPython and
Python-disabled configuration plus isolated continuation/cancellation cases.
Stage 4 reruns every matrix input invalidated by later implementation changes.
The task primitive/host bridge changes cross foundational headers and ABIs, so an
incremental workspace `build all` is required at integrated acceptance to cover
Engine, Sandbox and RoadWeaver consumers that bounded native targets cannot prove.
Use the existing explicit CPython SDK override; no new platform/GPU/smoke gate.

All new completion, suspension and cross-host choices are selected Durin policy.
No implementation depends on unverified UE 5.8 transaction wrappers or scheduler
lifetime, so no additional upstream parity claim or reference-access gate is added.

## Reference Boundary

The accepted native and Python transaction reference is UE 5.3.2 at Epic mirror
revision `c865e168d0935b8e5f4bd865ddcc1c733c8ce7cf`, with source provenance in
[Transactors](../Editor/Architecture/Transactors.md#transactions-and-scoped-recording)
and [PythonTools](../Editor/Architecture/PythonTools.md#transactions).
Do not relabel that evidence as UE 5.8 qualification.

UE 5.8's [FToolset](https://dev.epicgames.com/documentation/unreal-engine/API/Plugins/ToolsetRegistry/FToolset)
provides a Future return surface; its [Python automation scheduler](https://dev.epicgames.com/documentation/unreal-engine/write-editor-tests-with-python-in-unreal-engine?lang=en-US)
documents explicit latent callbacks/generators. Blender's
[timer implementation](https://github.com/blender/blender/blob/main/source/blender/python/intern/bpy_app_timers.cc)
and [modal operators](https://docs.blender.org/api/4.3/bpy.types.Operator.html#modal-execution)
inform cooperative execution and persistent task state. These references do not
prove identical transaction lifetime, cancellation or isolation semantics.

Cross-frame recording, completion ownership and Python composition above are
selected Durin policies. Claims about exact UE 5.8 automatic wrappers,
NonTransactableToolCall, scope lifetime or cancel forwarding still require pinned
source or reproducible reference probes. Missing reference access leaves those
claims open, but does not block independently specified Durin mechanisms. Preserve
accepted native behavior unless a deliberate change and rationale are recorded;
no implementation may silently infer automatic transactions from a Future.

## Implementation Stages

### Stage 0: Freeze foundation contracts and reuse

Outcome: concrete APIs and lifetime ownership are settled before implementation.
Depends on the accepted baseline and selected contracts above.

- [x] Consolidate the two plans, defer domain tools/MCP features and select one
  ordered mechanism-only execution path. This checkbox records planning only.
- [x] Inspect existing task/completion primitives and select the native Future
  type, ready adapter and may-pend descriptor surface. Specify result validation,
  subscription races, producer abandonment and terminal publication.
- [x] Map existing invocation, job and transaction identities/owners. Select scope
  validity and host binding using existing identities first; identify redundant
  OperationId, Python result caches and timeout shims to remove.
- [x] Freeze cancellation/detach transitions, pending/retention budgets, continuation
  ordering and the optional elapsed-time deadline. Classify any exact UE claims
  separately; obtain pinned evidence only for implementation that relies on them.
- [x] Inventory changed symbols and consumers across all source/test roots in
  Durin.dworkspace. Select native fixtures, real CPython cases and consumer builds;
  do not add domain providers merely to demonstrate infrastructure.

Gate: one completion contract, one continuation model and one history per editor
have explicit ownership and testable behavior; no new parallel manager or result
cache remains without a demonstrated need.

### Stage 1: Unify native tool completion

Outcome: existing tools share ready/pending completion with bounded lifetime.
Depends on Stage 0.

- [x] Change IToolset/FNativeToolset invocation to the selected Future contract;
  adapt synchronous handlers to ready Futures and migrate all affected consumers.
- [x] Keep CallNow synchronous through pre-dispatch may-pend checks. Validate
  ready/delayed final results once at the service boundary; a contract violation
  cannot trigger an automatic retry after effects.
- [x] Extend existing invocation storage, queue and receipts through pending work;
  remove unused OperationId and duplicate result ownership. Cover subscription
  races, repeated/late completion and safe GameThread publication.
- [x] Add pending/consumer-retained accounting, backpressure and Configure checks.
  Preserve a live consumer's result through receipt expiry; release ownership
  correctly on completion, cancellation, unsubscribe and shutdown.
- [x] Implement cancellation/drain/detach using existing domain producers and work
  gates. Keep Console and MCP on their existing entry/result model; no transport
  or production domain feature expansion is needed.
- [x] Pass the native completion/capacity/lifetime matrix and affected consumer
  builds; update implemented Core/editor tool contracts.

Gate: ready and pending paths share invocation identity and one authoritative
client-visible outcome, and existing synchronous consumers remain compatible.

### Stage 2: Make transaction scopes safe across host boundaries

Outcome: old scopes cannot corrupt new recording and Modify reaches the right host.
Depends on Stage 1 for the ordered implementation path; reuses accepted native
transaction behavior rather than introducing a separate cross-frame manager.

- [x] Add internal scope validity using transaction identity and host lifetime,
  preserving token-free Begin/End/Cancel and accepted inner-cancel semantics.
- [x] Activate the recorder at host execution boundaries, including nested host
  switches and unbound native entry. Remove cross-frame dependence on LIFO recorder
  restoration while preserving the runtime-neutral CoreDObject bridge.
- [x] Test old scope cleanup after Cancel/End plus new Begin, host destruction,
  A opens/B opens/A resumes, non-LIFO completion and host retirement while another
  host records. Prove same-host callers still compose and different hosts do not.
- [x] Pass affected native replay/GC/package tests and consumer builds, and update
  implemented transaction contracts before enabling Python suspension.

Gate: stale helpers are inert, host binding is correct, synchronous Undo/Redo and
existing single-host recording remain valid. This gate precedes Stage 3 suspension.

### Stage 3: Add cooperative Python continuations

Outcome: Python can wait across editor ticks without blocking or duplicating native
completion/transaction state. Depends on Stage 2 and Stage 1's Future contract.

- [x] Add one callback/generator continuation model to existing jobs/script pump.
  Support ordered registration, bare yield, yield invocation, fair ready-job
  admission and bounded per-tick dispatch; preserve plain synchronous scripts.
- [x] Make Python handles view native completion: call(), submit(), result()/NotReady
  and yield-result/error conversion. Remove completed-only shims and result caches;
  never reenter Python inline from a completion notification.
- [x] Retain/rebind complete job context and references on every step. Keep native
  entry outside provider dispatch; release temporary host/module guards on return.
- [x] Demonstrate prepare/wait/revalidate/short apply producing one Undo entry,
  then explicit cross-yield global recording with interleaved UI/native/job edits.
  Cover nested/no-op scopes, duplicate Modify and native Undo/Redo availability.
- [x] Cover job cancellation/exception End versus explicit transaction Cancel,
  generator close/finally, late results, suspended host/session close, callback Stop,
  restart and finalization. A job publishes terminal status only after safe cleanup.
- [x] Apply the optional elapsed-time job deadline and remove inert timeout handling.
  Pass real-interpreter and runtime-disabled tests; update Python usage/architecture
  with actual blocking, global recording and cooperative cancellation behavior.

Gate: real editor ticks separate Python steps; results feed later edits; no stale
scope, cross-host capture, hidden wait, private history or implicit rollback occurs.

### Stage 4: Accept the integrated foundation

Outcome: the final relevant inputs have complete evidence and one set of owning
contracts. Depends on Stages 0-3; no new domain tool or MCP feature is a prerequisite.

- [x] Run a CPU/real-Python fixture covering native edits, pending completion,
  preparation plus short apply, explicit cross-frame recording, UI/native
  interleaving, two hosts, plain native Undo/Redo and cancellation without rollback.
  Use test-only providers/objects, not new production history or authoring tools.
- [x] Verify existing Console, MCP codec/HTTP and Python-job compatibility, including
  detached status/cancel while a script runs. Keep transport production source and
  protocol unchanged; generic service changes must not alter existing wire results.
- [x] Confirm workspace consumer coverage, runtime-disabled behavior and shutdown
  safety. Reuse earlier results only for unchanged relevant inputs/environment;
  rerun native coverage invalidated by scope/recorder changes.
- [x] Finish owning Core/tool/transaction/Python documentation, record tested
  revisions, host/preset, selected cases and report paths, and validate changed
  documents/all plans. Keep unanswered UE comparisons explicit rather than claiming
  parity. Complete this plan when these mechanism gates pass.

Gate: one completion model, one script pump and one history per editor pass the
final integration and compatibility matrix; deferred features do not block closure.

## Validation And Evidence

Follow the [build workflow](../Agents/BuildAndRun.md) and
[test selection/result reuse workflow](../Agents/Testing.md). Resolve registrations
on the validation host rather than assuming names from historical receipts.
Shared API changes require consumer searches and appropriate builds across Engine,
Sandbox and RoadWeaver. Require an all build when foundational headers/ABI or the
build graph prevent reliable bounded coverage; explain that risk before expanding
scope. The historical explicit Stage 3 all-build gate passed and is preserved below.

| Area | Required observations |
| --- | --- |
| Completion | Ready/inline/delayed success/error, completion before/during subscription, duplicate/late notification, abandoned producer, invalid/oversized final output and one publication |
| Admission and bounds | May-pend rejection before effects, no retry after synchronous contract violation, pending/retained count and byte exhaustion, backpressure recovery, Configure while pending and receipt eviction with a live consumer |
| Native lifetime | Unsubscribe versus cancellation, completion races, provider removal/replacement, host close, retained objects, module producer drain/detach and no callbacks into unloaded code |
| Recording | Old scope after replacement recording, A/B/A and non-LIFO host completion, host retirement, duplicate Modify and 0/1/2/3 final capture, inner Cancel, no implicit rollback and GC/package state |
| Python | Ordered/dynamic continuations, bare yield, yield handle and NotReady, fair dispatch, restored context, actual return-to-tick, no inline reentry and no nested editor loop |
| Cleanup and compatibility | Cancellation/finally, pending scopes/references, Stop/restart/owner transfer, optional elapsed deadline, runtime unavailable and existing Console/MCP/Python-job behavior |

Use deterministic tick-driven fixtures and the real embedded interpreter;
wall-clock sleeps alone do not prove suspension. The accepted local host is macOS
arm64 with the explicit CPython SDK override described below. Keep the separate
runtime-disabled gate. No new Windows, GPU or application-startup requirement is
introduced; the former domain-tool visible-editor acceptance is outside this scope.

Store raw evidence under ignored Build storage and concise receipts here. Final
acceptance requires evidence for final relevant inputs, not merely an unchanged
commit ID. This consolidation changes documents only; it claims no new native,
Python, UE-reference or transport execution.

Consolidation receipt (2026-10-18, base `aff860306`): changed-document validation
passes for the consolidated plan and repaired Actor Subsystem reference. All-plan
validation passes for 9 active, 2 completed and 425 archived plans. Plan context
resolves exactly five new stages and selects Stage 0; old filenames remain only
as historical provenance. Git diff whitespace checks pass. No build or native test
was run for this documentation-only change.

### Stage 1 receipt: shared native completion

Tested source: `922e3942f` plus the Stage 1 implementation in this commit,
macOS arm64, profile `macos-xcode-arm64`, Debug preset
`MacOS-arm64-Debug-DurinEditor`, jobs 8, existing CPython 3.13.15 SDK override.
Final matrix: CoreToolTests 19, CoreConcurrencyTests 180, EditorOperationTests 119,
ExternalToolTests 6, EditorShellTests 59, MCPCodecTests 12, MCPIntegrationTests 18,
MCPPythonJobTests 14 and real PythonToolIntegrationTests 32: **459 passed**, with the SDK-driver-only case conditionally skipped.
Raw XML/logs use those target names under `Build/EditorExecutionFoundation/`;
`stage1-final-consumers.log` records each launcher exit status. Earlier focused
runs are diagnostic evidence and are superseded by that final matrix.

The matrix covers scheduler-independent ready/pending storage, subscription races,
unsubscribe, producer abandonment, duplicate completion, ownership attachments,
may-pend rejection before availability/effects, synchronous contract violations,
GameThread final publication, provider retirement, pending/result count and byte
limits, cancellation acknowledgement versus applied success, Configure while
pending, receipt expiry with live result aliases and shutdown/late data completion.
Generic task-handle cancellation cannot overwrite detached completion outcomes;
only the producer acknowledges cancellation. No Python continuation is enabled.

The existing FEditorToolInvocation snapshot keeps its owned Result compatibility
surface and an authoritative-storage retention pin, avoiding Console/MCP production
source or wire changes. Python's old completed-result cache remains for Stage 3;
it is not used by the native completion path. Feature ABI version 2 and all direct
IToolset/native bindings were migrated. Searches cover Engine, Sandbox and
RoadWeaver; the latter projects have no direct Toolset completion consumers.
MainFrame/LevelEditor/DurinMCP/DurinPython compile through the selected consumer
targets. The foundational ABI risk still requires the Stage 4 workspace all build.

Changed-document validation, all-plan validation and diff whitespace checks pass.
Stage 2 must rerun recording/GC/package evidence after its own changes; this receipt
does not qualify the new transaction binding or cooperative Python gates.

### Stage 2 receipt: recording validity and host execution

Tested source: `c77edb0cb` plus Stage 2 in this commit. Host/profile/preset and
CPython SDK match Stage 1. Final relevant inputs pass EditorOperationTests 124,
CoreObjectTests 174, EditorPropertyTests 68, LevelMutationTests 51,
AssetPackageReloadTests 14, WorldTests 174, MaterialGraphEditingTests 104,
RoadEditorIntegrationTests 8, EditorHostToolTests 26 and real
PythonToolIntegrationTests 32. Five new stale-scope/host regressions also pass
isolated CTest execution. Non-editor CoreObjectTests: 174 passed on
`MacOS-arm64-Debug-DurinGame`, qualifying the changed runtime-neutral bridge.
Evidence: `Build/EditorExecutionFoundation/stage2-<Target>.xml/.log`,
`stage2-EditorOperationTests-isolated.xml/.log` and `stage2-game-core.xml/.log`.
Earlier passing targets remain applicable: later changes only migrated the
RoadWeaver test and added one separately qualified EditorOperationTests GC case.

The explicit cases cover End/Cancel plus replacement Begin, destroyed transactors,
A/B/A nested execution, idle-host capture suppression, unbound ambiguity,
non-LIFO recording completion, a destroyed prior binding, suspended recording
retention through GC and actual host hierarchy destruction releasing references.
Existing nested final capture, synchronous replay, package checkpoints, native
composition and real-Python synchronous scopes remain valid. Host bindings own
only callback execution; scopes continue to share each editor's global recording.

Workspace consumer validation exposed a stale RoadWeaver test using the retired
scope Modify API and expecting Cancel/rejected preview to clear Modify dirtiness.
It now calls/asserts Asset->Modify and checks the accepted dirty/no-history rules;
all eight RoadWeaver editor cases pass. No domain behavior was changed to preserve
those obsolete test expectations. Native helpers require an explicit host/recorder
binding when multiple unbound recordings exist. The final workspace all build
remains Stage 4's shared ABI gate. No application smoke or GPU gate is claimed.

Changed-document/all-plan validation and diff whitespace checks pass. The Stage 2
gate is accepted before adding any Python continuation or suspension state.

### Stage 3 receipt: cooperative Python and native result views

Tested source: `1025cee18` plus Stage 3 in this commit, macOS arm64,
`macos-xcode-arm64`, `MacOS-arm64-Debug-DurinEditor`, jobs 8, the unchanged
explicit 3.13.15 GIL SDK override. The final matrix passes CoreConcurrencyTests 180,
CoreToolTests 19, EditorOperationTests 124, PythonToolIntegrationTests 46,
MCPIntegrationTests 18, MCPPythonJobTests 14, MCPCodecTests 12,
EditorHostToolTests 26, ExternalToolTests 6 and EditorShellTests 59: **504 passed**, with one SDK-driver-only case conditionally skipped.
The 16 selected continuation/Stop/cleanup cases pass isolated CTest; the updated
prepare/revalidate fixture was also requalified alone. Runtime-disabled
PythonRuntimeUnavailableTests: 1 passed. Enabled configuration was restored and
all final tool/Python/adapter consumers were rebuilt and rerun.

Evidence under `Build/EditorExecutionFoundation/`: `stage3-CoreConcurrencyTests.xml`,
`stage3-CoreToolTests.xml`, `stage3-accepted-<Target>.xml/.log`,
`stage3-python-isolated.xml/.log`, `stage3-prepare-isolated.xml/.log` and
`stage3-python-disabled-final.xml/.log`. `stage3-acceptance.log` records final
consumer exit statuses; final enabled/disabled configure logs retain SDK settings.
Later fixture-only changes affect preparation/fairness assertions; final whole-target
Python execution covers them. Other isolated cases and Core inputs are unchanged.

One existing runtime queue now rotates jobs, each taking at most one step per round,
16 steps/2 ms between-step budget. `durin.defer` accepts callbacks/generators;
registration order, dynamic append, bare yield, native-handle yield/send/throw and
NotReady are covered. Ready SubmitNow keeps synchronous handle compatibility while
admitting a native identity/Future before effects. Invocation cancellation and job
cancellation compose existing read-only tokens, with separate sources. The optional
JobDeadline measures admission-to-cleanup elapsed time, including suspension;
the mandatory 30 seconds and per-call/result timeout shim are removed.

Real cases prove result-dependent preparation/revalidation/short apply, explicit
cross-yield global recording with UI/native/other-job edits, two suspended hosts
with non-LIFO completion, ordinary native Undo/Redo, inner Cancel without rollback,
late completion, unconsumed invocation settlement, cancellation/finally, host close,
Stop/restart and interpreter ownership transfer. Cleanup also rejects new work from
cyclic finalizers and reports generator or transaction final-capture failures before
terminal publication. Host deinitialization invokes CloseEndpoint on the same pump;
no worker interpreter, nested tick, private history or second result cache is added.

The final ownership audit removed copied lookup result trees: FEditorToolResultView,
receipt Futures and Python handles now share immutable terminal storage. Legacy
optional materialization occurs only at adapter conversion; MCP production source
and protocol remain unchanged. Address-identity and live snapshot/alias TTL tests
prove retention/backpressure. Raw output trees are bounded before byte traversal
or copying. Core-owned FTaskReadiness notification code/data supports safe observer
detach from late producers; generic callback owners still drain in-flight code
before unload. These are small extensions of existing completion hooks/accounting.

Owning Core/tool/transaction/Python/MCP contracts and usage are updated. Changed-doc,
all-plan and whitespace validation pass. Shared ABI/workspace coverage and the final
native integration rerun remain explicit Stage 4 gates; no application/GPU or exact
UE 5.8 parity claim is introduced.

### Stage 4 receipt: integrated foundation accepted

Tested production: `cca6f4fb1` plus the garbage-owner bridge correction in this
commit. Final fixture changes and production diff fingerprints are recorded in
`Build/EditorExecutionFoundation/acceptance.json`. Host: macOS arm64, profile
`macos-xcode-arm64`, Debug, jobs 8, Tracy enabled, existing explicit CPython
3.13.15 release-GIL SDK. Final editor preset: `MacOS-arm64-Debug-DurinEditor`,
DURIN_ENABLE_PYTHON=ON. Source/toolchain/SDK inputs are unchanged after validation;
only these receipt/status edits follow the final runs.

| Final gate | Result | Evidence under Build/EditorExecutionFoundation/ |
| --- | --- | --- |
| Editor CPU and real Python | 1,100 passed, zero failures; one SDK-driver-only case skipped | stage4-acceptance.log, stage4-<Target>.xml/.log, acceptance.json |
| Non-editor object/bridge behavior | CoreObjectTests: 174 passed on MacOS-arm64-Debug-DurinGame | stage4-CoreObjectTests-game.xml/.log |
| Runtime unavailable | PythonRuntimeUnavailableTests: 1 passed with Python OFF | stage4-python-disabled.xml/.log, stage4-disabled-configure.log |
| Restored real runtime | PythonToolIntegrationTests: 47 passed with Python ON | stage4-python-restored.xml/.log, stage4-enabled-configure.log |
| Independent critical cases | Native lifetime 6, Python continuations/lifecycle 17, pending MCP 1 passed through isolated CTest | stage4-native-isolated.xml/.log, stage4-python-isolated.xml/.log, stage4-mcp-isolated.xml/.log |
| Workspace ABI/consumer compile | ./DevTool build all passed, covering Engine, Sandbox and RoadWeaver; final enabled runtime closure restored | stage4-workspace-restored-all.log; initial full workspace pass in stage4-workspace-all.log |
| Non-editor Engine compile | ./DevTool build Engine --preset MacOS-arm64-Debug-DurinGame passed | stage4-game-engine-final.log; initial pass in stage4-game-engine.log |
| Documents and plans | Changed-document/all-plan validation and whitespace checks pass | Final repository validator output |

The editor matrix comprises CoreToolTests 19, CoreConcurrencyTests 180,
CoreObjectTests 174, EditorOperationTests 125, EditorPropertyTests 68,
LevelMutationTests 51, AssetPackageReloadTests 14, WorldTests 174,
MaterialGraphEditingTests 104, RoadEditorIntegrationTests 8, EditorHostToolTests 26,
ExternalToolTests 6, EditorShellTests 59, MCPCodecTests 12, MCPIntegrationTests 19
passed plus 1 conditional skip, MCPPythonJobTests 14 and real
PythonToolIntegrationTests 47. This is 1,275 passed distinct configuration cases
including non-editor and disabled-runtime gates; the isolated runs are additional
executions of selected cases. The SDK-only ServesOfficialSDKClient case requires
MCPClientSmoke.py and its control environment and is intentionally skipped by the
ordinary target invocation. It is not claimed passed; the selected native HTTP,
codec and Python-job compatibility gates pass without adding a new SDK/app gate.

The integrated real-Python fixture uses actual DEditorEngine ticks: preparation
waits outside recording, an intervening UI revision invalidates a proposed apply
before effects, and a later explicit recording includes native/UI/other-job edits.
A second host's native history stays independent. Job cancellation runs finally,
Ends the still-valid global recording without restoring edits, and preserves one
grouped native Undo/Redo entry. Late preparation results cannot revive the job or
reapply data. Separate real-Python cases prove successful revalidation/short apply
and two suspended Python hosts completing non-LIFO. Only test providers/objects
were added; production domain catalog and MCP transport/protocol remain unchanged.
The pending native HTTP case verifies that raw producer completion is not an early
wire success: final validated data appears only after the owning native tick.

Final inspection exposed the garbage-marked/deferred-destruction interval: restoring
an old execution binding checked owner storage but not DObject liveness. The new
regression failed before the guard and passes after it; candidate selection and
prior-binding restoration now both reject garbage-marked owners. Evidence:
`stage4-garbage-binding-before.xml/.log` and `stage4-garbage-binding-after.xml/.log`.
The final native replay/GC/package and non-editor matrices, real Python, isolated
cases and workspace builds all cover this correction. The earlier passing build
alone was not substituted for final-input evidence.

Implemented contracts now have one authoritative completion path, one script pump
and one history per editor. No GUI application, GPU, Windows, new domain tools or
new protocol acceptance is claimed or required by this plan. The pinned UE 5.3.2
baseline remains historical reference; unanswered exact UE 5.8 comparisons remain
explicitly open and were not used to implement or qualify these Durin policies.

## Accepted Baseline Evidence

These receipts refer to the original transaction plan's stage numbers. They retain
historical acceptance and do not mark this consolidated plan's new stages complete.
Native Stages 0-3 were accepted in `c2a2fad14` against `ce42cf32b`; synchronous Python
Stage 4 was accepted in `7527b2715`. Earlier planning baseline:
`1c0c3718c0e02f5a0b27fc8d3266e7eb87a9171a`.

The nested 0/1/2/3 regression failed before the native fix (Redo 2 instead of 3),
then passed with transaction-owned final capture. Evidence remains under
`Build/UETransactionAlignment/stage0-nested-before.xml` and
`stage1-nested-after.xml`. Actor root-transform capture also failed before forwarding
was added (`actor-modify-before.xml`). Native workspace searches covered Engine,
Sandbox and RoadWeaver and found no production Deferred transaction consumers.

### Accepted native milestone: original Stages 0-3

Tested source: base `ce42cf32b` plus the original Stage 0-3 implementation. Later changes
to this receipt are documentation only. Host: macOS arm64, profile
`macos-xcode-arm64`, Debug, build concurrency 8. Python/MCP runtime and adapter
sources were unchanged at this native receipt. Stage 4 evidence is recorded
separately below. These receipts do not validate the new execution mechanisms.

| Gate | Selection and result | Retained evidence under Build/UETransactionAlignment/ |
| --- | --- | --- |
| Native CPU matrix | 19 targets, 1,589 cases passed on MacOS-arm64-Debug-DurinEditor | native-acceptance.json and each target's XML/log |
| Workspace compile | `./DevTool build all` passed; includes Engine, Sandbox, RoadWeaver and their editor consumers | workspace-all.log, gates.json |
| Non-editor compile | `./DevTool build Engine --preset MacOS-arm64-Debug-DurinGame` passed | game-engine.log |
| Non-editor object behavior | CoreObjectTests: 174 passed on MacOS-arm64-Debug-DurinGame; runtime-neutral Modify bridge also passed alone | game-core.xml, game-bridge-alone.xml |
| Isolated nested regression | NestedModifyPreservesOutermostFinalState passed through isolated CTest execution | nested-isolated.xml |
| Reflection generator | `.venv/bin/python -m pytest Engine/Source/Programs/DurinHeaderTool/tests -q`: 418 passed | dht.xml |
| Documentation | Changed-document validation and all-plan validation passed | Repository validator output |

The CPU matrix covers CoreUtilityTests, CoreObjectTests, EditorOperationTests,
EditorPropertyTests, LevelMutationTests, MaterialGraphEditingTests, ViewportTests,
EditorShellTests, AssetPackageReloadTests, WorldTests, ContentBrowserWorkflowTests,
EditorAssetWorkflowTests, EditorHierarchyTests, EditorHostToolTests,
ExternalToolTests, SplineTests, SkyBoxTests, StaticMeshTests, and TextureTests.
It includes root-transform recording, generated-component retirement/GC,
Transient/NonTransactional nested struct filtering, excluded-reference release,
historical validator bypass, cancellation without native rollback, saved-state
handling, synchronous composition, independent observers, module retirement
reentrancy, and truthful accounting/recovery failures.

The matrix was selected after `test affected --explain` expanded to all ordinary
tests because foundational headers and shared test support changed. Named CPU
targets cover the affected native owners/consumers; the separate explicit all
build covers remaining ABI and workspace compilation risk. No GUI application,
GPU qualification, other-platform execution, or real Python/MCP integration is
claimed. Those are outside this native milestone; supported-host CPython and
adapter integration were qualified separately in the original Stage 4 receipt.

The final removed-symbol search covered every workspace project's source/test
roots and found no transaction callers of the retired scope tokens, checked
preparation, Execute/CommitApplied, Deferred completion, synchronous qualification,
or ConsumeEvents surface. Native contracts now document the implemented model.

### Accepted synchronous Python milestone: original Stage 4

Tested source: `c2a2fad14` plus the original Stage 4 implementation. Host/profile/preset:
macOS arm64, `macos-xcode-arm64`, `MacOS-arm64-Debug-DurinEditor`, jobs 8.
Real CPython uses the explicit `DURIN_PYTHON_SDK_ROOT` override with the same
3.13.15 release GIL ABI. The user accepted this local host instead of requiring
Windows execution. Native transaction implementation and the Stage 3 broad
workspace/ABI evidence remain applicable; no native recording code changed.

The detached path is limited to editor.python status/cancel during a script pump.
The catalog refresh precedes execution, exact registration/source/client/schema
checks remain, and feature/host scopes pin code and editor lifetime. Controls
read owned runtime/job state only, including when the ordinary queue is full.
The worker interpreter, tool receipt polling, per-call Tick wait and worker join
were removed. Handles now contain synchronous results; timeout cannot preempt
native work. These contracts live in [PythonTools](../Editor/Architecture/PythonTools.md).

| Gate | Result | Evidence under Build/UETransactionAlignment/Stage4/ |
| --- | --- | --- |
| Real interpreter | PythonToolIntegrationTests: 32 passed | python-final.xml/.log |
| Isolated critical cases | Nested result-dependent Undo/Redo, detached controls, callback Stop and owner transfer: 4 passed | python-isolated.xml/.log |
| Native tools/consumers | EditorOperationTests 112, EditorHostToolTests 26, ExternalToolTests 6, EditorShellTests 59 passed | Each target's XML/log |
| Existing transport compatibility | MCPCodecTests 12, MCPIntegrationTests 19, MCPPythonJobTests 14 passed | Each target's XML/log |
| Runtime unavailable | PythonRuntimeUnavailableTests: 1 passed with DURIN_ENABLE_PYTHON=OFF | python-disabled.xml/.log |
| Consumer compilation | MainFrame passed with the restored enabled runtime | mainframe.log |
| Documentation | Changed-document and all-plan validation passed | Validator output |

The real-runtime matrix includes nested scopes and native domain composition,
0/1/2/3 final-state capture, one grouped entry with Undo/Redo, exception End,
inner Cancel without rollback, job cancellation preserving undoable applied edits,
actual 30-second deadline, stale handles/scopes, shared imports/job reuse, two
explicit hosts, queued and running shutdown, finite native return and owner-thread
finalization/restart and process-owner transfer. The owner-transfer regression
failed before the final lifecycle correction (`ownership-before.xml`): another
runtime was incorrectly rejected after callback Stop had finalized the interpreter.
Ownership now releases at actual finalization, and retiring the old registration
cannot release a new runtime's reservation. Existing HTTP coverage now pumps scripts explicitly and
cancels a running GameThread script from the HTTP thread. MCP source/protocol and
transaction exposure were not changed.

The official macOS installer is
[CPython 3.13.15](https://www.python.org/ftp/python/3.13.15/python-3.13.15-macos11.pkg),
SHA-256 `3b7eaf7f29825f796e8267024435540ddf1f17fc9a97ad58095daa7a75bfdcd3`.
Its GIL framework was extracted locally, copied into the documented SDK layout,
and given a local ad-hoc library signature. Source-only ZIP_STORED archives avoid
requiring a native zlib extension for startup. `acceptance.json` records SDK
hashes and XML totals. No Windows, application-startup or GPU execution is claimed.

`test affected --explain` expanded to Engine tests because the new optional-runtime
test source was not in the previous registry. Named CPU selections cover the
changed Python/tool-service contracts and consumers; workspace-wide symbol search
found no Sandbox/RoadWeaver consumers to migrate. The restored enabled preset
reuses the same SDK/source/package inputs as the passing real-runtime receipt.

## Required References

- [Core Toolsets](../Runtime/Core/Tools.md),
  [editor ToolService](../Editor/Architecture/EditorTools.md),
  [Python runtime](../Editor/Architecture/PythonTools.md),
  [Python usage](../Editor/Guides/PythonScripts.md), and
  [existing MCP adapter](../Editor/Architecture/MCPTools.md).
- [Transactors](../Editor/Architecture/Transactors.md),
  [transaction records](../Editor/Architecture/TransactionRecords.md),
  [property replay](../Editor/Architecture/ReflectedPropertyEditing.md),
  [function actions](../Editor/Architecture/EditorFunctionActions.md), and
  [GC ownership](../Runtime/Core/GarbageCollection.md).
- Related code: [Toolset API](../../Engine/Source/Runtime/Core/Public/Tools/Tool.h),
  [native bindings](../../Engine/Source/Runtime/Core/Public/Tools/NativeToolset.h),
  [task composition](../../Engine/Source/Runtime/Core/Public/Threading/TaskComposition.h),
  [transactor API](../../Engine/Source/Editor/DurinEd/Public/Transactions/Transactor.h),
  [recording implementation](../../Engine/Source/Editor/DurinEd/Private/Transactions/Transactor.cpp),
  [tool dispatch](../../Engine/Source/Editor/DurinEd/Private/Tools/EditorToolService.cpp), and
  [Python execution](../../Engine/Source/Editor/DurinPython/Private/PythonRuntime.cpp).
