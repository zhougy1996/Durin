# Metal Cooked Libraries and Load Preparation Plan

Summary: Cook Metal shader libraries and prepare retained Shader and PSO resources during loading through the same identities and creation paths used by rendering.

Last reviewed: 2026-10-18

Status: Active
Completed:

## Current Status

Implementation covers the binary Cook/native loading path and explicit program,
Shader and scene-PSO loading preparation. The working tree is based on
plan-authoring revision `ed20f8777`. Preparation uses the normal typed resources
and descriptors, retains their shared owners, and provides a cooked Game drawing
barrier. MacOS package, workspace and functional correctness gates have passed.
The plan remains Active for the complete phase baseline/numeric latency and RSS
qualification and the unavailable Windows Vulkan receipt; these are not waived.

The host initially lacked a usable Metal compiler. Xcode's supported component
installation reports Metal Toolchain `17F109`; `xcrun --toolchain Metal` resolves
Apple metal `32023.883` with SDK `26.5`. The default Xcode toolchain wrapper still
reports the component missing, so the producer explicitly selects `Metal`.

Initial focused receipts on Apple M4 / Xcode 26.6 / Debug / Tracy enabled:

- `ShaderFormatTests`: 7/7 passed, including binary profile, truncated/header
  rejection and binding-contract checks.
- `ShaderBuildQualificationTests` / `MetalLibraryBuildIsDeterministicAndCached`:
  passed. Two forced builds in distinct scratch directories emitted identical
  3,296-byte libraries; a third request hit DDC. The same fixture also compiles
  a generated Slang request through the source/native continuation.
- `MetalRHIHeadlessTests` / `BinaryImGuiShaderBuildMatchesVulkanThroughProductionRHI`:
  passed outside the sandbox with `MTL_DEBUG_LAYER=1`; binary Metal graphics
  creation and pixel readback match Vulkan in inline and threaded replay.

Reports are under `Build/NativeTestResults/MacOS-arm64-Debug-DurinEditor/`;
initial native logs include `Build/.agent-state/logs/20261018-191451-964205-94745-ctest.log`
and `Build/.agent-state/logs/20261018-191549-911379-95375-ctest.log`.
These are focused correctness receipts, not complete stage, package or timing
acceptance. The complete baseline, quiet-lane measurements, Windows coverage and
final performance/platform gates remain open. The user confirmed that no Windows runner
is available; Windows Vulkan remains a pending acceptance item.

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

## Binary Cook and Native Loading Implementation Evidence

The first implementation slice covers the core paths in Stages 1 through 4.
Stage 0 timing baseline and final performance/platform gates remain
open; checked implementation tasks below do not declare those acceptance gates
complete.

- The Shader domain passed all seven selected targets after profile and codec
  migration. The default-options regression caught an authored-backend selection
  issue; it was corrected without changing that existing test's expectations.
- `MaterialCookTests` passed 8/8, including a fresh MacOS built-in material
  package, six binary Game stages, matching program identity and graph-free
  reload; the existing Vulkan/material-instance paths also passed.
- `MetalShaderCookIntegrationTests.ProducesAndLoadsCompleteGameLibraryWithoutProducer`
  passed. It produces byte-identical complete fixed libraries twice, shuts down
  ShaderBuild, opens MacOS Cooked data and loads/composes every registered request.
  This test exposed a reentrant Shader-data lock: default option construction now
  reads a separately published atomic profile instead of reacquiring the lock.
- Native producer qualification passed again with cache-only local execution
  disabled on the warm hit, one-worker asynchronous continuations, and an
  independently canceled observer. Input version capture includes SDK build as
  well as SDK/compiler version; discovery processes now share the same process
  bound and cancellation observation as compilation.
- Native binary graphics and compute passed with Metal validation in inline and
  threaded replay. Graphics pixels match Vulkan; compute readback is exactly
  `0x12345678`. The counters establish zero MSL source-compile attempts for binary
  Shader creation and explicit binary load/function/PSO observations. The full native headless suite passed 48/48. Missing
  binary entry lookup and structurally valid but opaque-invalid library loading fail
  without source fallback. Shared-buffer readback uses the existing native
  qualification path after GPU completion; recorded buffer locks remain write-only.

