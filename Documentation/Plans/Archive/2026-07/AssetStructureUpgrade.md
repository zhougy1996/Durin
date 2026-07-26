# Asset Structure Upgrade Plan

Summary: Introduce structured asset compatibility reporting, explicit schema upgraders, and a two-phase Level Editor upgrade workflow.

Last reviewed: 2026-07-27

## Current Status

Completed and archived on 2026-07-27. AssetCore now has a structured load-report
contract, distinguishes registered safe upgrades from unknown incompatible
fields, and blocks normal saves that would discard compatibility-risk payloads.
The Level Editor now keeps compatibility-affected loads pending until the user
saves, opens without saving, explicitly accepts risky cleanup, or cancels.
The pending state also keeps the upgrade modal open across startup window
placement changes instead of relying on a one-frame popup request.
Engine now migrates the removed static-mesh component material fields into
GUID-keyed overrides, retaining excess assignments as explicit orphans.

Stage 4 extracted the pending upgrade decisions into a testable Level Editor
model, added checked-in old-level and unknown-newer-field fixtures, and recorded
the lasting AssetCore ownership and Level workspace opening contracts.

Final validation completed on 2026-07-27: all 49 AssetCore native tests and all
313 Engine native tests passed, including seven Level Editor decision-model
tests and two checked-in level-fixture tests. The complete `all` target built
successfully with the `Win64-Debug-DurinEditor-Tests` Agent Build Profile, and
the resulting editor remained alive for an eight-second hidden-window Sandbox
smoke test.

The reported `/Game/Levels/NewLevel` fixture contains empty legacy
`DStaticMeshComponent::Material` and `Materials` values. Its expected outcome is
one `SafeCleanup` issue covering two fields, with no material reassignment and
no reimport.

## Goal

Replace log-driven incompatible-field warnings with an asset-structure upgrade
workflow that tells users exactly what changed, prevents unknown newer data
from being silently removed, and does not replace the active level before the
user chooses an outcome.

## Scope

- Structured compatibility reports returned by asset loading.
- Engine-owned, class-specific structure upgraders registered with AssetCore.
- Safe-cleanup, migrated, data-loss-risk, and unknown-incompatible
  classifications.
- A Level Editor modal grouped by package, object, and change.
- Pending level loading, atomic save before activation, in-memory activation
  without saving, and cancellation with unload.
- Legacy `DStaticMeshComponent::Material` and `Materials` migration.
- Package and editor tests using real old-schema fixtures.

## Non-Goals

- Making AssetCore depend on Engine component types.
- Parsing logs in editor code.
- Automatically removing unrecognized fields.
- Reimporting a static mesh solely because its component material schema is old.
- Providing a permanent "never warn again" setting.
- Changing the on-disk package format in this work.

## Design Decisions and Invariants

- AssetCore owns serialized field payloads, object-reference resolution, load
  reports, classification enums, and upgrader registration. Engine owns
  concrete schema rules.
- A registered upgrader may mark an issue `SafeCleanup` or `Migrated`; only
  those classifications automatically make the loaded package Dirty.
- `DataLossRisk` and `UnknownIncompatible` never automatically make a package
  Dirty. Unknown fields are retained on disk because the editor does not save
  before explicit user consent.
- Normal package saving rejects a loaded package with compatibility-risk
  payloads. A caller must pass explicit data-loss consent to persist the
  representation that omitted those fields.
- Each issue contains package path, object path, declaring class, original
  field names/signatures/payloads, classification, summary, risk, and handler
  identifier.
- Related fields are reported as one object-level issue. The UI groups issues
  as package, object, then change rather than presenting one modal per field.
- Opening a level uses a pending loaded package. The current level remains
  active until the upgrade decision and any requested save complete.
- Save failure leaves the previous file and active level unchanged. Existing
  atomic package publication remains the persistence boundary.
- Cancelling unloads the pending level package and retains the current level.
- Opening a safely upgraded level without saving keeps the pending package
  Dirty, so the workflow appears again after a later unload/reload.
