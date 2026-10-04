# Metal RHI Plan

Summary: Introduce a native Metal backend for macOS Apple Silicon, qualify existing shaders and RHI semantics, and run real editor and packaged-game workloads before considering default adoption.

Last reviewed: 2026-10-04

Status: Active
Completed:

## Current Status

Stage 0 is in progress. The Metal backend has device admission, a headless
queue, initial shared-buffer and color 2D/2D-array/cube/cube-array/3D texture transfers, transfer
views, sampler creation, and a recorded single-color RGBA8 offscreen clear pass
whose output is checked after native GPU completion in inline and threaded modes.
Production shader integration, presentation, full
runtime qualification, and performance results remain open. The repeatable
`Tools/ShaderQualification/metal_routes.py` probe generated MSL for 11 authored
entry points and fixed-shader variants using pinned Slang 2026.5.2. The route
decision below combines code-generation and bounded GPU evidence; remaining
Stage 0 checks need broader shader layouts and output. Source inspection found an
established RHI contract and a Vulkan-oriented shader and platform startup path.
The workload/RHI inventory below is complete; it identifies seven generated
material entries and the Win64-only cooked admission path. The registered Metal
qualification target compiles resource-free unlit and masked textured production
material variants via `MIR::Compile`, translates all 14 resulting SPIR-V stages
through SPIRV-Cross, and creates Apple Metal libraries/functions with checked
stage types and material layouts. It also draws the production unlit vertex and
fragment shaders on Apple M4 with the eight local-vertex-factory attributes,
the 288-byte primitive uniform, and a checked RGBA8 pixel readback. A second
GPU case draws the generated masked/unlit textured fragment stage with
layout-driven uniform offsets and checks both retained texture×tint output and
discarded-pixel clear color. The unlit draw case also draws the generated Lit
geometry pass with a counterclockwise front face and checks all four GBuffer
target pixels, including encoded normals, roughness, and the lit flag. The same
production material sources now also pass
Slang direct-MSL generation and Apple Metal library/function validation for all
14 entry stages. This completes the direct-output exercise; it does not repair
the previously observed production culling ABI failure. The backend target and
artifact identity contract and capability comparison set below are fixed for
implementation. The initial support baseline below distinguishes qualified
M4 hardware from earlier untested Apple Silicon. Stage 0 now has six of seven
checklist items
complete.

The current probe host is an Apple M4 (10 GPU cores), macOS 27.0.1, Xcode
26.6, macOS SDK 26.5, Apple Clang 21, and Metal Toolchain 17F109. The native
qualification probe reports Apple GPU Family 7/8/9 supported and Family 10
unsupported on this device. The initial support baseline is macOS 27.0 or
newer on Apple Silicon reporting Apple GPU Family 9 or newer. Release
qualification is currently limited to the tested Apple M4 and macOS 27.0.1;
other Family 9 devices and newer OS versions remain unqualified until their
own run. Earlier Apple Silicon families 7/8, Intel Macs, and macOS versions
below 27 are outside this first baseline, not inferred compatible from the M4
result. The build/cook baseline is Xcode 26.6, macOS SDK 26.5, Apple Clang 21,
Metal Toolchain 17F109, pinned Slang 2026.5.2, and SPIRV-Cross C API 0.68.0
from the Vulkan SDK 1.4.357.0 source tag (now pinned by commit).
Those are tested
minimum toolchain versions for this first baseline, not claims about older
toolchain failure. Stage 1 must reject unsupported devices/OS versions before
publishing capabilities, and Stage 4 must repeat the workload/image matrix on
each supported hardware and OS cohort before expanding the release claim.
After installing the missing Xcode Metal component, the probe's
`--require-metal-compiler` mode passed all 11 generated MSL files through
Apple's `metal` compiler outside the Codex sandbox. The Codex sandbox cannot
discover the downloaded toolchain, so this result requires the normal host
environment. The first registered `MetalShaderQualificationTests` GPU culling
pass was invalidated: its test buffers matched Metal's padded `float3` layout
(64-byte candidates) instead of the production 32-byte candidate ABI. With
the production ABI, direct MSL failed the visible-instance readback check.
The separate direct-MSL push-constant and `RGBA8Unorm` storage-texture case
passes, so this is a resource-layout incompatibility, not a blanket failure of
Slang's Metal code generation.

