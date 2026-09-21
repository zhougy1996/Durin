# Scene Material Import Modes Plan

Summary: Replace automatic shared import parents with explicit local materials or instances of a selected parent.

Last reviewed: 2026-09-22

Status: Completed
Completed: 2026-09-22

## Current Status

Both import modes, automatic standard PBR mapping, per-material preview controls,
conservative reimport and regression coverage are implemented and validated.
Existing Sandbox imports are user content and are excluded from this task.

Validation on Win64-Debug-DurinEditor:

- `build --target all` passed on the final implementation, including LevelEditor,
  Sandbox and RoadWeaver consumers of the shared Engine header.
- `test affected --test-jobs 4 --report` passed 54 of 55 targets. The remaining
  SceneImportVulkanTests failure was an obsolete duplicate parent-package cleanup
  in the migrated test, corrected and successfully rerun below.
- Final `test SceneImportTests --report`: all 14 cases passed, including the full
  standard PBR parent with masked and translucent sources, duplicate source names,
  selected-parent protection, native texture-cache rebinding and publication rollback.
- Final `test SceneImportVulkanTests --report`: the rendering integration passed.
  The other affected passing targets remain valid; later changes are confined to
  scene import policy, its editor controls, and the scene import tests.
- Changed-document validation, all-plan validation and diff whitespace checks passed.

Lasting behavior is documented in [asset import architecture](../Editor/Architecture/AssetImportFramework.md#scene-import)
and [source workflows](../Editor/Guides/SourceFileWorkflows.md#scene-sources).
Interactive editor UI smoke was not performed; editor compilation and native
rendering integration were validated.

## Goal

Scene import defaults to editable, independent materials in the destination's
Materials directory. Instance import requires an explicitly selected parent,
with an import-wide default and per-source-material overrides. New imports never
create assets in Materials/ImportedParents. Existing parent references remain valid.

## Selected Design

- Create Materials publishes M_<source name> with the existing generated PBR
  graph, authored properties, textures, and factors directly on the material.
- Create Material Instances publishes MI_<source name> and uses an existing
  parent selected with the editor asset picker. Selection never edits that parent.
- Preview lists source materials, output paths, resolved mode, and compatibility.
  Per-row settings override the import-wide mode and parent.
- Initial mapping uses the existing stable PBR parameter IDs. Compatibility
  must establish supported sampling/UV/channel semantics, not guess from names.
  Unsupported custom graphs are rejected with an actionable diagnostic; arbitrary
  custom mapping and lossy import are deferred until an explicit mapping contract
  exists. The built-in full PBR Metallic/Roughness template and compatible imported
  PBR graphs are supported. The full template resets absent maps and UVs, uses
  the original base-color alpha for transparency, and avoids adding emissive
  factors twice when they were already baked into a derived image.
- Reimport preserves existing materials (including graph edits, parent selection,
  parameters, and static properties) and mesh material bindings by default.
  The current importer has no three-way edit baseline: preserve the complete
  material instead of guessing individual conflicts. An explicit rebuild option
  replaces material edits from source and the displayed selections. Ordinary
  reimport retains the stored asset type; a requested type conversion is rejected
  and requires a separate destination. A future conversion workflow is out of scope.
- Source/output receipts remain authoritative for identity and collisions.
  Existing legacy instances remain instances on preserve-mode reimport.
- Preserve cancellation, package publication rollback, and unrelated-asset
  collision protection. Shader sharing is independent of asset ownership.

## Implementation Notes

Local material texture defaults exposed native parameter-schema references that
were not covered by object replacement. Add an Engine material-reference
participant to scene publication and package reload, and validate rollback and
successful texture rebinding. This shared Engine API addition requires an all
build before handoff.

## Implementation Stages

### Stage 0: Record the selected design

- [x] Inspect current scene import, generated graph, editor dialog, and tests.
- [x] Record scope and conservative reimport behavior before implementation.

### Stage 1: Implement material policy and editor controls

Depends on Stage 0. Complete when both modes can be selected in the editor and
are enforced by the importer without automatic shared parents.

- [x] Add local material generation and explicit parent instance generation.
- [x] Add source preview, default and per-material controls, and compatibility checks.
- [x] Preserve material edits and bindings on reimport; expose explicit rebuild.
- [x] Migrate consumers and regression tests across workspace projects.

### Stage 2: Validate and document the delivered behavior

Depends on Stage 1. Follow [build guidance](../Agents/BuildAndRun.md) and
[test guidance](../Agents/Testing.md).

- [x] Cover defaults, selected parents, incompatible parents, overrides,
  reimport preservation, collision protection, and publication failure.
- [x] Run SceneImportTests and affected regression coverage; build editor targets.
- [x] Document the lasting contract and run documentation validators.
- [x] Commit isolated task changes with plan/stage provenance.
