# Metal Cooked Libraries and Load Preparation Plan

Summary: Cook Metal shader libraries and prepare retained Shader and PSO resources during loading through the same identities and creation paths used by rendering.

Last reviewed: 2026-10-18

Status: Active
Completed:

## Current Status

Planning only. No implementation stage has started and no runtime acceptance
gate has passed. Source inspection used revision
`a62f2d9f109348d4b09dde40e9787bf241b7ad1e`. This initial change adds only this
plan; implementation, builds, GPU execution and timing qualification remain
future work.

The inspected baseline has these constraints:

- Metal shader output is `Msl20Source`; the target profile, format admission,
  material Cook reader and fixed shader-library reader assume that profile.
- ShaderBuild translates Slang-produced SPIR-V through SPIRV-Cross and retains
  logical reflection and compact Metal binding metadata with the MSL.
- `CreateMetalShader` compiles source with `newLibrary` before resolving a
  function. `FShaderMapResource::GetShader` creates RHI shaders lazily.
- Cooked Material `PostLoad` checks payload presence; accepted-program access
  can trigger the first bulk decode. Async asset completion explicitly means
  object readiness rather than compilation or render-resource readiness.
- Renderer constructs exact PSO descriptions from material, geometry, Pass and
  render-target facts. RHI already provides shared asynchronous pipeline requests
  and Renderer already joins required work at resource-phase boundaries.
- Shader-map resource and RHI pipeline lookup entries use weak ownership.
  Metal's native pipeline lookup callbacks currently return no retained result.
  Preparation must retain resources and prove reuse by normal rendering.

Existing Metal and portable-binding plan closeout waivers do not waive this
plan's Cook, native loading, preparation, lifecycle or first-use gates.

## Goal

Produce source-independent Metal Game packages containing valid `metallib`
artifacts for generated Material and registered fixed shaders. Allow a loading
owner to prepare the selected program, native shaders and exact reachable PSOs,
retain them through scene activation, and observe an explicit readiness result.
The first covered frame must reuse those resources without MSL source compilation
or duplicate native Shader/PSO creation.

Preparation covers a declared set of configurations and drawing routes; it does
not promise readiness for every future runtime configuration or dynamically
introduced material/mesh combination.

## Scope and Selected Decisions

1. Preserve `Slang -> SPIR-V -> MSL`. Add an offline `MSL -> metallib` product
   owned by ShaderBuild, independent of a live RHI device.
2. Preserve the immutable editor MSL profile and introduce an explicit cooked
   binary profile/code format. Exact request/result identity remains strict;
   backend compatibility uses an allowlist of supported profiles rather than
   equality with one global Metal constant.
3. Preserve reflection, stage entry names, compute group sizes and Metal binding
   metadata alongside binary code. Never reconstruct the engine binding contract
   from a native library at runtime.
4. Initially emit one library per stage artifact. Library merging, a new global
   material-code deduplication format and function-name collision handling are
   outside this delivery.
5. Strict binary Cook fails on a cache miss when the native producer is
   unavailable. It never silently emits MSL under a binary identity. Editor
   source compilation remains supported through its explicit source profile.
6. Prepare only selected quality/FeatureLevel configurations and reachable
   geometry/Pass routes. Fixed shaders, compute pipelines and the fixed opaque
   shadow stage are part of coverage, not only generated material fragments.
7. Keep object loading and rendering preparation separate. A new preparation
   handle reports program, Shader and Pipeline readiness without changing the
   meaning of existing async-load success.
8. Reuse normal ShaderMap and PSO descriptions, identities and creation services.
   Preparation owns strong references with bounded retention and an explicit
   handoff/release policy; it must not populate a disconnected parallel cache.
9. Start native Shader preparation on the render thread with bounded scheduling.
   Do not move existing creation into arbitrary Core workers. Native asynchronous
   Shader creation can follow after device ownership and cancellation contracts
   are separately designed.
10. Metal Binary Archive integration is deferred. A `metallib` removes source
    compilation from this path but does not eliminate device-specific pipeline
    work. Loading preparation moves that remaining work before activation.