Relevant logs (all beneath ignored `Build/.agent-state/logs/`):
`20261018-192032-519081-99663-ctest.log` (Shader domain),
`20261018-192443-577966-1930-MaterialCookTests.log` (Material Cook),
`20261018-195440-190392-8654-MetalShaderCookIntegrationTests.log` (MacOS DSLB),
`20261018-193106-598458-4061-ctest.log` (producer qualification), and
`20261018-193550-658743-5029-ctest.log` (three focused native cases).
Reports use the target names in `Build/NativeTestResults/MacOS-arm64-Debug-DurinEditor/`.
Final workspace and package-launch receipts are recorded below. Windows and
quiet-lane performance/retention qualification remain outstanding.

## Load Preparation Implementation Evidence

- `FMaterialProgramPreparation` performs detached read/decode and current
  GameThread admission. `MaterialCookTests` passed 8/8 with independently canceled
  requests, quality-change rejection, accepted-pointer reuse and cooked-only
  resaving without additional Shader compilations. Receipt:
  `Build/.agent-state/logs/20261018-210520-705821-24452-MaterialCookTests.log`.
- RenderCore preparation contracts passed 19/19 slot tests, including bounded
  operation admission, cache-hit payload retention through slot reset, independent
  cancellation and capacity failure. World tests passed 174/174; Renderer scene
  contracts passed 63/63. Later uncovered-demand counters extend this contract
  and are included in the final rerun selection.
- `MetalCookedScenePreparationTests` builds fresh binary Material/fixed products,
  retires ShaderBuild and runs native inline and threaded device lifetimes. Its
  selected scene includes Local and Spline geometry, opaque/masked programs, a
  material instance, AO/contact compute and directional shadow routes. Preparation
  records zero draws; covered drawing adds zero native Shader/PSO creation and
  zero source compilation. A second owner shares ready resources, cancellation
  retires only one owner, readback matches subsequent normal drawing, and scene
  release clears retained ownership before device recreation. Latest route
  receipt: `Build/.agent-state/logs/20261018-205952-870716-23603-ctest.log`.
- `MetalSceneQualificationTests` independently exercises normal authored scene
  routes with preparation before covered frames; the first inline Metal case
  passed zero additional native creation checks. The final replay selection is
  still being validated.
- Explicit `NativeProducerIdentity` and `bNativeCacheOnly` request options now
  permit native cache replay without SDK discovery/local native execution. The
  producer qualification passed warm replay, explicit-provenance miss, forced
  determinism, output-budget rejection and one-worker/cancellation checks.
  Receipt: `Build/.agent-state/logs/20261018-211736-510885-25303-ctest.log`.

Timing from correctness runs is diagnostic only. The pre-implementation complete
phase baseline, exclusive quiet-lane latency/RSS qualification and Windows Vulkan
receipt remain open. These are not replaced by zero-creation or pixel assertions.

## Final Functional Acceptance Receipts

Tested inputs are the final implementation on base `ed20f8777`, Apple M4,
macOS/Xcode 26.6, SDK 26.5, Metal Toolchain 17F109 / metal 32023.883,
Debug, Tracy enabled. GPU correctness uses `MTL_DEBUG_LAYER=1` outside the host
sandbox. No timing acceptance is inferred from these receipts.

- Final Editor workspace `all` passed, including Engine, Sandbox and RoadWeaver:
  `Build/.agent-state/logs/20261018-211419-576134-25008-cmake.log`.
  Final Game `all` also passed:
  `Build/.agent-state/logs/20261018-211421-890495-25073-cmake.log`.
- Shader domain: 8/8 targets passed; binary/source admission, formats, caching,
  fixed-library production/reading and target-specific integration are covered.
  Receipt: `Build/.agent-state/logs/20261018-204356-898663-22232-ctest.log`.
  Later bulk/handle lifetime changes do not alter those codec/compiler inputs.
- RenderCore contracts: 229/229 passed, including a prepared shared PSO surviving
  slot eviction without another backend creation. The new eviction case passes
  alone. Receipts: `20261018-211234-437715-24914-RenderContractTests.log` and
  `20261018-211438-568752-25227-RenderContractTests.log` under the log directory.
