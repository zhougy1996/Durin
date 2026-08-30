# Asset Compatibility Developer Boundary Plan

Summary: Move project-wide asset compatibility audit and canonical-resave orchestration into a target-selected Developer module while retaining one shared Runtime schema validator for package load safety.

Last reviewed: 2026-08-30

Status: Completed
Completed: 2026-08-30

## Current Status

- `AssetMaintenance` is now a target-selected Developer module containing mounted-package snapshots, deterministic batch audit/reporting, and canonical-resave planning/application orchestration. `DurinEd`, `AssetTools`, `MainFrame`, and `DurinAssetTool` use that boundary.
- Runtime `Engine` retains the reflection catalog, per-package range probe, shared descriptor comparison, and load safety. Its built dylib no longer exports project snapshot/report or canonical-resave batch symbols.
- Ordinary authored loading now calls the same decoded-schema evaluator as compatibility inspection before object construction. Cooked native projection retains its separate manifest exception.
- V7 default audit decodes already range-read sections directly, without reconstructing a metadata ObjectStream. Payload decoding is opt-in for canonical-resave nested migration evidence and is admitted only for exact nested route descriptors.
- `DurinEditor`, `DurinAssetTool`, `MainFrame`, `AssetPackageTests`, and `EditorAssetWorkflowTests` build or pass in the configured Editor preset. The current host profile registers no `DurinGame` preset, so game exclusion is verified statically by target composition, exported-symbol inspection, and a non-Editor CMake guard; an actual game build remains open.
- The implementation reuses AssetRegistry's `FDecodedPackage` as the compact descriptor projection instead of adding a second package-schema model. Descriptor-only decode leaves `FValue` payloads empty; this avoids another conversion and keeps one validated table representation shared by loader and audit.

## Goal

Establish a strict ownership boundary in which Runtime owns package parsing and the single source of truth for schema/load validation, a new Developer `AssetMaintenance` module owns project-wide audit and canonical-resave workflow, and Editor modules own only presentation and editor-specific task integration. The normal game target must neither link nor expose the project audit stack.

## Scope

- Extract a reusable Runtime package-schema snapshot and comparison result from the existing load and audit implementations.
- Add a target-selected `AssetMaintenance` Developer module for project enumeration, audit requests/results, report serialization, cancellation, fingerprint policy, and canonical-resave planning/orchestration.
- Migrate `DurinEd`, `AssetTools`, `MainFrame`, and `DurinAssetTool` to the new boundary.
- Replace the V7 metadata-only ObjectStream reconstruction with direct section/descriptor inspection and narrow any detailed decode to exact migration candidates.
- Remove obsolete Runtime project-audit APIs and implementation after all consumers migrate.

## Non-Goals

- Reintroducing V6 package reading, conversion, or compatibility policy.
- Changing the V7 on-disk format solely to complete this ownership move.
- Moving the low-level atomic package write, load, or object mutation primitives out of Runtime `Engine` when they are also required by normal asset operations.
- Making Editor UI types, task-system handles, or dialogs part of the Developer module API.
- Caching compatibility verdicts across reflection-catalog changes. Only package-derived schema data may be cached independently.

## Selected Design

### Runtime boundary

- Runtime `Engine` owns seekable package access, V7 section validation, package-schema extraction, load preflight, and low-level canonical package operations.
- Introduce a compact package-derived schema representation containing only identity, class/type references, object/field descriptors, value-location facts needed by validation, and package-format evidence. It must not contain recursively decoded `FValue` trees or payload bytes.
- Introduce one schema comparison engine used by both ordinary package loading and Developer audit. The comparison accepts an explicit reflection/schema catalog and returns structured findings suitable for either load rejection/migration routing or report conversion.
- Keep load-time validation local to the package being loaded. Runtime must not enumerate projects, compute whole-project reports, schedule audit jobs, or depend on `AssetMaintenance`.

### Developer boundary

- Add `Engine/Source/Developer/AssetMaintenance` as a shared, UI-neutral module selected only by authoring/tool targets.
- `AssetMaintenance` owns audit request/result/report types, mounted-package enumeration, cancellation checkpoints, batch progress, fingerprint policy, report persistence, canonical-resave selection, planning, and batch orchestration.
- The module may depend on Runtime `Core`, `CoreDObject`, `AssetRegistry`, and `Engine`; Runtime modules must not depend on it. It must not depend on any Editor module.
- Canonical resave remains two-layered: Developer selects and coordinates packages from audit evidence; Runtime performs the low-level single-package load/validate/write transaction.

### Consumer boundary

- `DurinEd` adapts the UI-neutral Developer operation to editor task state and notifications.
- `MainFrame` owns the compatibility window, filters, controls, and presentation only.
- `AssetTools` owns editor policy and delegates compatibility analysis and batch resave planning to `AssetMaintenance`.
- `DurinAssetTool` links `AssetMaintenance` directly for audit/report/resave commands. It must not acquire an Editor module dependency.
- Add `AssetMaintenance` to the `DurinEditor` target composition and to the tool target, but not to `DurinGame` or Runtime base modules.

### Audit cost model

