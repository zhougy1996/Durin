# Material Async Derived Data Build Plan

Summary: Move authored material compilation to staged asynchronous orchestration with cached material translation, asynchronous ShaderBuild requests, and generation-safe publication.

Last reviewed: 2026-10-14

Status: Active
Completed:

## Current Status

The staged approach is selected; implementation has not started. Repository
inspection confirms that MaterialCompileLifecycle prepares normalized MIR on
GameThread, then runs CompilePreparedMaterialProgram synchronously on a Core
worker. ShaderBuild uses a persistent inline DDC session and waits on its request
owner. Materials therefore benefit from Shader DDC without an asynchronous
end-to-end material build chain or a persistent material translation product.

This plan adds only implementation stages and acceptance gates. Existing
[build](../Runtime/Assets/DerivedDataBuild.md),
[storage](../Runtime/Assets/DerivedDataCacheStorage.md),
[asset compilation](../Runtime/Assets/AssetCompilation.md), and
[shader](../Runtime/Rendering/ShaderCache.md) contracts remain authoritative until
their implementation changes land. No runtime tests or performance qualification
have been executed for this plan.

## Goal

Keep object traversal and immutable snapshot capture on GameThread; perform
detached translation, Shader cache access/compilation, and CPU assembly through
asynchronous stages. Warm translation and Shader hits skip their corresponding
producers. Cache waits must not retain an occupied producer worker. Preserve
semantic program sharing, independent consumer freshness, bounded admission,
last-known-good visibility, complete-candidate publication, and compiler-free Game
delivery. Explicit synchronous callers join the same pipeline.

## Selected Architecture

```text
GameThread snapshot capture and request admission
  -> Material.Translate DDC request (normalize, layout, requirements, Slang)
  -> semantic program sharing and retained-program lookup
  -> asynchronous ShaderBuild request (dependency validation, DDC, compiler)
  -> detached CPU assembly and reflection/layout validation
  -> completion mailbox and GameThread freshness checks
  -> complete candidate publication
```

Engine owns the orchestration, material transformations, typed assembly, consumer
diagnostics, resource admission, and publication. ShaderBuild owns dependency
closure capture/verification, compiler scheduling, Shader single-flight/LRU,
compiler contexts, and Shader DDC adaptation. DerivedDataCache remains a
backend-neutral request executor and store; Core remains the task scheduler.

The current IBuildFunction::Build contract produces and freezes output before
returning. Keep it synchronous. Engine connects separate asynchronous requests;
do not invoke a blocking Shader request inside a material build function or add
a generic DDC task graph, worker pool, or asynchronous producer context.

### Interface Changes

Names below describe selected responsibilities; Stage 0 freezes exact signatures
before implementation. Keep producer details private unless another module needs
the interface.

| Boundary | Required adjustment |
| --- | --- |
| Material snapshot | Retain SnapshotMaterialCompilerInput as the object access boundary. Capture detached compiler input, function-owner stamps, diagnostic mapping, and request configuration before worker admission. Audit graph emission that must remain on the owning thread separately from detached MIR normalization. |
| Material translation | Split PrepareMaterialProgram and CompilePreparedMaterialProgram into detached translation, generated-Shader request construction, and typed final assembly. Translation returns normalized MIR, semantic identity, active binding contracts, requirements, and generated source without invoking ShaderBuild. |
| Translation DDC adapter | Add an Engine-owned Material.Translate function, bounded immutable input resolver, output codec/assembler, and persistent inline/task sessions using the existing production build service and family session holder. Register before sessions capture the registry. |
| ShaderBuild module | Extend IShaderBuildModule with asynchronous generated-request submission: owned immutable request, consumer cancellation/priority, typed admission result, completion, and a handle/owner retirement boundary. Keep DDC types private to ShaderBuild rather than adding a DDC dependency to RenderCore's public interface. |
| Shader request lifetime | Distinguish one subscriber from its shared compiler flight. Completion is exactly once for accepted subscribers; rejection invokes no callback. Support inline completion, early cancellation, and completion before the submit call returns. |
| Material flight | Replace the single synchronous worker lifetime with an Engine-owned operation holding stage state, consumer list, cancellation, DDC owner(s), Shader subscription, CPU task handles, and detached completion storage. |
| Synchronous entrypoints | Route MIR::Compile, offline preparation, and Cook through shared transforms and assemblers. Provide explicit waiting at the caller boundary; no internal material worker waits for Shader cache I/O. |

