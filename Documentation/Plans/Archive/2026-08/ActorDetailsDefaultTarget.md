# Actor Details Default Target Plan

Summary: Make Level Editor Actor selection inspect the RootComponent by default while preserving explicit Actor details and valid rootless Actors.

Last reviewed: 2026-08-04

Status: Archived
Completed: 2026-08-04

## Current Status

Completed on 2026-08-04 against plan baseline commit `fb8f401c`. A
module-private target resolver defaults rooted Actors to RootComponent,
preserves null component selection as the explicit Actor page, and recovers an
invalid component to RootComponent or Actor. Focused targeting tests,
`ViewportTests`, `EditorPropertyTests`, the full `all` build, editor startup
smoke, and all-plan validation pass.

The native ImGui Details panel has no accessibility automation surface, so the
interaction matrix was validated through deterministic target-policy tests,
inspection of the Actor/component click assignments that consume that policy,
full production-panel compilation, and editor startup rather than fragile
screen-coordinate automation.

## Goal

Remove the extra component-tree click required for the common case by showing a
selected Actor's RootComponent details immediately, without requiring every
Actor to own a RootComponent or removing access to Actor-specific reflected
properties and customizations.

## Scope

- Default details-target selection when the primary Level Editor Actor changes.
- Explicit Actor and component selection in the Details component tree.
- Rootless Actor behavior and invalid component-target recovery.
- Active reflected-property edit safety while the inspected object changes.
- Focused regression coverage, editor workflow validation, and lasting editor
  architecture documentation.

## Non-Goals

- Requiring, synthesizing, or silently adding a RootComponent to every Actor.
- Changing runtime Actor/component ownership, transform, attachment, creation,
  or serialization semantics.
- Merging Actor and RootComponent properties into one logical details object.
- Adding new reflected Actor properties or redesigning Actor details
  customizations.
- Changing multi-Actor editing, the primary-selection rule, or viewport
  selection semantics.
- Broad Details panel modularization beyond the smallest testable target-policy
  seam required by this change.

## Design Decisions and Invariants

- `AActor::RootComponent` remains optional. A rootless Actor is a valid object
  and may own non-scene components; selecting it must not mutate it.
- The Details component tree continues to expose Actor and component nodes as
  distinct targets. Actor details are not deleted, hidden, or replaced by a
  permanent RootComponent alias.
- `SelectedComponent == nullptr` continues to mean that the Actor itself is the
  inspected object. The default-selection path assigns the RootComponent when
  one exists and otherwise leaves `SelectedComponent` null.
- Default targeting is applied when the primary Actor changes or when the
  current component target becomes invalid. An explicit click on the Actor node
  remains on Actor details until the user selects another target or changes the
  primary Actor.
- A rootless Actor uses the ordinary Actor details path, including the existing
  empty-property presentation when the class has no reflected `Edit`
  properties. The component tree and Add Component workflow remain available.
- Adding the first scene component keeps the runtime invariant already enforced
  by `AActor::AddInstanceComponent`: the new component becomes RootComponent.
  The existing Add Component workflow continues to select that new component.
- Changing the inspected object must continue to resolve any active property
  preview through `FReflectedPropertyView::HandleOwnerContext`; the new default
  must not bypass commit/cancel safeguards.
- Actor-specific details customizations continue to run only on the Actor page;
  RootComponent and derived-component customizations run on the default
  RootComponent page.

| Interaction | Inspected details target |
| --- | --- |
| Select an Actor with a RootComponent | RootComponent |
| Select an Actor without a RootComponent | Actor |
| Click the Actor node | Actor |
| Click an owned component node | That component |
| Current component is no longer owned | Current Actor's RootComponent, otherwise Actor |
| Add the first scene component to a rootless Actor | Newly added RootComponent |

## Current Foundations and Gaps

Implemented foundations:

- `FDetailsPanel` already tracks the primary Actor separately from the selected
  component and renders either object through one reflected property view.
- Actor and component nodes are separately selectable in the component tree.
- Actor details customizations can compose properties owned by another object;
  the current Actor customization exposes the RootComponent transform without
  changing the presented owner.
- `AActor::AddInstanceComponent` promotes the first added scene component to
  RootComponent, and the Details panel selects a successfully added component.
- Reflected-property owner changes already protect active preview edits.

Remaining gaps:

- A primary Actor change currently clears `SelectedComponent`, so Actor details
  are shown even when a RootComponent exists.
- Invalid component selection currently falls back to Actor details instead of
  applying the same default-target policy.
- The target-selection policy is embedded in panel state transitions and lacks
  focused root/rootless regression coverage.
- The owning editor architecture documentation does not yet state the default
  RootComponent versus explicit Actor-page behavior.

## Implementation Stages

### Stage 0: Lock the details-target contract

Dependencies: none.

- [x] Keep RootComponent optional at runtime and in editor workflows.
- [x] Select RootComponent as the default details target when one exists.
- [x] Select Actor as the default target only when RootComponent is absent.
- [x] Preserve explicit Actor-node and component-node details selection.
- [x] Preserve active property-edit owner-transition safeguards.

#### Acceptance Gate

- The transition table above has one unambiguous target for every in-scope
  interaction and does not require runtime object mutation.

### Stage 1: Implement deterministic default targeting

Dependencies: Stage 0.

- [x] Introduce a module-private, independently testable default-target resolver
  that maps an Actor to its RootComponent when present and otherwise to the
  Actor.
- [x] Apply the resolver after a successful primary-Actor transition.
- [x] Apply the same resolver when a previously selected component is no longer
  owned by the current Actor.
- [x] Preserve explicit Actor-node selection as `SelectedComponent == nullptr`
  and explicit component-node selection as the selected component.
