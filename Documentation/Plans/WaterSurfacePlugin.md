# Water Surface Plugin Plan

Summary: Add an opt-in Engine water plugin for finite, animated water surfaces using the existing material and mesh systems.

Last reviewed: 2026-11-13

Status: Completed
Completed: 2026-11-13

## Current Status

All four stages are complete and validated on Win64 in Debug DurinEditor and
DurinGame. Enabled projects can place and persist a reflected water actor,
render its three-wave finite surface, and query matching height and normal.
The original workspace descriptor was restored byte-for-byte; existing projects
remain opted out. Full Cook, Stage and packaged Game qualification remain deferred.

Stage 0 identified the runtime material boundary: DurinGame strips typed
expressions and disables authored material compilation. On 2026-11-13 the user
approved bounded precompilation of water validation resources. Game consumes
compiled material families and a prepared global Shader library; no source
compiler or alternate water renderer was added to Game.

The plugin owns a bounded procedural StaticMesh grid and an ordinary detached
material-IR recipe. Editor compiles the recipe through Engine; Game decodes the
plugin-owned cooked-program family. The first qualified platform is Win64,
explicitly declared in the descriptor. Other platforms need matching resources
and runtime qualification. The only shared API foundation was exporting
DMeshComponent's existing constructor for external component subclasses; its
signature and behavior are unchanged. StaticMesh and Spline consumers were
identified across every workspace project's source/test roots and validated.

The implemented contract is [Finite Water Surfaces](../Runtime/World/WaterSurfaces.md).

### Acceptance evidence

Validated 2026-11-13 on the final implementation working tree based on
`b2ea9f72dd6759ef724fa40f3ddc1b48f70b42b3`, with the `windows-msvc-x64` profile,
Debug Editor/Game presets, Vulkan and an NVIDIA GTX 1060 6GB. Production source/resource
inputs are unchanged after the final receipts. Subsequent edits finish this
record and nonsemantic test formatting/enablement declarations. The test-only
plugin macro's enabled value remains 1; its corrected disabled value 0 was
validated after workspace restoration.

- `WaterSurfaceRenderingVulkanTests`: two tests passed with the isolated opt-in
  fixture in each runtime variant. Coverage includes independent CPU/GPU grid
  agreement, Lit animation/replay, conservative bounds, Editor hit proxies, and
  actual component settings, unregister and register behavior. Final logs are
  `Build/.agent-state/logs/20261113-122858-341515-23572-WaterSurfaceRenderingVulkanTests.log`
  (Editor) and `20261113-123043-572474-41372-WaterSurfaceRenderingVulkanTests.log`
  in that same directory (Game).
- `WaterSurfaceFeatureProbe`: queries passed in both variants, covering 48
  representative CPU/Float32 material samples through one hour, inverse
  mapping, finite-difference normals, outside/invalid/overflow inputs, grid
  tessellation, reflected edit admission, live settings and world transforms.
  Editor persist/save/unload/reopen passed; both variants loaded and queried
  the same saved nondefault water Level. Receipts are under ignored
  `Build/WaterSurfaceValidation/Editor-*.log` and `Game-*.log`.
- `WaterSurfaceStartupProbe`: enabled and disabled fresh processes passed in
  both variants using the same inventory without rebuilding between choices.
  The probe rejects early loading and checks reflected actor availability.
- Plugin-resolution Python regression: 90 passed, one platform-capability case
  skipped. It includes explicit true/false/absent Water selections in both
  variants and preservation of existing project roots.
- Constructor-consumer regression: `StaticMeshMaterialTests` passed 10 tests;
  `SplineTests 'FSplineMesh*Tests.*'` passed 48. Ordinary Editor and Game build
  graphs contain no Water target after restoring the workspace. The unselected
  Game water rendering target passed its one independent proof case.
- The default Game material family contains Low/High and ES3_1/SM5/SM6
  configurations. Its SHA-256 is
  `4391372fc0c06daf55cf4ccd1488a3a948fe4fc8526531113b4601f5812d5e1f`.

Reproduction and explicit resource generation are described in the
[isolated fixture](../../Engine/Tests/Fixtures/WaterSurface/README.md). MSVC
native builds/tests ran outside the sandbox after its documented C1902 PDB
failure. The affected-test heuristic selected all because it treats new plugin
resources/test CMake declarations as unbounded inputs. Bounded coverage was used
because production impact is isolated to Water and one unchanged constructor
export, with its two existing component consumers explicitly covered; no shared
renderer, foundational header, or ordinary build graph was changed.

## Goal

An enabled project can place a water surface in a level, edit its dimensions and wave parameters, save and reopen it, and see the same animated surface during development runtime. Gameplay can query water height and normal from the same wave definition used by the visible surface. A project that does not enable the plugin has no Water module in its build or startup selection.