## Ownership and Dependency Boundaries

| Owner | Responsibilities |
| --- | --- |
| RHI | Code-format/target values, immutable Shader descriptions, existing PSO request and lifetime contracts |
| ShaderFormat | SDK-independent profile resolution, artifact identity, structural code and binding-metadata admission |
| TargetPlatform | Explicit tool target selection; no SDK execution, asset policy or device probing |
| ShaderBuild | Native toolchain discovery/execution, offline library build definition, cache identity, shared flights, cancellation and fixed-library production |
| Engine | Detached material Cook preparation, selected cooked-program decode/admission, asset lifetime and loading coordination |
| RenderCore | Shared ShaderMap/RHI shader preparation seam, typed result/lifetime values and renderer-module interface |
| Renderer | Reachable usage collection, exact Pass/geometry PSO descriptions, render-thread scheduling, retention and activation handoff |
| MetalRHI | Native binary library/function loading, device/function validation and native pipeline creation |

Engine must not gain a dependency on Renderer-private types. Cross-boundary
requests carry immutable values/strong resource handles through the existing
renderer-module boundary or a focused RenderCore interface. Workers retain no
borrowed `DObject`, scene-proxy or mutable Renderer-cache state.

## Implementation Stages

### Stage 0: Freeze profile, preparation and qualification contracts

**Outcome:** A reviewed implementation inventory and bounded acceptance workload
resolve the remaining choices before changing public types or disk products.

**Dependencies:** None. Recheck the inspected baseline against current HEAD.

**Change scope:** This plan, relevant source/test discovery, and qualification
inputs under ignored `Build/`; no production behavior change.

- [ ] Search target/profile, code-format and shared interface consumers across
  every project in `Durin.dworkspace`: Engine, Sandbox and RoadWeaver.
- [ ] Define editor-source versus cooked-binary profile selection at each caller,
  including Material compiler environments, default/error surfaces, fixed shaders,
  DSLB inventory lookup, native RHI admission and cooked-only resaving.
- [ ] Freeze supported OS/deployment target, language/optimization settings,
  SDK/compiler provenance and the runtime compatibility policy. Distinguish
  recorded producer provenance from runtime format/ABI admission requirements.
- [ ] Specify the offline product descriptor and keys, program/artifact identity
  composition, codec versions and old-package migration/recook policy.
- [ ] Freeze preparation input ownership, progress/terminal states, cancellation,
  priority, budgets, activation handoff and device/Shader-generation invalidation.
- [ ] Choose a bounded scene covering Local/Spline, Forward/GBuffer, opaque and
  masked Shadow, a material instance and a required fixed compute pipeline.
  Include GPU-culling geometry only where enabled and declared by that workload.
- [ ] Record a baseline with separate bulk/decode, Slang, SPIRV-Cross, native
  library/function creation, PSO queue/wait and first-frame observations.
- [ ] Locate registered test selections and define numeric load-time and retained
  resource budgets before candidate measurements. Record toolchain, hardware,
  cold/warm cache conditions and retrievable raw-sample locations.

**Acceptance:** The above choices and workload are recorded here; every required
route has an owner and test selection. Numeric budgets and repeat/noise policy
are fixed before implementation timing results are evaluated. Existing historical
Cook receipts are not treated as acceptance for the new binary format.

### Stage 1: Add binary artifact and immutable target-profile support

**Outcome:** Common contracts can represent and admit source and binary Metal
artifacts without confusing exact identity with backend compatibility.

**Dependencies:** Stage 0.

**Change scope:** RHI definitions; ShaderFormat profile/validation/identity code;
TargetPlatform selection; RenderCore codecs and all affected target consumers.

- [ ] Add the binary code-format value and immutable cooked profile. Preserve
  existing serialized enum numbers and the meaning of the source profile digest.
- [ ] Add explicit purpose/profile selection where one platform previously had
  one Shader target. Keep availability checks with the producer, not policy.