Stage 0 route decision: direct MSL is rejected for the current baseline because
its structured-buffer layout is incompatible with the production candidate
ABI. The selected route is pinned Slang SPIR-V 1.5 followed by the Vulkan
SDK's SPIRV-Cross MSL backend. All 11 representative authored entries and
fixed-shader variants pass SPIRV-Cross and Apple's `metal` compiler via
`Tools/ShaderQualification/metal_routes.py --route spirv-cross
--require-metal-compiler`. Its generated culling
shader uses `packed_float3` for the 32-byte candidate and explicit remaps from
Vulkan set 0 bindings 0–3 to Metal buffer slots 0–3. The registered test
compiled the authored `GBufferGPUCulling.slang`, created a native Metal compute
pipeline, and checked GPU readback of its indirect instance count and
visible-instance index on the Apple M4. A second case translated ImGui vertex
and fragment shaders, reserved vertex buffer slot 0, explicitly mapped the
projection uniform to slot 1, drew a textured triangle, and checked the
render-target pixel. This route uses an explicit MSL 2.0 target: the default
SPIRV-Cross MSL level rejected arrays of textures, while MSL 2.0 compiled and
the registered qualification case dynamically selected two texture/sampler
array elements with checked GPU buffer output. It requires a pinned
SPIRV-Cross dependency for shader cooking and checked per-stage binding
allocation. The `spirv-cross` dependency manifest now pins Khronos'
`vulkan-sdk-1.4.357.0` source tag (commit
`6c09849fe88c48eaed08413aa022aaa136a3a057`). The native Metal shader
qualification target builds and links its C API and MSL backend from that
source; it no longer depends on a host Vulkan SDK dynamic library. Production
ShaderBuild integration remains open. Slang 2026.5.2 needed no upgrade; the host
required installation
of Xcode Metal Toolchain 17F109. Receipt:
`Build/NativeTestResults/MacOS-arm64-Debug-DurinEditor/MetalShaderQualificationTests.xml`
(`./DevTool test MetalShaderQualificationTests --mode qualification --report`,
seventeen cases passed outside the sandbox). The culling case uses the renderer's
row-major host float array with a non-identity x scale and translation.
Native Metal probes additionally checked R8 storage writes, RGBA16F and
R11G11B10F float render output, exact RG32U output, D32 depth writes and
three-layer comparison sampling, sRGB encoding/decoding, RGBA16F cube face
sampling, RGBA32F linear filtering, and the 2D screen-coordinate orientation
by GPU readback. These synthetic probes
establish hardware behavior. The selected SPIRV-Cross route also executes the
authored public compute shader with its push constant and checked
`R8Unorm`/`RGBA8Unorm`/`RGBA16Float` storage-texture output. Its first MSL library build
exposed a collision between the default push-constant `[[buffer(0)]]` and the
output buffer; the qualification remap reserves buffer slot 4 for push constants
and validates the output again. Production binding allocation must encode that
reserved slot explicitly. Remaining format operations and Metal/Vulkan image
comparison remain open.
The production shader route remains subject to those Stage 0
acceptance checks. The material cases run under the authored shader environment
and GPU resource lock. The unlit vertex draw reserves Metal buffer slot 0 for
the primitive uniform and uses slot 1 for the vertex stream; the masked fragment
uses buffer/texture/sampler slot 0. Production binding allocation must
generalize and validate those per-stage mappings.

The separate synthetic resource-array fixture also generates and compiles
direct MSL with pinned Slang, but that compile result does not resolve the
production structured-buffer ABI failure or establish direct-route array
binding behavior at runtime.

The current shader session selects `SLANG_SPIRV` and `spirv_1_5` in
`Engine/Source/Developer/ShaderBuild/Private/SlangSessionEnvironment.h`.
Public shader reflection carries set/binding coordinates and push-constant
ranges. ApplicationCore initialization discovers Vulkan surface requirements
and fails when that discovery fails. These are concrete integration boundaries
to address, beyond adding native resource and command wrappers.

Direct MSL output retains requested entry-point names (the SPIR-V path records
`main`). In the compute fixture, Slang mapped the storage buffer to Metal
`buffer(0)`, push constants to `buffer(1)`, and storage texture to `texture(0)`.
This is an observed example, not a general binding contract. Shader variant
keys include compiler environment and the fixed SPIR-V target name; cooked
shader admission currently accepts only Win64, and authored shader data selects
Win64 editor validation. Backend target identity must be explicit in each of
these paths before Metal artifacts are admitted.