- The default audit is descriptor-only: read the DURF/DAST directory, required tables, schema/object/field descriptors, and value location/type tags; validate payload spans by bounds and seek past their bytes.
- `PayloadBytesSkipped` records bytes actually bypassed by the reader, not an inferred package total.
- Avoid unconditional strong hashing during an interactive Editor audit. Use stable registry/file identity for unchanged-file reuse; request a content hash only when a reproducible CLI/report contract explicitly requires one.
- Deprecated-field handling is two-tiered. First identify exact class/field/type migration candidates from descriptors; only those candidates may request detailed value evidence. A coarse package-level deprecated-field flag must not trigger full decoding.
- If a detailed decode remains necessary for a migration rule, expose it as a separate opt-in/result state so the default audit's memory and cancellation guarantees stay observable.

### Cache and invalidation

- Cache only package-derived schema snapshots, keyed by package identity plus the file/version evidence already maintained by `AssetRegistry`.
- Compare a cached snapshot against the current reflection catalog on each audit generation. A reflection-catalog generation change invalidates verdicts and reports, not necessarily the package snapshot.
- Do not put project audit reports, UI state, resave plans, or reflection-dependent findings in Runtime `AssetRegistry` caches.

## Implementation Stages

### Stage 0: Freeze contracts and dependency direction

- [x] Inventory every caller and exported symbol in the current Runtime compatibility and canonical-resave APIs.
- [x] Record the report fields, finding categories, exit codes, cancellation behavior, and transactional resave guarantees that existing Editor and CLI consumers rely on.
- [x] Define the allowed dependency graph: Runtime modules -> no Developer dependency; `AssetMaintenance` -> Runtime only; Editor/tool consumers -> `AssetMaintenance`.
- [x] Decide which fingerprint modes remain externally observable and which report fields may change when default interactive hashing is removed.
- [x] Add focused characterization tests before moving code where current behavior is not already covered.

Completion condition: the migration surface and compatibility promises are explicit, and no unresolved ownership or report-format decision remains.

### Stage 1: Unify Runtime schema validation

- [x] Define the compact package-schema snapshot, comparison input, structured finding, and validation-policy types in the smallest Runtime public surface shared by loading and maintenance.
- [x] Extract class, field, type, missing-field, and deprecated-route comparison from the existing reader/audit paths into one implementation.
- [x] Change ordinary package loading to consume the shared comparison result without weakening its corruption, bounds, or construction-before-validation protections.
- [x] Adapt the existing audit path temporarily to the same comparison engine so loader and audit parity can be tested before module movement.
- [x] Add tests proving equivalent package/catalog inputs produce the same compatibility classification in load-preflight and audit policies.

Completion condition: one Runtime comparison engine is the source of truth, and the old duplicated comparison logic is removed.

### Stage 2: Make V7 schema extraction section-native

- [x] Add direct range-backed readers for the required V7 name, type, schema, object, field, and value-descriptor data.
- [x] Validate section offsets, counts, cross-references, and payload spans without concatenating a metadata ObjectStream.
- [x] Remove the compatibility-only ObjectStream reconstruction once the direct schema snapshot covers all audit findings.
- [x] Replace the coarse deprecated-field fallback with exact descriptor candidate detection and a separately measured detailed-evidence path.
- [x] Bind byte-read, range-request, payload-skipped, and detailed-decode statistics to actual reader operations.
- [x] Add corrupt-range, cancellation, large-payload, exact-deprecated-route, and no-package-sized-allocation tests.

Completion condition: default V7 compatibility inspection creates neither a complete package buffer, a reconstructed ObjectStream, nor recursive `FValue` trees.

### Stage 3: Introduce the AssetMaintenance Developer module

- [x] Add the module descriptor, build registration, public/private layout, and `Engine.dproject` registration for `AssetMaintenance`.
- [x] Move project audit request/result/report models, snapshot enumeration, batch progress, cancellation, fingerprint selection, and report serialization out of Runtime `Engine`.
- [x] Move canonical-resave selection and batch planning/orchestration into the module while calling retained Runtime single-package transaction primitives.
- [x] Keep the public API synchronous and UI-neutral at its core, with caller-provided cancellation/progress hooks so Editor tasks and CLI execution share the same operation.
- [x] Add module-level tests for deterministic ordering, cancellation between packages/ranges, unchanged-file reuse, report output, and partial resave failure.

Completion condition: project-wide compatibility and resave policy can be used without linking an Editor module, and Runtime no longer owns their orchestration.

### Stage 4: Migrate authoring and tool consumers

- [x] Add `AssetMaintenance` to the `DurinEditor` extra-module set while leaving `DurinGame` and base modules unchanged.
- [x] Change `DurinEd` to wrap Developer progress/cancellation/results in editor task state.
- [x] Change `AssetTools` canonical resave to call Developer planning/orchestration and preserve existing editor transaction/report behavior.
- [x] Keep `MainFrame` as a thin UI consumer and remove direct knowledge of Runtime audit internals.
- [x] Link `DurinAssetTool` to `AssetMaintenance`, preserving command names, exit semantics, and editor-free execution.
- [x] Verify both Editor and CLI consume the same report/finding conversion path.