- Asset package tests: 191/191 passed. Bulk content revision and same-sized inline
  replacement rejection pass; the new bulk case also passes alone. Receipts:
  `20261018-205103-800355-22973-AssetPackageTests.log` and
  `20261018-211442-519324-25240-AssetPackageTests.log`.
- Material Cook: 8/8 passed, including MacOS binary families, early program
  preparation, cancellation, quality/bulk-generation rejection and compiler-free
  cooked-only resaving. Canceled resaving preserves the accepted program.
- Shared RHI pipeline service: 14/14 passed; World: 174/174; Renderer contracts:
  63/63. Receipts: `20261018-204128-734931-22021-RHIPipelineCreationTests.log`,
  `20261018-204135-336142-22047-WorldTests.log`, and
  `20261018-204140-969858-22123-RendererSceneContractTests.log`.
- DDC build protocol: 83/83 passed, including persistence failure preserving
  output and cancellation/continuation lifetimes. An existing fixture destroyed
  borrowed events before their owner; its declaration order now
  keeps those captures alive through owner cancellation. The corrected case
  passes alone. Receipts: `20261018-204031-255970-21907-DerivedDataBuildTests.log`
  and `20261018-211444-862464-25262-DerivedDataBuildTests.log`.
- Native producer qualification passes forced deterministic builds, physical
  corrupt-record repair, native compilation failure with no published output,
  bounded output rejection, single-worker/cancellation behavior and explicit
  native cache replay while `DEVELOPER_DIR` points to an unavailable SDK. The
  default request and a cache-only miss fail without source substitution.
- Native binary graphics/compute headless coverage passed 48/48 on the same Metal
  code/profile inputs. The cooked scene fixture passes real Local/Spline,
  opaque/masked, material-instance, shadow and AO/contact compute preparation,
  native first-use reuse, pixel equality, cancellation and device recreation.
  Direct global Shader-generation changes also make handles Stale and release
  their retained resources on the render thread. Normal authored preparation
  passes all four Metal/Vulkan inline/threaded replay cases:
  `Build/.agent-state/logs/20261018-204225-694215-22160-ctest.log`.
- Fresh Sandbox Cook passed with 8 packages and no failed/omitted packages;
  `/Game/Models/GrayboxPawn` is an explicit extra root for the native gameplay
  reference. Receipt: `Build/.agent-state/logs/20261018-211930-762789-25391-DurinAssetTool.log`.
  Game then ran 500 ticks with no ShaderBuild module while file data reads were
  denied for Engine/Sandbox/RoadWeaver authored Content, Shader directories and
  editor DDC. It prepared 12 Shaders and 9 PSOs, retaining 53 objects / 115,503
  accounted bytes, rendered and exited successfully without the missing-pawn
  warning. Receipt: `Build/.agent-state/logs/20261018-212310-248190-25644-DurinGame.log`.
  The process-only profile is `Build/.agent-state/MetalCookedGameQualification.sb`;
  directory metadata remains readable for engine-root discovery. An initial
  broader metadata denial stopped root discovery before runtime initialization;
  source files and global permissions were never changed.

Reports are under `Build/NativeTestResults/MacOS-arm64-Debug-DurinEditor/`.
Single-case runs may replace a target XML; the named full-target logs above retain
its full receipt. Relevant inputs are unchanged for reused source/format/RHI
receipts; later differences concern scoped loading ownership, bulk admission or
test-only lifecycle setup and are covered by their final focused runs.

Remaining work is qualification: recover a separate complete phase baseline,
freeze numeric load-time/RSS acceptance and noise/repeat policy before evaluating
candidate performance, collect exclusive quiet-lane samples, and run Windows
Vulkan when a runner becomes available. The user explicitly accepted Windows as
pending. No exclusive GPU window has been confirmed for performance acceptance.

## Initial Contract Decisions

- Editor and EditorValidation retain the original Metal/MSL profile. MacOS Game
  Cook selects `MetalLibraryShaderTarget`, output format `MetalLibrary`, profile
  `Durin.ShaderTargetProfile.MetalLibrary.1`; binary production uses macOS MSL 2.0,
  minimum macOS 14.0 and `-O2`. Runtime admission does not compare the producing
  Xcode/SDK version with an installed compiler.