### Stage 0 workload and RHI inventory

The baseline is driven by the existing `FDynamicRHI` and `IRHICommandContext`
contracts, not by a clear-frame-only implementation. Together these workloads need
device/queue initialization, frame begin/end, GPU submission and completion,
buffer and texture creation (including exact format/usage support checks),
immutable views, samplers, shaders, graphics/compute pipelines, resource
transitions, upload/copy/readback, and completion-governed retirement. Windowed
paths additionally need viewport/back-buffer acquisition, presentation, and
resize. Command replay needs render passes, viewport/scissor/depth bias,
vertex/index streams, reflected resources, push constants, direct/indexed and
indirect draws, and compute dispatch. The single-queue baseline must preserve
the existing queue-transfer and split-transition fallback contracts.

| Workload | Existing shader/program variants | Distinguishing operations |
| --- | --- | --- |
| Editor UI | ImGui vertex/fragment; editor grid, gizmo, hit-proxy overlay, simple line/sprite, texture/cube preview | Alpha-blended textured/indexed UI, clip scissor, transient vertex/index upload, viewport presentation, editor target/readback |
| Material preview | Seven generated material entries: `FragmentMain`, `GeometryFragmentMain`, `ShadowFragmentMain`, `HitProxyFragmentMain`, `VertexMain`, `SplineVertexMain`, `GPUCullingVertexMain`; masked/opaque, lit/unlit, resource-free and texture/parameter layouts | Preview scene mesh and material pipelines, vertex factory variants, texture/sampler bindings, depth and render-target state, shader rebuild/replacement |
| Scene rendering | Static-mesh/GBuffer and GPU-culling compute; deferred directional lighting, contact shadow, sky/sky lighting, GTAO, cloud/shadow/temporal/composite, GBuffer debug, post-process copy/FXAA | Storage buffers and textures, culling dispatch plus indexed-indirect draw, multi-target/depth passes, sampled intermediate images, transitions, readback |
| Cooked game startup | Runtime global shader set plus finite material programs selected by cooked inventory; same scene/post-process entries needed by the level | Game viewport, shader library and material/mesh/texture payload admission, pipeline creation, first present, shutdown with in-flight work |

This is an operation inventory, not evidence that Metal supports each row yet.
`MaterialCompiledEntryPoints` currently contains seven entries. `EShaderTargetPlatform` and
`ECookTargetPlatform` currently admit only Win64, so the cooked-game row is a
required migration, not a runnable macOS fixture. The required format,
capability and image-comparison matrix is recorded below; its unverified
behaviors remain qualification gates.

### Stage 0 backend target and artifact identity

The Metal target identity is the tuple `MacOS / Metal / MSL 2.0 source /
Slang SPIR-V 1.5 -> SPIRV-Cross / binding-map schema 1`. The existing Win64
target remains `Win64 / Vulkan / SPIR-V 1.5`. Add `MacOS = 2` to both
`EShaderTargetPlatform` and `ECookTargetPlatform`; retain their existing Win64
numeric values and `Game`/`EditorValidation` profile meanings. The Metal editor
compiler produces SPIR-V with pinned Slang 2026.5.2, translates it with a pinned
SPIRV-Cross build, and stores MSL source plus the checked per-stage Vulkan
set/binding-to-Metal buffer/texture/sampler map. The cooked game loads that MSL
artifact without requiring Slang or SPIRV-Cross at runtime. The selected route
does not use a device-specific `.metallib` as its portable cooked artifact.