No worker or DDC callback may dereference a material UObject, resolve a current
function asset, or mutate Renderer resources. Final application continues through
the material lifecycle on GameThread.

### DDC Product and Identity Boundaries

| Product | Immutable inputs and identity | Persisted output | Consumer-only data |
| --- | --- | --- | --- |
| Material.Translate | Canonically encoded detached pre-normalization MIR, parameter/collection schema contracts, relevant static selections and material properties, translation/generator identity, adapter/schema versions, and environment/limits actually consulted by translation. Exact field audit is a Stage 0 gate. | Normalized MIR and semantic program identity, active parameter and collection schemas, render layout, requirements, generated Slang, and bounded deterministic semantic diagnostics. Use fixed value IDs and bounded metadata; validate typed semantics after a hit. | Asset path, object handles, authored/parent/dependency revisions, publication generation, runtime values, collection defaults, diagnostic source mapping, and timings. |
| Shader | Generated source, exact ordered entries/stages, macros, target/compiler identity, and metadata-bound immutable dependency closure. ShaderBuild retains its existing action/schema ownership. | Complete validated Shader output, reflection, bytecode blocks, and cacheable deterministic messages permitted by Shader policy. | Material ownership, material publication status, subscriber cancellation/priority, and runtime resource state. |
| Assembled material candidate | Translation product plus matching validated Shader output; Engine revalidates their identity and binding contract. | No additional persistent Material.Program record in the required stages. Retain immutable candidates in the existing bounded process cache. | Current configuration, collection defaults, asset-qualified diagnostics, freshness qualifiers, GPU handles, and request observations. |

Translation request identity and semantic program identity serve different roles.
Canonical pre-normalization inputs select translation work; normalized semantics
select shared compiled programs. Different configurations or snapshots may miss
the same translation key yet converge to one semantic program flight. Do not salt
the semantic identity with asset paths or inactive configuration merely to simplify
request tracking. Keep compiler/dependency changes in the final program identity
even when a translation record can be reused.

Diagnostic node/port/function locations require a snapshot-owned mapping from
stable producer locations. Strip locations from cached payloads or encode stable
input-relative ordinals; never reuse another asset's GUIDs or paths after a hit.
Validate this mapping for both positive and deterministic negative hits.

Input resolvers own captured state. Describe reads immutable metadata; Resolve
materializes and verifies captured blocks only after a miss. Before Shader
execution, bind captured source artifacts or a verified immutable closure whose
identity agrees with the final program key. Dependency changes must fail/retry
under a new identity, never compile newer bytes under an older material identity.

DDC envelope checks do not replace typed validation. Wrong function/output schema,
truncated data, identity disagreement, invalid MIR/layout, and incompatible entry
sets must be rejected before use. A rejected cached product falls back once when
policy permits; fresh invalid output fails and is never persisted. Store failures
retain a usable validated local result. Deterministic input errors may be cached;
module availability, resource limits caused by admission pressure, infrastructure
failures, and cancellation must not become persistent negative entries. Preserve
typed diagnostics without parsing display strings as control flow.

### Scheduling, Sharing, and Retirement

- Admit snapshot/translation work under bounded request count and captured-byte
  limits before normalization; do not acquire semantic-flight capacity only after
  allocating unbounded preliminary work. Bound translation outputs, Shader source
  closures, assembly, retained results, and transition overlap. Resource ownership
  stays outside DDC, and nested stages must not double-reserve the same payload.
- Coalesce translation requests by captured identity when useful, then merge
  successful translations by semantic program identity. Forced and ordinary
  requests have separate flights. Force bypasses translation, retained-program,
  and Shader output reuse while retaining input verification.
