# Editor Actor Subsystem Plan

Summary: Move general Actor and Component editing and structural history into DurinEd through DEditorActorSubsystem, and unify LevelEditor callers while retaining document-owned selection.

Last reviewed: 2026-10-17

Status: Active
Completed:

## Current Status

Planning only; implementation has not started. Inspected baseline:
`432fee00632ab7e2895287f6193484f5275f4990`, including structural history change
`a8f4fe836`. Actor/Component presence, duplication, rename and reparent replay
already retain exact identities. This plan preserves those semantics while
moving ownership and establishing a reusable service boundary.

The user selected DEditorActorSubsystem in DurinEd, migration of general Actor
operations and transaction implementations, consolidation of Component editing
infrastructure, and retention of selection in document context. Complete Stage 0
before production API migration. This document does not authorize starting the
implementation as part of the planning handoff.

## Goal

Provide one editor-host service for general Actor editing, reusable by UI and
future Toolset adapters without private LevelEditor dependencies. Existing
Outliner, viewport, placement and Component Details paths must share admission,
preparation, transaction and replay behavior. Preserve the single DTransactor
history and existing structural Undo/Redo guarantees.

## Baseline and Migration Inventory

| Existing location | Selected destination and responsibility |
| --- | --- |
| `LevelEditor/Private/Operations/ActorMutations.*` | DurinEd Actor editing feature: public subsystem entry points and private preparation/presence/rename changes |
| `LevelEditor/Private/Panels/ActorAttachmentTransaction.*` | DurinEd Actor editing feature: attachment validation, capture and replay; panels stop constructing entries/changes |
| `LevelEditor/Private/Operations/ComponentMutations.*` | DurinEd Component editing implementation used by the subsystem; preserve add/remove/duplicate/rename/reparent behavior |
| WorldOutlinerPanel | Call service operations for create/delete/duplicate/rename/attach/detach; retain menus, confirmation and selection |
| SceneViewportPanel and LevelEditorViewportEditing | Route placement and structural shortcuts through the service; retain viewport interaction |
| DetailsComponentTree | Route structural Component edits through the service; retain tree presentation and selected Component |
| SkyBoxPlacement and StaticMeshLevelMutations | Keep specialized orchestration in LevelEditor; compose exported DurinEd structural changes instead of private ActorMutations |
| FLevelEditorContext | Keep document World/Level, Actor/Component/sub-element selection, selection anchors and focus policy |

Source roots are `Engine/Source/Editor/DurinEd`,
`Engine/Source/Editor/LevelEditor`, and `Engine/Tests/Native/EngineTests`.
Implementation must also search all source/test roots declared by
`Durin.dworkspace` (Engine, Sandbox and RoadWeaver), including direct runtime
mutation calls that may bypass the named helpers. Runtime construction, loading
and gameplay mutation are not automatically editor-service consumers.

## Selected Design

### UE reference and Durin ownership

Use Epic's [UEditorActorSubsystem API](https://dev.epicgames.com/documentation/unreal-engine/API/Editor/UnrealEd/UEditorActorSubsystem)
as the reference for an editor-wide Actor utility service: spawn, destroy,
duplicate and related Actor operations are callable independently of panels.
This reference establishes API responsibilities, not an assertion that UE's
private transaction implementation has been inspected or should be copied.
UE also exposes selection helpers; Durin deliberately keeps selection with the
document for this refactor.

- Add reflected `DEditorActorSubsystem : DEditorSubsystem` to DurinEd and
  register it through the existing Editor subsystem provider mechanism. Resolve
  it through the admitting host's `GetEditorSubsystem<DEditorActorSubsystem>()`.
- Put public interfaces under `DurinEd/Public/Actors/` and implementation under
  `DurinEd/Private/Actors/`; Component helpers may have a sibling `Components/`
  feature directory. Keep `Editor/` focused on host/subsystem lifecycle.
- Expose Actor spawn/delete/duplicate/rename/attach/detach and owned Component
  add/delete/duplicate/rename/reparent through this service. Component mechanics
  remain separate implementation units; this scope does not introduce a second
  Component subsystem or a parallel public static command facade.
- DurinEd depends on Runtime mechanisms only. It must not depend on LevelEditor,
  MainFrame, widgets, ImGui, or a document's private context. Runtime Actor/Level
  APIs remain the mechanism boundary and acquire no editor-service dependency.
- Keep SkyBox/material/static-mesh policy in its feature owner. Generic Actor
  editing must not absorb asset-specific preparation or conversion policy.

### Targets, admission and results

- Every mutation identifies its Level and participants explicitly. The service
  does not cache the active World/Level or infer a mutation target from selection.
- Use a narrow edit context carrying required host/document editability facts,
  rather than passing `FLevelEditorContext` or a widget. Read-only state must be
  supplied from the document owner, with an explicit policy for non-document
  callers; a default `false` boolean must not silently bypass admission.