| Boundary | Required identity and admission rule |
| --- | --- |
| Compile request and captured build input | Carry target platform, runtime backend, intermediate and output formats, MSL language level, and remap schema explicitly. The compiler environment identity includes the pinned Slang and SPIRV-Cross builds. Reject a request whose target does not match the active compiler route. |
| DDC and in-process shader map | Separate target namespaces and hash the complete target tuple into variant/dependency/build-input keys. Bump the affected build-function and payload schemas. Payload decoding checks target and code magic/format before publishing a stage; an old SPIR-V-only payload cannot be read as MSL. |
| Runtime shader and pipeline | Carry target, code format, binary entry name, byte digest, and remap identity on each compiled stage and RHI shader. Metal pipeline creation validates the complete binding map and shader format before publication. Shader and graphics/compute pipeline hashes domain-separate by backend and artifact format; identical source or code bytes cannot make cross-backend cache entries interchangeable. |
| Cooked library and material program | Bump the library and material-program schemas for Metal-capable payloads. Encode platform/profile and per-stage format/remap metadata; validate them against the selected device before opening or constructing a shader. Keep Win64 legacy admission only for its existing Vulkan path. Reject a wrong-platform, wrong-format, wrong-entry, or stale-remap artifact with a recoverable diagnostic, without falling back to source compilation in cooked mode. |

Compile requests and captured inputs now carry the complete target tuple;
variant, dependency, and DDC keys separate it. The Slang compiler now translates
Metal requests through the pinned SPIRV-Cross MSL backend, allocates explicit
per-stage buffer/texture/sampler slots from reflection, reserves vertex-stream
and push-constant buffer slots, and records the native map and its digest on
each compiled stage. The direct compiler path passes textured vertex/fragment
and resource-array compute tests. `ShaderBuilder` now admits canonical Metal
requests on macOS. Its cold and warm cache paths separate Metal MSL from Vulkan
SPIR-V, and `ShaderSharedOutput` schema 5 carries the native binding map and
rejects stale map digests or malformed MSL. `ShaderCompiledOutput` schema 3
now carries target-specific SPIR-V or MSL plus the validated Metal binding map.
The cooked shader library schema 3 admits MacOS/Metal records and checks
platform/profile separation. Opening a library validates every required
record's payload and binding map before publishing it. Unit round trips verify
Metal library loading and rejection of malformed MSL with recomputed file and
record digests;
the full production MacOS cook and game load remain unqualified. Remaining
Stage 3 work includes Metal shader/pipeline creation and runtime binding
submission and validation for material programs.
`FShaderMap` now preserves the compile target, rejects a
stage with a mismatched target or code format, and separates identical code
bytes by target in its cache key. RHI shaders and graphics/compute pipeline
keys carry target, code format, entry name, and remap identity; Vulkan shader
creation rejects Metal artifacts. All 56 RenderShaderContractTests cases pass;
the 12 RenderShaderCacheTests and 24 RenderShaderBuilderTests cases pass;
the six RenderShaderCookedLibraryTests and the existing Vulkan cooked-library
integration test pass;
the 118 RHICommandListTests and 13 RHIPipelineCreationTests cases also passed.
An earlier test run on this host crashed in `dyld4::Loader::loadAddress` before
startup; retrying with `DYLD_PRINT_LIBRARIES=1` ran all cases.
`ShaderData` now admits MacOS for authored and cooked shader domains, and
ShaderBuild and Launch select that target when `DURIN_RHI_BACKEND=metal`.
Material generated compilation now passes the selected Metal target through
ShaderBuild. Material cooked-program schema 11 carries each stage's target,
code format, and native binding map; MacOS/Game serialization, decode, and
wrong-platform rejection pass focused tests, including real Metal material
compilation. A full asset cook and packaged Metal game launch remain unqualified.

### Stage 0 capability and image comparison matrix

The required texture format/use combinations come from
`RenderTargetLayouts.cpp`, the renderer's resource builders, hit-proxy
readback, texture previews, and sky-lighting resources. On the Apple M4 probe,
`RendererTextureFormatUsageAllocates` creates the following native Metal
textures with the stated usage flags, including a writable RGBA16F cube and a
three-layer D32 shadow array. Allocation is one qualification result; it does
not establish every listed draw, filter, storage write, readback, or presentation
semantic. An `RHIIsTextureSupported` result may become true only after the
corresponding native behavior is implemented and verified.