- [ ] Replace single-Metal-target assumptions with validated profile resolution;
  retain exact target/output-format checks at request and payload boundaries.
- [ ] Split MSL structural admission from bounded binary structural admission.
  Remove text-entry searching from the binary path; retain hash, metadata,
  reflection, frequency and compute-group checks. Native validation remains final.
- [ ] Ensure target, code format, settings, code bytes and binding metadata enter
  artifact identity. Finalize binary identities before publishing immutable results.
- [ ] Extend DSHD/DSLB/DMAT admission and version policy as required by their
  actual representation changes; do not bump unrelated schemas mechanically.
- [ ] Migrate source callers and fixtures across the workspace and add rejection
  cases for mismatched format/profile, corrupt bytes and unsupported settings.

**Acceptance:** Source behavior still passes its contracts; binary round trips
retain all metadata; malformed or incorrectly labeled artifacts fail before
publication. Old packages either follow the explicit compatibility policy or
produce a deterministic recook diagnostic. Affected consumer targets compile.

### Stage 2: Build and cache metallib through ShaderBuild

**Outcome:** Immutable MSL input produces a validated, reusable native library
without acquiring a GPU device or changing live material state.

**Dependencies:** Stage 1.

**Change scope:** ShaderBuild native producer, build definitions/output codecs,
cache/version accounting, diagnostics and native-toolchain test support.

- [ ] Implement explicit native compiler/SDK discovery and supported command-line
  compilation/linking. Return structured unavailable-toolchain and execution errors.
- [ ] Capture MSL bytes and explicit settings; key the action by source content,
  relevant target settings, actual toolchain provenance and algorithm versions.
  Do not include absolute installation or temporary directory paths in keys.
- [ ] Preserve independent stage reuse through existing build/DDC conventions and
  shared immutable buffers. Coalesce equal native compilation requests.
- [ ] Bound concurrent processes, input/output/diagnostic sizes and intermediate
  storage. Poll cancellation, terminate/reap owned process trees and clean scratch
  files on success, failure, cancellation and shutdown.
- [ ] Validate determinism across scratch directories; control embedded paths,
  debug data and other nondeterministic output before claiming portable reuse.
- [ ] Assemble final code format, hash, entry, target and artifact identity while
  retaining the original reflection and binding contract. Source/debug retention
  is an explicit editor/tool policy rather than a runtime dependency.
- [ ] Exercise cache hits without an executable local producer, cold misses with
  missing tools, corrupt-hit repair, forced rebuild and failed-store behavior.

**Acceptance:** Equal inputs share work and hit the native-product cache; settings
or toolchain changes invalidate the correct products. Strict misses never fall
back to source. Repeated builds under different scratch paths satisfy the frozen
determinism policy. No process, task or temporary-file ownership survives drain.

### Stage 3: Integrate binary products into Material and fixed Shader Cook

**Outcome:** MacOS Game output contains complete binary stage sets for material
families and the registered runtime shader inventory.

**Dependencies:** Stages 1 and 2.

**Change scope:** Engine Material Cook/environment/identity paths; ShaderBuild
fixed-library producer; RenderCore cooked-library handling; Cook dependency reuse.

- [ ] Select the binary target before detached Material compilation and final
  assembly. Preserve configuration selection, stage sharing, exact required-stage
  order, Game HitProxy exclusion and atomic result admission.
- [ ] Route `ProduceCookedShaderLibrary` through the same binary build product,
  including fixed opaque-shadow stages and required global/compute requests.
- [ ] Project native producer/settings changes into the owning Cook dependency
  declarations and production identities, so CookState reuse cannot retain stale
  source-profile packages or libraries under binary configuration.
- [ ] Update material compiler environment and program digests coherently; do
  not mutate an accepted source result or reuse its artifact identity after conversion.
- [ ] Keep native production in preparation, before Archive emission. Serialization
  consumes qualified detached products and never starts compilers or DDC requests.
- [ ] Check binary-size effects against per-stage, material-family, prepared-product
  and serialization limits. Adjust measured budgets deliberately if necessary.
