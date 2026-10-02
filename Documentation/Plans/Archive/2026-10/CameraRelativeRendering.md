# Camera Relative Rendering Plan

Summary: Move GPU spatial calculations to per-view translated world space while preserving CPU double world coordinates.

Last reviewed: 2026-10-01

Status: Archived
Completed: 2026-10-01

## Current Status

All stages are complete. The implementation uses translated GPU world positions
without an absolute material compatibility layer, as requested by the user.
The final `all` build passed; all 48 affected CPU targets passed.

Validation receipts on the macOS arm64 Debug DurinEditor profile:

- `all` builds Engine, Sandbox, RoadWeaver, and their enabled editor modules.
- `EditorRenderingTests`: 83 cases passed, including fractional mesh/light offsets at origins around 10^12, independent view origins, inverse transforms, shadow receiver matrices/depth, cloud phases, and moving-origin reprojection.
- `test affected --report`: 48 targets passed; receipt `Build/NativeTestResults/MacOS-arm64-Debug-DurinEditor/affected.xml`.
- Unsandboxed Vulkan/MoltenVK correctness on Apple M4, API 1.3.334: `StaticMeshRenderPreparationVulkanTests` with `FStaticMeshRenderPreparationVulkanTests.HitProxyIdsRespectDepthBackgroundAndCancellation` passed, including matching IDs and distances at the origin and a 10^12 origin.
- `AssetPackageReloadVulkanTests` passed production cloud pixel readback.
- `DirectionalShadowBaselineVulkanTests` with `FDirectionalShadowBaselineVulkanTests.ContactShadowRunsAndDarkensNearFieldBounded` passed shadow, contact visibility, GBuffer, and deferred-lighting coverage. Timing output is diagnostic; no performance gate is claimed.
- GPU receipts are in `Build/NativeTestResults/MacOS-arm64-Debug-DurinEditor/` under their exact target names. Application-hosted smoke and the extensive cloud timing/quality matrix were not run.
- Changed-document and all-plan lifecycle validation passed. The implementation, permanent contracts, and completed plan are committed together with Stage 4 provenance.

The first affected CPU run exposed that sky rendering accepts standalone
ViewProjectionMatrix inputs. Its existing double-precision translation removal
was retained; the final affected run passed. The permanent contract is
[Camera relative rendering](../../../Runtime/Rendering/CameraRelativeRendering.md).

## Goal

Subtract the view origin in double precision before converting GPU positions to
float. CPU scene transforms, visibility, gameplay, and persisted assets remain in
absolute double world space. Each view uses its own `ViewLocation` as the render
origin; shadow caster views inherit the receiver origin. GPU matrices and their
inverse matrices must use the same origin as their inputs.

Material WorldPosition, CameraPosition, ObjectPosition, and World
TransformPosition use translated world space. CameraPosition is zero in each
view; WorldPosition and ObjectPosition are relative to that view origin.
The user confirmed on 2026-10-01 that there are no external assets, so no absolute
position reconstruction, duplicate relative nodes, or legacy compatibility layer
is needed. Repository-authored source fixtures migrate to the new contract.
Directions, normals, and world position offsets
retain their existing meaning. Bump generated material cache/ABI versions.

Cloud layers are translated in double; periodic density coordinates use
double-computed texture phases rather than reconstructing large absolute floats.
History reprojection maps the current origin directly into the previous clip
space in double. CPU clip-space simple elements and existing relative editor
grid/sky paths remain valid.

## Implementation Stages

### Stage 0: Coordinate inventory and selected contracts

- [x] Inventory mesh, instance culling, materials, lighting/shadows, depth reconstruction, clouds, picking, and editor drawing.
- [x] Select per-view origin and authored-material coordinate rules.
- [x] Validate this plan with the repository plan validator.

### Stage 1: View and mesh coordinate boundary

Depends on Stage 0. Outcome: direct and indirect GPU mesh paths share translated
transforms and bounds, with CPU transforms unchanged.

- [x] Add double-precision view translation helpers and shared primitive uniform preparation.
- [x] Translate mesh transforms, inverse transforms, bounds, instance culling, and hit-proxy distances.
- [x] Cover fractional offsets at large coordinates and independent view origins with CPU tests.

### Stage 2: Material and lighting migration

Depends on Stage 1. Outcome: generated materials and lighting consume the same
translated coordinates with one explicit translated-world contract.

- [x] Make existing material position nodes and World transforms consistently camera-relative; update editor descriptions and tests.
- [x] Update generated-material versions and all consumers across workspace projects.
- [x] Translate lights, shadow receiver matrices/depth, caster origins, deferred reconstruction, contact shadows, and AO view transforms.
- [x] Verify generated shader semantics and coordinate round trips.

### Stage 3: Cloud spatial and temporal migration

Depends on Stage 2. Outcome: cloud geometry, density phases, cloud shadows, and
history reprojection remain consistent when view origins move.

- [x] Translate cloud reconstruction, layer heights, and camera positions.
- [x] Preserve base/detail/weather texture phases with double modulo calculations.
- [x] Translate history reprojection using the current origin and previous absolute matrix.
- [x] Verify periodic sampling and reprojection at large origins.

### Stage 4: Integration and handoff

Depends on Stages 1–3. Follow [build guidance](../../../Agents/BuildAndRun.md) and
[test guidance](../../../Agents/Testing.md).

- [x] Document lasting coordinate and material contracts in Runtime documentation.
- [x] Review all remaining absolute-to-float spatial upload sites.
- [x] Pass changed-document and all-plan validation.
- [x] Complete an `all` build and affected CPU tests, including all workspace projects.
- [x] Run bounded existing GPU correctness coverage for materials, GBuffer, and clouds when GPU services are available; record unavailable coverage explicitly.
- [x] Commit isolated changes with exact plan and stage provenance.