| RHI format | Required use | M4 evidence and remaining gate |
| --- | --- | --- |
| `R8_UNORM` | Visibility/AO render, sample, cloud/contact storage write | Native allocation with render, read and write usage; native MSL and selected-route compute writes with exact-byte readback passed; render and sample pending |
| `RGBA8_UNORM`, `SRGBA8_UNORM`, `SBGRA8_UNORM` | GBuffer, UI, texture sample, sRGB editor/present output | Native allocation with render/read usage; RGBA8 draw and selected-route compute storage output/readback passed; native sRGB RGBA/BGRA render encoding and sampled linear decoding passed with checked channel order and float readback; layer presentation pending |
| `R11G11B10_FLOAT` | GBuffer emissive render and sample | Native allocation with render/read usage; native MSL float render and exact packed-bit readback passed; selected-route output and sampling pending |
| `RGBA16_FLOAT` | Scene color, cloud compute/storage, cube environment | Native 2D render/read/write and cube read/write allocation; native MSL float render and exact half-bit readback passed; selected-route compute storage write and half-bit readback passed; Metal RHI cube-face mip transfer and exact-byte readback passed; native cube +X/−X nearest sampling passed; linear filtering and selected-route sampling pending |
| `RGBA32_FLOAT`, `R16_FLOAT`, `RG8_UNORM` | Normal/default and authored texture sampling | Native sampled allocation; RGBA32 2D upload and linear filtering passed with exact float readback; selected-route sampling pending |
| `RG32_UINT` | Hit-proxy integer target and readback | Native render-target allocation; native MSL integer render and exact 64-bit pixel readback passed; selected-route hit-proxy output pending |
| `D32` | Scene/shadow depth, sampled depth array | Native 2D and three-layer depth-array allocation; native MSL depth clear/write/store and float readback passed; three distinct array-layer clears and comparison sampling passed; selected-route depth sampling pending |

The single-queue baseline requires 2D/3D/cube/array dimensions, views, upload,
copy, asynchronous texture readback, four simultaneous GBuffer color targets,
depth, sampled/storage resources, indexed-indirect draw, dispatch, and their
actual completion/lifetime semantics. Capabilities must advertise only checked
limits and formats. Unsupported format/usage/dimension/sample combinations
return `false` from `RHIIsTextureSupported`; creation returns a recoverable
`FRHICreationError`. Unsupported independent queues, native split barriers,
transient aliasing, non-solid fill, wide lines, and depth clamp report absent
capabilities or use the documented full-barrier/single-queue fallback rather
than exposing an unimplemented path.

The fixed Metal/Vulkan comparison set uses the same shader/material sources,
geometry, camera, lights, exposure, resolution, color space, and captured frame
index on both backends. Compare GPU readbacks before UI scaling or display color
management. Preserve reference images and per-image diff summaries with the
Stage 4 qualification receipt:

| Capture | Comparison acceptance |
| --- | --- |
| ImGui textured/alpha-blended panel with clipping, and hit-proxy integer ID | UI RGBA8 per-channel difference at most 1 LSB outside blend boundaries; hit-proxy IDs exact and coverage identical |
| Unlit texture plus masked material preview, both mask outcomes | RGBA8 per-channel difference at most 1 LSB on retained pixels; discarded coverage and alpha exact |
| GBuffer static mesh plus GPU-culling visible and rejected instances | Indirect counts and visible indices exact; GBuffer 8-bit channels within 2 LSB, depth within `1e-5` away from raster edges |
| Lit scene with directional shadow, contact shadow, GTAO and sky lighting | Linear RGBA16F p99 absolute error at most `0.01` per channel and p99 relative error at most 2% for reference values at least `0.05`; no missing geometry or inverted depth |
| Post-process copy/FXAA, cloud temporal/composite and sRGB present | Final 8-bit p95 difference at most 2 LSB and p99 at most 8 LSB per channel; compare temporal passes after identical warm-up frames and separately inspect silhouette/occlusion regions |

These are acceptance thresholds for future paired captures, not claims that
Metal images already match Vulkan. Both paths must report unsupported features
explicitly; a skipped pass cannot be counted as a matching image.

The [RHI public API stability contract](../Runtime/Rendering/RHIPublicAPIStability.md)
provides the caller-facing boundary. The active
[multi-queue plan](RdgRhiMultiQueueExecution.md) retains open native split-barrier
and transient-aliasing gates. Metal baseline work need not wait for those
optimizations and must not mark their Vulkan qualification gates complete.

## Goal

Provide a selectable native Metal backend that runs existing global shaders,
materials, scene rendering, editor UI, and cooked game content on a declared
macOS Apple Silicon support baseline. Preserve backend-neutral caller behavior,
including failure, GPU completion, readback, and resource-retirement semantics.
Use the second implementation to expose and resolve Vulkan assumptions at their
owning boundaries without spreading native Metal types into Renderer code.

