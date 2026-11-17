# Material Shader and Binding Contracts Refactor Plan

Summary: Align Shader/VF ownership and parameter binding with UE's separation of semantic metadata, compiled allocation and runtime submission.

Last reviewed: 2026-11-17

Status: Active
Completed:

## Current Status

M15 of the [Material System roadmap](../Roadmaps/MaterialSystem.md). Stages 0-3
are complete: obsolete paths are removed, registered Shader/VF descriptions own
compilation, and one C++/Slang catalog unifies existing binding rules and admission.
Recorded material, ShaderBuild, Vulkan, Cook and consumer tests and Editor/Game
workspace builds pass for those stages.

The Stage 3 catalog still assigns global logical set/binding numbers. Centralizing
those rules is a completed intermediate result, not completion of UE-style
parameter binding. Stage 4 now separates composable parameter metadata from
compiler allocation; the former final-acceptance stage moves to Stage 5. This
revision changes planning only; implemented contracts remain documented in
[Material shader stages](../Runtime/Rendering/Materials/MaterialShaderStages.md).

The owner permits underlying API/derived-format changes. The preceding
[M14 plan](MaterialAttributesAndExecutionContractsRefactor.md) was completed with
its Stage 6 and native Metal acceptance explicitly skipped. Those skips are not
passes. Native Metal acceptance remains excluded from M15; the new Stage 5
Windows integration gates remain required.

## Goal and Boundaries

One registered Shader identity spans selection, compilation, Cook and runtime.
Material evaluation remains independent of pass entry bodies and geometry source.
Parameter metadata describes semantic fields and ownership; compiled output owns
the accepted mapping to actual resource locations. Renderer binds through that
mapping without reconstructing descriptor numbers from global material roles.

Preserve typed GUID attributes, functions, static selection, detached worker
inputs, ShaderBuild/DDC sharing, immutable publication, cancellation/stale-result
rejection, dynamic instances and source-free Game execution. Migrate all consumers
in `Durin.dworkspace` and selected plugins together. Reject incompatible derived
products and recook isolated outputs; authored content changes only if its source
schema changes. No parallel scheduler, compatibility registry or source fallback.
Compact uniform packing, preshaders, Water optics, Substrate and new domains remain
outside this plan.

## Selected Ownership and Transition

Local reference: `E:/Programs/Epic Games/UE_5.8`, UE 5.8.0, CL 53629095. Follow
responsibilities rather than binary layouts or every legacy path. Relevant
boundaries include `MaterialShaderType.h`, `MeshMaterialShaderType.h`,
`VertexFactory.h`, `ShaderParameterMetadata.h`, `ShaderParameterStruct.cpp`,
`ShaderCore.h` and Vulkan ShaderFormat's `SpirVShaderCompiler.inl`.

| Boundary | Selected contract |
| --- | --- |
| Material semantics | Engine owns attributes, schemas, normalized evaluation and analyzed reads. Workers receive detached values without authored-object queries. |
| Shader/VF composition | Registered types own applicability, permutations and entries; VFs own geometry source/environment. Actual type, target, permutation and applicable VF remain exact through Cook/runtime. |
| Parameter metadata | Reuse and extend RenderCore's existing `FShaderParametersMetadata` foundation. Material, Pass, VF and Scene owners contribute composable schemas with stable qualified field identity, kind, shape/array policy and required/optional semantics. Resource numbers do not belong to these semantic declarations. |
| Compilation and allocation | ShaderBuild matches metadata to compiler layout/reflection and emits an immutable parameter allocation map for each accepted Shader product. ShaderFormat owns backend remapping/compact allocation and target constraints. Compiler-assigned locations may differ across Shader types, permutations and targets. |
| Runtime binding | Renderer resolves semantic providers/structure members through the accepted map. Uniform submission policy derives from parameter metadata and pipeline/backend requirements rather than material-domain special cases. |
| RDG | Explicit parameters and declared reads determine graph dependencies and lifetimes. Binding numbers do not infer or manufacture RDG ownership. |
| Fixed conventions | Keep only justified root-layout/static-uniform conventions, named and localized to the responsible lower layer. They are explicit exceptions, not a global per-resource material slot table. |
| Publication and formats | Persist schema/allocation identity and accepted bindings with compiled content. Validate before publication; retain independent compilation-content and device-resource lifetimes. Bump affected identities/codecs together without compatibility readers. |

