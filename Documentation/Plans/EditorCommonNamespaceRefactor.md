# Editor Common Namespace Refactor Plan

Summary: Move reusable editor infrastructure out of the `Durin` root namespace and remove redundant editor/reflection prefixes from its public vocabulary.

Last reviewed: 2026-08-11

Status: Completed
Completed: 2026-08-11

## Current Status

All stages are complete. Transaction and reflected-property editing APIs now
live in `Durin::Editor`; redundant `Editor` and `ReflectedProperty` stems were
removed from their type names, and the public/private source pairs use concise
filenames. All repository consumers were migrated without root-namespace
compatibility aliases.

The three previously duplicated property-target comparisons are centralized as
named `FPropertyEditTarget` contracts for mutation recursion, stable UI drafts,
and continuous edits. `EditorPropertyTests` passes 27 tests, the complete native
test suite passes, and the `Win64-Debug-DurinEditor` full `all` build passes.

## Goal

- Establish `Durin::Editor` as the ownership boundary for reusable editor-only APIs.
- Make property editing and transaction names concise after namespace qualification.
- Preserve behavior, ABI export coverage, undo/redo semantics, and editor UI behavior.
- Remove duplicated reflected-property target comparison implementations.

## Scope

- Move transaction APIs into `Durin::Editor` and rename them from
  `Editor::FTransactionManager`-style names to `FTransactionManager`-style names.
- Move reflected-property editing and view APIs into `Durin::Editor`, removing
  the redundant `ReflectedProperty` stem where the remaining name is unambiguous.
- Rename the three public/private source pairs to `Transaction`,
  `PropertyEditing`, and `PropertyView`.
- Update all engine/editor consumers, native tests, the architecture document,
  and active plans that name the migrated headers.
- Centralize mutation, stable, and continuous-edit target comparisons on the
  property edit target contract.

## Non-Goals

- Moving runtime reflection, property change, or snapshot types out of `Durin`.
- Moving `DEditorEngine`, workspace, notification, asset-picker, thumbnail, or
  source-management APIs in this change.
- Renaming concrete Level Editor, Material Editor, or Texture Editor types.
- Providing compatibility aliases in `Durin`; aliases would preserve the root
  namespace pollution this plan removes.

## Design Decisions

- The public boundary is `Durin::Editor`, without a deeper `Property` namespace.
  Names such as `Editor::FPropertyEditTarget` remain specific while avoiding a
  long qualification chain.
- Editor-only prefixes are removed only when the namespace supplies the same
  information. Runtime-owned names such as `FPropertyValueSnapshot` remain
  unchanged.
- Target identity is explicit through named comparison methods rather than
  `operator==`, because mutation recursion, UI draft continuity, and continuous
  map-key rename intentionally use different identity rules.
- This is an intentional source migration. All repository consumers move in the
  same change; no deprecated aliases are retained.

## Stages

### Stage 1: Transaction namespace boundary

- [x] Rename transaction files and move the public contract and implementation
  into `Durin::Editor`.
- [x] Remove redundant `Editor` stems from transaction type names.
- [x] Update all consumers and transaction tests.

### Stage 2: Property editing namespace and identity

- [x] Rename property editing/view files and move their APIs into
  `Durin::Editor`.
- [x] Shorten reflected-property editing and view type/function names.
- [x] Replace duplicated target comparison loops with named target methods.
- [x] Update all consumers and property editing/view tests.

### Stage 3: Documentation and validation

- [x] Update the reflected-property editing architecture document and active
  plan references.
- [x] Run the smallest affected native test target.
- [x] Complete a full `all` build because the public editor API crosses modules
  and changes user-visible editor code.
- [x] Mark this plan completed with the final validation evidence.

## Validation

- `EngineTests` through `\.\DevTool.bat test --target EngineTests`.
- `Win64-Debug-DurinEditor` full `all` build through DurinDevTool.
