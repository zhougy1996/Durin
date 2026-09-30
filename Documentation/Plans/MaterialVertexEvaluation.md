# Material Vertex Evaluation Plan

Summary: Execute authored World Position Offset and explicit vertex-to-pixel interpolators through accepted material programs.

Last reviewed: 2026-10-01

Status: Completed
Completed: 2026-10-01

## Current Status

Implemented the separate WPO root, explicit Vertex Interpolator and three
accepted generated vertex artifacts. The final material domain run passed all
11 targets, covering compilation/reflection, function expansion, graph editing,
package round trips, runtime generations, thumbnails and source-free Cook.
RendererSceneContractTests passed 60 cases and RenderShaderCookIntegrationTests
passed its integration case. The added function-expansion case also passed alone.
The shared API all build completed for Engine, Sandbox and RoadWeaver.

Validation evidence on the macos-xcode-arm64 Debug DurinEditor profile:

- Material domain: `Build/.agent-state/logs/20261001-005920-938681-14838-ctest.log`.
- Renderer contracts: `Build/.agent-state/logs/20261001-005941-996375-15158-RendererSceneContractTests.log`.
- Shader Cook integration: `Build/.agent-state/logs/20261001-010010-727314-15212-RenderShaderCookIntegrationTests.log`.
- All build: `Build/.agent-state/logs/20261001-010035-848802-15246-cmake.log`.

The mixed-stage regression exposed canonical operand sorting that did not
permute scalar broadcast metadata; the fix is included because normal-scaled
WPO requires it. Initial Cook validation also exposed a Slang stall when a
fixed shader entry called another shader entry. All fixed entries now call an
ordinary shared geometry function; subsequent full material Cook and shader
Cook integration passed. Visual GPU execution remains unverified as recorded
below. Lasting contracts live in
[Material Vertex Evaluation](../Runtime/Rendering/MaterialVertexEvaluation.md).

## Goal

Expose a zero-default World Position Offset Float3 input on the material terminal
and an independent Vertex Interpolator numeric expression. Material Attributes
mode retains the vertex input. Vertex interpolators also support customized UV
calculations without introducing a second UV authoring mechanism.

## Selected Decisions

- Keep the eight pixel Surface attributes unchanged; add a separate vertex root.
- WPO accepts vertex-legal non-spatial Float3 or world direction/normal values;
  positions require explicit subtraction to become an offset.
- Evaluate vertex expressions after vertex-factory deformation and before clip
  projection. World Position becomes legal in both stages, using pre-WPO position
  in the vertex stage and interpolated displaced position in the pixel stage.
- Explicit interpolation preserves shape and spatial semantics, changes the
  legal stage to Pixel, and rejects pixel-only inputs and nested interpolators.
  Bound interpolator allocation and produce deterministic interface locations.
- Compile local, spline and GPU-culling vertex entry points with the accepted
  fragment set. Resource reflection, binding, shader identities and Cook must
  cover the complete stage set. Preserve the independent error-material path.
- Forward, GBuffer, opaque/masked shadow, preview, thumbnails and hit proxies
  consume matching displaced geometry. Existing mesh bounds remain authored;
  document bounds requirements rather than silently expanding them.

## Implementation Stages

### Stage 0: Record scope and contracts

- [x] Inspect terminal ownership, stage validation, shader generation, Cook and
  renderer binding boundaries.
- [x] Record selected decisions and acceptance gates.
- [x] Validate the plan with the all-plan validator.

### Stage 1: Author vertex roots and interpolators

Depends on Stage 0.

- [x] Add terminal WPO input, reflection/persistence and graph command support.
- [x] Add Vertex Interpolator expression, catalog entry and semantic validation.
- [x] Include vertex roots in reachability, parameter/collection queries,
  normalization and canonical identity; invalidate incompatible cache contracts.
- [x] Verify editing, function propagation, invalid-stage diagnostics and package
  round trips with focused native tests.

### Stage 2: Compile and render vertex evaluation

Depends on Stage 1.

- [x] Generate stage-specific expression closures and bounded interpolator fields.
- [x] Compile and validate local, spline and GPU-culling vertex artifacts.
- [x] Bind vertex material/view/collection resources and match all geometry passes.
- [x] Update Cook stage contracts and cover source-free shader delivery.
- [x] Test shader compilation/reflection, renderer contracts and Cook rejection
  of incomplete or incompatible stage sets.

### Stage 3: Validate and publish

Depends on Stage 2. Follow [build guidance](../Agents/BuildAndRun.md),
[test guidance](../Agents/Testing.md) and
[documentation guidance](../Agents/Documentation.md).

- [x] Publish lasting runtime and editor contracts.
- [x] Run relevant material/compiler/editor/runtime/Cook native coverage.
- [x] Complete an all build covering Engine, Sandbox and RoadWeaver consumers.
- [x] Review GPU coverage availability and report any unexecuted visual checks.
  macOS Metal services are unavailable in the default sandbox; optional GPU
  qualification and application smoke are not executed, following the host
  testing guidance. Compilation/reflection and CPU contracts remain the acceptance
  gates; visual rendering is explicitly unverified.
- [x] Validate changed documentation and all plans; review and commit isolated
  changes with exact plan/stage provenance.