- [ ] Preserve transactional failure/cancellation and cooked-only resaving for
  the exact supported target/configuration; never invoke an authoring producer there.

**Acceptance:** Inspect generated DMAT and DSLB artifacts to prove all selected
Metal entries are binary and match declared identities/stages. Cook reuse, producer
invalidation, corrupt inputs and failure rollback pass focused integration tests.
No MIR, MSL file, shader source mount or editor DDC is required to decode output.

### Stage 4: Load native binary libraries and functions in MetalRHI

**Outcome:** The device creates Shader resources from cooked bytes and still
supports explicit editor source shaders.

**Dependencies:** Stage 1; Stage 2 supplies native fixtures and Stage 3 supplies
the packaged integration inputs required to close this stage.

**Change scope:** MetalRHI Shader and pipeline target admission, native code-byte
ownership, diagnostics, generation/shutdown behavior and qualification fixtures.

- [ ] Dispatch library creation by admitted code format: source compilation for
  the source profile, binary-data loading for the cooked profile.
- [ ] Define binary buffer/API ownership, resolve the recorded entry and validate
  its native function type and device compatibility before publishing `FMetalShader`.
- [ ] Continue consuming compiler-agreed binding metadata; preserve current
  graphics/compute pipeline layout validation and prepared binding behavior.
- [ ] Distinguish library load, missing entry, wrong frequency and incompatible
  deployment/target failures. Strict cooked paths do not invoke source compilation.
- [ ] Add counters or equivalent observations separating source compilation,
  binary library loading, function creation and native PSO creation.
- [ ] Exercise resource release, device recreation and source/binary coexistence
  through the supported profile admission rules.

**Acceptance:** Native graphics and compute fixtures execute from binary code
with correct output and Metal validation. Wrong entry/type and corrupt-library
cases fail without partial publication. A source-free cooked run records zero
MSL source-library compilations. Editor source creation still works.

### Stage 5: Add retained program and Shader preparation during loading

**Outcome:** A loading owner can explicitly obtain ProgramReady and ShaderReady
for a selected material/fixed-shader set before the first scene submission.

**Dependencies:** Stages 3 and 4.

**Change scope:** Engine cooked material decode/admission and loading coordinator;
RenderCore preparation contracts/resource cache seam; Renderer scheduling/retention.

- [ ] Add a separate preparation handle with asynchronous progress and terminal
  results. Preserve existing `FAsyncLoadHandle` object-readiness semantics.
- [ ] Start selected cooked bulk reads/decode before accepted-program demand;
  detach CPU processing where supported and admit current results on the owner
  thread after object/load-generation/configuration validation.
- [ ] Publish immutable accepted program values to render preparation through
  the module boundary. Include required registered fixed stages in the request.
- [ ] Create the exact typed ShaderMaps/resources through normal rendering paths,
  then force creation of requested RHI Shader entries. Match normal resource keys
  and stage grouping; identical bytes alone do not prove ShaderMap cache reuse.
- [ ] Budget render-thread creation by count and measured cost between operations.
  Document that one native operation is not preemptible by a frame-time budget.
- [ ] Retain strong resource references through activation and transfer them to
  normal resource owners, or retain them in a bounded shared preparation owner.
- [ ] Support shared consumers, priority, partial progress, cancellation, unload,
  terminal failure and device/Shader-generation invalidation without stale publication.
  Resource-preparation failure must not masquerade as CPU asset-load failure.

**Acceptance:** Prepared and demand-loaded shaders resolve to the same retained
resources; a covered first use adds zero native Shader creations. Cancellation
withdraws only its observer, unload releases ownership, stale completions cannot
publish, and no-device/server loading remains valid without GPU preparation.

### Stage 6: Prepare exact scene PSOs and hand them to normal rendering

**Outcome:** Scene loading can reach PipelineReady for a declared usage set and
activate without repeating its native pipeline work.

**Dependencies:** Stage 5 and the existing RHI pipeline creation service.

**Change scope:** Renderer static/spline/GBuffer/shadow/feature preparation;
shared initializer construction; RenderCore request retention; scene activation.

