# Keyboard and Mouse Input Actions Plan

Summary: Add session-owned keyboard and mouse actions, deterministic contexts, cancellation, and binding overrides, verified through Sandbox gameplay.

Last reviewed: 2026-09-14

Status: Archived
Completed: 2026-09-14

## Current Status

Both stages are complete. Action evaluation, context routing, controller
lifecycle, ImGui capture bridging, Sandbox migration, and persistent overrides
are implemented. The lasting contract is [Input Actions](../../../Runtime/Gameplay/InputActions.md).

Validation on Windows/MSVC, `Win64-Debug-DurinEditor`:

- The shared Engine API `all` build passed (15.16 seconds).
- The bounded `@kind=contract+feature,domain=input+gameplay+world+viewport+editor-shell`
  native selection passed all 12 targets and 360 cases. This includes
  InputActionTests (12), SandboxGameplayTests (13), and WorldTests (145).
- `test affected --explain` selected unrelated rendering/asset integration
  targets through the shared Engine module; the bounded input, gameplay, World,
  viewport, and editor-shell set covers this CPU input change without GPU work.
- Changed-document validation, all-plan validation, and whitespace/diff review
  passed. The implementation and completed checklist are committed together with
  Stage 1 provenance.
- No interactive GUI smoke was run. The editor's existing shortcut collection
  remains outside the migration; a competing tool context is covered by native
  priority/consumption tests. No settings editor or interaction-target system was
  added.

## Goal

Provide ordinary C++ action definitions and runtime evaluation above
`FGameInputState`. Preserve Pawn intent and existing Sandbox movement/camera
behavior. Support Button, Axis1D, Axis2D, keyboard and mouse bindings, stable
priority ordering, physical-source consumption, modal device blocking, explicit
cancellation, and persistent user overrides. Do not add input assets, gamepads,
complex trigger graphs, or migrate every editor shortcut.

## Selected Decisions

- Controllers own their action evaluator; definitions are copied plain values.
  Editor tools may own independent evaluators. Engine owns physical window input.
- Contexts resolve by descending priority and stable insertion order. Consumption
  reserves physical sources for lower contexts; actions aggregate bindings inside
  a context. Modal contexts can reserve complete keyboard/mouse domains.
- Normal release and forced cancellation are distinct. Cancellation clears values
  immediately and retains one observable cancellation edge until evaluation.
  Context/override changes suppress already-held sources until release.
- Physical bookkeeping receives releases even when UI captures input. ImGui
  capture is bridged to gameplay device blocking; captured gameplay mouse input
  retains the current host policy. Escape/click capture remains host-owned.
- Evaluation occurs before gameplay intent creation. Pause, stop, possession,
  window changes, and focus loss cancel state; transient samples are not replayed.
- Overrides identify a context and binding slot, preserve defaults, validate
  conflicts, and use atomic file publication. Sandbox exposes a minimal controls
  API with a user configuration path; no settings editor is required.

## Implementation Stages

### Stage 0: Record scope and contracts

- [x] Inspect current physical input, routing, controller, and Sandbox behavior.
- [x] Select ownership, consumption, cancellation, and first-slice boundaries.
- [x] Validate this plan using the repository plan validator.

Completion: the selected implementation is documented and the plan validates.

### Stage 1: Implement and qualify the keyboard and mouse slice

Depends on Stage 0.

- [x] Implement action values, contexts, evaluation, and validated overrides.
- [x] Integrate physical/UI capture and lifecycle cancellation without lost releases.
- [x] Migrate all shared controller API consumers across Engine, Sandbox, and
  RoadWeaver source/test roots; retain source-neutral Pawn intent.
- [x] Add Sandbox defaults and persistent rebinding/reset access.
- [x] Cover same-frame taps, multiple bindings, context priority/consumption,
  modal UI/tool competition, held-key suppression, focus loss, pause/single-step,
  restart, mouse deltas, and override validation/persistence with native tests.
- [x] Document the implemented runtime contract and Sandbox controls.
- [x] Pass relevant native tests and the shared Engine API `all` build gate.
- [x] Pass changed-document and all-plan validation; review and commit changes.

Completion: tests and the integration build pass, lasting contracts are updated,
and implementation is committed with plan/stage provenance. Report any omitted
interactive application smoke explicitly.

Validation follows [agent testing](../../../Agents/Testing.md) and
[agent build/run](../../../Agents/BuildAndRun.md). Native tests provide the automated
acceptance gate; interactive GUI smoke is supplementary.
