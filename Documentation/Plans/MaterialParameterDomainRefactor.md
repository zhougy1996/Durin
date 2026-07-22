# Material Parameter Domain Refactor Plan

Last reviewed: 2026-07-23

## Current Status

Stage 0 is complete: Core provides `FGuid`, and GUID values are supported by
DHT, reflection, object snapshots, asset serialization, and the generic property
editor with undo/redo. Material APIs and storage still use `std::string_view`
and three reflected string-keyed maps. The Material Editor uses a private fixed
descriptor table and specialized scalar, color, and texture paths bound directly
to those maps. Stage 1 has not started.

This plan is an independently executable slice of Stage 2 in
`Documentation/Plans/MaterialSystem.md`. Check off each stage only after its
acceptance gate has been satisfied.

## Goal

Replace free-form string material storage with stable GUID identity, `FName`
lookup, explicit runtime parameter descriptions, typed values, and GUID-based
instance overrides. Drive the current five-parameter editor from that runtime
schema while preserving existing rendering behavior, inheritance, transactions,
dependency propagation, and render-thread snapshots.

## Scope

- Add a reusable `FGuid` type and complete support through DHT, reflection,
  object snapshots, asset serialization, and generic property editing.
- Define reflected material parameter definitions, values, overrides, and
  resolved-value/source information in Runtime Engine.
- Replace the three parameter maps in `DMaterial` and `DMaterialInstance` with
  ordered definition and override collections keyed by stable GUID.
- Change public material parameter lookup APIs from `std::string_view` to
  `FName`, with GUID as the authoritative persistent identity.
- Replace the editor-private descriptor table and map-specific bindings with a
  runtime-schema-driven parameter panel.
- Update focused tests and long-lived material architecture documentation.

## Non-Goals

- Editing the parameter declaration collection: parameters cannot yet be added,
  removed, renamed, or retyped by users.
- Migrating legacy material parameter values or instance overrides.
- Static material properties, shader-map identity, pipeline permutations, or a
  new render-data layout.
- Material graphs, parameter nodes, dynamic material instances, or batched
  runtime updates.

## Design Decisions and Invariants

- `FGuid` is a reusable Core value composed of four `uint32` fields. It supports
  comparison, hashing, `NewGuid()`, canonical string formatting, and strict
  parsing. Reflection and serialization store the four numeric fields, never a
  process-local index or hash.
- `FGuid` receives a first-class reflected property kind. It must work as a
  direct property and inside reflected structures and arrays, including
  snapshots and undo/redo.
- Parameter GUID is persistent identity. `FName` is case-insensitive human/API
  lookup. A material definition set cannot contain an invalid GUID, duplicate
  GUID, `None` name, or duplicate name.
- The five built-in parameters have hard-coded GUIDs that never change:
  `BaseColor`, `BaseColorTexture`, `Opacity`, `SpecularStrength`, and
  `Shininess`.
- `FMaterialParameterValue` contains scalar, vector, and texture fields. The
  definition's `EMaterialParameterType` selects the active field; reflection
  does not gain `std::variant` support in this slice.
- `FMaterialParameterDefinition` contains GUID, `FName`, value type, current
  base-material value, display name, group, sort order, presentation hint,
  optional numeric range, and texture-usage hint. Built-in definition identity,
  type, ordering, and metadata are canonical; only their values are editable.
- `FMaterialParameterOverride` stores only parameter GUID and value. Instances
  never duplicate declaration name or type.
- Resolution order is current instance override, then each parent instance,
  then the root material value. Resolution reports the object supplying the
  value and whether the current instance owns an override.
- An instance setter rejects unknown parameters and type mismatches without
  modifying storage, dirtying its package, or incrementing render state.
- Parent changes preserve unmatched overrides as orphans. Orphans do not
  participate in rendering and remain visible for explicit removal in the
  editor.
- Renderer-facing code continues to consume immutable `FMaterialRenderData`.
  GUID and `FName` lookup remains on the material/game-thread side.