- A report containing risk items disables the normal upgrade-and-save action.
  Risky cleanup requires a separate explicit decision.
- All work runs on the object/game thread; no report or upgrader state crosses
  into render-thread data.

## Current Foundations and Gaps

### Foundations

- Asset packages already serialize each reflected field with declaring class,
  name, kind, type signature, and independent payload.
- Object references are resolved after package objects and dependencies exist.
- `SavePackage` publishes bytes through the existing atomic replacement path.
- `FLevelDocumentController` already owns unsaved-level confirmation,
  activation, previous-package unload, and deferred document completion.
- Static meshes expose stable material-slot GUIDs and components persist sparse
  GUID-keyed overrides.

### Gaps

- Level loading still activates immediately after `LoadAsset`.
- No editor surface renders `FAssetLoadReport`.
- The legacy static-mesh component fields were removed after the historical
  transitional migration path, so their rule must be restored as an Engine
  upgrader instead of reflected storage.
- High-risk confirmation copy and interaction tests do not yet exist.

## Implementation Stages

### Stage 1: Structured AssetCore Compatibility Contract

- [x] Return `FAssetLoadReport` from optional load call sites without breaking
  existing callers.
- [x] Preserve incompatible field payloads in grouped object reports.
- [x] Add class-specific structure-upgrader registration and object-reference
  payload helpers.
- [x] Mark packages Dirty only for registered `SafeCleanup` and `Migrated`
  issues.
- [x] Report unhandled fields as `UnknownIncompatible` with an
  `UnknownNewerSchema` risk and leave the package clean.
- [x] Reject ordinary saves of compatibility-risk packages and require an
  explicit data-loss option.
- [x] Cover registered cleanup and unknown-field protection in AssetCore tests.

#### Acceptance Gate

- An unknown renamed field produces a structured risk item, does not suggest a
  resave, and leaves the package clean.
- A registered safe-cleanup rule groups its recognized fields, identifies its
  handler, and leaves the package Dirty for an explicit save.
- AssetCore native tests pass through the repository BuildTool.

### Stage 2: Pending Level Activation and Upgrade Modal

- [x] Extend `FLevelDocumentController` with a pending loaded level, package
  path, and load report.
- [x] Return `Deferred` while an upgrade decision is open and complete the
  document request only after activation or cancellation.
- [x] Draw a modal titled "Asset Structure Upgrade Required" with the package
  summary and expandable package/object/change rows.
- [x] Show old type, target structure, migration rule, original field names,
  classification, summary, and risk for each change.
- [x] Implement "Upgrade, Save and Open", "Open Without Saving", and "Cancel
  Open" for reports without risk items.
- [x] Disable normal save for risk reports and provide a distinct explicit
  risky-cleanup decision.
- [x] Unload the pending package on cancel or failed activation; retain it on
  save failure so the user can retry or cancel.

#### Acceptance Gate

- The active world and editor transaction state do not change before a modal
  decision.
- Save success activates the pending level and unloads the previous level.
- Open-without-save activates a Dirty level.
- Cancel and save failure preserve the previous active level and on-disk
  package.

### Stage 3: Static-Mesh Component Material Upgrader

- [x] Register an Engine upgrader for the removed `Material` and `Materials`
  fields on `DStaticMeshComponent`.
- [x] Decode the legacy object reference and object-reference array through
  AssetCore migration context helpers.
- [x] Map `Materials[index]` to the current static-mesh slot GUID.
- [x] Use `Material` only as the slot-zero fallback when the array lacks index
  zero.
- [x] Preserve non-null entries beyond the current slot count as explicit
  orphan overrides with diagnostics.
- [x] Classify null `Material` plus empty `Materials` as one `SafeCleanup`
  issue with two fields and zero migrated values.
- [x] Report each mapped or orphaned value in the issue summary without
  requiring reimport.

#### Acceptance Gate

- `/Game/Levels/NewLevel` reports one object, two legacy fields, zero migrated
  values, and zero risk items.