Stage 3's catalog, exact identity checks and negative-admission coverage are the
migration baseline. Retain useful semantic validation while replacing its global
role/slot dependency. Do not grow another allocation table alongside it. Freeze
exact C++/wire signatures and ownership before Stage 4 source edits; if a general
parameter-struct facility is missing, extend its existing owner instead of adding
a material-only replacement framework.

## Implementation Stages

### Stage 0: Select ownership and transition boundaries

- [x] Compare the local UE reference, select cross-module ownership and record M14's completion/waiver.

### Stage 1: Remove obsolete evaluation and cache paths

- [x] Require normalized property-root generation, remove the old aggregate/eight-input path, and lazily allocate independent fallback uniform caches; validate affected consumers.

### Stage 2: Unify Shader type and Vertex Factory compilation contracts

- [x] Replace fixed entry selection and aliases with registered detached Shader/VF policy, VF-owned geometry source and exact compile/Cook/runtime identity; validate an additional VF and migrated consumers.

### Stage 3: Unify binding metadata and runtime admission

- [x] Centralize current declarations/validation/mapping, publish canonical accepted binding references, reject incompatible products and validate material/render/Cook consumers. Fixed logical allocation remains transitional.

### Stage 4: Separate parameter metadata from binding allocation

Outcome: adding a parameter to a registered Pass/VF changes its owning schema,
not a central material role/slot list; compiled locations can change while runtime
submission remains correct through the accepted allocation map.

- [ ] Freeze metadata composition, stable field identity, compiler allocation records, accepted-map validation and runtime submission signatures across RenderCore/Engine/ShaderBuild/ShaderFormat/Renderer; inventory all workspace and selected-plugin consumers.
- [ ] Compose Material, Pass, VF and Scene parameter schemas through existing metadata facilities. Describe names/qualified paths, kinds, dimensions/arrays and usage/ownership without global per-resource set/binding assignments. Generate matching shader declarations; remove ordinal pass masks and central-enum edits as extension requirements.
- [ ] Use compiler layout/reflection to construct the deterministic field-to-location allocation map, including target/type/permutation, schema identity, actual resource location/kind/array and relevant constant offsets. Keep numeric allocation/remapping in compilation/backend ownership; validate any named fixed-convention exceptions explicitly.
- [ ] Persist the accepted allocation map through compiled content and Cook. Migrate Surface/PostProcess, Local/Spline/custom VF, shadows, hit proxies, PSO/preview/thumbnail and scene-resource consumers to bind via metadata and accepted maps. Remove slot arithmetic, reflection-to-global-role inference, domain-specific binding-kind switches and the obsolete fixed-slot catalog in the same migration.
- [ ] Preserve analyzed material/collection/scene reads, required/optional rules, optimized-out fields, RDG lifetimes, atomic publication and source-free execution. Bump affected derived identities/formats together; reject missing/extra/wrong-kind/array, wrong schema/map identity and cross-product substitution before publication.
- [ ] Prove extensibility with a new Pass/VF parameter without editing material core selection or a global resource enum. Prove allocation independence with differing locations across products and with unused fields optimized out; exercise runtime submission and source-free Cook replay through the accepted maps. Run owning/affected consumer coverage and stable Editor/Game workspace `all` builds for the shared API/layout migration.

### Stage 5: Validate integrated consumers and publish contracts

Entry gate: Stage 4's metadata/allocation/runtime migration and extension tests
pass; final acceptance does not substitute for implementing that boundary.