- Revalidate service/host lifetime, game-thread access, target World/Level
  ownership, supported play state, membership, class/creation constraints,
  duplicate participants and hierarchy constraints before effects. Reject
  mismatched hosts, expired targets and unsupported cross-Level operations.
- The normal service path uses its host's DTransactor. Preserve an explicit
  transactor injection seam for independent tests/internal operations without
  introducing another production history owner or falling back to unrecorded
  mutations when history is unavailable.
- Return owned domain results with changed status, resulting participants and
  actionable failure information. Do not infer success solely from an empty
  message or a non-null Component. Preserve replay failure causes and format
  presentation text once at the appropriate boundary. Stage 0 fixes exact types
  and no-op/batch semantics before migrating callers.
- Selection changes, confirmation, focus, notifications and error presentation
  belong to adapters. The service must work without an open LevelEditor widget.

### Preparation, composition and history

- Keep DTransBuffer as the only ordered history. Public synchronous operations
  perform validation/preparation and submit one logical transaction; no service
  history, ID bridge or remote begin/end transaction session is introduced.
- Separate internal preparation from submission. Export only the narrow native
  structural-change composition API required by SkyBox and StaticMesh workflows,
  using opaque ITransactionCustomChange ownership rather than exposing concrete
  replay classes. Consumers can prepare several changes and submit their existing
  compound operation once; they must not call auto-committing commands in a loop.
- Define ownership/cleanup for prepared but unpublished objects, failed
  preparation, abandoned plans and rejected history submission. Complete copying,
  cross-graph reference remapping, reconstruction and derived-state rebuilding
  before publication; preparation must not dirty packages or expose boundary
  attachments. Do not retain arbitrary UI callbacks in history.
- Preserve identity across absent membership, complete Outer-subtree GC
  traversal, authored ordering, root/primary-camera state, attachment transforms,
  registration/lifecycle and package revision/checkpoint behavior. Deletion in
  editor history remains reversible membership removal, not irreversible runtime
  destruction or resurrection by name/path.
- Preserve validate-before-write and reverse compensation, including compound
  operation failures. A rejected edit leaves no visible partial mutation or new
  history; incomplete compensation must retain the existing recovery-required
  disposition rather than being reported as success.
- Move custom-change module tags with their actual implementation owner. General
  changes become DurinEd-owned; specialized LevelEditor changes keep their own
  tags. Audit stored callbacks and module unload/retirement dependencies.
- Reuse current transaction/package events for refresh and history consumers.
  Add a domain event only for a demonstrated missing consumer need; if needed,
  define execution and Undo/Redo timing after successful completion and prevent
  duplicate or premature publication.

### Scope boundaries

Selection remains in FLevelEditorContext, including reconciliation after deletion
and Undo/Redo. The subsystem neither retains that context nor selects returned
objects. This work does not change property editing, introduce general Actor
conversion/clipboard features, or implement Python/MCP tools. Future tools must
call this service; coordinate that boundary with the
[Editor Agent Toolset Expansion plan](EditorAgentToolsetExpansion.md).

Transform gizmo gestures and their heterogeneous target protocol remain owned by
LevelEditor. Preserve their continuous-edit/one-gesture history semantics; do not
route every drag tick through a new auto-committing command. Attachment/reparent
transform handling is included because it belongs to the migrated structural
changes. Standalone general transform commands are a follow-up unless Stage 0
identifies an existing structural caller that requires one.

## Implementation Stages

### Stage 0: Freeze service and composition contracts

Outcome: concrete signatures and a complete consumer/validation map.
Dependencies: none.

- [ ] Audit Actor/Component edits and structural changes across every workspace
  project; distinguish runtime mechanisms, general editing and specialized policy.
- [ ] Finalize public operations, target/editability context, typed results,
  no-op behavior, supported batch atomicity and internal test injection.
- [ ] Specify prepared-object ownership and the minimal cross-module composition
  API needed by SkyBox and StaticMesh; prove one logical history entry per command.
- [ ] Map subsystem registration, module tags, retirement and test target/build
  dependencies; record exact affected targets from DevTool discovery.
- [ ] Update this plan with resolved API choices before implementation. Completion
  requires no unresolved ownership, read-only admission or composition contract.

### Stage 1: Establish DurinEd Actor editing ownership

Outcome: working host subsystem and DurinEd-owned Actor structural history.
Dependencies: Stage 0.

- [ ] Register DEditorActorSubsystem using the existing lifecycle contract.
- [ ] Move general Actor preparation, presence, rename and attachment changes to
  DurinEd; implement service admission and submission without LevelEditor includes.
- [ ] Export the narrow structural composition seam and migrate native test seams.
- [ ] Verify host separation, unavailable/retiring service rejection, exact identity,
  GC retention, attach/detach replay, rejected submission and abandoned preparation.
