# Slow Task Feedback

Summary: Define shared slow-task progress, lexical work scopes, cancellation, and presentation snapshots without owning execution or an event loop.

Modules: Core, DurinEd, MainFrame, LevelEditor, AssetForgeBuiltins

Last reviewed: 2026-10-27

## Ownership and Lifetimes

`Misc/SlowTask.h`, `Misc/ScopedSlowTask.h`, and `Misc/FeedbackContext.h` expose
`FSlowTask`, `FScopedSlowTask`, and `FFeedbackContext`. Core contains no ImGui,
Engine, editor, native-window, or work-scheduler dependency for this service.

`FSlowTask` is a noncopyable feedback lifetime that can be held across ordinary
host frames. Its owner explicitly calls `Complete`, `Fail`, or `Cancel` when the
corresponding operation ends. Destruction removes feedback without publishing a
business result. Terminal transitions are idempotent; later updates cannot
overwrite the first result or diagnostic. Elapsed time freezes at termination;
subsequent snapshots and eligibility checks do not consult the clock again.

`FScopedSlowTask` adds lexical work accounting. It registers on the context's
scope stack for its construction thread. Normal scope exit completes accounting;
exception unwinding fails the scope, and a pending cancellation request cancels
it. These accounting results do not emit success or failure notifications. A
scope must be destroyed on its construction thread. Nested scopes should fit
inside their parent's lifetime; parent completion or destruction invalidates
unfinished descendants.

`FFeedbackContext` serializes updates, cancellation, and snapshot reads, with
independent lexical stacks for each producer thread. Its default instance is
thread-local. Worker producers that need main-thread presentation must use an
explicit shared context observed by the host. Retiring or destroying a context
invalidates surviving task handles: updates become inert, snapshots disappear,
and `ShouldCancel` becomes true. No producer callbacks are retained by the UI.

## Progress and Cancellation

Positive total work enables measured progress. `EnterProgressFrame` closes the
previous frame and reserves a bounded amount of the remaining total for the new
frame. A nested scope contributes its fractional completion to that reservation.
Sequential child operations should reserve separate parent frames. An active
child's message supplies the parent's current phase text; after child retirement
the parent message resumes.

Zero, negative, or nonfinite total work is indeterminate. `SetProgress` adapts an
external fraction, clamped to `[0, 1]`; `nullopt` or a nonfinite fraction means
unknown. An indeterminate child makes its reserved parent frame indeterminate.
Phase indices and product counts are not converted into estimated percentages.

`MakeDialog` requests immediate presentation. `MakeDialogDelayed` measures its
threshold from task creation, so tasks that finish before the threshold never
display a dialog. `GetVisibleTask` selects the newest eligible request and
returns a detached `FSlowTaskSnapshot`. Finished tasks are never eligible. A
snapshot includes title, current message, optional fraction, elapsed time,
details, status, and cancellation availability.

`RequestCancel` accepts an eligible cancellable task ID once. The request is
shared with its lexical descendants, but does not terminate work automatically.
Owners poll `ShouldCancel` at safe boundaries and then stop or roll back their
own operation. Independent root tasks do not share cancellation. `Cancel`
records the terminal result once cancellation has been handled.

## Editor Presentation and Execution

`DurinEd` owns `Feedback/SlowTaskWidget.h`. `DrawSlowTaskProgress` renders the
shared centered message, optional title/details, thin progress indicator, elapsed
time, and optional cancellation control. `FSlowTaskDialogPresenter` adds a modal
shell around an optional running snapshot and returns a cancel click to its
owner. Missing or terminal snapshots close the modal. Its feedback-context
overload obtains the eligible snapshot and forwards cancellation to that context.
MainFrame continues to observe the default owner-thread context for generic slow
tasks on ordinary UI frames.

MainFrame startup projects bootstrap state and, when available, scene preparation
progress directly into a snapshot. It displays the scene operation's elapsed
time during scene loading and hides timing where the bootstrap model has none.
The Level document controller projects `PendingSceneLoad->GetProgress()` into a
modal snapshot only for interactive opens. Cancel calls the existing scene
operation's cancellation interface; ordinary owner frames still process its
terminal result, publish deferred document completion, and commit successful
activation. Default-Level startup shares the loading path without a second
modal. Neither startup nor Level switching retains a feedback Task or context
alongside its authoritative business state.

Existing business progress can also project directly to `FSlowTaskSnapshot`
without creating a task or feedback context. `DrawSlowTaskProgress` accepts these
snapshots and can hide elapsed time when the business model has no timing data.
The returned cancel click belongs to the business owner; no task ID or feedback
admission is needed for this embedded presentation.

FBX and glTF import dialogs use the pure `MakeAssetImportFeedback` function to
project phase-local counts, activity, and cancellation from the session. They
embed the shared widget and call `Session.Cancel()` directly. The session owns
all execution, cleanup, terminal results, saved outputs, and cancellation state;
its public tick publishes cancellation after phase transitions replace progress.
There is no additional task lifecycle, modal, or cancellation latch in the form.

The Canonical Resave window likewise projects its pending plan, completed count,
and existing cancel flag directly into an inline snapshot. Cancellation is
checked before admitting the next package. Plan validation, per-package atomic
publication, partial results, the subsequent audit, and project-change cleanup
remain business-state-machine responsibilities. No parallel feedback state is
created or synchronized for these workflows.

These interfaces follow the nesting and work-frame concepts of Unreal's
[FScopedSlowTask](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Core/FScopedSlowTask),
the dialog and cancellation APIs of
[FSlowTask](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Core/FSlowTask),
and the feedback boundary of
[FFeedbackContext](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Core/FFeedbackContext).
They are not an API-compatible implementation. Durin's context owns progress
feedback; Unreal's output-device and warning-history responsibilities are outside
this service. In particular, this engine supports persistent cross-frame task
owners and detached snapshots. No `MakeDialog` or progress update pumps UI,
tasks, or a nested engine tick. Main-thread work that blocks an entire host frame
cannot refresh this UI mid-call: divide it across frames or publish progress from
a worker through an explicit shared context. Execution remains governed by the
[runtime lifecycle](../Application/RuntimeLifecycle.md).

## Example

```cpp
FScopedSlowTask Import(100, "Importing asset...");
Import.MakeDialogDelayed(0.25, true);
Import.EnterProgressFrame(40, "Reading source");
ReadSource();
if (Import.ShouldCancel()) return;
Import.EnterProgressFrame(60, "Building products");
BuildProducts();
```

The example demonstrates work accounting. Its default dialog is visible only
when the owner supplies ordinary UI frames during the operation; a blocking
main-thread call is not made responsive by the feedback API.