- [ ] Validate owning and affected consumers across workspace projects, including isolated source-free Cook/Game execution for Surface, Cube/WPO, ClearCoat and PostProcess with final allocation maps and no shader-source/compiler fallback.
- [ ] Satisfy the final workspace `all` gate for changed foundational headers/layouts/build inventory. Reuse Stage 4 Editor/Game results when relevant inputs and environment are unchanged; rebuild affected closure when they change.
- [ ] Check rendering behavior, required resource/descriptor counts, parameter updates without compilation and compatible-instance sharing. Record actual size/work observations without claiming unmeasured timing acceptance.
- [ ] Publish final ownership in Runtime/Rendering contracts, update the roadmap and record final tested inputs, selections and limits. Native Metal acceptance remains skipped by owner instruction and is not a passing result.

## Completed Validation Summary

Stages 1-3 were validated on 2026-11-17, windows-msvc-x64, Debug/Tracy,
MSVC 19.44, 14 jobs, with the applicable Win64-Debug-DurinEditor/Game presets.
GPU correctness used GTX 1060 6GB / Vulkan 1.4.312 and SDK 1.3.296.0 validation
layers. These are stage-specific receipts, not validation of future Stage 4.

| Stage | Recorded input and coverage | Primary receipts |
| --- | --- | --- |
| 1 | `7c67b81a5` plus cleanup; 13 material targets with the compiler caller repaired and 119 compiler cases passing; focused Vulkan/Shader/cross-project tests and Game Engine build. No workspace `all` or packaged Game claim. | `20261117-112905-455223-41052-ctest.log`; compiler `20261117-113129-874401-46248-MaterialCompilerTests.log`. |
| 2 | `cfab588fb` plus migration; 13 material targets/120 compiler cases, Shader feature/contracts, Vulkan/PSO, Shader Cook, editor and both project consumers; Editor/Game `all`. | `20261117-132935-292706-26172-ctest.log`; all builds `20261117-135704-970985-19708-cmake.log` and `20261117-135736-053111-27936-cmake.log`. |
| 3 | `304b580d8` plus binding migration; 13 material targets/121 compiler cases, Shader feature/contracts, 27 Vulkan/PSO cases, scene/PostProcess/reload, source-free Shader Cook, 93 editor cases and both project consumers; Editor/Game `all`. Final binding case adds runtime, canonical-order and schema-version rejection. | `20261117-150227-586217-12000-ctest.log`; final binding `20261117-151829-091999-37128-MaterialCompilerTests.log`; all builds `20261117-152015-749710-38352-cmake.log` and `20261117-152052-050199-36288-cmake.log`. |

Raw logs/source hashes remain under ignored
`Build/AgentValidation/MaterialShaderAndBindingContracts/Stage1/`, `Stage2/` and
`Stage3/`; the latter two contain `TestedInputs.json`. Earlier failed caller,
include-path and fixture attempts were corrected; the recorded passing receipts
supersede them. Valid-input evidence was reused only where later edits preserved
relevant behavior; final binding/Cook/consumer runs and all builds cover the last
Stage 3 guards and equivalent runtime projection changes. No native Metal,
packaged final corpus or timing qualification is claimed.

The Stage 3 baseline is binding schema 1, pass ABI 23, DMAT payload 26,
translation schema 15, normalization/generator revisions 16/28 and compile adapter
24. Physical uniform packing remains version 5; Stage 4 must version its own
incompatible transition. Authored schemas and the workspace descriptor were
unchanged in completed stages. Detailed implemented contracts live in
[MaterialShaderStages](../Runtime/Rendering/Materials/MaterialShaderStages.md) and
[MaterialSystem](../Runtime/Rendering/Materials/MaterialSystem.md); Git history
preserves the expanded completed-stage notes.

Follow the [build](../Agents/BuildAndRun.md),
[test selection/result reuse](../Agents/Testing.md) and
[documentation](../Agents/Documentation.md) workflows. Keep raw evidence under
ignored `Build/AgentValidation/MaterialShaderAndBindingContracts/`.
