# Metal RHI Plan

Summary: Introduce a native Metal backend for macOS Apple Silicon, qualify existing shaders and RHI semantics, and run real editor and packaged-game workloads before considering default adoption.

Last reviewed: 2026-10-04

Status: Active
Completed:

## Current Status

Stage 0 is in progress. The Metal backend has device admission, headless
frame begin/end and queue submission, initial shared-buffer and color 2D/2D-array/cube/cube-array/3D texture transfers, transfer
views, sampler creation, and a recorded single-color RGBA8 offscreen clear pass
whose output is checked after native GPU completion in inline and threaded modes.
Apple's Metal-cpp is now pinned as a source dependency, and device admission and
command-queue creation use its C++ API. The remaining resource, command, and
presentation implementation still uses Objective-C++ under ARC; migration of
those calls and ownership is open. `MetalRHI` builds and its headless GPU
qualification target passes after this first boundary change on the M4 host.
Production shader integration, full presentation qualification, full
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
ShaderBuild integration was subsequently added through the selected SPIR-V-to-
MSL route. Slang 2026.5.2 needed no upgrade; the host
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
uses buffer/texture/sampler slot 0. Production binding allocation now validates
canonical per-stage maps; broader authored-material output parity remains open.

The separate synthetic resource-array fixture also generates and compiles
direct MSL with pinned Slang, but that compile result does not resolve the
production structured-buffer ABI failure or establish direct-route array
binding behavior at runtime.

The current shader session selects `SLANG_SPIRV` and `spirv_1_5` in
`Engine/Source/Developer/ShaderBuild/Private/SlangSessionEnvironment.h`.
Public shader reflection carries set/binding coordinates and push-constant
ranges. ApplicationCore now discovers Vulkan surface requirements only when the
Vulkan presentation device requests them; eager discovery was removed during
Metal startup integration.