- `Durin.Metal.Library` version 1 / constants schema 1 / output `Metal.Library`
  schema 1 uses the existing Shader DDC bucket. Explicit MSL bytes, producer
  version/SDK and immutable target settings determine its action. Source stage
  products retain their existing compiler identities and caches. Final binary
  artifacts acquire new code hashes and artifact identities before assembly.
- DMAT/DSHD/DSLB field layouts do not change merely to carry the new enum value.
  MacOS Game admission now requires the binary profile; older source-profile
  MacOS Game packages need recooking. Material Cook schema 7 prevents old package
  reuse. EditorValidation remains a separately admitted source profile.
- Native actions use a unique scratch directory, stable relative source name,
  a two-producer limit, 120-second process deadline, 16 KiB captured diagnostics,
  1 MiB MSL input and 64 MiB binary output limit. Material stage-set limits remain
  authoritative. Binary request overlap reserves twice its output allowance in
  the existing 1 GiB Shader subscriber budget. This accounts for retained subscriber
  products, not process RSS or temporary-file peaks; each of at most two native
  producers separately permits a 64 MiB AIR and a 64 MiB library file.
- Structural binary validation admits the MTLB envelope and declared extent;
  native loading validates the opaque library, platform and requested function.
  Reflection and Metal binding metadata retain their existing exact contract.

Stage 0's complete phase baseline and numeric latency/RSS acceptance remain open;
count and accounted-retention admission limits are frozen above.
The profile/native-producer work above is being validated as a prerequisite spike
so a real binary route exists for the planned source-versus-binary measurements;
it does not count as passing the missing baseline or authorize performance claims.

## Preparation Implementation Decisions

The scene preparation entry point will reuse the existing logical, view-resource,
scene-resource and feature-resource phases and stop before graph authoring,
temporal history mutation or drawing. Each render-thread pump rebuilds its local
logical snapshot rather than retaining borrowed scene proxies across frames.
Callers supply the actual scene, view, output and render options; readiness covers
that declared use. New streamed uses retain the existing demand fallback.

A separate handle will own retained ShaderMap code/resources, completed slot
payloads and shared pipeline requests. Renderer will retire those owners on scene
release, Shader/device invalidation or cancellation. Admission is limited to four
active owners, 4,096 retained objects and 256 MiB of accounted retained metadata
and shader bytes per owner. These are not estimates of driver memory or process
RSS. A pump admits at most eight new native Shaders and sixteen pipeline requests,
with a nominal 2 ms scheduling deadline checked between new operations. One
native call cannot be interrupted; each pump may admit one operation to guarantee
progress. No pre-implementation wall-clock acceptance claim is made.

Preparation will yield with Pending instead of waiting on pipeline completion.
The native request service remains responsible for compilation and sharing.
Successful transactional slots keep their bounded request identities while their
payload is resident, so a preparation owner can retain the same weak-cache entry
rather than merely a native PSO pointer. Same-sized inline bulk replacement cannot be detected from range metadata alone.
Engine therefore adds a transient BulkData content revision, renewed on storage
replacement or write admission, and compares it at pending program publication.
Read/unload operations do not change it. This closes stale admission without
forcing synchronous content hashing or exposing a storage pointer.

Engine now exposes a separate cooked-program owner for asynchronous bulk read
and detached decode. GameThread admission checks the material revision, weak
object, package range/root, configuration and collection/declaration contracts.
Cooked default-level startup explicitly requests selected Local/Spline geometry
in batches of four, waits for CPU/GPU residency, then pumps up to eight programs,
then activates the level and enables its viewport drawing barrier. Scene
preparation consumes immutable programs published through material proxies and
does not redefine async asset success. Pipeline service priority remains Normal;
loading owners determine pump order instead of creating another queue.

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

- [x] Search target/profile, code-format and shared interface consumers across
  every project in `Durin.dworkspace`: Engine, Sandbox and RoadWeaver.
- [x] Define editor-source versus cooked-binary profile selection at each caller,
  including Material compiler environments, default/error surfaces, fixed shaders,
  DSLB inventory lookup, native RHI admission and cooked-only resaving.
