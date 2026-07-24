# C++ Coding Standards Migration Plan

Last reviewed: 2026-07-25

## Current Status

Inventory is complete for the first migration tranche: `Core`, `CoreDObject`, and
`Engine`. Stages 1 and 2 are complete after successful `Core` and `CoreDObject`
target builds. Stage 3 is in progress. Later module groups remain pending and
are intentionally outside the current change.

## Goal

Bring repository-owned C++ into conformance with the comment, reflected-member
spacing, and module export conventions in
[C++ Coding Standards](../Setup/CodingStandards.md) without combining the work
into one unreviewable repository-wide rewrite.

## Scope

- Add comments where key types or non-obvious member contracts require them.
- Separate consecutive reflected member units with one blank line.
- Replace class- and struct-level module export macros with per-function exports.
- Migrate modules in dependency-aware, independently validated stages.

## Non-Goals

- Reformat unrelated code.
- Add comments that merely restate names or implementations.
- Modify generated files or third-party sources.
- Change runtime behavior, serialization schemas, reflection identities, or
  public API shapes.
- Complete modules outside the current stage group as part of the
  `Core`/`CoreDObject`/`Engine` tranche.

## Design Decisions and Invariants

- Comments describe responsibility, contracts, ownership, units, invariants, or
  tradeoffs; comment count is not a completion metric.
- A reflected member comment, its `DPROPERTY()` macro, and its declaration form
  one unit. Consecutive units have one blank line between them.
- Per-function export migration preserves the cross-module callable surface of
  each formerly exported type. Inline functions remain unannotated.
- Export migration proceeds from lower-level dependencies to consumers:
  `Core`, then `CoreDObject`, then `Engine`.
- Generated reflection outputs are regenerated only through BuildTool.
- Each implementation commit records the exact completed stages in its plan
  provenance.

## Current Foundations and Gaps

- `Documentation/Setup/CodingStandards.md` is the authoritative convention.
- The initial scan covered 308 repository-owned headers and excluded generated
  and third-party code.
- In the first tranche, class- or struct-level export macros appear on 2 `Core`
  types, 1 `CoreDObject` type, and 23 `Engine` types.
- `CoreDObject` and `Engine` contain 46 reflected types without a leading role
  comment.
- Four consecutive reflected-member gaps are missing in
  `Engine/Public/Components/CameraComponent.h`.
- Member-comment requirements need semantic review and cannot be closed by a
  regex-only count.

## Implementation Stages

### Stage 0: Inventory and migration boundaries

- [x] Establish the authoritative coding standard and exclusions.
- [x] Inventory reflected declarations, reflected-member spacing, and
  class-level exports for the first tranche.
- [x] Select dependency-aware module ordering and validation boundaries.

#### Acceptance Gate

- The first-tranche counts, exclusions, and ordering are explicit.
- Semantic comment review is distinguished from mechanical checks.

### Stage 1: Core

- [x] Review key public types and non-obvious member contracts in `Core`.
- [x] Replace class-level `CORE_API` use with per-function exports.
- [x] Build the `Core` target.

#### Acceptance Gate

- `Core` has no repository-owned class- or struct-level module export macros.
- Reviewed key declarations follow the comment standard.
- The `Core` target builds successfully.

### Stage 2: CoreDObject

- [x] Add required comments to reflected and other key `CoreDObject` types and
  non-obvious member contracts.
- [x] Replace class-level `COREDOBJECT_API` use with per-function exports.
- [x] Build the `CoreDObject` target.

#### Acceptance Gate

- Every `CoreDObject` reflected type has a leading role comment.
- `CoreDObject` has no repository-owned class- or struct-level module export
  macros.
- The `CoreDObject` target builds successfully.

### Stage 3: Engine

- [ ] Add required comments to reflected and other key `Engine` types and
  non-obvious member contracts.
- [ ] Normalize reflected-member spacing.
- [ ] Replace class- and struct-level `ENGINE_API` use with per-function exports.
- [ ] Build the `Engine` target and then the complete `all` target.

#### Acceptance Gate

- Every `Engine` reflected type has a leading role comment.
- Consecutive reflected member units have one blank line between them.
- `Engine` has no repository-owned class- or struct-level module export macros.
- The targeted and full builds succeed.

### Stage 4: Rendering foundation

- [ ] Inventory and migrate `RHI`, `VulkanRHI`, `RenderCore`, and `Renderer` in
  dependency order.

#### Acceptance Gate

- Each migrated module passes its targeted build and the rendering group passes
  an appropriate integration build.

### Stage 5: Application and UI runtime

- [ ] Inventory and migrate `ApplicationCore`, `MonaCore`, and `MonaImGui`.

#### Acceptance Gate

- Each migrated module passes its targeted build and the runtime group passes an
  appropriate integration build.

### Stage 6: Editor modules

- [ ] Inventory and migrate repository-owned editor modules in dependency order.

#### Acceptance Gate

- Each migrated editor module passes its targeted build and the editor runtime
  passes a hidden-window smoke test.

### Stage 7: Final repository verification

- [ ] Rescan all repository-owned C++ headers.
- [ ] Review any remaining semantic comment candidates.
- [ ] Run the complete test-enabled `all` build and required smoke tests.
- [ ] Move lasting migration results into the coding standard and archive this
  plan.

#### Acceptance Gate

- No known in-scope convention violations remain.
- Full validation succeeds and the plan is archived.

## Validation Matrix

| Area | Validation |
| --- | --- |
| Comment and spacing changes | Targeted static rescan; DHT regeneration through BuildTool for reflected headers |
| Per-function exports | Static rescan plus owning-module build |
| `Core` | `Core` target |
| `CoreDObject` | `CoreDObject` target |
| `Engine` | `Engine` target |
| First tranche integration | Complete `all` target |
| Later rendering/editor stages | Module builds plus the integration and smoke coverage named by each stage |

## Definition of Done

- All repository-owned modules satisfy the coding standard.
- Every stage has acceptance evidence and an independent provenance-bearing
  commit.
- Generated reflection outputs are current.
- Full build and required runtime validation succeed.
- The completed plan is archived according to the plan lifecycle rules.

## Deferred Follow-ups

- Consider an automated check for class- or struct-level module export macros.
- Consider a reflection-aware layout check that treats a leading comment,
  `DPROPERTY()`, and its declaration as one unit.
- Keep semantic comment review human-owned; do not enforce comment-count targets.

## Related Documentation

- [C++ Coding Standards](../Setup/CodingStandards.md)
- [Build and Run](../Setup/BuildAndRun.md)
- [Generated Reflection System](../Architecture/ReflectionSystem.md)
- [Build System](../Architecture/BuildSystem.md)

## Related Code

- `Engine/Source/Runtime/Core`
- `Engine/Source/Runtime/CoreDObject`
- `Engine/Source/Runtime/Engine`
- `Engine/Source/Programs/DurinHeaderTool`