- [ ] Completion: owning module and relevant test targets compile; focused tests
  pass, with migrated changes tagged to the correct module.

### Stage 2: Consolidate Component editing infrastructure

Outcome: owned Component editing uses the same admission/history foundation.
Dependencies: Stage 1.

- [ ] Move Component add/delete/duplicate/rename/reparent implementation to DurinEd
  and expose the agreed service methods; share only mechanisms with identical
  semantics, keeping Actor and Component replay constraints explicit.
- [ ] Preserve creation-method restrictions, root protection, authored ownership,
  subtree retention, registration and child attachment restoration.
- [ ] Verify Component errors and no-ops through the new result contract and
  reject detached, foreign-owner, generated or otherwise unsupported targets.
- [ ] Completion: Component structural regression tests pass through the new
  boundary, including isolated execution of new lifecycle/failure cases.

### Stage 3: Unify existing editor callers

Outcome: all existing general editing paths use the new service or composition API.
Dependencies: Stages 1 and 2.

- [ ] Migrate Outliner, viewport shortcuts and placement, and DetailsComponentTree.
  Remove panel-built attachment changes and per-caller general transaction logic.
- [ ] Migrate SkyBox and StaticMesh composite workflows while preserving their
  specialized preparation, atomic replay and single history entry.
- [ ] Keep document selection/focus/error handling in adapters; verify stale
  selection reconciliation and consistency between Outliner and viewport actions.
- [ ] Remove obsolete ActorMutations/ComponentMutations interfaces, private-header
  test dependencies and build registrations; search every workspace project for
  legacy includes/symbols and accidental bypasses.
- [ ] Completion: consumer targets compile and relevant placement, structural,
  Details and viewport contract tests pass without a private LevelEditor API bridge.

### Stage 4: Validate integration and document final contracts

Outcome: final service ownership and preserved behavior are evidenced.
Dependencies: Stage 3.

- [ ] Run the bounded final validation matrix below and record tested revision,
  selections, preset/environment, outcomes and any remaining gates here.
- [ ] Update EditorSubsystems, TransactionRecords, LevelEditorDetails,
  ViewportEditing and StaticMeshLevelMutations contracts, plus CodeModules/routing
  only where ownership or task triggers changed. Reconcile the Toolset plan's
  scene-operation ownership so it does not introduce a competing mutation path.
- [ ] Confirm no DurinEd dependency on LevelEditor/UI, no duplicate public command
  implementation, no selection ownership transfer and no second history.
- [ ] Complete only after all required gates pass; follow plan lifecycle rules
  for completion and later archive.

## Validation and Acceptance

Follow [Build and Run](../Agents/BuildAndRun.md) and
[Testing](../Agents/Testing.md), using DevTool's host-selected profile and registry
discovery rather than guessed target names. Inspect affected coverage before
execution. This planning change requires document validation and diff review only.

Implementation coverage must include:

- Service admission/lifecycle without a UI, multiple hosts, read-only and PIE
  rejection, absent/busy/recovering transactor, foreign Level/owner and stale inputs.
- Actor create/delete/duplicate/rename/attach/detach execute/Undo/Redo; same live
  identity, graph references, attachment transforms, primary camera, ordering,
  package revisions and GC retention while absent and after history release.
- Component create/delete/duplicate/rename/reparent, root and creation-method
  restrictions, complete subtree retention, reconstruction and registration.
- Preparation failure/cleanup, replay conflicts, compensation and compound
  SkyBox/StaticMesh history; no partial publication, dirtying or success on rejection.
- UI adapter parity, document selection reconciliation and unchanged gizmo
  gesture semantics. New lifecycle/conflict tests must also pass individually.

Build DurinEd and affected consumer/test targets, including discovered Sandbox or
RoadWeaver consumers if present. Broad all-build/all-test coverage is not a
default gate: expand only if generated/reflected API or build-graph impact cannot
be bounded, and record the uncovered risk before expansion. Native tests do not
substitute for a consumer build where that consumer is outside test dependencies.
Reuse applicable passing results when relevant inputs remain unchanged.

Record concise evidence here; generated reports/logs belong under ignored Build/.
Validate changed documents and run the all-plan validator for plan lifecycle edits.

## Required References

- [Editor subsystems](../Editor/Architecture/EditorSubsystems.md)
- [Editor transactor core](../Editor/Architecture/Transactors.md)
- [Transaction record foundation](../Editor/Architecture/TransactionRecords.md)
- [Level Editor Details](../Editor/Architecture/LevelEditorDetails.md)
- [Viewport editing](../Editor/Architecture/ViewportEditing.md)
- [Static mesh level mutations](../Editor/Architecture/StaticMeshLevelMutations.md)
- [Level system](../Runtime/World/LevelSystem.md)
- [Code modules](../Workspace/CodeModules.md)
- [Documentation workflow](../Agents/Documentation.md)