- Cancel/supersede one consumer without canceling work needed by others. Last
  subscriber cancellation latches the stage token, withdraws queued work, and
  cooperatively cancels running work. An uninterruptible compiler may finish, but
  its canceled candidate is discarded. Never attach a new consumer to an
  irreversibly canceled flight.
- Do not hold the flight mutex across submission, completion callbacks, waits, or
  session drain. Record task/dispatch publication independently of business
  completion so synchronous callbacks cannot release live captures prematurely.
- A completed callback is not proof of operation retirement. Finish/shutdown join
  producer submission, DDC owner retirement, Shader subscriber retirement, and CPU
  tasks before releasing operation state or unloading modules. Progress must not
  require GameThread pumping until the final publication step.
- Stop admission before cancellation/drain; keep accepted producers and functions
  alive through retirement. Drain material operations before ShaderBuild/DDC and
  Core teardown. Preserve selected-owner finish/cancel and bounded deferred retries.

## Implementation Stages

### Stage 0: Freeze contracts and capture baseline evidence

Dependencies: none. Outcome: reviewed signatures, schemas, state transitions, and
acceptance fixtures, with no unresolved lifetime or identity decision.

- [ ] Audit snapshot/graph emission versus normalization, all synchronous callers,
  compiler-result consumers, and module startup/shutdown ordering.
- [ ] Freeze translation input/output schemas, semantic identity composition,
  diagnostic mapping, version invalidation, and source-closure verification.
- [ ] Freeze Shader asynchronous admission/completion/retirement signatures,
  priority aggregation, cancellation races, and synchronous adapter behavior.
- [ ] Define byte/count budgets and ownership transfer across every stage; specify
  progress/cache diagnostics without changing identity or retaining request history.
- [ ] Record reproducible cold, persistent-warm, and retained-warm baselines for a
  small material, a function-heavy graph, and shared instances. Measure GameThread
  capture/normalization, producer execution, cache wait, and end-to-end latency
  separately; record host, cache setup, sample count, and noise.

Completion: decisions above are recorded in this plan; baseline evidence and
adversarial fixture design are available. A timing claim requires measured data.

### Stage 1: Separate detached translation and candidate assembly

Dependencies: Stage 0. Outcome: current callers use one set of pure transforms
without changing observable material behavior.

- [ ] Extract translation, generated-request construction, and final assembly from
  MaterialProgramGenerator; add private typed intermediate products.
- [ ] Preserve validation, generated entry ordering, reflection/resource limits,
  semantic program sharing, and source-diagnostic mapping.
- [ ] Route existing synchronous and lifecycle paths through the extracted stages;
  remove duplicate generation/assembly logic.

Completion: MaterialCompilerTests, MaterialFunctionTests, and
MaterialCompileLifecycleTests pass with equivalent generated artifacts, layout,
diagnostics, and publication behavior. New tests target boundary errors and
equivalence rather than mirroring the extracted implementation.

### Stage 2: Add asynchronous ShaderBuild submission

Dependencies: Stage 1. Outcome: generated Shader requests progress without a
material producer worker waiting on cache I/O.

- [ ] Implement the asynchronous module entrypoint and task-backed persistent
  Shader session while preserving compiler service/module lifetime ownership.
- [ ] Move dependency preparation and cache/compiler continuations onto appropriate
  Core/DDC execution paths; retain immutable captured closure verification.
- [ ] Keep Shader single-flight/LRU and independent subscriber cancellation;
  implement agreed priority behavior and exactly-once completion.
- [ ] Preserve synchronous APIs as explicit submit-and-wait adapters; drain both
  execution modes at shutdown without destroying active Slang contexts.

Completion: RenderShaderBuilderTests, RenderShaderCacheTests, and
RenderShaderContractTests pass. Deterministic fixtures prove inline completion,
dispatch rejection, queued cancellation, shared subscribers, shutdown, and
single-worker cooperative waiting. A blocked cache fixture proves the dispatch
worker is released while unrelated work progresses. Run the required shared API
all build after migrating consumers.

