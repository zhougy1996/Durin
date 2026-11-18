# Shader Parameter Allocation Convergence Plan

Summary: Completed migration to compiler-owned shader allocations with explicit low-level fixture boundaries.

Last reviewed: 2026-11-18

Status: Archived
Completed: 2026-11-18

## Current Status

Ordinary shader sources/emitters use compiler allocation. Direct, prepared and
Cook paths share the registered stage policy; typed instances retain accepted
locations. Fixed-layout exceptions are exact source-policy test cases. Subsequent
shadow and UE interface work established the main interfaces at `a9653dcde`;
the four follow-ups below implement the remaining static-work and registration convergence.

Use the runtime contracts for new work:
[shader parameters](../../../Runtime/Rendering/Shaders/ShaderParameters.md),
[GPUScene](../../../Runtime/Rendering/Scene/GPUScene.md),
[RDG](../../../Runtime/Rendering/Pipeline/RenderGraph.md).
Old inventories, superseded designs and per-stage logs remain in Git history;
this completed plan is not a second architecture specification.

## Implementation Stages

### Stage 0: Inventory consumers and approve exception boundaries

- [x] Reviewed all workspace projects/plugins; approved only exact low-level
  fixtures, native interface contracts and controlled relocation tests.

### Stage 1: Complete the shared path with ContactShadow

- [x] Unified registered compile policy, complete semantic admission and Cook
  identity/version rejection; migrated fragment/compute ContactShadow products.

### Stage 2: Migrate small and feature-owned ordinary programs

- [x] Migrated UI/debug/preview/postprocess/sky programs and typed exact views;
  removed the orphan TextureCube thumbnail shader and historical slot assertions.

### Stage 3: Migrate resource-heavy and compute programs

- [x] Migrated lighting/AO/cloud/culling and qualification programs/providers;
  retained image formats, optimized subsets and useful output/readback checks.

### Stage 4: Enforce boundaries and close artifact acceptance

- [x] Added the workspace source-policy guard with non-stale exact exceptions;
  closed cache/Cook, portable format, GPU-consumer and Editor/Game build gates.

## Cascaded-shadow RDG follow-up

`a2cb7c141` repaired repeated GPUScene read declarations within an immediate
cascade pass without weakening its pixel/depth assertions. The temporary
feature-level deduplication was superseded by the shared RDG compatibility rule.

## UE Interface Alignment Follow-up

`a9653dcde`, compared with local UE 5.8.2 CL 56702186, moved compatible-read
collection into RDG, removed `FGPUSceneReadParameters` and shadow-specific dedup,
reused `FInstanceCullingDrawParams`, and cached immutable registration schemas.
Original parameter ranges, external-contract checks, independent view/cascade
domains, invalid-access rejection and Material/Cook identities remain intact.

## UE Static Work And Registration Follow-up

The four changes are implemented. The latest recheck compares `89dc56fda`
with local UE 5.8.2 CL 56702186:

- Cache the composed shader/graph leaf join per product and immutable layout;
  retain dynamic capability/lifetime checks and atomic resource/view staging.
- Collect schema and host offsets together at registration; instance admission
  and Cook identity reuse them. Detached metadata and artifacts retain validation.
- Collect RDG resource grouping during declaration validation and reuse its
  buffer indices. Compatible read aliases skip pairwise overlap comparisons;
  original ranges, write conflicts and diagnostics remain intact.
- Share scene/ID graph registration. Scene getters outside updates expose the
  committed generation and counts; retained snapshots remain independent.
  Visible draw submission owns direct/CPU-compacted versus indirect ID selection.

The lifetime recheck replaces update-builder addresses with existing graph owner
IDs, uses weak layout identities and copy-safe cache reset, and makes collected
grouping the sole RDG validation state. Shader initialization reuses binding
storage; failure still clears it. Scene snapshots and retirement keep their
existing owners. Regression cases cover transient-layout release, copied-binding
rebind and a destroyed/reconstructed builder at the exact same address.

Acceptance: Windows MSVC Debug/Vulkan, parent `89dc56fda` plus this fix:

- `RenderContractTests` 250, `RendererSceneContractTests` 177 and
  `RenderShaderContractTests` 84 cases passed.
- Complete Game Cook/map initialization passed. The two Material stage cases
  `RegisteredFactoryPolicyCompilesAndRejectsEntryOnlySubstitution` and
  `GeometryPassSelectionAndDetachedRoundTripAreExact` passed.
- Shadow behavior/sub-texel and ContactShadow cases passed in qualification mode;
  the cloud/lighting/AO/culling scene passed. Assertions and thresholds are unchanged.
- Editor and Game workspace `all` builds passed.
- Native Apple/Metal and timing qualification were not run; no frame-time claim.

Raw receipts under `Build/.agent-state/logs/` include
`20261118-040748-124082-40932-RenderContractTests.log`,
`20261118-040532-651838-26496-RendererSceneContractTests.log` and
`20261118-040836-689793-24084-ctest.log`. Workspace build receipts are
`20261118-040923-554336-19036-cmake.log` (Editor) and
`20261118-041006-700005-34144-cmake.log` (Game). Runtime contracts own the lasting rules;
Git retains prior migration inventories, superseded receipts and resolved reviews.