- [x] Preserve rootless Actor rendering and Add Component behavior without
  creating a component as a side effect of selection.
- [x] Keep property-preview commit/cancel handling ahead of every target change.

#### Acceptance Gate

- Selecting a spatial Actor immediately displays its RootComponent class and
  editable component properties.
- Clicking the Actor node displays Actor details and does not jump back to the
  RootComponent on subsequent frames.
- Selecting a rootless Actor remains stable, exposes component management, and
  performs no package mutation.
- Losing the inspected component resolves to RootComponent when available and
  Actor otherwise, without retaining a stale object.

### Stage 2: Add focused regression coverage

Dependencies: Stage 1.

- [x] Cover default-target resolution for Actors with and without a
  RootComponent.
- [x] Cover invalid-component recovery for rooted and rootless Actors.
- [x] Preserve coverage that Actor details customization exposes the root
  transform only when the Actor page is explicitly inspected.
- [x] Exercise the editor interaction matrix through target-policy tests and
  panel wiring inspection for rooted Actors, a rootless Actor, explicit
  Actor-node selection, component selection, and adding the first scene
  component.
- [x] Verify switching primary Actors while a property edit is active preserves
  the existing commit/cancel contract.

#### Acceptance Gate

- Focused native tests pass through the repository test entrypoint.
- The tested target policy and inspected panel wiring match every row in the
  design table, including a rootless Actor and an explicit return to Actor
  details.

### Stage 3: Document and validate the user-visible workflow

Dependencies: Stage 2.

- [x] Update the owning reflected-property/editor architecture documentation
  with the default RootComponent and explicit Actor-target contract.
- [x] Complete a successful full `all` build through the DurinDevTool entrypoint
  required by the repository build documentation.
- [x] Launch the editor from the same Agent Build Profile and smoke-test the
  Details workflow against the built executable.
- [x] Run the all-plan validator, record completion evidence, and update this
  plan's lifecycle fields and checklists.

#### Completion Evidence

- `FDetailsPanelTargetingTests.*` passed 2 of 2 focused rooted/rootless and
  invalid-target cases.
- `ViewportTests` passed 47 of 47 cases, including existing Actor details and
  camera/light customization coverage.
- `EditorPropertyTests` passed 25 of 25 reflected property-edit cases.
- `.\DevTool.bat build --target all` succeeded for the
  `Win64-Debug-DurinEditor-Tests` preset and compiled both `DetailsPanel.cpp`
  and `DetailsPanelTargeting.cpp` into `DurinEditor-LevelEditor.dll`.
- The resulting `DurinEditor.exe` remained running after the eight-second
  startup smoke window and was then closed by the validation script.
- `.\DevTool.bat doc plan validate --scope all` passed on 2026-08-04.

#### Acceptance Gate

- The full `all` build succeeds and the verified editor executable exhibits the
  documented rooted and rootless behavior.
- Lasting behavior is recorded in owning documentation, all required plan
  checks are complete, and plan validation passes.

## Validation Matrix

| Area | Required evidence |
| --- | --- |
| Default rooted selection | Focused resolver/state test plus editor smoke test |
| Rootless Actor support | Focused test proving Actor fallback and no mutation; Add Component smoke test |
| Actor details retained | Actor-node interaction check and Actor customization regression |
| Component details | Root and non-root component interaction checks |
| Invalid target recovery | Rooted and rootless stale-component tests |
| Property-edit safety | Active-edit Actor-switch regression or focused interaction test |
| User-visible integration | Successful full `all` build and editor launch from the same Agent Build Profile |
| Plan integrity | `.\DevTool.bat doc plan validate --scope all` |

Builds, tests, and runtime launches must follow
[Build and Run](../../../Development/Build/BuildAndRun.md); native tests must follow
[Native Tests](../../../Development/Build/NativeTests.md).

## Definition of Done

- RootComponent remains optional and rootless Actor selection is stable and
  non-mutating.
- Selecting a rooted Actor defaults to RootComponent details without an extra
  tree click.
- Actor details remain explicitly reachable and continue to dispatch Actor
  customizations.
- Component selection, invalid-target recovery, and active property edits obey
  the documented transition contract.
- Focused tests, the editor interaction matrix, a full `all` build, editor smoke
  validation, and all-plan validation pass.
- Long-lived editor documentation owns the completed behavior, and this plan is
  marked completed for later archival.

## Deferred Follow-ups

- Add richer Actor-level reflected properties such as tags, tick policy, or
  editor metadata when gameplay/editor requirements exist.
- Consider a dedicated rootless-Actor empty-state action if the ordinary Actor
  empty-property message and existing Add Component control prove unclear in
  usability testing.
- Consider UE-style aggregated Actor/component property presentation only as a
  separate Details information-architecture change.
- Continue broader component-tree model/presenter extraction under the
  Level Editor modularization plan without coupling it to this behavior change.

## Related Documentation

- [Reflected Property Editing](../../../Editor/Architecture/ReflectedPropertyEditing.md)
- [Build and Run](../../../Development/Build/BuildAndRun.md)
- [Native Tests](../../../Development/Build/NativeTests.md)
- [Actor Component System Plan](../2026-07/ActorComponentSystem.md)
- [Level Editor Modularization Plan](../../LevelEditorModularization.md)

## Related Code

- `Engine/Source/Editor/LevelEditor/Private/Panels/DetailsPanel.h`
- `Engine/Source/Editor/LevelEditor/Private/Panels/DetailsPanel.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Customizations/ObjectPropertyEditorCustomizations.cpp`
- `Engine/Source/Runtime/Engine/Public/Engine/Actor.h`
- `Engine/Source/Runtime/Engine/Private/Engine/Actor.cpp`
- `Engine/Tests/Native/EngineTests/Private/Viewport/ViewportCustomizationTests.cpp`
