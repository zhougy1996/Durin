# Tagged Asset Field Upgrades Plan

Summary: Use DAST tagged-field identity and scoped legacy-field upgraders for authored asset evolution without package-local schema versions or deprecated runtime members.

Last reviewed: 2026-07-30

Status: Completed
Completed: 2026-07-30

## Current Status

The selected compatibility model is now the existing DAST v2 tagged-field
model. Object and struct fields carry their declaring reflected type, property
name, property kind, recursive type signature, and a bounded payload. Ordinary
field additions, removals, and declaration-order changes therefore do not
require an asset version. Exact matches deserialize normally, missing fields
retain constructor defaults, and unmatched fields remain available as logical
`FAssetLegacyField` snapshots for audit and owner-defined conversion.

Texture2D and TextureCube already use this model for the retired source-string
fields. Their runtime reflection contains only canonical `SourceImportData`.
Engine-owned inspection and materialization upgraders recognize the exact old
field identities, preserve canonical provenance when it is already present,
otherwise migrate resolvable source paths, and report ambiguous or unavailable
sources for manual repair.

An experimental package-local Custom Version registry and ordered migration
planner was implemented in `b6e9c9de`, but no production asset or serialized
package uses it. Review determined that it duplicated information already
present in tagged fields and would make ordinary reflected-property evolution
depend on GUID streams and migration chains. Stage 2 removes that speculative
surface before any wire-format dependency is introduced. DAST remains v2.

Stage 2 completed on 2026-07-30 against baseline `2ebf0614`. The working set
was AssetCore's public package API and implementation, AssetPackageTests, this
plan, Asset Packages, and the dependent Project Asset Upgrade Workflow plan.
The AssetCore code and PackageTests now match the pre-`b6e9c9de` baseline:
`FAssetCustomVersionContainer`, every `FAssetSchema*` type and registry, ordered
planning, schema report fields, and their five native tests are gone. Existing
`RegisterAssetStructureInspectionUpgrader` and
`RegisterAssetStructureUpgrader` behavior is restored; Texture2D and
TextureCube continue to register exact legacy-field handlers through those
interfaces.

Validation passed all 28 AssetPackageTests and all 69 TextureTests. Changed
documentation validation passed. Repository-wide plan validation remains
blocked only by the unrelated existing
`RecoverableRendererResourceCreation.md` lifecycle error: `Completed` must be
empty or contain `YYYY-MM-DD`. Open questions: none.

## Goal

Keep authored assets forward-evolvable through explicit serialized field
identity while ensuring retired fields exist only in historical package
snapshots and upgrade code, never as competing reflected runtime state.

## Scope

- DAST object and struct tagged-field compatibility.
- Object-free inspection and materialized upgrade callbacks for recognized
  legacy fields.
- Texture2D `SourceFile` migration into `SourceImportData.Source`.
- TextureCube face and panorama source-string migration into
  `SourceImportData`.
- Risk reporting, Dirty-state publication, and save refusal for unrecognized
  incompatible fields.
- Removal of the unused Custom Version registry, migration planner, and tests.
- Lasting AssetCore documentation for the selected compatibility model.

## Non-Goals

- Adding a package-local Custom Version table.
- Adding GUID-based schema registration or mandatory `N -> N+1` migration
  chains.
- Incrementing the package format for reflected field changes.
- Retaining `_DEPRECATED`-style reflected members after their canonical
  replacement exists.
- Automatically converting a field whose old identity or represented meaning
  cannot be proven.
- Building a general redirect table before more than one concrete pure-rename
  use case exists.
- Changing project-wide audit/editor presentation owned by the Project Asset
  Upgrade Workflow plan.
- Compacting repeated schema strings or otherwise changing the DAST wire
  encoding.

## Design Decisions and Invariants

### Version boundaries

- `AssetVersion` describes only the DAST envelope and byte layout. It changes
  only when a reader/writer wire contract changes.
- Reflected property addition, removal, reordering, or owner-defined migration
  does not change `AssetVersion`.
- Generated data such as DMSH, TXPL, DBLK, DDC records, importers, builders,
  and projections retains its own independently owned payload versions.
