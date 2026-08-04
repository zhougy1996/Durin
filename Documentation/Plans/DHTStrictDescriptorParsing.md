# DHT Strict Descriptor Parsing Plan

Summary: Replace DHT's reflective dataclass loader with schema-backed, explicit project and module descriptor parsing that rejects malformed configuration before metadata generation.

Last reviewed: 2026-08-04

Status: Active
Completed:

## Current Status

Stages 0 through 2 are complete. The schemas and loading boundary are active,
DHT explicitly constructs both configuration models, model annotations match
their runtime values, and the reflective dataclass decoder has been removed.
Stage 3 is next: align DurinDevTool and verify failure-before-publication
behavior.

DHT currently
uses the generic `dataclass_from_dict` helper only for `DurinProjectConfig` and
`DurinModuleConfig`. The helper recursively reflects over dataclass annotations,
but returns scalar values without checking their types and iterates only known
fields, so unknown JSON members are silently discarded. A misspelled module
field such as `PrivateDependecies` therefore becomes the default empty list.

`DurinProjectConfig.from_file` and `DurinModuleConfig.from_file` check only for
`ProjectName` and `ModuleName`. Their model annotations also declare those
names as `Path` even though JSON loading stores and consumes them as strings,
and module collection fields use unparameterized `list` annotations.

DurinDevTool has a separate explicit parser for the same descriptors and
already checks several non-empty-string, list, duplicate, and cross-reference
rules, but it does not reject unknown fields. There is no machine-readable
schema shared by DHT, DurinDevTool scaffolding, and descriptor authors.

The project schema must cover the complete shared `.dproject` contract, not
only DHT's current projection. In particular, the runtime-owned `Mounts`
section documented in [Workspace And Projects](../Workspace/WorkspaceProjects.md)
is valid even though DHT does not retain it in `DurinProjectConfig`.

### Stage 0 Handoff

- Baseline commit: `f6d5b916`.
- Working set: DHT project/module configuration and JSON helper, DurinDevTool
  descriptor models and scaffolding, project mount parsing, module target
  generation, and DHT CMake fingerprint inputs.
- Key symbols: `DurinProjectConfig.from_file`,
  `DurinModuleConfig.from_file`, `dataclass_from_dict`,
  `load_project_descriptor`, `load_module_descriptor`,
  `ParseProjectMounts`, and `DURIN_DHT_TOOL_INPUTS`.
- Decisions: Draft 2020-12 schemas own structural validation; `Shared` and
  `Static` are the only supported `LinkType` values; the incidental DHT
  `Interface` API-macro branch is not a supported descriptor value; `Mounts`
  is part of the complete project schema; omitted optional fields retain
  current defaults; filesystem and cross-descriptor rules remain semantic.
- Open questions: none.
- Validation: targeted consumer searches covered all tracked descriptor keys,
  all 2 project and 22 module descriptors, DurinDevTool scaffolding, CMake
  linkage selection, and the C++ seven-field mount parser.

### Stage 1 Handoff

- Baseline commit: `f1c03be0`.
- Working set: `requirements.txt`, DHT descriptor schemas and JSON helper,
  schema-focused DHT tests, CMake DHT fingerprint inputs, and this plan.
- Key symbols: `load_json_descriptor`, `_load_schema_validator`,
  `_object_without_duplicate_keys`, `_json_path`, and
  `DURIN_DHT_TOOL_INPUTS`.
- Decisions: use pinned `jsonschema==4.25.1`; keep schemas outside the Python
  package under the DHT root; cache checked Draft 2020-12 validators; report the
  first deterministic schema error; retain `load_json_file` without implicit
  descriptor validation for generated JSON formats.
- Open questions: none.
- Validation: both schemas passed Draft 2020-12 self-validation; 2 tracked
  projects, 22 tracked modules, rendered project/module templates, complete
  mounts, malformed JSON, duplicate keys, unknown fields, wrong types, nulls,
  and duplicate items passed 15 focused tests.

### Stage 2 Handoff