- [ ] Extract shared exact Pipeline description builders from normal resource
  resolution. Preload and drawing use the same keys, ShaderMap ownership, layout,
  vertex declaration, topology, raster/depth/blend and attachment configuration.
- [ ] Collect reachable combinations after mesh slots, component overrides,
  material variants and registered Vertex Factory facts are available. Include
  required fixed feature/compute PSOs selected by the rendering configuration.
- [ ] Define explicit render-configuration input for target formats/sample counts
  and enabled routes. Asset-only requests stop at the strongest readiness they
  can establish rather than inventing a complete PSO from a material alone.
- [ ] Submit through existing shared precache/request services; use continuation
  or owner-thread polling for pending work. Respect current worker/replay wait
  restrictions and do not block render-command replay waiting on itself.
- [ ] Retain pipeline request/result handles under bounded budgets. Transfer to
  the corresponding Renderer slots or provide compatible shared ownership until
  the scene's declared preparation residency ends.
- [ ] Add a load-screen activation barrier and a background-streaming policy;
  keep the current frame resource waiting boundary as the fallback for uncovered
  or changed combinations. Report coverage and misses separately from failures.
- [ ] Handle settings/material changes, cache eviction, scene unload and device
  invalidation. Readiness is qualified by configuration and generation, not permanent.

**Acceptance:** On the frozen workload, the first covered frame adds zero native
PSO creations and zero waits for those prepared PSOs, with identical visual output.
Two consumers share work; preparation handoff survives original handle release;
changed geometry/state/attachments select a distinct correct PSO. Budgets and
eviction do not cause a false PipelineReady result or use of retired resources.

### Stage 7: Qualify packaged loading, first use and lifecycle behavior

**Outcome:** Evidence establishes source-free binary execution, effective preload
reuse and bounded lifecycle/cost for the implemented final state.

**Dependencies:** Stages 0 through 6.

**Change scope:** Focused tests, packaged qualification fixtures/observability,
owning Runtime contracts, and this plan's status/evidence.

- [ ] Run format/codec/profile, ShaderBuild/cache, Material Cook/lifecycle,
  fixed-library and RHI pipeline-service tests selected through the registry.
- [ ] Qualify native Metal binary graphics/compute, bindings, retained preload
  reuse and cold/warm scene preparation with Metal API validation where supported.
- [ ] Cook fresh MacOS Game output and run it without authoring sources or editor
  DDC. Verify materials, instances, fixed feature shaders and compute execution.
- [ ] Compare prepared and demand paths for identical output. Cover cancel/unload,
  shared observers, preparation failure, configuration changes, eviction and
  supported device/Shader-generation retirement behavior.
- [ ] Capture per-phase times, covered first-frame creation/wait counts and peak
  retained resources. Meet Stage 0 numeric budgets; report compiler work moved
  into Cook separately from device work moved into loading.
- [ ] Complete one final MacOS workspace `all` build: the foundational RHI target
  definition and renderer-module interfaces cross module/project boundaries whose
  aggregate compilation cannot be established from native-test selection alone.
  Migrate and validate affected Engine/Sandbox/RoadWeaver consumers; retain a
  Windows Vulkan build/correctness receipt for changed shared paths. Additional
  platform-wide build scope requires a stated uncovered risk.
- [ ] Update Material lifecycle, Shader Cache, target policy, fixed Shader and
  Renderer preparation contracts with lasting behavior and links. Keep execution
  checklists and stage evidence in this plan.
- [ ] Run changed-document and all-plan validation, record final evidence and close
  only satisfied gates. Complete/archive through the owning lifecycle workflow.

**Acceptance:** All mandatory gates above have current, applicable receipts.
Unavailable hardware/toolchain execution remains an open gate, never a passing
skip. Binary Archive, merged libraries and background native Shader creation
remain deferred and do not block this delivery. No claim of universal stutter
elimination or fully precompiled device pipelines follows from this acceptance.

## Validation and Evidence Policy