- A concrete future semantic change that cannot be distinguished from tagged
  field identity may use an owner-local version carrier, as
  `DStaticMesh::MaterialSlotsVersion` already does. A general Custom Version
  facility requires a demonstrated cross-type need and a separate decision.

### Tagged-field matching

- One serialized field identity is
  `(DeclaringType, PropertyName, PropertyKind, RecursiveTypeSignature)`.
- An exact identity match deserializes into the current reflected property.
- A current property absent from the package keeps its constructed default.
- An unmatched serialized identity is never reinterpreted as a similarly
  shaped current field. AssetCore retains its exact payload as
  `FAssetLegacyField`.
- A same-name field with a different kind or recursive type signature remains
  incompatible unless an owning module explicitly recognizes and decodes that
  exact legacy identity.

### Upgrade ownership

- AssetCore owns field inspection, payload retention, compatibility reports,
  object-reference decoding helpers, risk policy, and save protection.
- The module that owns a reflected type owns its legacy-field recognition and
  conversion callbacks. AssetCore does not depend on concrete Engine assets.
- Inspection and materialization callbacks use the same stable handler ID and
  exact legacy identities. The project-wide upgrade workflow verifies their
  reported result before publishing a save.
- Upgrade code consumes logical field snapshots. It does not branch on raw v2
  byte offsets and is not part of current asset serialization.

### Runtime and cook boundaries

- Deprecated fields do not exist in the reflected runtime object, import
  transaction state, swap logic, cook filtering, or rollback state.
- Historical parsing runs only when unmatched legacy fields are present in a
  loaded or inspected package.
- A successful safe cleanup or migration marks the authored package Dirty so
  an explicit save rewrites only canonical fields.
- Unknown or risky fields remain visible in the report and block an ordinary
  save without explicit data-loss consent.
- Canonical packages take the normal load/build/cook path without invoking
  historical field conversion.

### Rename policy

- A pure rename may be handled by a small owner-defined upgrader that recognizes
  one old field identity and writes one canonical property.
- A shared redirect facility is deferred until repeated pure-renames establish
  required scope, chaining, conflict, and lifetime semantics.
- Structural moves, consolidation of several fields, source recovery, and
  ambiguous conversion remain explicit owner-defined upgraders rather than
  redirects.

## Current Foundations and Gaps

| Area | Current foundation | Remaining gap |
| --- | --- | --- |
| DAST format | Deterministic v2 reader/writer with tagged object and struct fields | No gap for the selected model |
| Compatibility snapshot | `FAssetLegacyField`, package inspection, retained payloads, and exact type signatures | No gap |
| Upgrade callbacks | Separate object-free inspection and materialized application registration with stable handler IDs | No generic redirect syntax; deferred |
| Save safety | Risk classification, load mutation reporting, Dirty state, and explicit data-loss consent | Project-wide presentation remains in its owning plan |
| Texture provenance | Canonical reflected `SourceImportData` plus legacy-field inspection/application handlers | Validate after removing speculative schema code |
| Custom schema code | Unused registry/planner introduced in `b6e9c9de` | Remove before further asset work |

## Implementation Stages

### Stage 0: Select Tagged Fields As The Compatibility Contract

- [x] Confirm DAST object and struct records already serialize declaring type,
  field name, kind, recursive type signature, and bounded payload.
- [x] Separate package-format versions from reflected property evolution and
  generated payload versions.
- [x] Select exact tagged-field matching plus retained legacy snapshots as the
  default compatibility mechanism.
- [x] Reject a mandatory per-class or per-field Custom Version stream.
- [x] Keep future semantic version carriers local to demonstrated owners.

#### Acceptance Gate

- Ordinary reflected field additions, removals, and declaration-order changes
  require no version increment.
- The package format remains v2 because this plan changes no bytes.
- Unknown data remains protected from silent loss.

### Stage 1: Remove Texture Legacy Members And Migrate By Field Identity

- [x] Remove Texture2D `SourceFile` from reflection and runtime state.
- [x] Remove TextureCube's seven retired source-string members from reflection
  and runtime state.
- [x] Keep canonical source provenance only in `SourceImportData`.
- [x] Register exact legacy-field inspection and materialization handlers for
  Texture2D and TextureCube.