## Scope and Selected Direction

- Add a `MetalRHI` module, integrated through the existing dynamic-RHI module
  boundary. Keep Vulkan selectable for regression comparison and recovery.
- Start with one physical command queue supporting graphics, compute, and
  transfer work. Publish capabilities conservatively; use documented fallback
  behavior for independent queues and experimental split transitions.
- Include buffers, textures and views, samplers, graphics and compute pipelines,
  reflected bindings, required direct/indirect commands, uploads, copies,
  readback, submission completion, retirement, and window presentation needed by
  the accepted workloads. Stage 0 defines the exact supported matrix.
- Preserve authored Slang sources where feasible. Select the native shader
  production route only after testing the repository's shaders and binding
  semantics. Make backend target and artifact format explicit in compilation,
  cache identity, and cooked-library compatibility checks.
- Isolate native window/layer integration and Objective-C++ implementation in
  platform/backend code. Make Vulkan surface discovery conditional on the
  selected backend rather than a universal startup prerequisite.
- Exclude iOS, Intel Mac qualification, ray tracing, mesh/tessellation feature
  expansion, independent async queues, native split optimization, and transient
  aliasing from this baseline. Do not change the default backend in this plan.
- Treat performance as measured evidence. Native Metal is not assumed faster
  than the existing Vulkan/MoltenVK path.

## Implementation Stages

### Stage 0: Qualify shader routes and freeze the baseline

Dependencies: current RHI contracts and the repository's pinned shader toolchain.

- [x] Inventory required RHI operations and existing shader permutations for
  editor UI, material preview, scene rendering, and cooked game startup.
- [x] Declare minimum macOS version, GPU family, SDK/compiler versions, and
  available qualification hardware; separate supported scope from untested scope.
- [x] Exercise representative vertex/fragment and compute shaders, material
  permutations, resource arrays, push constants, and vertex inputs through
  Slang's direct Metal output using the pinned toolchain.
- [ ] Verify actual parameter layouts and rendered/readback output, including
  entry-point names, matrix layout, resource indices, constant-data offsets,
  coordinate conventions, and required storage-texture/format support.
- [x] If direct output cannot satisfy the baseline, evaluate SPIR-V-to-MSL
  translation with explicit binding remaps. Record the selected route, dependency
  implications, limitations, and any toolchain upgrade before expanding work.
- [x] Define backend target identity and artifact metadata for compile requests,
  shader/DDC caches, pipeline identities, and cooked-library admission.
- [x] Record the capability matrix and a representative workload/image comparison
  set, with explicit tolerances and unsupported-feature behavior.

Acceptance: real shaders execute in a bounded qualification harness and produce
checked output, with a selected compilation/binding route and declared support
matrix. Compile success or a clear-only frame is insufficient. If neither route
works, record the blocker here before proceeding to full backend implementation.

### Stage 1: Integrate device selection, platform startup, and presentation

Dependencies: Stage 0 route and support decisions accepted.

