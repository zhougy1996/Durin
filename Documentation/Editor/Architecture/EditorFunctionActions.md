# Reflected Editor Function Actions

Summary: Define single-object CallInEditor buttons, invocation admission, and automatic object transactions.

Modules: DurinEd, CoreDObject, Engine

Last reviewed: 2026-10-16

## Shared Details Layout

The [shared Details layout](PropertyDetailsViews.md#object-view-customization)
uses `DrawDetailPanel` to append effective inherited `CallInEditor` functions after
customizations, host extensions, and default properties. Buttons share category
ordering, tooltips, search, and visible-row counts. Search includes method name,
declaring type, display name, category, and tooltip. Widget identity uses object
and root function identity independently of the display label. Empty states
account for both properties and actions.

Only void/no-argument signatures are action rows. Parameterized or returning
functions remain reflected, but automatic and explicit placement exclude them;
execution admission repeats that signature check even when `CallInEditor` is set.

`IDetailLayoutBuilder::AddFunction` places the inspected object's effective slot;
category builders can place it in another category. Explicit placement suppresses
default duplicates. `HideFunction` hides the slot, including explicit rows.
`HideCategory` hides both row types. `ReplaceDefaultProperties` remains
property-only. Layout code contains no Actor, asset, or project-specific branches.

Clicks produce `FEditorFunctionRequest` identities: exact `FObjectKey`, function
name, and root owner. Execution occurs after traversal and table closure, with
no reuse of property addresses or row pointers afterward. Layouts rebuild on
every draw; retained hosts consume `bRefreshRequested` for derived presentation.
`bActionInvoked` reports a returned native call independently of `bChanged`,
which reports a recorded authored change. A no-op or failed history commit is
not a successful recorded change merely because native code ran.

## Admission And Lifetime

The non-ImGui `ExecuteEditorFunction` completes the active property edit through
the checked `FPropertyView::FinishActiveEdit` terminal operation. Failure blocks
invocation; success keeps the property edit and action as separate history
entries. It resolves the request again and checks game thread, read-only policy,
liveness, compatible effective slot, flag, non-template target, host transactor,
idle standalone admission, and reentrancy. Known unavailable buttons are disabled
with a reason; execution repeats all checks. Wrong-thread and recursive requests
cannot touch the active edit.

A history-budget discard is a failed property-edit completion, not a successful
finish. The preview remains cancelable and blocks action execution. If its scope
has already closed, further commit attempts reject with a cancellation instruction
rather than silently accepting an edit without history.

Target and host transactor are strongly retained during synchronous execution.
Requests do not retain objects across frames. There is no fallback to another
editor's history. `CanTransact()` admits Idle and ordinary Recording through the abstract service.
Nested actions join that recording; replay, recovery, and destruction remain unavailable.

## Transactions And Author Obligations

The adapter requires recorded mode and opens a normal scope on the active
editor's transactor with the action label,
slot identity, and primary target, then calls `Target->Modify()`. The return value
reports recording and does not veto invocation; nontransactional native actions
can still execute. Capture failures remain available through diagnostics.

Invocation enters the target's virtual `DObject::ProcessEvent` with an explicit
empty parameter view; the adapter does not bypass object dispatch or use a
separate no-argument native entry. After native return, the adapter sends one full-object `PostEditChange()` before
capturing final state and explicitly ending the scope. No changed leaf path is
invented. The body must preserve its invariants; arbitrary C++ calls cannot use
detached pre-write validation. Adapted methods should not duplicate this terminal
notification. Undo/Redo restore before/after values through ordinary property
notifications; they never run the method again. Derived state must reconstruct
through edit and replay hooks. Unchanged state creates no history entry, but Modify still marks the package
dirty. Successful recording uses existing package revisions/checkpoints.

Capture follows virtual Modify; Actor includes its native root component. It
does not traverse arbitrary participants. Methods must not edit other uncaptured
objects/components, change
ownership, create/delete objects, change files, perform external effects, destroy
the target, pump events, open modal loops, or start deferred authored work.
Nested scopes share the transaction and do not extend the action adapter's single-object capture contract.
Reconstructible transient derived caches are allowed. Complex commands need an
explicit domain/custom-change adapter; a signature cannot prove undoability.

`Cancel` discards history, not mutations. Final snapshot or history-budget failure
may follow native execution. The result preserves `bInvoked` and the history
failure, reports through `Context.ReportError`, requests refresh, conservatively
marks the target package dirty, and invalidates its saved checkpoint. There is
no automatic retry or promise of atomic rollback.

## Related Documentation

- [Native function reflection](../../Runtime/Core/FunctionReflection.md)
- [Reflected property editing](ReflectedPropertyEditing.md)
- [Property Details views](PropertyDetailsViews.md)
- [Transactors](Transactors.md)