- Baseline commit: `fd91b6cd`.
- Working set: DHT project/module configuration models, JSON helper exports,
  DHT configuration/schema tests, and this plan.
- Key symbols: `DurinProjectConfig.from_file`,
  `DurinModuleConfig.from_file`, `DurinProjectRuntimeVariantConfig`, and
  `load_json_file`.
- Decisions: preserve programmatic dataclass construction used by focused
  reflection tests while keeping every internal field out of the explicit JSON
  extraction map; copy input lists/maps during construction; remove the now
  unused `required_fields` option from the general generated-JSON loader; treat
  invalid UTF-8 as a descriptor-boundary error.
- Open questions: none.
- Validation: the complete DurinHeaderTool Python suite passed 145 tests;
  targeted search found no remaining reflective decoder, Pascal-case mapper,
  or JSON-key dataclass metadata in production DHT code.

## Goal

Make malformed `.dproject` and `.dmodule` files fail deterministically during
configuration with a file-qualified, property-qualified diagnostic, while
reducing DHT configuration parsing to two explicit constructors plus a small
schema-validation boundary.

## Scope

- Add Draft 2020-12 JSON Schema files for the complete `.dproject` and
  `.dmodule` structural contracts.
- Reject unknown root and nested fields, wrong JSON types, nulls where values
  are required, empty names and paths, duplicate JSON object keys, and invalid
  closed-set values.
- Preserve the current omission defaults for optional fields.
- Parse `DurinProjectConfig`, nested runtime-variant configs, and
  `DurinModuleConfig` explicitly after schema validation.
- Correct configuration model annotations so stored JSON values have their
  actual Python types and derived runtime-only fields cannot be supplied by
  JSON.
- Remove `dataclass_from_dict` and its public export after its two callers are
  migrated; retain general JSON file reading for export and cache models.
- Make schema changes participate in DHT's CMake configure dependency and tool
  fingerprint contract.
- Keep DurinDevTool descriptor loading and rendered scaffolding outputs
  conformant with the same schemas.
- Document the schema locations and the boundary between structural schema
  validation and cross-file/workspace semantic validation.

## Non-Goals

- Replacing explicit serializers or parsers for DHT exports, manifests,
  persistent caches, or reflection models.
- Building another general dataclass decoder, schema generator, coercion
  framework, or repository-wide JSON abstraction.
- Changing project/module defaults, dependency visibility, runtime-variant
  enablement, reflection header behavior, generated CMake content, or build
  ordering.
- Encoding filesystem existence, path containment, project ownership,
  dependency graph validity, or case-insensitive workspace uniqueness in JSON
  Schema; those remain explicit semantic checks after structural parsing.
- Requiring every descriptor to contain a `$schema` member or publishing a
  remotely hosted schema URL.
- Making the C++ runtime embed a general JSON Schema engine.

## Design Decisions and Invariants

- The machine-readable structural contracts live beside DHT as
  `schemas/durin-project.schema.json` and
  `schemas/durin-module.schema.json`. They use JSON Schema Draft 2020-12 and
  are repository-owned inputs, not generated files.
- The project schema describes all currently supported top-level sections,
  including `$schema`, `ProjectName`, `ModuleDirs`, `BaseModules`,
  `ExtraModules`, and runtime-owned `Mounts`. DHT validates the complete
  document and explicitly extracts only its build-configuration projection.
- The module schema describes `$schema`, `ModuleName`, `LinkType`, `PCH`, all
  four dependency lists, and `ReflectHeaders`. Every object whose keys are a
  closed contract uses `additionalProperties: false`; map-shaped sections use
  constrained property names and value schemas.
- `ProjectName` and `ModuleName` remain the only required DHT construction
  fields. Optional omissions retain today's defaults: empty maps/lists,
  `LinkType = "Shared"`, `PCH = "Self"`, and project base modules derived from
  `ModuleDirs` when `BaseModules` is omitted.
- `LinkType` accepts only the casing and values intentionally supported by the
  build contract. Stage 0 must reconcile the existing generator's incidental
  `Interface` branch with DurinDevTool's supported `Shared`/`Static` creation
  surface before the enum is frozen.