- [x] Freeze supported OS/deployment target, language/optimization settings,
  SDK/compiler provenance and the runtime compatibility policy. Distinguish
  recorded producer provenance from runtime format/ABI admission requirements.
- [x] Specify the offline product descriptor and keys, program/artifact identity
  composition, codec versions and old-package migration/recook policy.
- [x] Freeze preparation input ownership, progress/terminal states, cancellation,
  priority, budgets, activation handoff and device/Shader-generation invalidation.
- [x] Choose a bounded scene covering Local/Spline, Forward/GBuffer, opaque and
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

- [x] Add the binary code-format value and immutable cooked profile. Preserve
  existing serialized enum numbers and the meaning of the source profile digest.
- [x] Add explicit purpose/profile selection where one platform previously had
  one Shader target. Keep availability checks with the producer, not policy.
- [x] Replace single-Metal-target assumptions with validated profile resolution;
  retain exact target/output-format checks at request and payload boundaries.
- [x] Split MSL structural admission from bounded binary structural admission.
  Remove text-entry searching from the binary path; retain hash, metadata,
  reflection, frequency and compute-group checks. Native validation remains final.
- [x] Ensure target, code format, settings, code bytes and binding metadata enter
  artifact identity. Finalize binary identities before publishing immutable results.
- [x] Extend DSHD/DSLB/DMAT admission and version policy as required by their
  actual representation changes; do not bump unrelated schemas mechanically.
- [x] Migrate source callers and fixtures across the workspace and add rejection
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

- [x] Implement explicit native compiler/SDK discovery and supported command-line
  compilation/linking. Return structured unavailable-toolchain and execution errors.
- [x] Capture MSL bytes and explicit settings; key the action by source content,
  relevant target settings, actual toolchain provenance and algorithm versions.
  Do not include absolute installation or temporary directory paths in keys.
- [x] Preserve independent stage reuse through existing build/DDC conventions and
  shared immutable buffers. Coalesce equal native compilation requests.
- [x] Bound concurrent processes, input/output/diagnostic sizes and intermediate
  storage. Poll cancellation, terminate/reap owned process trees and clean scratch
  files on success, failure, cancellation and shutdown.
- [x] Validate determinism across scratch directories; control embedded paths,
  debug data and other nondeterministic output before claiming portable reuse.
- [x] Assemble final code format, hash, entry, target and artifact identity while
  retaining the original reflection and binding contract. Source/debug retention
  is an explicit editor/tool policy rather than a runtime dependency.
- [x] Exercise cache hits without an executable local producer, cold misses with
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

- [x] Select the binary target before detached Material compilation and final
  assembly. Preserve configuration selection, stage sharing, exact required-stage
  order, Game HitProxy exclusion and atomic result admission.
- [x] Route `ProduceCookedShaderLibrary` through the same binary build product,
  including fixed opaque-shadow stages and required global/compute requests.
- [x] Project native producer/settings changes into the owning Cook dependency
  declarations and production identities, so CookState reuse cannot retain stale
  source-profile packages or libraries under binary configuration.
- [x] Update material compiler environment and program digests coherently; do
  not mutate an accepted source result or reuse its artifact identity after conversion.
- [x] Keep native production in preparation, before Archive emission. Serialization
  consumes qualified detached products and never starts compilers or DDC requests.
- [x] Check binary-size effects against per-stage, material-family, prepared-product
  and serialization limits. Adjust measured budgets deliberately if necessary.
- [x] Preserve transactional failure/cancellation and cooked-only resaving for
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

- [x] Dispatch library creation by admitted code format: source compilation for
  the source profile, binary-data loading for the cooked profile.
- [x] Define binary buffer/API ownership, resolve the recorded entry and validate
  its native function type and device compatibility before publishing `FMetalShader`.
- [x] Continue consuming compiler-agreed binding metadata; preserve current
  graphics/compute pipeline layout validation and prepared binding behavior.
- [x] Distinguish library load, missing entry, wrong frequency and incompatible
  deployment/target failures. Strict cooked paths do not invoke source compilation.
- [x] Add counters or equivalent observations separating source compilation,
  binary library loading, function creation and native PSO creation.