Follow [build guidance](../Agents/BuildAndRun.md) and
[test selection/result reuse](../Agents/Testing.md). Discover actual registered
targets before execution; likely coverage owners include ShaderFormat,
RenderCore Shader builder/cache/Cook tests, Material Cook/lifecycle tests,
RHI pipeline creation tests and Metal shader/headless qualification tests.
The final workspace build has its explicit foundational-header/interface risk
above; do not repeat it after every stage when passing evidence remains applicable.

GPU execution and timing follow the
[qualification environment](../Development/Build/NativeTestQualification.md#gpu-qualification-environments)
and [performance rules](../Development/Build/NativeTestQualification.md#performance-qualification-and-concurrent-agents).
Use a quiet exclusive lane for timing. A correctness receipt does not establish
latency or memory improvement. Application-hosted packaged execution follows its
required authorization/environment workflow rather than implicit sandbox smoke.

Record tested revision, source/fixture applicability, selected targets, build
configuration, OS/GPU, compiler/SDK/deployment settings, cache conditions, raw
report/sample locations, counts and remaining gaps. Store generated packages,
DDC, libraries, captures and raw reports under ignored `Build/` or CI artifacts.
Commit concise summaries here; a later documentation-only commit does not by
itself invalidate unchanged runtime evidence.

## Related Code

- `Engine/Source/Runtime/RHI/Public/RHIDefinitions.h`
- `Engine/Source/Runtime/ShaderFormat/Private/ShaderFormat.cpp`
- `Engine/Source/Runtime/ShaderFormat/Private/MetalShaderFormat.cpp`
- `Engine/Source/Developer/TargetPlatform/Private/TargetPlatform.cpp`
- `Engine/Source/Developer/ShaderBuild/Private/MetalShaderTranslator.cpp`
- `Engine/Source/Developer/ShaderBuild/Private/ShaderLibraryProducer.cpp`
- `Engine/Source/Runtime/RenderCore/Private/Shader/ShaderCookedLibrary.cpp`
- `Engine/Source/Runtime/RenderCore/Private/Shader/ShaderMap.cpp`
- `Engine/Source/Runtime/Engine/Private/Materials/MaterialCook.cpp`
- `Engine/Source/Runtime/Engine/Private/Materials/MaterialCookedProgram.cpp`
- `Engine/Source/Runtime/Engine/Private/Materials/MaterialInterface.cpp`
- `Engine/Source/Runtime/Engine/Public/Asset/AsyncLoad.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/StaticMeshRenderer.cpp`
- `Engine/Source/Runtime/RenderCore/Private/RenderPipelineCreation.cpp`
- `Engine/Source/Runtime/RHI/Private/PipelineStateCache.cpp`
- `Engine/Source/Runtime/MetalRHI/Private/MacOS/MetalDynamicRHI.cpp`
- `Engine/Source/Runtime/MetalRHI/Private/MacOS/MetalPipeline.cpp`

## Required References

- [Material compilation and cooked programs](../Runtime/Rendering/MaterialSystem.md#compile-lifecycle-and-cooked-programs)
- [Material stage selection and sharing](../Runtime/Rendering/MaterialShaderStages.md)
- [Shader cache](../Runtime/Rendering/ShaderCache.md)
- [Global and fixed shaders](../Runtime/Rendering/GlobalShaders.md)
- [Target platform policy](../Runtime/Assets/TargetPlatforms.md)
- [Cook lifecycle and publication](../Runtime/Assets/Cooking.md)
- [Asset packages and loading](../Runtime/Assets/AssetPackages.md)
- [Renderer frame preparation](../Runtime/Rendering/RendererFramePreparation.md)
- [Graphics state and bindings](../Runtime/Rendering/GraphicsStateAndBindings.md)
- [RHI public API stability](../Runtime/Rendering/RHIPublicAPIStability.md)
- [Apple: Precompile source into a shader library](https://developer.apple.com/documentation/metal/building-a-shader-library-by-precompiling-source-files)
- [Apple: Metal binary archives](https://developer.apple.com/documentation/metal/metal-binary-archives)