- An optional `$schema` string is accepted for editor integration but ignored
  by the runtime model. Repository tooling must not depend on a descriptor's
  relative location to discover the validator used at runtime.
- The pinned `jsonschema` library performs structural validation. DHT does not
  reimplement JSON Schema or reflect over dataclass annotations. Compiled
  validators are cached per process and schema errors are normalized into a
  stable diagnostic containing the descriptor path and JSON property path.
- The common parsing layer is limited to UTF-8 JSON object loading, duplicate
  member detection, schema selection/validation, and diagnostic formatting.
  `DurinProjectConfig.from_file` and `DurinModuleConfig.from_file` own explicit
  field extraction and construction.
- Parsing never coerces values. A string is not accepted as a one-element
  array, a number or boolean is not accepted as a string, and `null` is not
  treated as omission.
- Exact duplicate list entries are rejected structurally with `uniqueItems`.
  Case-insensitive duplicates and cross-list conflicts remain semantic checks
  where the build system's case-insensitive naming contract can be reported
  with better context.
- `project_dir`, `config_file_path`, derived module descriptor paths,
  `module_dir`, `owning_project`, and `api_macro` are internal fields. They are
  assigned by the loader or post-initialization and never appear in the schema
  or accepted JSON field set.
- All descriptors needed by one configuration operation are validated before
  generated project/module metadata is published. A validation error cannot
  leave a partially updated configuration result.
- Schema files are included in `DURIN_DHT_TOOL_INPUTS`; changing a schema
  invalidates CMake configure state and DHT's tool fingerprint just like a
  Python implementation or dependency change.
- DurinDevTool may retain its own immutable descriptor models and semantic
  checks, but structural acceptance must be driven by the same schema files.
  Its create commands must never emit a descriptor that DHT rejects.

## Current Foundations and Gaps

- `load_json_file` already centralizes UTF-8 JSON loading, but does not convert
  malformed-JSON, I/O, root-type, or duplicate-key failures into a consistent
  descriptor diagnostic.
- `dataclass_from_dict` spans roughly 130 lines, builds an unused forward field
  mapping, recursively handles shapes that the two configurations do not need,
  and deliberately returns scalar values unchanged.
- Unknown root fields and unknown nested `ExtraModules.<variant>` fields are
  ignored rather than rejected.
- Container shapes are not checked before iteration, and the current module
  list annotations do not state an element type.
- Repository `.dproject` files currently use project identity, module maps,
  base roots, and runtime-variant roots; `.dmodule` files use the complete
  dependency/PCH/reflection field set. No current descriptor opts into a
  schema.
- DurinDevTool's explicit descriptor parser provides useful semantic checks
  that should remain after shared structural validation, but its accepted
  field set can drift from DHT.
- DHT's tool fingerprint currently globs only package `*.py` files plus
  `requirements.txt`, so adding schema files without updating the fingerprint
  would allow stale configuration/cache identity.
- The existing DHT configuration test proves each dataclass initializes once,
  but does not cover unknown fields, scalar/container element types, nested
  runtime-variant fields, duplicate keys, diagnostic paths, or mutation-free
  failure.

## Descriptor Contract Matrix

The schemas enforce the structural rows below. "Semantic" constraints are
validated by the named consumer after schema validation because they depend on
filesystem state, case folding, or another descriptor.