ApplicationCore now defers GLFW Vulkan extension discovery until a Vulkan
presentation device requests it; its public snapshot still reports a failed
query to Vulkan initialization. `ApplicationCore` built and
`NativeWindowModalLoopTests` passed (four cases) on the M4/macOS 27.0.1 host.
The Cocoa window's installed `CAMetalLayer` is now passed by non-owning handle
through presentation startup and viewport creation. Metal presentation remains
rejected during RHI initialization until drawable and frame ownership are ready.
The RHI loader now accepts `DURIN_RHI_BACKEND=vulkan|metal`, defaults to
Vulkan, rejects invalid names, and unloads the selected module by identity.
MetalRHI is registered in the Engine closure. On M4/macOS 27.0.1, its native
device and command queue initialize and shut down through the module loader in
both inline and threaded headless modes (`MetalRHIHeadlessTests`). It rejects
older macOS versions, devices below Apple GPU Family 9, and presentation until
drawable ownership exists. The single physical queue is now reported and empty
logical submissions remain pending after CPU replay, submit on `SubmitToGPU`,
and become complete only from native Metal command-buffer completion. The
qualification target checks two ordered submissions in both execution modes
and cancellation of replayed but unsubmitted work at shutdown. Native shared
buffers now support initial data, staged writes/uploads, and buffer-to-buffer
copies through recorded commands. Immutable Metal sampler creation now maps
filtering, addressing, comparison, border, anisotropy, and LOD state, rejecting
unsupported descriptors. Validated unformatted buffer views retain their
parent allocation; formatted views remain unsupported. The qualification
target verifies GPU copy output after releasing the source wrapper before
replay in both execution modes, recoverable rejection of an invalid buffer
descriptor, native sampler allocation, and buffer-view lifetime. Private
2D textures in 11 color formats now admit only transfer/readback
usage. Recorded buffer/texture and texture/texture blits, padded row layouts,
pitched 2D uploads, synchronous and asynchronous readback, validated transfer
views, and cancellation of unsubmitted readback have native GPU coverage in
both execution modes. A format-matrix case additionally verifies exact GPU
byte preservation and synchronous readback for R8, RG8, R16F, RGBA8, BGRA8,
sRGB RGBA/BGRA, R11G11B10F, RGBA16F, RGBA32F, and RG32_UINT. Cube faces in
RGBA8 and RGBA16F and 3D volume mips now pass recorded multi-slice blits,
texture copies, and pitched volume uploads in both execution modes. RGBA8 2D
and cube arrays
also pass recorded layer copies and readback, including a two-face copy across
the cube boundary and 2D-to-array layer copies, in both execution modes. Other
array/cube/volume formats,
shader/render uses, and remaining texture operations are unsupported.
The production Metal command path now records a single-color RGBA8 offscreen
clear pass and reads its exact pixels after GPU completion in inline and
threaded modes. Other render formats, depth, resolves, and drawing remain
unsupported. Resource capabilities stay unpublished until the required resource surface is
implemented and verified. Native failure injection, submission timeouts under
delayed GPU work, and comprehensive retention across completion remain open.
Receipt: `Build/NativeTestResults/MacOS-arm64-Debug-DurinEditor/MetalRHIHeadlessTests.xml`
(`./DevTool test MetalRHIHeadlessTests --mode qualification --report`, sixteen cases
passed on the stated M4/Xcode/macOS host).
The application-host `MacOSWindowLifecycleTests` target built with
`DURIN_ENABLE_APPLICATION_TESTS=ON`, but CTest discovery timed out in other
application-host targets before running it. That configuration was restored to
`OFF`. An unrelated `StaticMeshRenderPreparationVulkanTests` qualification run
failed in shader reflection (`Transform` missing) and later trapped during
test cleanup; it does not validate the changed presentation path. Real Vulkan
window startup and Metal presentation remain unverified.

- [x] Register `MetalRHI` and its platform build dependencies; implement explicit
  backend selection with useful unsupported-platform/device diagnostics.
- [ ] Decouple ApplicationCore startup from unconditional Vulkan requirements;
  retain the existing Vulkan startup path and headless initialization contract.
- [ ] Create the Metal device, baseline queue, and immutable capability report.
- [ ] Implement native layer/drawable ownership and viewport creation,
  presentation, resize, minimization, close, and shutdown behavior.
- [ ] Verify both inline and threaded command execution ownership where applicable.

Acceptance: a selectable Metal application presents a clear frame and survives
repeated resize/minimize/close cycles; headless initialization works; Vulkan
startup remains valid. Missing drawables and startup failures produce defined
outcomes without leaked ownership or teardown hangs.

### Stage 2: Implement resources, submission, and lifetime correctness

Dependencies: Stage 1 device and execution ownership established.

- [ ] Implement required buffer/texture descriptions, views, samplers, transfers,
  uploads, readback, exact support queries, and recoverable creation failures.
- [ ] Implement single-queue submission and owning GPU sync points, including
  empty submissions, timeouts, cancellation, failure, and shutdown.
- [ ] Retain command resources and staging allocations through their actual GPU
  use; reclaim only after valid retirement prerequisites are satisfied.
- [ ] Map logical resource access and transition semantics to Metal synchronization
  and encoder boundaries; preserve full-barrier fallback where required.
- [ ] Add native backend coverage for delayed completion, upload reuse, readback,
  failed submission, and teardown with work outstanding. Use deterministic
  fixtures when native failure injection is unavailable and label that evidence.

Acceptance: transfer output is verified and lifecycle tests pass in inline and
threaded modes. CPU replay completion never authorizes premature GPU resource
reuse. Unsupported operations follow the documented capability/error contract.