- [x] Preserve canonical provenance when both old and new representations are
  present.
- [x] Migrate resolvable legacy source paths and report manual repair when
  recovery is ambiguous or unavailable.
- [x] Verify import, swap, cook serialization, rollback, and runtime load no
  longer save, clear, or restore retired members.

#### Acceptance Gate

- Runtime reflected objects have one source-provenance truth.
- Old fields are observable only through package inspection/upgrader snapshots.
- Current packages serialize only `SourceImportData`.

### Stage 2: Remove The Speculative Custom Schema Layer

- [x] Remove `FAssetCustomVersionContainer`, schema descriptors, migration
  plans, registration maps, and schema report fields from AssetCore.
- [x] Remove tests that exist only for the unused registry and ordered planner.
- [x] Restore the established structure inspection/application registration
  behavior without changing the texture legacy handlers.
- [x] Remove the planned DAST v3 Custom Version table and symbol-table work.
- [x] Update Asset Packages and Project Asset Upgrade Workflow documentation to
  consume tagged-field reports instead of schema-version plans.
- [x] Run AssetCore package tests and texture migration/runtime tests.
- [x] Run the repository plan validator and record any unrelated pre-existing
  failure.

#### Acceptance Gate

- No `FAssetSchema*`, package-local Custom Version, or v3 serialization surface
  remains in production code or native tests.
- The active DAST reader and writer remain exactly v2.
- Texture legacy audit, migration, deterministic cook, and runtime load tests
  pass with only tagged-field upgraders.
- Long-lived documentation describes one compatibility model.

## Validation Matrix

| Area | Required evidence |
| --- | --- |
| Package format | Frozen DAST v2 prefix and malformed/bounded header tests |
| Tagged fields | Missing defaults, unknown field retention, type mismatch, and nested struct coverage |
| Texture2D | Old `SourceFile` cleanup/migration and canonical provenance |
| TextureCube | Seven old strings cleanup/migration, deterministic cook, and runtime load |
| Save safety | Safe Dirty state, risky-save refusal, and explicit consent |
| Regression | AssetCore package suite, TextureTests, and plan validation |

Build and test execution follow
[Build And Run](../Development/Build/BuildAndRun.md) and
[Native C++ Tests](../Development/Build/NativeTests.md).

## Definition of Done

- DAST package format versions describe wire compatibility only.
- Ordinary reflected field evolution uses tagged identity without version
  increments.
- Recognized historical fields are migrated by the owning module from logical
  snapshots.
- Retired texture fields do not exist in reflection or runtime state.
- Unknown incompatible payloads cannot be silently overwritten.
- No unused general Custom Version registry or migration planner remains.
- Lasting behavior is documented in Asset Packages.
- Required focused validation passes.

## Deferred Follow-ups

- Add a shared field-redirect facility only after multiple concrete pure-rename
  migrations establish its required semantics.
- Reconsider package-local Custom Versions only for a demonstrated semantic
  change that cannot be identified through field tags or an owner-local
  version carrier.
- Consider compact schema-symbol encoding as an independent measured storage
  optimization, not as an asset-upgrade prerequisite.

## Related Documentation

- [Asset Packages](../Runtime/Assets/AssetPackages.md)
- [Project Asset Upgrade Workflow](ProjectAssetUpgradeWorkflow.md)
- [Asset Structure Upgrade](Archive/2026-07/AssetStructureUpgrade.md)
- [Build And Run](../Development/Build/BuildAndRun.md)
- [Native C++ Tests](../Development/Build/NativeTests.md)

## Related Code

- `Engine/Source/Runtime/AssetCore/Public/AssetSystem.h`
- `Engine/Source/Runtime/AssetCore/Private/AssetSystem.cpp`
- `Engine/Source/Runtime/Engine/Public/Texture/Texture2D.h`
- `Engine/Source/Runtime/Engine/Public/Texture/TextureCube.h`
- `Engine/Source/Runtime/Engine/Private/Texture/Texture2D.cpp`
- `Engine/Source/Runtime/Engine/Private/Texture/TextureCube.cpp`
- `Engine/Tests/Native/AssetCoreTests/Private/PackageTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/TextureTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/TextureCubeTests.cpp`