- Legacy string maps are removed without compatibility fields or migration.
  Old base materials load canonical defaults; old instances retain `Parent`
  but lose legacy overrides. The existing asset loader warning for skipped
  incompatible fields is the diagnostic.

## Public Interfaces and Types

- Add `FGuid` in Core plus `std::hash<FGuid>`.
- Add `EMaterialParameterType`, `EMaterialParameterPresentation`,
  `FMaterialParameterValue`, `FMaterialParameterDefinition`,
  `FMaterialParameterOverride`, and `FResolvedMaterialParameter` in Engine.
- Expose the canonical built-in GUID and `FName` constants from Runtime Engine.
- `DMaterialInterface` exposes definition enumeration, lookup by GUID or
  `FName`, value resolution by GUID, and resolved source information.
- Typed scalar/vector/texture getters and setters accept `FName`. Setters and
  clear operations return `bool`; no `std::string_view` compatibility overloads
  remain.
- `DMaterial` owns an ordered reflected definition collection.
  `DMaterialInstance` owns an ordered reflected override collection and exposes
  GUID-based set, clear, local-override, and orphan queries.

## Implementation Stages

### Stage 0: Establish FGuid Infrastructure

- [x] Implement `FGuid` storage, validity, equality/order, hashing, generation,
  canonical formatting, and strict parsing in Core.
- [x] Add the GUID property kind, property class, generator parameters, cast
  flags, construction, and DHT type recognition.
- [x] Add GUID handling to asset serialization, object graph archives, property
  snapshots, comparisons, restore paths, and generic property display/editing.
- [x] Add DHT, Core, CoreDObject, AssetCore, and reflected editor tests for
  direct, nested, and array GUID values.

#### Acceptance Gate

- GUID values round-trip byte-for-byte through assets and object snapshots;
  reflected edits support undo/redo; all focused tests pass.

### Stage 1: Introduce the Runtime Parameter Schema

- [ ] Move parameter type, presentation, defaults, labels, ranges, and ordering
  out of the Material Editor and into reflected Runtime Engine types.
- [ ] Define permanent built-in GUID and `FName` constants and create the five
  canonical definitions in deterministic order.
- [ ] Replace `DMaterial`'s three maps with its ordered definition collection.
- [ ] Validate the complete canonical schema during `PostLoad`; reject missing,
  duplicate, renamed, retyped, reordered, or otherwise invalid definitions with
  a specific error.
- [ ] Change material lookup and typed APIs to GUID/`FName` and update all
  in-repository call sites.

#### Acceptance Gate

- Every newly constructed or reloaded material exposes the same five stable
  identities and order; GUID/name lookup, type rejection, and invalid-schema
  diagnostics are covered by focused tests.

### Stage 2: Replace Instance Storage and Preserve Rendering

- [ ] Replace the three instance maps with the unified GUID override collection.
- [ ] Implement definition-aware set, clear, local override, orphan detection,
  resolved value, and resolved source APIs across arbitrary valid parent chains.
- [ ] Preserve orphan overrides on parent changes while excluding them from
  resolved values and `FMaterialRenderData`.
- [ ] Rebuild `GetRenderData()` from the five built-in GUIDs while preserving
  current defaults, clamp rules, texture resources, and output values.
- [ ] Route direct API edits, reflected transactions, and parent propagation
  through the existing package and render invalidation rules.
- [ ] Verify nested texture references remain visible to dependency collection,
  serialization, duplication, and garbage collection.

#### Acceptance Gate

- Inheritance, clear/reset, parent replacement, orphan behavior, texture
  lifetime, live scene-proxy updates, and no-op version behavior pass focused
  Engine tests without changing visible material output.

### Stage 3: Make the Parameter Panel Schema-Driven

- [ ] Remove the editor-private descriptor table, reflected map-name lookup, and
  `BindStringMapValue` usage from the Material Editor.
- [ ] Add a reusable material parameter panel model whose inputs are definitions,
  resolved values, value sources, and override policy.
