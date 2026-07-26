# Enum Editor Metadata Plan

Summary: Reflected enum display metadata from generation through editor presentation.

Last reviewed: 2026-07-24

## Current Status

Completed on 2026-07-24. DHT validates and transports enum display annotations,
CoreDObject owns derived labels and record lookup, and both generic and Texture
Editor enum presentation use the shared runtime metadata. All 47 DHT tests, 49
CoreDObjectTests, and 200 EngineTests passed. The full
`Win64-Debug-DurinEditor-Tests` `all` build succeeded, and DurinEditor reached
successful engine initialization in a `--hidden-window` smoke run before the
validation process was intentionally stopped through BuildTool.

## Goal

Make editor-facing enum type and value labels first-class reflection metadata so
all editor consumers present the same names without handwritten switches or
feature-local string transformations.

## Scope

- Support an optional `DisplayName` on a reflected enum type.
- Support optional per-enumerator editor metadata, initially `DisplayName`.
- Generate readable default display names when explicit metadata is absent.
- Expose stable runtime lookup of the complete reflected enum value record.
- Make the generic reflected property editor consume enum display names.
- Migrate Texture Editor status labels to the shared reflection presentation API.
- Document the annotation syntax, runtime ownership, fallback behavior, and
  serialization boundary.

## Non-Goals

- Localized text resources or culture-dependent runtime label selection.
- Changing enum property storage, archive formats, or numeric conversion rules.
- Serializing `DisplayName` or using it as a lookup identity.
- Supporting aliases with duplicate numeric values beyond preserving their
  declaration-order metadata.
- Adding arbitrary editor metadata to every reflected declaration kind in this
  plan.
- Changing non-reflected enums.

## Design Decisions and Invariants

- `FEnumValue::Name` and `FEnumValue::Value` remain stable runtime and
  serialization identities. `DisplayName` is presentation-only metadata.
- Enum type metadata uses `DENUM(DisplayName = "...")`.
- Enumerator metadata uses a trailing annotation:

  ```cpp
  DENUM(DisplayName = "Texture Build Status")
  enum class ETextureBuildStatus : uint8
  {
      Unbuilt       DMETA(DisplayName = "Not Built"),
      MissingSource DMETA(DisplayName = "Missing Source"),
      Ready,
  };
  ```

- `DMETA` is initially valid only on enumerators parsed as part of a reflected
  enum. Unsupported keys and misplaced annotations produce a DHT diagnostic
  rather than being silently ignored.
- Missing enum type display names are derived from the short C++ name by removing
  a conventional leading `E` when followed by an uppercase letter, then
  humanizing word boundaries.
- Missing enumerator display names are derived by humanizing the enumerator
  identifier. Explicit metadata is reserved for wording, acronym, punctuation,
  or domain-label cases that cannot be inferred reliably.
- Default derivation has one CoreDObject-owned implementation. DHT transports
  explicit metadata and does not embed independently generated fallback strings
  into every generated translation unit.
- Editor code requests a reflected value record or its display name from
  `DEnum`; it does not repeat name/value scans or call `HumanizeName` locally.
- Unknown numeric values remain visible through the existing numeric fallback;
  display metadata must not make an invalid value look valid.
- For duplicate numeric values, value-to-record lookup returns the first
  declaration, matching generated declaration order. Name-to-record lookup
  remains unambiguous.

## Current Foundations and Gaps

- `ReflectedEnumInfo` and `ReflectedEnumValueInfo` already model enum identity,
  representation, and values, but contain no display metadata.
- DHT already captures string metadata for
  `DCLASS(DisplayName = "...")`; its metadata parsing and escaping behavior can
  be reused.
- `FEnumValueParams` and `FEnumValue` currently carry only name and numeric
  value.
- `DEnum` exposes separate `FindValueByName` and `FindNameByValue` operations,
  but no API returning the complete value record.
- `ReflectedPropertyView` renders both combo preview and entries from raw
  `FEnumValue::Name`.
- Texture Editor resolves reflected enum names and humanizes them locally,
  demonstrating the presentation ownership gap this plan closes.

## Implementation Stages

### Stage 0: Annotation Grammar and Diagnostics

- [x] Add the empty compile-time `DMETA(...)` macro alongside the existing
  reflection macros.
- [x] Extend DHT preprocessing so trailing enumerator annotations survive as
  Clang annotations attached to the intended enum constant.
- [x] Parse `DENUM(DisplayName = "...")` and
  `DMETA(DisplayName = "...")` with the existing quoted-string rules.
- [x] Reject duplicate `DisplayName` keys, malformed strings, unknown keys, and
  `DMETA` outside a reflected enum with actionable source diagnostics.
- [x] Verify explicit-value, signed-value, scoped, unscoped, and comma-layout
  enum syntax remains parseable.

#### Acceptance Gate

- DHT fixtures prove that enum type and enumerator annotations bind to the
  correct declarations, valid existing `DENUM()` headers remain compatible,
  and invalid metadata fails with a deterministic diagnostic.

### Stage 1: Generated and Runtime Metadata

- [x] Add optional display-name fields to `ReflectedEnumInfo` and
  `ReflectedEnumValueInfo`.
- [x] Extend `FEnumParams`, `FEnumValueParams`, generated enum tables, and
  `FEnumValue` to transport explicit display names.
- [x] Store the enum type display name in `DEnum`.
- [x] Centralize type-prefix removal and display-name humanization in a shared
  CoreDObject helper used by classes and enums where their conventions match.