- [x] Exercise resource release, device recreation and source/binary coexistence
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

- [x] Add a separate preparation handle with asynchronous progress and terminal
  results. Preserve existing `FAsyncLoadHandle` object-readiness semantics.
- [x] Start selected cooked bulk reads/decode before accepted-program demand;
  detach CPU processing where supported and admit current results on the owner
  thread after object/load-generation/configuration validation.
- [x] Publish immutable accepted program values to render preparation through
  the module boundary. Include required registered fixed stages in the request.
- [x] Create the exact typed ShaderMaps/resources through normal rendering paths,
  then force creation of requested RHI Shader entries. Match normal resource keys
  and stage grouping; identical bytes alone do not prove ShaderMap cache reuse.
- [x] Budget render-thread creation by count and measured cost between operations.
  Document that one native operation is not preemptible by a frame-time budget.
- [x] Retain strong resource references through activation and transfer them to
  normal resource owners, or retain them in a bounded shared preparation owner.
- [x] Support shared consumers, priority, partial progress, cancellation, unload,
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

- [x] Reuse exact Pipeline description builders through the normal resource
  resolution phases. Preload and drawing use the same keys, ShaderMap ownership, layout,
  vertex declaration, topology, raster/depth/blend and attachment configuration.
- [x] Collect reachable combinations after mesh slots, component overrides,
  material variants and registered Vertex Factory facts are available. Include
  required fixed feature/compute PSOs selected by the rendering configuration.
- [x] Define explicit render-configuration input for target formats/sample counts
  and enabled routes. Asset-only requests stop at the strongest readiness they
  can establish rather than inventing a complete PSO from a material alone.
- [x] Submit through existing shared precache/request services; use continuation
  or owner-thread polling for pending work. Respect current worker/replay wait
  restrictions and do not block render-command replay waiting on itself.
- [x] Retain pipeline request/result handles under bounded budgets. Transfer to
  the corresponding Renderer slots or provide compatible shared ownership until
  the scene's declared preparation residency ends.
- [x] Add a load-screen activation barrier and a background-streaming policy;
  keep the current frame resource waiting boundary as the fallback for uncovered
  or changed combinations. Report coverage and misses separately from failures.
- [x] Handle settings/material changes, cache eviction, scene unload and device
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

- [x] Run format/codec/profile, ShaderBuild/cache, Material Cook/lifecycle,
  fixed-library and RHI pipeline-service tests selected through the registry.
- [x] Qualify native Metal binary graphics/compute, bindings, retained preload
  reuse and cold/warm scene preparation with Metal API validation where supported.
- [x] Cook fresh MacOS Game output and run it without authoring sources or editor
  DDC. Verify materials, instances, fixed feature shaders and compute execution.
- [x] Compare prepared and demand paths for identical output. Cover cancel/unload,
  shared observers, preparation failure, configuration changes, eviction and
  supported device/Shader-generation retirement behavior.
- [ ] Capture per-phase times, covered first-frame creation/wait counts and peak
  retained resources. Meet Stage 0 numeric budgets; report compiler work moved
  into Cook separately from device work moved into loading.
- [x] Complete one final MacOS workspace `all` build: the foundational RHI target
  definition and renderer-module interfaces cross module/project boundaries whose
  aggregate compilation cannot be established from native-test selection alone.
  Migrate and validate affected Engine/Sandbox/RoadWeaver consumers. Additional
  platform-wide build scope requires a stated uncovered risk.
- [ ] Retain a Windows Vulkan build/correctness receipt for changed shared paths;
  no Windows runner is currently available, as confirmed by the user.
- [x] Update Material lifecycle, Shader Cache, target policy, fixed Shader and
  Renderer preparation contracts with lasting behavior and links. Keep execution
  checklists and stage evidence in this plan.
- [x] Run changed-document and all-plan validation, record final evidence and close
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
- `Engine/Source/Runtime/Engine/Public/Asset/BulkData.h`
- `Engine/Source/Runtime/Engine/Public/Materials/MaterialProgramPreparation.h`
- `Engine/Source/Runtime/RenderCore/Public/RenderResourcePreparation.h`
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