- [ ] Select scalar, ranged scalar, color, and asset-picker controls exclusively
  from parameter type, presentation, and metadata.
- [ ] Show override state, inherited value, supplying ancestor, reset-to-parent,
  and orphan removal for instances.
- [ ] Submit edits against the reflected collection root and locate the target
  entry by GUID in scratch storage so transactions never depend on a mutable
  array index.
- [ ] Preserve continuous-edit coalescing, Escape cancellation, undo/redo,
  package dirtiness, save/reload, and live preview updates.

#### Acceptance Gate

- All five controls are produced from Runtime Engine definitions; material and
  instance edits, override toggles, texture selection, undo/redo, save/reload,
  source display, and orphan removal pass editor-focused tests.

### Stage 4: Remove Legacy Paths and Record the Architecture

- [ ] Remove obsolete string-map helpers, constants, tests, and editor-specific
  parameter descriptors without adding migration fields.
- [ ] Add an explicit legacy-load test proving incompatible map fields are
  skipped, base defaults are restored, parent links survive, and old overrides
  are absent.
- [ ] Update material architecture documentation and this plan's status to
  record GUID identity, `FName` lookup, schema ownership, and renderer boundary.
- [ ] Run all affected test targets, a full `all` build, and a hidden-window
  DurinEditor smoke test using the repository build workflow.

#### Acceptance Gate

- No production material path depends on free-form string maps or the private
  descriptor table; full build, tests, and editor smoke validation succeed.

## Validation Matrix

| Area | Required Evidence |
| --- | --- |
| GUID foundation | Generation/parse/format/hash tests; DHT output; reflected snapshot, asset, nested-struct, array, and undo/redo round-trips |
| Schema | Stable IDs/order, GUID and case-insensitive name lookup, uniqueness checks, canonical metadata, corrupted schema rejection |
| Instances | Multi-level resolution and source, override clear, parent cycles, parent swap, orphan exclusion/removal, unknown/type-mismatch no-op |
| Resources | Nested texture save/load, dependency discovery, duplication, GC reachability, missing texture fallback |
| Rendering | Render-data parity, clamp behavior, parent propagation, stale revision handling, live proxy update without reassignment |
| Editor | Data-driven widgets, continuous edit/cancel, override enable/reset, inherited source, orphan display, asset picker, undo/redo, save/reload |
| Compatibility boundary | Legacy fields skip safely; canonical defaults and retained Parent behavior are asserted |
| Integration | Affected native tests, full `all` build, and DurinEditor hidden-window smoke pass |

## Definition of Done

- Every stage acceptance gate is satisfied and checked off.
- Persistent parameter and override identity uses `FGuid`; public name lookup
  uses `FName`; no material API accepts `std::string_view`.
- Material and instance storage contains no legacy scalar/vector/texture maps.
- The Material Editor renders parameters from Runtime Engine schema and contains
  no built-in per-parameter descriptor table.
- Current material rendering, inheritance, transactions, serialization, GC,
  dependency propagation, and preview behavior are preserved.
- Long-lived rules are reflected in Architecture documentation and all required
  validation is recorded in this plan.

## Deferred Follow-ups

- User-authored parameter declarations and graph parameter nodes.
- Static material properties and shader/pipeline identity.
- A versioned renderer parameter layout with compact compiled indices.
- Dynamic material instances and coalesced runtime updates.
- A legacy material conversion tool if pre-refactor assets later need recovery.

## Related Documentation

- `Documentation/Plans/MaterialSystem.md`
- `Documentation/Plans/ReflectedPropertyEditing.md`
- `Documentation/Architecture/MaterialSystem.md`
- `Documentation/Architecture/RuntimeArchitecture.md`

## Related Code

- `Engine/Source/Runtime/Core/Public/Misc/Name.h`
- `Engine/Source/Runtime/Engine/Public/Materials/MaterialInterface.h`
- `Engine/Source/Editor/MaterialEditor/Private/Widgets/MMaterialEditor.cpp`