### Stage 3: Add cached material translation

Dependencies: Stages 1 and 2. Outcome: translation is a registered, bounded DDC
product usable through task and synchronous sessions.

- [ ] Implement canonical input capture/encoding, resolver, function registration,
  output values/metadata, and typed assembly with independent version identities.
- [ ] Exclude consumer configuration/defaults and source locations as specified;
  reconstruct per-consumer diagnostics after cache hits.
- [ ] Implement force behavior, deterministic negative output, infrastructure
  failure translation, and local-result preservation on persistence failure.
- [ ] Add Engine-side injectable cache/session support scoped through request
  retirement; do not expose private schemas or fixture controls as runtime APIs.

Completion: MaterialCompilerTests plus relevant DerivedDataBuildTests and
DerivedDataCacheTests pass. Fresh-service persistent hits invoke neither the
translation producer nor payload resolution. Cover key sensitivity, equivalent
input ordering, output round-trip, wrong schema, corruption, oversized tables,
semantic rejection, negative hits, canceled misses, and store failure.

### Stage 4: Migrate material lifecycle to staged asynchronous operations

Dependencies: Stages 2 and 3. Outcome: normal authored requests use the complete
asynchronous pipeline, and GameThread no longer normalizes detached MIR.

- [ ] Replace the monolithic MaterialCompile task with bounded operation state,
  translation ownership, semantic-flight merging, Shader subscription, assembly
  task, and completion mailbox.
- [ ] Preserve independently qualified consumers, retained-program reuse, retry
  fairness, selected finish/cancel, last-known-good behavior, and atomic publication.
- [ ] Propagate cancellation/priority and force policy across stages; retain count
  and byte admission until the relevant captures retire.
- [ ] Apply current collection defaults/configuration only at typed consumer
  assembly/application; keep source maps associated with each consumer snapshot.
- [ ] Implement stop-admission and complete drain before producer module teardown.

Completion: MaterialCompileLifecycleTests and MaterialRuntimeTests pass. Cover
two translation inputs converging to one program, independent cancellation,
cancel-then-resubmit, supersession at every stage, stale function/parent/source
revisions, inline callback reentry, late task-handle publication, capacity retry,
owner destruction, and shutdown with pending cache/compile/assembly work. Tests
prove no live-object worker access and no blocked material worker during cache I/O.

### Stage 5: Unify synchronous and Cook paths and qualify the migration

Dependencies: Stage 4. Outcome: one production pipeline serves editor and offline
callers, with current runtime contracts and reproducible validation evidence.

- [ ] Migrate MIR::Compile, offline preparation, import/load paths, and MaterialCook
  to shared stage products and explicit waiting. Remove obsolete synchronous
  lifecycle implementations and compatibility-only helpers after consumer audit.
- [ ] Search every source/test project in Durin.dworkspace for changed shared
  symbols and migrate all consumers; validate each affected project target.
- [ ] Preserve DMAT/Shader package codec compatibility unless a separately
  justified payload change is recorded. Prove cooked Game does not acquire
  ShaderBuild/DDC dependencies or submit authoring requests.
- [ ] Run the validation matrix below and repeat Stage 0 workload measurements.
  Record warm producer counts, GameThread work, cache wait worker occupancy,
  cancellation latency, peak resident bytes, and cold-path cost.
- [ ] Publish implemented contracts in MaterialSystem, ShaderCache,
  DerivedDataBuild, and AssetCompilation; update material roadmap status only for
  implemented outcomes. Complete/archive this plan under local lifecycle rules.

Completion: all required correctness gates pass, the shared API all build and
affected project builds pass, lifecycle memory/count bounds hold, and measurements
demonstrate eliminated GameThread normalization and producer-worker cache waits.
Record and resolve material cold-path or memory regressions rather than assuming
that an extra cache layer is beneficial. Unavailable explicit gates remain open.

## Validation and Handoff