### Stage 3: Implement shaders, bindings, and graphics/compute execution

Dependencies: Stages 0 and 2 accepted.

- [ ] Integrate the selected shader route into ShaderBuild and runtime shader
  loading, with target-specific cache and cooked-artifact separation.
- [ ] Implement graphics/compute pipeline creation, reflection-to-native binding
  maps, constant data, arrays, vertex input, and required render/depth state.
- [ ] Implement the supported draw, indexed draw, dispatch, and indirect paths;
  preserve pipeline complete-or-failure publication and binding validation.
- [ ] Run actual global/material shader and compute/readback workloads through
  production command recording, replay, submission, and retirement.
- [ ] Verify wrong-backend shader artifacts are rejected and cannot contaminate
  caches or silently load into an incompatible device.

Acceptance: representative textured/material draws and compute output match
Stage 0 expectations using production APIs. Shader rebuild/reload and pipeline
replacement preserve resource lifetimes and target identity.

### Stage 4: Qualify editor and cooked-game workloads

Dependencies: Stage 3 production execution accepted.

- [ ] Run editor UI, material preview, and a representative scene including the
  agreed lighting, shadow, post-processing, and compute workloads.
- [ ] Exercise repeated viewport creation/destruction, resize, material changes,
  reload, and application shutdown with validation diagnostics enabled.
- [ ] Cook target-specific content and launch the game without a ShaderBuild
  runtime dependency; test missing and incompatible shader-library diagnostics.
- [ ] Compare Metal and Vulkan/MoltenVK output on the same scenes, resolutions,
  camera states, and settings. Record tolerances and investigate mismatches.
- [ ] Measure CPU/GPU frame timing, shader/pipeline preparation, memory, and
  presentation behavior using comparable workloads and declared timing methods.
  Record regressions and uncertainty rather than assuming a performance win.
- [ ] Document implemented behavior in owning Runtime/Development contracts;
  complete affected validation and the workspace-wide build when shared Engine
  APIs change, preserving Vulkan consumer coverage.

Acceptance: the declared editor and cooked-game workload matrix passes on the
named hardware, remaining limitations are explicit, and output/performance
receipts support continued opt-in use. Default adoption and advanced Metal
optimization require a separate decision after this baseline is accepted.

## Validation and Handoff

Follow [build and run guidance](../Agents/BuildAndRun.md) and
[native-test guidance](../Agents/Testing.md) for implementation validation.
Use the [rendering performance baseline](../Development/Build/RenderingPerformanceBaseline.md)
for comparable measurements. Reuse existing conformance contracts where possible
and add Metal-specific execution tests for native behavior; Vulkan test passes
alone do not qualify Metal.

For each stage, record the revision, SDK/toolchain, OS/GPU, selected backend,
workloads, test/report paths, results, and explicit coverage gaps. Update this
plan's status and checklists only from that evidence. Changes to shared APIs
must migrate all consumers in projects declared by `Durin.dworkspace`; lack of
another platform's hardware remains an explicit qualification gap.

## Required Contract References

- [RHI capabilities and Vulkan startup](../Runtime/Rendering/RHICapabilitiesAndVulkanStartup.md)
- [RHI command execution](../Runtime/Rendering/RHICommandExecution.md)
- [RHI resource transitions](../Runtime/Rendering/RHIResourceTransitions.md)
- [RHI resource views and transfers](../Runtime/Rendering/RHIResourceViewsAndTransfers.md)
- [RHI diagnostics and conformance](../Runtime/Rendering/RHIDiagnosticsAndConformance.md)
- [Graphics state and bindings](../Runtime/Rendering/GraphicsStateAndBindings.md)
- [Synchronous compute pipelines](../Runtime/Rendering/SynchronousComputePipelines.md)
- [Viewport rendering](../Runtime/Rendering/ViewportRendering.md)

## External Qualification References

These inform Stage 0 experiments; they do not establish compatibility with the
repository's pinned compiler or qualify any implementation here.

- [Slang target support](https://github.com/shader-slang/slang#target-support)
- [Slang Metal-specific behavior](https://docs.shader-slang.org/en/stable/external/slang/docs/user-guide/a2-02-metal-target-specific.html)
- [Slang reflection API](https://docs.shader-slang.org/en/stable/external/slang/docs/user-guide/09-reflection.html)