- Real old-package tests cover empty cleanup, slot-zero fallback, multi-slot
  conversion, excess-entry orphan retention, null values, and incompatible
  referenced object types.
- A save writes only `MaterialOverrides`; reloading produces no compatibility
  issue and preserves every migrated assignment.

### Stage 4: End-to-End Validation and Lasting Documentation

- [x] Add Level Editor controller/model tests for every modal outcome and
  deferred-document completion.
- [x] Add a real old-level package fixture and unknown-newer-field fixture.
- [x] Document the lasting ownership and opening workflow under the closest
  Runtime and Editor architecture documents.
- [x] Complete the required full `all` build and hidden-window editor smoke
  test because this stage changes a user-visible editor workflow.
- [x] Archive this plan after all acceptance gates pass.

#### Acceptance Gate

- Automated tests cover safe, migrated, risky, unknown, save-failure, cancel,
  and no-save paths.
- The full editor build and smoke test succeed from the same Agent Build
  Profile.
- Lasting architecture documentation contains the final contracts and this
  plan is archived.

## Validation Matrix

| Area | Required evidence |
| --- | --- |
| AssetCore report | Counts, grouping, handler ID, classifications, and risks |
| Unknown protection | Unknown fields leave the package clean and cannot use normal upgrade-and-save |
| Payload resolution | Null, internal, external, array, malformed, and wrong-type object references |
| Static mesh migration | Index-to-GUID mapping, slot-zero fallback, orphan retention, empty cleanup |
| Pending open | Active level unchanged before decision; cancellation unloads pending package |
| Persistence | Atomic save success/failure and clean reload after upgrade |
| Editor UX | Grouped expandable rows, summary counts, safe/risk action availability |
| Regression | AssetCoreTests, EngineTests, Level Editor tests, full `all` build, editor smoke |

Build and test execution follows
`Documentation/Development/Build/BuildAndRun.md` and
`Documentation/Development/Build/NativeTests.md`.

## Definition of Done

- Asset compatibility is a structured API contract rather than a log message.
- Unknown incompatible fields are never automatically made Dirty or silently
  saved by the normal upgrade action.
- Registered safe and complete migrations produce explicit, reviewable
  results.
- Level activation occurs only after the user's upgrade decision and any
  requested atomic save.
- The historical static-mesh component material schema migrates without wrong
  slot assignment, silent loss, or mandatory reimport.
- The editor has no permanent compatibility-warning suppression.

## Deferred Follow-ups

- Batch upgrading selected packages from the Content Browser.
- Command-line audit and upgrade tooling for continuous integration.
- Package-version ranges and chained multi-version upgraders if a second
  schema requires ordered transformations.
- Persisted orphan labels if multiple migration domains need richer orphan
  provenance.

## Related Documentation

- `Documentation/Plans/Archive/2026-07/StaticMeshMaterialSlots.md`
- `Documentation/Runtime/Rendering/MaterialSystem.md`
- `Documentation/Runtime/World/LevelSystem.md`
- `Documentation/Editor/Architecture/ReflectedPropertyEditing.md`
- `Documentation/Development/Build/BuildAndRun.md`
- `Documentation/Development/Build/NativeTests.md`

## Related Code

```text
Engine/Source/Runtime/AssetCore/Public/AssetSystem.h
Engine/Source/Runtime/AssetCore/Private/AssetSystem.cpp
Engine/Source/Runtime/Engine/Public/Components/StaticMeshComponent.h
Engine/Source/Runtime/Engine/Private/Components/StaticMeshComponent.cpp
Engine/Source/Editor/LevelEditor/Private/Documents/LevelDocumentController.h
Engine/Source/Editor/LevelEditor/Private/Documents/LevelDocumentController.cpp
Engine/Tests/Native/AssetCoreTests/Private/PackageTests.cpp
Engine/Tests/Native/EngineTests/Private/Materials/StaticMeshMaterialTests.cpp
```