Follow [testing](../Agents/Testing.md) and [build/run](../Agents/BuildAndRun.md).
The target names below were checked against the configured native test registry;
re-query the registry when target registration changes. Run relevant cases alone
and their target together for singleton/session cleanup. Use deterministic cache
gates instead of sleeps for race coverage, including a single-worker scheduler.

| Area | Required coverage |
| --- | --- |
| Material semantics and diagnostics | MaterialCompilerTests, MaterialFunctionTests; positive/negative warm hits map diagnostics to the requesting graph; WPO and all seven compiled entries retain their contracts. |
| Lifecycle and publication | MaterialCompileLifecycleTests, MaterialRuntimeTests; latest-wins, selected finish, independent shared consumers, complete code/layout publication, pending last-known-good, failure fallback, and shutdown. |
| Editor and persistence | MaterialGraphEditingTests, MaterialEditorInteractionTests, MaterialEditingPersistenceTests, MaterialPackageTests, MaterialThumbnailTests; compile/apply/save, reload, preview and thumbnail readiness, base/instance parity. |
| Shader and cache | RenderShaderBuilderTests, RenderShaderCacheTests, RenderShaderContractTests, relevant DerivedDataBuildTests/DerivedDataCacheTests; dependency-content conflict, forced flights, failure recovery, rejected hits, and complete retirement. |
| Cook and Game | MaterialCookTests, RenderShaderCookIntegrationTests, RenderShaderCookedLibraryTests, plus CookFunctionalTests where affected; cold/warm equivalent payloads, function/static variants, target separation, compiler-free cooked loading. |
| Build compatibility | Affected owning/consumer targets across Engine, Sandbox, and RoadWeaver; an all build before handing off shared Engine/RenderCore API changes. |

GPU execution is required only if implementation changes rendering/resource
behavior or produces a specific unresolved GPU concern; select the registered
integration target under the testing workflow. This scheduling/cache refactor
does not itself require application-hosted execution or a quiet-GPU performance
lane. Record omitted coverage explicitly.

For each implementation commit, update this plan's status, passed checklists, and
exact evidence paths/results. Add Plan = Documentation/Plans/MaterialAsyncDerivedDataBuild.md
and the exact applicable Stage heading as git commit trailers, following repository
handoff rules. Plan authoring alone does not satisfy an implementation stage.

## Deferred Extensions

A persistent complete Material.Program product is not a completion gate. After
Stage 5 measurements, add a stage only if assembly or repeated validation remains
materially expensive. Specify its key, relationship to translation/Shader values,
storage duplication, invalidation, and measured benefit first. Per-entry Shader
cache/preprocessing deduplication is also separate from this migration; retain the
current complete generated-request contract initially. Remote execution, generic
DDC graphs, PSO caches, and changes to material language/rendering behavior are
outside this plan.

## UE Reference Basis

These references are Epic public API documentation and release notes, not an
inspection of private UE source or proof that UE runs material compilation as one
IBuildSession task. They inform responsibilities rather than prescribe Durin APIs.

- [FMaterial](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/Engine/FMaterial): asynchronous begin/finish caching and separate GameThread/RenderThread shader-map state.
- [FMaterialShaderMap](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Engine/FMaterialShaderMap): asynchronous DDC load, job submission, result assembly, and map persistence.
- [FShaderCompilingManager](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Engine/FShaderCompilingManager): asynchronous compilation and owner-facing result application.
- [FShaderCompileJobCollection](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/Engine/FShaderCompileJobCollection): in-flight/completed job caching and duplicate completion handling.
- [FMaterialCompilationOutput](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Engine/FMaterialCompilationOutput): persisted material translation outputs and their version contract.
- [FMaterialShaderMapId](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Engine/FMaterialShaderMapId): explicit static/environment/dependency identity; Durin retains its content-based semantic sharing policy.
- [UE 5.4 release notes](https://dev.epicgames.com/documentation/en-us/unreal-engine/unreal-engine-5.4-release-notes?application_version=5.4): historical per-Shader preprocessing/key deduplication; not a claim about current defaults or a prerequisite for this plan.