| Descriptor path | Required | Structural contract | Default or semantic owner |
| --- | --- | --- | --- |
| `.dproject.$schema` | No | Non-empty string | Ignored by runtime models; editor association only. |
| `.dproject.ProjectName` | Yes | C++ identifier string | Workspace identity uniqueness is semantic. |
| `.dproject.ModuleDirs` | No | Object from C++ identifier keys to non-empty strings | Defaults to `{}`; relative containment and overlap are semantic. |
| `.dproject.BaseModules` | No | Unique array of C++ identifier strings | Defaults to all `ModuleDirs` keys; ownership is semantic. |
| `.dproject.ExtraModules` | No | Object from runtime-variant identifier keys to closed objects | Defaults to `{}`; runtime-variant naming is workspace-owned. |
| `.dproject.ExtraModules.*.Modules` | No | Unique array of C++ identifier strings | Defaults to `[]`; project ownership is semantic. |
| `.dproject.Mounts` | No | Unique array of closed seven-field mount objects | Defaults to no custom mounts; runtime path system owns semantic checks. |
| `.dproject.Mounts[].VirtualRoot` | Yes | Non-empty forward-slash virtual-root string | Reserved roots, canonical overlap, and dependency graph are semantic. |
| `.dproject.Mounts[].Owner` | Yes | `Extension` or `ExternalSources` | No default. |
| `.dproject.Mounts[].Root` | Yes | Non-empty string | Descriptor-relative containment is semantic. |
| `.dproject.Mounts[].ContentPath` | Yes | Non-empty string | Root-relative containment is semantic; `.` remains valid. |
| `.dproject.Mounts[].AutoScan` | Yes | Boolean | No default. |
| `.dproject.Mounts[].AuthoringWritable` | Yes | Boolean | No default. |
| `.dproject.Mounts[].Dependencies` | Yes | Unique array of non-empty virtual-root strings | Referenced roots and cycles are semantic. |
| `.dmodule.$schema` | No | Non-empty string | Ignored by runtime models; editor association only. |
| `.dmodule.ModuleName` | Yes | C++ identifier string | Registration match and workspace uniqueness are semantic. |
| `.dmodule.LinkType` | No | `Shared` or `Static` | Defaults to `Shared`. |
| `.dmodule.PCH` | No | Non-empty CMake-target identifier string | Defaults to `Self`; target existence is semantic. |
| `.dmodule.PrivateDependencies` | No | Unique array of C++ identifier strings | Defaults to `[]`; graph validity is semantic. |
| `.dmodule.PublicDependencies` | No | Unique array of C++ identifier strings | Defaults to `[]`; graph validity is semantic. |
| `.dmodule.OptionalPrivateDependencies` | No | Unique array of C++ identifier strings | Defaults to `[]`; graph validity is semantic. |
| `.dmodule.OptionalPublicDependencies` | No | Unique array of C++ identifier strings | Defaults to `[]`; graph validity is semantic. |
| `.dmodule.ReflectHeaders` | No | Unique array of non-empty strings | Defaults to `[]`; path existence/containment is semantic. |

## Implementation Stages

### Stage 0: Freeze the shared descriptor contract

- [x] Inventory every `.dproject` and `.dmodule` field consumed by DHT,
  DurinDevTool, CMake scaffolding, and the C++ project/mount loader.
- [x] Record each field's required/optional state, JSON type, default, non-empty
  and uniqueness constraints, and whether validation is structural or semantic.
- [x] Resolve the supported `LinkType` set from actual CMake behavior rather
  than preserving unreachable or accidental string values.
- [x] Define the schema location, stable `$id` values, optional `$schema`
  behavior, descriptor-path/JSON-path diagnostic format, and duplicate-key
  behavior.
- [x] Capture valid compatibility fixture requirements for minimal and fully populated
  descriptors, including a project with `Mounts`, plus invalid fixtures for the
  currently silent failure modes.

#### Acceptance Gate

- One reviewed field matrix covers the complete shared descriptor format, no
  valid current consumer-owned field would be rejected as unknown, and every
  proposed schema constraint has a named runtime or tooling owner.

### Stage 1: Add schemas and the strict loading boundary

- [x] Pin `jsonschema` in `requirements.txt` and add the two Draft 2020-12
  schema files with closed objects, typed map values, typed array elements,
  `minLength`, `uniqueItems`, required fields, defaults-as-annotations, and the
  selected `LinkType` enum.
- [x] Add a small descriptor loader that reads a top-level JSON object, rejects
  duplicate member names, validates it with a cached schema validator, and
  raises deterministic file/path-qualified errors.