## Selected Design

- Put `Water.dplugin` and a `Water` Runtime module under `Engine/Plugins/Water/`. Projects opt in through their `.dproject` plugin selection. Add an Editor module only when a concrete editor feature needs one; ordinary reflected properties and component editing are the starting point.
- Keep water geometry, authored parameters, conservative bounds, and CPU surface queries in the plugin. Use the existing material system for appearance and vertex displacement. Do not add a parallel water rendering pipeline.
- Start with one finite rectangular water body. A regular grid provides enough vertices for displacement; the component owns its extent and material. Use a small, deterministic Gerstner wave set with bounded amplitudes. Define coordinate space, time source, edge behavior, and CPU/GPU agreement before implementation.
- Use the existing Lit/translucent, `Time`, `WorldPosition`, and World Position Offset capabilities where qualified. A missing material expression or grid asset path is a Stage 0 dependency, not a reason to introduce a general renderer extension API.
- Keep the first implementation independent of terrain modification, physics simulation, and specialized water shading. Record any future renderer API requirement as a separate design decision with a concrete visual case.

## Implementation Stages

### Stage 0 proof and resulting convention

The independent proof uses one Gerstner wave with A=0.08, Q=0.5, k=6,
omega=2, D=(1,0), zero phase, and a 32-by-32-cell grid. It preserves undisplaced
XY in UV0, derives its normal analytically, and pads section/primitive bounds.
GPU images are compared with CPU-displaced geometry with a rasterization
allowance of fewer than one percent of pixels differing by more than two
8-bit channel values. Replaying the same explicit time produces identical output.

The production spectrum, parameter/grid limits, coordinate/time convention,
query inversion and measured tolerances now belong to the
[implemented contract](../Runtime/World/WaterSurfaces.md#wave-and-query-contract).

### Stage 0: Prove the rendering path

- [x] Identify the smallest existing grid mesh and material authoring path that can express wave displacement and a usable surface normal. Build a minimal development scene without plugin-specific renderer changes.
- [x] Verify time-driven WPO, transparent or opaque Lit appearance, conservative displacement bounds, and editor selection in a focused renderer/component test or controlled visual smoke.
- [x] Record the wave equations, parameter limits, time source, world/local space convention, and CPU/GPU tolerance. If an essential capability is missing, specify and implement only that bounded foundation before Stage 2.

Completion: a finite animated grid is visible and stable in Editor and development Game; the required lower-level work is known and bounded.

### Stage 1: Establish the plugin boundary

- [x] Add the Engine plugin descriptor and Runtime module with the minimal dependency set. Keep it disabled by default in existing projects.
- [x] Verify project opt-in selects and builds Water, while an unselected project does not compile or admit it. Cover plugin descriptor discovery and module startup with focused tests.

Completion: Water can be enabled and loaded through the ordinary plugin workflow without changes to unrelated project roots.

### Stage 2: Author and render a finite water surface

- [x] Add a reflected, placeable water surface actor/component with extent, elevation, material, and bounded wave parameters.
- [x] Provide the regular grid and default water material through plugin-owned resources. Apply the wave definition in WPO, produce a consistent visible normal, and enlarge bounds for maximum displacement.
- [x] Validate property edits, registration/unregistration, level save/reopen, editor viewport rendering, and development Game rendering. Keep failure diagnostics local to the surface or plugin.

Completion: a level author can place and persist a water body and see stable animation in Editor and development Game.

### Stage 3: Expose surface queries

- [x] Implement deterministic CPU height and normal queries using the Stage 0 wave convention, including points outside the finite extent.
- [x] Test CPU queries at representative positions and times against the material wave definition within the recorded tolerance.
- [x] Verify parameter edits update visible and query state together without stale snapshots.

Completion: gameplay queries agree with the rendered water within the stated tolerance.

## Validation and Deferred Work

Use the repository [build](../Agents/BuildAndRun.md) and [test](../Agents/Testing.md) workflows to select the smallest owning-module and consumer coverage. Validate both enabled and disabled project selection and one Editor/development Game scene. A Game preset is required for the runtime-variant boundary; a repository-wide `all` build is needed only if bounded targets cannot cover a shared renderer or build-graph change.

Except for the user-authorized bounded precompilation of water validation
resources, full Cook, Stage, packaged Game startup, and packaged asset/shader
validation are **not acceptance gates for this plan**. Before claiming the plugin
is release-ready, open a follow-on stage or plan for those paths. The same applies
to refraction, underwater effects, buoyancy, spline rivers, terrain interaction,
and infinite ocean meshing.

Related contracts: [plugin metadata and selection](../Workspace/Plugins.md), [material vertex evaluation](../Runtime/Rendering/Materials/MaterialVertexEvaluation.md), and [forward lighting](../Runtime/Rendering/Features/ForwardLighting.md).
