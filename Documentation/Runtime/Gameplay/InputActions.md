# Keyboard and Mouse Input Actions

Summary: Define session-owned action evaluation, context consumption, cancellation, and persistent keyboard/mouse binding overrides.

Modules: Engine, MonaCore, Mona, MonaImGui, Sandbox

Last reviewed: 2026-09-14

## Ownership and Evaluation

`DEngine` owns the enabled game window's `FGameInputState`. It accumulates key,
mouse-button, position, delta, and wheel state, plus ordered digital transitions.
`FinishGameTick` advances its sample number and clears transient data. Reset
advances a separate reset number, clears input and mouse tracking, and rejects
keyboard repeat events as a way to reconstruct a held key after focus loss.

Each `APlayerController` owns an `FInputActionEvaluator`. Definitions and contexts
are copied ordinary C++ values, without reflected input assets or global action
state. `DefineAction` registers a stable string identity and Button, Axis1D, or
Axis2D type. `AddContext` registers named, stable binding slots with physical
sources, axis scales, and consumption flags. Unknown snapshot reads are neutral.

The controller evaluates the sample before calling
`BuildControlIntent(const FInputActionSnapshot&)`. Derived controllers read
logical actions and translate them into `FPawnControlIntent`; Pawn and movement
components remain independent of keyboard identities. Repeated evaluation of an
unchanged sample returns the same snapshot. Consumers must submit its transient
intent once; the World/controller seam owns that scheduling.

Buttons aggregate digital sources with OR semantics. Ordered transitions preserve
same-frame taps and overlapping alternate bindings: releasing one key does not
end an action still held by another. Axis bindings sum scaled values; opposing
digital bindings cancel. Mouse axes are per-sample deltas, not velocities, and
must not be multiplied by delta time. Game-specific filtering and sensitivity
remain in the controller.

## Context and UI Ownership

Active contexts resolve by descending priority, with insertion order breaking
ties. A consuming binding reserves its physical source against lower contexts,
regardless of the action name or current value. Non-consuming bindings allow
lower contexts to observe the same source. Keyboard and mouse modal flags block
all remaining sources in those domains after the context's own bindings resolve.
Duplicate physical sources within one context are rejected; overlaps between
contexts are intentional. This first slice does not implement modifier chords.

An editor command can therefore use a higher context to reserve `W` against a
lower gameplay `Move`, and a UI modal context can reserve both domains. Context
activation and removal are explicit owner operations. The existing editor
shortcut collection is not automatically migrated or registered as contexts.

Mona continues delivering normalized physical events to both UI and gameplay,
including releases. `FMonaEventHandler::GetInputCapture` exposes UI domain
ownership separately. Engine samples that ownership before World evaluation;
it does not interpret an event handler's return value as permission to drop a
release. ImGui text editing and popups block gameplay; an uncaptured ImGui window
also uses `WantCaptureKeyboard` and `WantCaptureMouse`. A captured game viewport
does not count as ImGui keyboard navigation or ordinary mouse capture. Escape
and click-to-capture remain host policy.

## Cancellation and Resumption

Snapshots contain a value, active state, and independent `bStarted`, `bCompleted`,
and `bCancelled` edges. A tap can start and complete within one sample. Natural
release or returning an axis to zero completes an action. Forced interruption
cancels it and sets its value to zero. Cancellation is immediately visible and
retained for the next evaluation once, including when no gameplay Tick runs.

Context changes and binding overrides conservatively cancel the evaluator's
active actions and suppress previously observed held sources until release.
Device capture changes cancel only actions using the changed domains. Losing
eligibility through routing also suppresses held sources, so dismissing a menu
does not resume movement without a fresh press. Pending mouse delta is discarded
when reconfiguring an already-sampled evaluator.

World pause, stop, restart/possession changes and controller teardown cancel
actions and pending Pawn intent. Paused World ticks still evaluate blocked input
to observe releases; they do not create control intent. Single-step admits one
eligible current sample, preserving the requirement for a fresh press after
interruption. Engine input disable, window replacement, and focus loss cancel the
controller immediately. Physical reset also carries a reset number so a reset
and reactivation between evaluations cannot hide the interruption.

## Binding Overrides

`Rebind(Context, Slot, Source)` validates a detached candidate before
changing live mappings. Unknown slots, invalid physical codes, delta sources for
buttons, and duplicate effective sources within a context are rejected with a
diagnostic. `GetBindingSource` returns the effective source; `ResetBindings()`
restores defaults in memory and returns `void`. These operations run on the owning game thread between input
evaluations. They do not invoke callbacks during routing.

`Rebind`, `LoadOverrides`, `SaveOverrides`, and the Sandbox persistence wrappers
return `FInputBindingResult`. `Error` is an `EInputBindingError` code for branching;
`Message` is diagnostic text and must not be parsed. Success has `Error == None`
and converts to `true`. Unknown context, unknown slot, invalid source, reserved
source, conflict, unsupported format, invalid file, read failure, and write
failure have distinct codes. Oversized or malformed records and duplicate slots
are invalid files; unsupported source-kind/code values are invalid sources.

When available, `Context` and `Slot` identify the failing binding. For direct
rebinding conflicts, `Slot` is the requested slot and `ConflictingSlot` names its
existing peer regardless of declaration order. File-load conflicts identify both
slots in deterministic definition order. File-wide failures leave these fields
empty. Intermediate callers propagate the result; the operation owner decides
whether to display or log it. Sandbox logs startup-load failures at BeginPlay and
returns interactive rebinding/persistence failures to its caller.

`SaveOverrides` atomically publishes a versioned UTF-8-compatible text file using
Core file publication. Each record contains quoted context and slot identities,
source-kind number, and source code. Only overrides are saved. `LoadOverrides`
bounds file size to 1 MiB and validates the entire detached file, including
duplicate and unknown slots, before replacing live state. Failure preserves the
previous mappings; a successful load cancels old actions. Files from incompatible
binding schemas are rejected rather than partly applied.

Sandbox loads `Saved/Configs/SandboxInput.bindings` beneath `FPaths::LaunchDir()`
at controller BeginPlay. Missing files use defaults; malformed files report a
warning and preserve defaults. `RebindControl` and `ResetControlBindings` save a
candidate first and publish it only on successful persistence. Escape is reserved
by the Sandbox host capture policy. `Interact` is available as a button action;
it does not implement target discovery or an interaction system.

## Validation

`InputActionTests` covers tap ordering, multi-binding aggregation, axis samples,
priority, tool/UI competition, modal consumption, cancellation and held-source
suppression, focus reset, and transactional override files. `WorldTests` covers
controller lifecycle and pause/single-step/restart integration.
`SandboxGameplayTests` covers default and rebound controls plus existing movement
and camera behavior. Interactive GUI smoke is supplementary to these native
gates. See [Sandbox gameplay](SandboxGameplay.md) for the concrete control map.