- [x] Keep `load_json_file` available to non-configuration JSON models without
  implicitly applying a descriptor schema.
- [x] Extend `DURIN_DHT_TOOL_INPUTS` to cover the schema files and add a focused
  check proving schema content changes affect the computed tool fingerprint.
- [x] Add schema self-validation and data-driven tests over minimal, complete,
  malformed, wrong-type, null, unknown-field, nested-unknown, duplicate-key,
  empty-string, and duplicate-list fixtures.
- [x] Validate every tracked descriptor and every rendered project/module
  scaffolding template against the schemas.

#### Acceptance Gate

- Both schemas pass Draft 2020-12 self-validation, all supported repository and
  rendered-template descriptors validate, and `PrivateDependecies` plus every
  other invalid fixture fails with the descriptor path and offending JSON path.

### Stage 2: Replace reflective DHT construction

- [x] Implement explicit project parsing that constructs
  `DurinProjectRuntimeVariantConfig` values and `DurinProjectConfig` from only
  the supported build fields after whole-document validation.
- [x] Implement explicit module parsing that supplies each supported field and
  omission default directly to `DurinModuleConfig`.
- [x] Change project/module names to `str`, parameterize every collection, and
  mark derived/internal fields so callers cannot confuse them with descriptor
  inputs.
- [x] Preserve exactly-once post-initialization, base-module derivation, module
  config path derivation, API macro generation, and resolved path ownership.
- [x] Remove `dataclass_from_dict`, `_snake_to_pascal`, reflection imports, and
  the public `durin_header_tool.io` export once no caller remains.
- [x] Normalize malformed JSON, schema failure, and file I/O errors at the
  configuration boundary without hiding their original line/column or property
  context.

#### Acceptance Gate

- Valid minimal and complete descriptors produce configuration objects equal
  to the pre-change behavior, internal fields are derived rather than parsed,
  and no generic dataclass decoding code remains in DHT.

### Stage 3: Align tooling and configuration failure behavior

- [ ] Make DurinDevTool's project/module descriptor loaders apply the same
  structural schemas before their existing semantic checks.
- [ ] Preserve DurinDevTool's stronger case-insensitive uniqueness,
  self-dependency, missing-reference, and path-containment diagnostics after
  structural validation.
- [ ] Update scaffolding/schema editor associations or schema references without
  embedding location-fragile paths in generated external project descriptors.
- [ ] Add parity tests showing DHT and DurinDevTool accept the same structural
  fixtures and reject unknown/wrong-type fields before semantic graph checks.
- [ ] Add a DHT `prepare_project_build` integration test proving an invalid
  project or owned module fails before generated metadata or stamps are
  published.
- [ ] Verify a valid project containing runtime `Mounts` passes DHT validation
  even though the DHT project model intentionally does not retain that section.

#### Acceptance Gate

- DHT configuration and DurinDevTool scaffolding agree on the structural
  contract, malformed descriptors cannot reach metadata generation, and the
  runtime-owned project fields remain accepted and protected by the schema.

### Stage 4: Qualification and contract handoff

- [ ] Run the focused DurinHeaderTool and DurinDevTool Python suites through the
  repository environment described in [Build And Run](../Development/Build/BuildAndRun.md).
- [ ] Run a normal root configuration for the registered Engine and Sandbox
  descriptors and complete an `all` build through DurinDevTool.
- [ ] Exercise representative unknown-field, wrong-element-type, malformed
  JSON, duplicate-key, invalid nested runtime variant, and valid `Mounts`
  failures through the real DHT CLI entrypoint.
- [ ] Update [Workspace And Projects](../Workspace/WorkspaceProjects.md) with
  schema locations, optional-field defaults, strict unknown-field behavior,
  and the structural-versus-semantic validation boundary.
- [ ] Record the baseline commit, working set, schema/version decision, accepted
  field matrix, diagnostic examples, and validation outcome in the final stage
  handoff.

#### Acceptance Gate

- Focused Python suites and the full `all` build pass, valid repository
  descriptors generate unchanged build metadata, and every targeted malformed
  descriptor fails before configuration output publication with an actionable
  diagnostic.