- [x] Apply fallback display names during reflected enum construction when
  generated metadata omits them.
- [x] Add record-returning lookup APIs for name and numeric value, while keeping
  compatibility wrappers where they avoid unnecessary caller churn.
- [x] Define string ownership so generated UTF-8 pointers are copied into
  process-lifetime runtime metadata.

#### Acceptance Gate

- Generated enums expose explicit and derived type/value display names through
  `DEnum`, preserve signed and unsigned numeric values exactly, and retain
  existing qualified-name registration and name/value lookup behavior.

### Stage 2: Generic Editor Adoption

- [x] Render reflected enum combo previews with the selected value's
  `DisplayName`.
- [x] Render combo choices with `DisplayName` while assigning the unchanged
  numeric value.
- [x] Preserve the existing numeric representation for values absent from the
  reflected table.
- [x] Use the enum type display name in generic editor surfaces that currently
  expose its raw short name, where such surfaces exist.
- [x] Ensure UI labels have stable ImGui identities independent of duplicate
  display text.

#### Acceptance Gate

- A reflected property enum shows explicit and derived labels consistently,
  edits the correct value even when two labels are identical, and displays an
  unknown stored value without crashing or substituting another enumerator.

### Stage 3: Texture Status Migration

- [x] Add intentional display metadata to texture build status, render-resource
  state, and texture usage values where automatic humanization is insufficient.
- [x] Replace Texture Editor's qualified-name lookup plus local humanization
  path with the shared `DEnum` display API.
- [x] Search editor modules for handwritten reflected-enum name switches or
  feature-local humanization and migrate only equivalent presentation code.
- [x] Leave behavioral switches intact when they encode logic rather than
  presentation.

#### Acceptance Gate

- Texture Editor and the generic reflected property editor show the same label
  for the same enum value, and no Texture-specific status-to-text mapping or
  humanization remains.

### Stage 4: Documentation and Completion Validation

- [x] Update `Documentation/Runtime/Core/ReflectionSystem.md` with annotation
  syntax, defaults, runtime APIs, duplicate-value behavior, and serialization
  invariants.
- [x] Add DHT generation tests for explicit, implicit, malformed, and escaped
  display names.
- [x] Add CoreDObject tests for type/value fallback generation and both lookup
  directions across signed and unsigned enums.
- [x] Add editor coverage for explicit labels, derived labels, duplicate display
  labels, and unknown numeric values.
- [x] Run the task-relevant native test suites and the repository-required full
  editor validation described in the setup documentation.

#### Acceptance Gate

- DHT, CoreDObject, editor, and texture-focused tests pass; the full configured
  build succeeds; and a hidden-window DurinEditor smoke run completes without
  assertion or startup failure.

## Validation Matrix

| Area | Required Evidence |
| --- | --- |
| DHT parsing | Valid `DENUM`/`DMETA` fixtures and deterministic invalid-metadata diagnostics |
| Code generation | Generated type and value display strings, including escaping and omitted metadata |
| Runtime reflection | Explicit/fallback display names, signed/unsigned preservation, duplicate-value ordering |
| Serialization | Existing enum name/value identity and underlying-width round trips remain unchanged |
| Generic editor | Preview and choices use display labels; unknown values retain numeric fallback |
| Texture Editor | Shared reflection labels replace local humanization without changing status behavior |
| Integration | Relevant native suites, full editor build, and hidden-window smoke run |

## Definition of Done

- Enum type and enumerator display names are generated and available at runtime.
- Explicit metadata and deterministic fallback naming both work.
- Generic editor enum presentation has no dependency on raw C++ names.
- Texture status presentation uses the same runtime API as generic enum editors.
- Display text cannot affect persistence, lookup identity, or numeric storage.
- Architecture documentation describes the lasting behavior.
- All acceptance gates and required validation pass.

## Deferred Follow-ups

- Localized display-text keys and culture-aware resolution.
- Additional enumerator metadata such as `ToolTip`, `Hidden`, `Deprecated`, and
  grouping.
- Generalizing `DMETA` to reflected classes, structs, properties, and functions.
- Cached name/value lookup tables if profiling shows linear scans are material.

## Related Documentation

- `Documentation/Runtime/Core/ReflectionSystem.md`
- `Documentation/Development/Build/BuildAndRun.md`
- `Documentation/Development/Build/NativeTests.md`
- `Documentation/Plans/TextureSupport.md`

## Related Code

- `Engine/Source/Runtime/CoreDObject/Public/DObject/ObjectMacros.h`
- `Engine/Source/Runtime/CoreDObject/Public/DObject/Class.h`
- `Engine/Source/Runtime/CoreDObject/Public/DObject/DObjectGlobals.h`
- `Engine/Source/Runtime/CoreDObject/Private/DObject/Class.cpp`
- `Engine/Source/Programs/DurinHeaderTool/durin_header_tool/model/reflection_info.py`
- `Engine/Source/Programs/DurinHeaderTool/durin_header_tool/writers/reflection_source_writer.py`
- `Engine/Source/Programs/DurinHeaderTool/tests/test_reflection_generation.py`
- `Engine/Source/Editor/DurinEd/Private/Editor/ReflectedPropertyView.cpp`
- `Engine/Source/Runtime/Engine/Public/Texture/Texture2D.h`
- `Engine/Source/Editor/TextureEditor/`