Completion condition: all project-audit and batch-resave consumers enter through `AssetMaintenance`, with no behavior-specific fork between UI and CLI.

### Stage 5: Remove obsolete Runtime audit surface

- [x] Delete the Runtime public project-audit/report/snapshot API and its implementation after downstream migration.
- [x] Remove compatibility-only full decode, metadata ObjectStream adapters, duplicate finding conversion, and obsolete V6-era naming or branches.
- [x] Audit includes and link dependencies so game Runtime cannot reach project enumeration, report serialization, hashing policy, or batch resave orchestration.
- [x] Add a target/dependency assertion or build-time check that prevents `AssetMaintenance` from entering `DurinGame` transitively.
- [x] Confirm low-level Runtime package validation remains independently tested and usable without Developer modules.

Completion condition: Runtime contains only per-package format/schema/load safety and low-level package operations; authoring maintenance code is absent from the game dependency graph.

### Stage 6: Qualification and durable documentation

- [x] Run the repository-prescribed native tests for Runtime package parsing/validation, `AssetMaintenance`, Editor integration, and `DurinAssetTool`. The complete registered native suite passes; the tool builds and its help/argument host smoke passes because no separate CLI native-test target is registered.
- [x] Build or otherwise verify the `DurinGame`, `DurinEditor`, and `DurinAssetTool` target compositions according to the repository build instructions. Editor/tool builds pass; the host has no macOS `DurinGame` preset, so game exclusion is verified by the target descriptor, non-Editor CMake guard, and absence of maintenance symbols from Engine.
- [x] Measure representative large-package audits and record peak allocation, bytes read, payload bytes skipped, cancellation latency, and wall time against the pre-migration baseline.
- [x] Update module ownership, asset package, canonical resave, Editor workspace, task-system, and CLI documentation with the final APIs and target boundaries.
- [x] Close the plan only after all acceptance gates have evidence and long-lived rules have moved to their owning documentation.

Completion condition: supported targets and tests pass, the cost guarantees are measured, and durable documentation describes the shipped ownership model.

## Acceptance Gates

- [x] `DurinGame` neither links `AssetMaintenance` nor exports project audit/report/resave-orchestration symbols.
- [x] Ordinary Runtime loads perform only per-package validation and never enumerate a project, compute a project report, or perform unconditional strong package hashing.
- [x] Loader preflight and Developer audit share the same schema comparison implementation and pass parity tests.
- [x] The default audit performs no package-sized allocation, reconstructs no full ObjectStream, recursively constructs no `FValue` tree, and reads no payload body solely to skip it.
- [x] `PayloadBytesSkipped` and byte/range counters are derived from actual reader behavior and covered by tests.
- [x] Detailed deprecated-field evidence is requested only for exact descriptor matches and remains cancellable before full value work.
- [x] Editor and CLI findings, ordering, report semantics, and canonical-resave selection agree for the same package set and reflection catalog.
- [x] Canonical resave preserves validation-before-replace, atomic write/replace, failure reporting, and bounded partial-success guarantees.
- [x] `AssetMaintenance` depends only on Runtime modules and has no Editor dependency.
- [x] Runtime V7 load safety and corruption rejection remain independently covered after the old audit surface is removed.

## Validation Guidance

Use [BuildAndRun.md](../Agents/BuildAndRun.md) before configuring or building targets and [Testing.md](../Agents/Testing.md) before selecting native tests. Plan and documentation lifecycle changes must follow [Documentation.md](../Agents/Documentation.md).

For performance qualification, include at least one package with a payload much larger than its metadata and one project-sized batch. Report peak resident allocation separately from cumulative bytes read so removal of package/ObjectStream/value duplication is visible.

## Related Code and Documentation

- [Runtime compatibility public API](../../Engine/Source/Runtime/Engine/Public/Asset/Compatibility.h)
- [Runtime compatibility implementation](../../Engine/Source/Runtime/Engine/Private/Asset/AssetCompatibility.cpp)
- [V7 package codec](../../Engine/Source/Runtime/Engine/Private/Asset/AssetPackageV7Codec.cpp)
- [ObjectStream reader and load validation](../../Engine/Source/Runtime/Engine/Private/Asset/AssetPackageObjectStreamReader.cpp)
- [Editor compatibility audit adapter](../../Engine/Source/Editor/DurinEd/Private/Asset/AssetCompatibilityAudit.cpp)
- [Editor compatibility window](../../Engine/Source/Editor/MainFrame/Private/AssetCompatibilityWindow.cpp)
- [AssetTools operations](../../Engine/Source/Editor/AssetTools/Private/AssetTools/AssetOperations.cpp)
- [DurinAssetTool host](../../Engine/Source/Programs/DurinAssetTool/Private/AssetToolMain.cpp)
- [Engine target composition](../../Engine/Engine.dproject)
- [Code module ownership](../Workspace/CodeModules.md)
- [Asset package contract](../Runtime/Assets/AssetPackages.md)
- [Canonical resave guide](../Editor/Guides/CanonicalResave.md)
- [Editor workspace architecture](../Editor/Architecture/WorkspaceFramework.md)