Direct MSL output retains requested entry-point names (the SPIR-V path records
`main`). In the compute fixture, Slang mapped the storage buffer to Metal
`buffer(0)`, push constants to `buffer(1)`, and storage texture to `texture(0)`.
This is an observed example, not a general binding contract. Shader variant
keys include compiler environment and target identity. Cooked shader admission
now accepts MacOS Metal artifacts separately from Win64 Vulkan artifacts;
runtime loading rejects a mismatched target.
The current RenderShaderCookedLibraryTests (6), RenderShaderCacheTests (13),
RenderShaderBuilderTests (24), and native MetalShaderQualificationTests pass on
the M4. The Builder target initially faulted during CMake's test discovery
before any case ran; retrying with `DYLD_PRINT_LIBRARIES=1` discovered and ran
all 24 cases. These results and the MacOS Cook/Game smoke close the selected
shader-route integration and wrong-backend artifact checks. Output parity for
the declared Metal/Vulkan image set remains open.

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
SPIR-V, and `ShaderSharedOutput` schema 6 carries the native binding map and
rejects stale map digests or malformed MSL. `ShaderCompiledOutput` schema 4
now carries target-specific SPIR-V or MSL, validated Metal binding maps, and
reflected compute thread-group dimensions checked against SPIR-V execution modes.
The cooked shader library schema 4 admits MacOS/Metal records and checks
platform/profile separation. Opening a library validates every required
record's payload and binding map before publishing it. Unit round trips verify
Metal library loading and rejection of malformed MSL with recomputed file and
record digests. Generic Cook manifests, incremental states, and Cook contexts
now admit MacOS target identity; MacOS manifest/state encode-decode round trips
pass alongside the existing Win64 golden hash and all 40 AssetCookTests cases.
The workspace-wide `all` build passes. The Cook command and engine contributors
now accept MacOS/Game. A seven-package project Cook succeeds in both dry-run and
published modes, producing the CMNF manifest, Cook state, package payloads, and
detached Metal shader library. A second incremental run reused six packages
(6,108,785 bytes) and recaptured one level (7,484 changed bytes). The five
Cook CLI tests, 40 AssetCookTests, 156 StaticMeshTests, 134 TextureTests, five
CookFunctionalTests, and workspace-wide `all` build pass. Runtime package and
nested payload readers now derive their target from the published manifest.
The Debug Game preset builds, and the matching Sandbox MacOS Cook reaches level
initialization and creates post-process and skybox pipelines with the Metal
pipeline cache. An explicit Sandbox pawn Cook root expands the output to eight
packages and removes the prior missing-pawn warning. The first 30-tick smoke
crashed when Metal treated a CPU-authored uniform-buffer view as a native
`FMetalBuffer`; the crash handler initially obscured the RHI-thread fault.
Metal now resolves the ordered deferred-buffer snapshot into an immutable native
copy for graphics and compute binding, retaining the copy and snapshot sidecar
references through submission. The scene also binds vertex and index buffers
before its graphics pipeline; Metal now accepts this order. The 30-tick Sandbox
Cooked Game smoke passes without renderer errors and exits normally on the M4.
The MacOS Cook output was staged into the Debug Game runtime directory for this
run; its module log contains MetalRHI but no ShaderBuild load. This qualifies
the Cook/launch/diagnostics checklist, not standalone distribution packaging.
Temporarily removing the cooked shader library or corrupting its header produces
specific renderer diagnostics for both post-process and skybox requests; these
two bounded diagnostic runs exit zero despite resource failure. The original
library bytes were restored after each run.
Both Debug Editor and Debug Game `all` builds pass after this migration. The
MetalRHIHeadlessTests and MetalShaderQualificationTests qualification targets
pass on the M4 host; AssetCookTests (41), AssetPackageTests (189),
CookedMeshLoadingTests (4), StaticMeshTests (156), TextureTests (134), and 32
native-test tooling cases pass. The successful game smoke receipt is
`Build/.agent-state/logs/20261004-121112-837002-3669-DurinGame.log`.
The first Metal Editor smoke exposed `WriteBuffer` calls before an explicit GPU
submission. Metal now records those uploads as ordered implicit submissions;
the existing headless triangle fixture verifies this in inline and threaded
modes and binds its vertex stream before pipeline selection. A hidden-window
Sandbox Editor 30-tick run initializes and shuts down cleanly. Its remaining
texture warning initially reported an unsupported BC1 sRGB sky cube. The M4
reports BC texture compression support and allocates a native BC1 sRGB cube.
Metal now admits BC1 cubes on supporting devices and uses block-aware upload
and readback pitches. A native fixture verifies pitched BC1 cube uploads,
exact compressed-byte readback through the smallest mip, and sampled sRGB-to-
linear decoding in inline and threaded modes. The subsequent hidden-window
Editor 30-tick run has no texture fallback warning; visible editor image
equivalence remains open. Receipt:
`Build/.agent-state/logs/20261004-122611-137724-4704-DurinEditor.log`.
An additional Metal GPU fixture dispatches twice after updating one CPU-authored
uniform buffer without rebinding. In inline and threaded replay, its native
result buffer contains the first and second versions in separate slots (7 and
13), including after releasing the logical uniform before GPU completion.
Metal now caches one native copy per snapshot and queue context, retaining its
snapshot and sidecars through GPU completion while reusing it across bindings.
This checks ordered snapshot resolution and retirement for that binding path;
the wider Stage 0 parameter/layout and image comparison matrix remains open.
Stage 3 now
has native Metal shader-function creation for checked MSL vertex,
fragment, and compute entries. The RHI shader descriptor carries the complete
native binding map and push-constant slot; Metal verifies canonical slots and
the remap digest before compiling MSL, while Vulkan rejects Metal maps. The
headless GPU test admits valid functions and rejects wrong targets, formats,
entries, and stale or noncanonical maps. Native compute pipeline construction
now validates the reflected binding layout against the shader's Metal map and
publishes the pipeline only after Metal accepts it; a mismatched layout or
thread-group size exceeding native limits is rejected. Recorded compute
dispatch now binds canonical buffer views through the native remap, validates
complete binding layouts and buffer ranges, and retains resources through GPU
completion. An offset structured-buffer write passes exact-value GPU readback
in both inline and threaded replay modes. The same path now snapshots and
binds validated compute push constants; its GPU readback also passes in both
modes. Metal now creates native 2D sampled/storage views, maps compute texture
and sampler bindings, and passes a GPU storage-write then sampled-read workload
with exact image and buffer readback in both execution modes. Graphics pipeline
creation and direct triangle draw now pass exact RGBA8 GPU readback through
production recording in both modes for one color attachment. Float2 vertex
stream input and 16-bit indexed draw with an index-buffer offset also pass
exact color readback. The native vertex map admits the Float, Half, UByte4N,
and Short4N formats used by current renderer paths. Fragment texture, sampler,
uniform-buffer, and push-constant bindings now pass a sampled draw with exact
GPU color readback in both modes; the graphics path checks complete layout
bindings before drawing. Recorded viewport and scissor commands pass a clipped
draw with exact per-pixel GPU readback. A one-color-plus-D32 pass now clears,
tests, and writes depth: a near draw occludes a later far draw, and exact
float depth readback is 0.25 in both execution modes. One-color blend factors,
operations, and color write masks now map to Metal; straight-alpha blending over
a cleared blue target and red-only writes pass exact RGBA8 readback in both
execution modes. A conservative headless capability report now admits native
indirect draw and dispatch. Direct and indexed indirect triangle records pass
exact RGBA8 GPU readback, and an indirect compute dispatch updates its result
buffer with checked values, in both execution modes. A GPU-authored indexed
indirect argument record now crosses a compute-to-graphics encoder boundary in
one submission and fills the target with checked pixels in both modes. The
remaining constant-color, constant-alpha, and source-alpha-saturate blend
factors now map to Metal. A zero blend constant matches the current Vulkan
pipeline default; constant-factor and saturation draws pass exact RGBA8 readback
in both execution modes. Broader render state and full editor material-preview
qualification remain open.
Metal graphics pipelines now admit line-list topology and carry it through
direct, indexed, and indirect draws. A recorded line draw produces the expected
partial 8x8 target coverage in both execution modes; editor line rendering
remains part of the Stage 4 application workload gate.
The authored `/Engine/ImGui` vertex and fragment shaders now compile through
the selected ShaderBuild route for both Metal and Vulkan, then execute through
production RHI recording, submission, and readback. A textured, vertex-tinted,
straight-alpha panel clipped to a 2x2 region of a 4x4 target matches between
Metal and Vulkan/MoltenVK within 1 LSB per RGBA8 channel in both inline and
threaded modes; pixels outside the scissor stay at the clear color. The authored
`/Engine/HitProxyOverlay` shader also passes an exact paired `RG32_UINT` ID and
distance-bit readback for covered and clipped pixels on both backends and both
execution modes. These are bounded shader-to-RHI paired-image slices. A
representative editor panel and retained capture/diff artifacts remain open.
Two RGBA8 color attachments now share a render pass with independently mapped
blend state; a fragment shader writing red and green outputs passes exact GPU
readback from both targets in inline and threaded modes. BGRA8, sRGB RGBA/BGRA,
and RGBA16F 2D render targets now accept graphics pipelines and pass exact byte
or half-float GPU readback after a draw in both execution modes. The advertised
color-attachment limit is now four; one pass writes the production GBuffer's
three RGBA8 targets and R11G11B10F emissive target with exact byte and packed
float readback in both modes. The same four-target pass now also writes D32
depth with exact float readback, qualifying the combined geometry attachment
layout in both execution modes.
The R8 visibility/AO render format and RG32_UINT hit-proxy render format now
accept graphics pipelines. Draws into each pass with exact byte and integer
GPU readback in both execution modes.
Native sampled views now admit 2D arrays, cubes, and cube arrays alongside 2D;
a cube's uploaded +X face passes an exact red result through a production RHI
compute sample and buffer readback in both execution modes. D32 2D arrays now
admit single-layer depth attachment views and depth-only render passes.
The sky-light path's RGBA16F cube storage descriptor and single-face 2D storage
view now execute a compute write through production RHI; the +X face returns
the exact expected half-float texel. Other faces, mips, and full sky-light
processing remain unqualified.
RGBA8 and R8 3D textures now admit sampled usage, and a mip-specific native 3D
sampled view is created in the volume transfer qualification. A recorded GPU
copy into mip 1 followed by a 3D compute shader sample returns the exact first
voxel value in both execution modes. Complete cloud rendering remains
unqualified.
Three layers independently clear and receive a full-screen depth draw, with exact
0.125 depth readback from each layer in both execution modes. Broader sampled
dimension workloads and shadow rendering remain unqualified. The shadow path's
dynamic raster depth-bias command now maps to Metal, and a depth-only pipeline
with bias enabled passes the layered draw qualification in both modes; bias
strength and complete production shadow output still need scene qualification.
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
ShaderBuild. Material cooked-program schema 12 carries each stage's target,
code format, native binding map, and compute thread-group dimensions;
MacOS/Game serialization, decode, and
wrong-platform rejection pass focused tests, including real Metal material
compilation. A MacOS project Cook and Debug Game run now pass; standalone
distribution packaging remains unqualified.

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
| `RGBA8_UNORM`, `SRGBA8_UNORM`, `SBGRA8_UNORM` | GBuffer, UI, texture sample, sRGB editor/present output | Native allocation with render/read usage; RGBA8 draw and selected-route compute storage output/readback passed; Metal RHI direct triangle draw and storage-write/sampled-read pass with exact image/buffer readback passed; native sRGB RGBA/BGRA render encoding and sampled linear decoding passed with checked channel order and float readback; layer presentation pending |
| `R11G11B10_FLOAT` | GBuffer emissive render and sample | Native allocation with render/read usage; native MSL float render and exact packed-bit readback passed; selected-route output and sampling pending |
| `RGBA16_FLOAT` | Scene color, cloud compute/storage, cube environment | Native 2D render/read/write and cube read/write allocation; native MSL float render and exact half-bit readback passed; selected-route compute storage write and half-bit readback passed; Metal RHI cube-face mip transfer and exact-byte readback passed; native cube +X/−X nearest sampling passed; linear filtering and selected-route sampling pending |
| `RGBA32_FLOAT`, `R16_FLOAT`, `RG8_UNORM` | Normal/default and authored texture sampling | Native sampled allocation; RGBA32 2D upload and linear filtering passed with exact float readback; selected-route sampling pending |
| `RG32_UINT` | Hit-proxy integer target and readback | Native render-target allocation and exact 64-bit pixel readback passed; authored selected-route hit-proxy overlay output matches Vulkan/MoltenVK exactly on covered and clipped pixels in a 4x4 production-RHI fixture; editor viewport capture pending |
| `D32` | Scene/shadow depth, sampled depth array | Native 2D and three-layer depth-array allocation; native MSL depth clear/write/store and float readback passed; Metal RHI one-color-plus-D32 depth occlusion and exact float readback passed; three distinct array-layer clears and comparison sampling passed; selected-route depth sampling pending |
| `BC1_UNORM`, `BC1_UNORM_SRGB` | Authored sky-cube sampling | M4 reports BC compression support. Metal RHI cube allocation, pitched block-row upload, exact compressed-byte readback through a one-texel mip, and BC1 sRGB sampled linear decoding passed in inline and threaded modes; full editor image comparison pending. |

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
- Use pinned Apple Metal-cpp for the native backend. Move resource and command
  ownership to explicit C++ RAII as calls migrate; keep Objective-C++ only where
  Cocoa window interop requires it. Do not treat a syntax-only conversion as
  complete without GPU lifetime and readback validation.
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
through presentation startup and viewport creation. Metal now retains this layer,
creates an sRGB BGRA or BGRA offscreen back buffer, and submits a same-queue
blit/present after recorded rendering. A standalone `CAMetalLayer` qualification
case clears, presents, resizes, and recreates the viewport in both execution
modes, with exact back-buffer pixel readback. Real window integration and
minimize/close cycles remain unverified. After fixing fallback texture admission,
implicit viewport clear submission, and the default shader compile target,
`DURIN_RHI_BACKEND=metal ./DevTool run` initialized the editor and remained
running for 43 seconds without a startup assertion; the run was stopped with
Ctrl-C. Visible output and interactive window behavior were not verified on
this host, where ImGui reported no platform monitors.
A later visible Editor attempt on this host initialized Metal and renderer
modules but could not finish Editor startup while the macOS session was locked;
screen capture showed a system removable-volume permission dialog for the
application-test host. The attempt was interrupted after 71 seconds. It does
not qualify visible presentation, resize, or close behavior; the hidden-window
Editor and native `CAMetalLayer` tests remain the bounded evidence.
The RHI loader now accepts `DURIN_RHI_BACKEND=vulkan|metal`, defaults to
Vulkan, rejects invalid names, and unloads the selected module by identity.
MetalRHI is registered in the Engine closure. On M4/macOS 27.0.1, its native
device and command queue initialize and shut down through the module loader in
both inline and threaded headless modes (`MetalRHIHeadlessTests`). It rejects
older macOS versions and devices below Apple GPU Family 9. The single physical queue is now reported and empty
logical submissions remain pending after CPU replay, submit on `SubmitToGPU`,
and become complete only from native Metal command-buffer completion. The
qualification target checks two ordered submissions in both execution modes
and cancellation of replayed but unsubmitted work at shutdown. It now also
checks three consecutive frame boundaries whose pending GPU submissions are
committed at frame end in both modes. Native shared
buffers now support initial data, staged writes/uploads, and buffer-to-buffer
copies through recorded commands. Immutable Metal sampler creation now maps
filtering, addressing, comparison, border, anisotropy, and LOD state, rejecting
unsupported descriptors. Validated unformatted buffer views retain their
parent allocation; formatted views remain unsupported. The qualification
target verifies GPU copy output after releasing the source wrapper before
replay in both execution modes, recoverable rejection of an invalid buffer
descriptor, native sampler allocation, and buffer-view lifetime. Private
2D textures in 11 color formats support transfer/readback, with selected
sampled, storage, and render usage verified later in Stage 3. Recorded buffer/texture and texture/texture blits, padded row layouts,
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
array/cube/volume formats and remaining texture operations are unsupported.
The production Metal command path now records a single-color RGBA8 offscreen
clear pass and reads its exact pixels after GPU completion in inline and
threaded modes. A conservative immutable capability report now describes the
up-to-two color/D32 depth targets and single-sample 2D resource subset,
shader resource limits, compute dispatch limits, and native indirect draw/dispatch.
Other render formats and resolves remain unsupported. Native failure injection, submission timeouts under
delayed GPU work, and comprehensive retention across completion remain open.
Receipt: `Build/NativeTestResults/MacOS-arm64-Debug-DurinEditor/MetalRHIHeadlessTests.xml`
(`./DevTool test MetalRHIHeadlessTests --mode qualification --report`; the
headless target passed on the stated M4/Xcode/macOS host).
The application-host `MacOSWindowLifecycleTests` target built with
`DURIN_ENABLE_APPLICATION_TESTS=ON`, but CTest discovery timed out in other
application-host targets before running it. That configuration was restored to
`OFF`. An unrelated `StaticMeshRenderPreparationVulkanTests` qualification run
failed in shader reflection (`Transform` missing) and later trapped during
test cleanup; it does not validate the changed presentation path. Real Vulkan
window startup and visible Metal presentation in a real application window
remain unverified.

- [x] Register `MetalRHI` and its platform build dependencies; implement explicit
  backend selection with useful unsupported-platform/device diagnostics.
- [x] Decouple ApplicationCore startup from unconditional Vulkan requirements;
  retain the existing Vulkan startup path and headless initialization contract.
- [x] Create the Metal device, baseline queue, and immutable capability report.
- [ ] Implement native layer/drawable ownership and viewport creation,
  presentation, resize, minimization, close, and shutdown behavior.
- [x] Verify both inline and threaded command execution ownership where applicable.

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

- [x] Integrate the selected shader route into ShaderBuild and runtime shader
  loading, with target-specific cache and cooked-artifact separation.
- [ ] Implement graphics/compute pipeline creation, reflection-to-native binding
  maps, constant data, arrays, vertex input, and required render/depth state.
- [ ] Implement the supported draw, indexed draw, dispatch, and indirect paths;
  preserve pipeline complete-or-failure publication and binding validation.
- [ ] Run actual global/material shader and compute/readback workloads through
  production command recording, replay, submission, and retirement.
- [x] Verify wrong-backend shader artifacts are rejected and cannot contaminate
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
- [x] Cook target-specific content and launch the game without a ShaderBuild
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