## Validation Matrix

| Scenario | Required result |
| --- | --- |
| Minimal `.dproject` | `ProjectName` loads; optional maps/lists use current defaults. |
| Minimal `.dmodule` | `ModuleName` loads; linkage, PCH, dependency, and reflection defaults are preserved. |
| Fully populated descriptors | Explicit models match current generated metadata and ordering. |
| Project with valid `Mounts` | Whole descriptor validates; DHT ignores the runtime-only projection without treating it as unknown. |
| Misspelled root field | Configuration fails and suggests or identifies the unknown key and descriptor path. |
| Unknown nested runtime-variant/mount field | Validation reports the complete JSON path and does not construct a partial model. |
| Wrong list/container/scalar type | Validation fails without coercion, including wrong list element types. |
| `null` in an optional typed field | Validation fails; omission alone selects the default. |
| Duplicate JSON object member | Loading fails instead of silently keeping the last value. |
| Duplicate exact list item | Structural validation fails through `uniqueItems`. |
| Case-insensitive or cross-file conflict | Existing semantic validation reports the workspace-level error after schema validation. |
| Malformed JSON | Diagnostic retains file, line, and column. |
| Schema changes | CMake reconfigures and DHT's tool fingerprint changes. |
| DurinDevTool-created descriptor | Rendered output validates and is accepted by DHT. |
| Invalid configure input | No project/module generated metadata or success stamp is partially published. |
| Existing repository corpus | All tracked descriptors validate and the `all` build succeeds. |

## Definition of Done

- The two checked-in schemas are the documented structural source of truth for
  `.dproject` and `.dmodule` files.
- DHT uses explicit project/module parsing and contains no general reflective
  dataclass JSON constructor.
- Unknown fields, primitive/container type mismatches, invalid nested shapes,
  duplicate keys, and invalid closed-set values fail during configuration with
  actionable file/property context.
- Existing optional-field defaults and valid generated metadata remain
  unchanged.
- Schema files participate in configure dependencies and the DHT tool
  fingerprint.
- DurinDevTool loaders and scaffolding conform to the same structural contract
  while retaining their semantic checks.
- Focused tests, real CLI failure tests, root configuration, and a full `all`
  build pass.
- Lasting descriptor rules and schema locations are documented in the workspace
  documentation, and the implementation commits record this plan's stage
  provenance.

## Deferred Follow-ups

- A schema/version migration protocol can be added when the first incompatible
  descriptor format change is selected; this plan does not add a mandatory
  descriptor version solely for future use.
- Hosting the schemas for editors outside a Durin source checkout remains an
  installed-engine/tool-distribution concern.
- Reusing the schemas from the C++ runtime without embedding a general JSON
  Schema engine requires a separate generated-validation or conformance-test
  design.
- Strict parsing for generated DHT exports, manifests, caches, and other JSON
  families remains owned by their individual versioned formats.
- Consolidating DHT and DurinDevTool's Python descriptor model classes is
  separate from sharing their structural schemas.

## Related Documentation

- [Workspace And Projects](../Workspace/WorkspaceProjects.md)
- [Build System](../Development/Build/BuildSystem.md)
- [Build And Run](../Development/Build/BuildAndRun.md)

## Related Code

- [`json_helper.py`](../../Engine/Source/Programs/DurinHeaderTool/durin_header_tool/io/json_helper.py)
- [`project_config.py`](../../Engine/Source/Programs/DurinHeaderTool/durin_header_tool/config/project_config.py)
- [`module_config.py`](../../Engine/Source/Programs/DurinHeaderTool/durin_header_tool/config/module_config.py)
- [`test_build_configuration.py`](../../Engine/Source/Programs/DurinHeaderTool/tests/test_build_configuration.py)
- [`descriptors.py`](../../Tools/DurinDevTool/durin_dev_tool/build/descriptors.py)
- [`ProjectSetup.cmake`](../../CMake/Project/ProjectSetup.cmake)
