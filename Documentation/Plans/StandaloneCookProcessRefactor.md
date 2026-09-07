# Standalone Cook Process Refactor Plan

Summary: Simplify Cook around a dedicated process and workflow-stable source tree while preserving explicit incremental dependencies and transactional output.

Last reviewed: 2026-09-08

Status: Completed
Completed: 2026-09-08

## Current Status

Stages 0–4 are complete. Production Cook uses ordinary loading and file-backed
resources with separate dependency, state and output-store implementations. All
76 routine native targets, the full host build and documentation gates passed.
The selected operating contract: Cook does not modify its input directories, writes to a separate output
directory, and runs in a dedicated process. Editing assets during Cook is an
unsupported workflow violation, not something the engine must prevent or detect.

The baseline is `4a6acea45`, following the completed
[Asset Registry and Cook Consistency plan](AssetRegistryCookConsistency.md).
That implementation supplies explicit dependencies, Cook-state v2, private
loading, owned input artifacts, and live-operation capture guards. This plan
preserves the dependency and publication work while replacing the in-process
input-isolation protocol. The earlier plan remains valid historical evidence;
its stronger capture guarantees are deliberately withdrawn as this plan lands.

`DurinAssetTool cook` already refreshes the Registry, registers contributors,
invokes the coordinator, and emits structured results in a standalone executable.
Its bootstrap also starts task/compilation services and loads build providers.
The refactor should reuse this host rather than introduce another executable.
`FAssetLiveLoadGuard` also has a package-linker consumer; its entire implementation
cannot be assumed Cook-only.

## Goal

Make the supported production flow read as: initialize the Cook process, discover
inputs and dependencies, evaluate reuse, load/build required packages, and publish
to a separate output tree. Ordinary process-local object residency provides
isolation from the interactive editor. Remove the private Cook object universe
and repeated defenses against input mutation during the run.

Completion requires a net removal of Cook-specific isolation machinery and its
cross-module hooks, not a rename of `FCookInputCapture` or a second selectable
implementation. Do not claim a percentage or line-count reduction before the
consumer inventory and resulting diff establish it.

## Selected Operating Contract

### Process and input ownership

- One invocation of `DurinAssetTool cook` performs one Cook run and then exits.
  Production callers use this process boundary. The coordinator may remain
  testable as an internal component; it does not promise safe use inside a live
  interactive editor or concurrent/reentrant sessions.
- The invoking workflow saves intended edits and finishes imports before launch.
  All input packages, companions, declared external files, shader sources,
  configuration, and native build code remain stable until process exit.
  Unsaved editor memory is not part of the Cook input.
- The workflow must not edit, save, import, relocate, delete, synchronize, or
  check out participating inputs during Cook. This is a documented precondition.
  Do not add directory locks, write interception, filesystem permission changes,
  watchers, mutation counters, repeated rehashes, or editor-wide Cook guards to
  enforce it. A violation has no consistency guarantee or required diagnostic;
  its output must be discarded and Cook rerun from stable inputs.
- Cook itself never saves or repairs authored input files. Derived preparation
  may change process-local derived state but must not persist authored changes.
  Audit callbacks and bootstrap services for writes rather than adding a global
  mutation gate. Input immutability is tested as Cook behavior.
- Output is a separate tree outside every participating input tree. Reject an
  overlapping output destination before creating files, resolving existing
  filesystem aliases when validating the destination. This validates Cook's own
  write destination; it does not police another process's edits.
- DDC, shader intermediates, logs, temporary files, scan caches, and reports must
  also use writable locations outside the protected source trees. A sibling
  output/cache directory under the project is acceptable when it is outside
  input mounts and excluded from discovery. Do not require a full source copy.

### Loading and lifecycle

Use the ordinary package loader and resource system in the dedicated process.
Packages loaded earlier in that run are valid residents; editor-resident objects
cannot cross the process boundary. Recursive loads, aliases, cycles, lazy bulk,
and GC ownership retain their normal loader contracts.

Load modules and register types/providers before executing package work. Keep
ordinary ownership and shutdown order: finish/join owned work, release objects
and resources, and only then release provider code. Process isolation does not
make callbacks safe after module unload. Remove Cook-only registration freezes,
retirement probes, and repeated participant admission checks where the stable
host contract replaces them. Preserve shared lifetime mechanisms used elsewhere.

Cook preparation must complete the derived work required for serialization.
Replace private-graph checks in family code with the smallest explicit offline
preparation contract needed; do not accidentally start editor import/apply work
or introduce a new global mutation-prohibition protocol.

### Incremental identity and publication

- Preserve pure Registry queries and ordinary live Load/mutation admission.
  Only Cook-specific repeated live validation is removed. Refresh/discover once
  from the stable source tree and retain normal malformed/missing-input errors.
- Preserve source/bulk content identities, exact references, direct/transitive
  package dependencies, cycle termination, build-only versus runtime reachability,
  external values/files, schema and native producer versions, and canonical bounds.
  Unrelated Registry revisions and timestamps remain outside content identity.
- Evaluate declarations before cache lookup. Missing declarations continue to
  disable reuse; external/native inputs must remain explicit. A dependency map
  may retain paths, hashes, and small values without owning every file's bytes.
  Files can be opened again because the workflow guarantees stable inputs.
- Remove mandatory whole-run source/bulk/shader copying where its only purpose
  was concurrent-edit isolation. Preserve buffering required by codecs, compiler
  APIs, lazy-resource lifetime, or measured performance. No streaming rewrite is
  required merely to reduce allocation counts.
- Preserve verified cache-hit output buffers, detached save plans, output-store
  single-writer protection, cancellation, manifest-last publication, and existing
  rollback/recovery behavior. Do not promise crash guarantees beyond the store's
  implemented contract. Dry runs do not publish or modify authored input.
- Prefer retaining Cook-state v2 when dependency meaning and output recipes stay
  compatible. If meaning changes, explicitly bump the relevant producer or state
  version and treat incompatible state as a miss. No authored/runtime format
  migration is justified solely by this process refactor.

## Implementation Stages

### Stage 0: Inventory isolation consumers and host side effects

- [x] Inventory callers of the coordinator, private Cook loads, active-capture
  queries, live-load guards, schema freezes, module/provider leases, and fixed
  shader input APIs. Classify each as Cook-only, shared, or still necessary.
- [x] Trace the existing tool bootstrap, PostLoad, contributor preparation, DDC,
  shader compilation, and shutdown for authored writes and asynchronous work.
  Record exact output/cache destinations and ownership ordering.
- [x] Identify production launch sites and test registration for process-level
  Cook. Record the existing command/result/cancellation contract to preserve.
- [x] Record a concrete deletion/migration list, including family hooks and tests
  whose input-mutation guarantees are intentionally removed. Resolve shared
  consumers before deleting their infrastructure.

Completion condition: a bounded consumer map identifies what is deleted, what
replaces it, and which shared behavior must survive. No new isolation mechanism
is introduced to replace the retired one.

#### Stage 0 evidence and migration map

- Production: `Tools/DurinDevTool/durin_dev_tool/cook.py` already launches
  `AssetToolMain.cpp`; all other `FCookCoordinator` callers are native fixtures.
  Preserve repeatable roots, Win64/game selection, JSON schema v1, deterministic
  package ordering, SIGINT polling and exit codes 0/1/2/130. There is no existing
  registered test that launches the real Cook executable. Add that coverage.
- Delete Cook-only `FCookInputCapture` phases/TLS, resident-conflict checks,
  participant/mount revalidation, private skeleton/object pins and owned bulk
  resources. Retain inspection, exact reference checks, bounded dependency graph,
  source/bulk digests and schema encoding in dependency analysis. Open declared
  external files from their locators when needed; keep small canonical values.
- `CookBuildProviders.cpp` invocation nesting, validator callbacks and descriptor
  TLS exist only for Cook. Replace with ordinary provider descriptor queries.
  Keep modular-feature invocation/retirement and module code leases themselves:
  they have shared non-Cook consumers. Remove only coordinator lease acquisition.
- `FScopedTypeRegistrationFreeze` has one production scope (coordinator); its
  other scopes are private-loader/Cook tests. Remove the scope, reflection mutation
  hooks and corresponding rejection tests together. Private package loading and
  `IsGraphPrivate` are also used by relocation, redirector fix-up and package
  operations: retain shared graph publication and linker behavior.
- `FAssetLiveLoadGuard` is shared with `AssetPackageLinkerLoader` custom dependency
  policies. Retain its non-Cook load/mutation checks and tests; delete only the
  coordinator scope. Do not remove ordinary Registry admission.
- Family migration: StaticMesh PostLoad currently skips async compilation for
  private Cook graphs; its Cook contribution already builds a detached product.
  Material chooses synchronous compilation with the same predicate. Replace these
  with explicit offline derived preparation. EnvironmentLighting PostLoad and
  contribution read captured `.iblbulk`; ordinary authored payload reads suffice.
  Texture recipe persistence uses DDC, not authored package saving.
- ShaderData Cook input scopes pin the provider, copy source artifacts, prebuild
  the library and redirect compiler calls through TLS. Fixed source artifacts also
  serve compiler/integration tests. Remove mandatory Cook copies and retirement
  checks while preserving explicit shader/compiler identity and shared APIs until
  their remaining consumers are resolved.
- Host side effects: explicit project initialization reads descriptor/mounts;
  Registry refresh writes `<DDC>/AssetRegistry/Registry.bin`; texture/mesh recipes
  use DDC; shader manifests/intermediates use `<DDC>/Shaders/SPIR-V/<namespace>`.
  Default DDC is `<Project>/DerivedDataCache`; default logs are
  `<Launch>/Saved/Logs`. Audit and redirect these before logger/service startup.
  Cook reports go to stdout, output staging/state/manifest go under `OutputRoot`.
  Resave remains a separate explicitly selected operation.
- Shutdown defect: the raw Renderer inventory library currently destructs before
  `FScopedEditorServices`. Join compilation/tasks and release ordinary loaded
  objects before releasing that library; shared module retirement remains intact.
- Compatibility: `FCookSavePlan::FingerprintVersion` has no reader. State v2 and
  detached buffers remain compatible. `FCookContext::Publish` still serves direct
  family fixtures; preserve or document that lower-level role separately from
  production coordinator orchestration.
- Validation registry: `AssetCookTests`, `AssetBulkContainerTests`,
  `CookedMeshLoadingTests`, `RenderShaderCookIntegrationTests` and
  `RenderShaderCookedLibraryTests` are relevant. `TextureCookIntegrationTests`
  is a GPU qualification lane; do not treat unavailable GPU execution as a CPU
  implementation gate. Baseline `./DevTool build --target DurinAssetTool` passed.
  Retire mid-run source replacement, provider replacement, type mutation rejection
  and resident conflict assertions only where they test the withdrawn Cook
  contract; keep stable-input invalidation, publication and shared loader tests.

### Stage 1: Establish the standalone host contract

- [x] Make the existing tool the supported production entry point; migrate any
  production in-process callers to launching it. Preserve CLI/JSON/exit outcomes.
- [x] Establish source/output separation before writes and route caches,
  intermediates and logs outside input trees. Do not change source permissions.
- [x] Initialize modules, settings, mounts and root providers before package work;
  make one-run ownership and orderly cancellation/shutdown explicit.
- [x] Document stable-input workflow prerequisites and unsupported concurrent
  edits at the tool's owning usage documentation, without confirmation prompts
  or runtime prevention of edits.
- [x] Add process-level coverage for source preservation, output separation and
  loading saved assets independently of another process's resident state.

Completion condition: the real Cook executable runs with separate writable
outputs and leaves authored input intact. The process boundary is usable before
capture protections are removed.

Stage 1 validation: `./DevTool test StandaloneCookProcessTests` passed (1
registered process integration test, 8.13 seconds). It invokes the real executable
for fresh/repeat/dry-run/missing-input runs, compares source inventories and content
hashes, rejects source/ancestor/symlink output overlap before files are created,
and verifies that an unsaved parent-process object is absent from cooked output.
DDC/shader intermediates and logs are routed beneath the validated output tree.
Project root modules load before package work; compilation/tasks and loaded asset
resources shut down before the raw Renderer inventory library is released.

### Stage 2: Replace private capture with ordinary process-local loading

- [x] Retain dependency discovery/evaluation as a focused component; replace
  `FCookInputCapture` private object loading with the normal package loader.
- [x] Remove Cook-specific resident conflicts, capture TLS/state transitions,
  sealed-input fallback interception, and repeated mount/fence/type validation.
  Preserve ordinary runtime admission and non-Cook loader guards.
- [x] Remove mandatory retained source/bulk copies and owned lazy-resource routes
  from Cook where ordinary file-backed resources satisfy the stable-input contract.
- [x] Migrate StaticMesh, Material, EnvironmentLighting and texture preparation
  away from private-graph predicates while preserving payloads, declared input
  identities, synchronous readiness where needed, and absence of authored writes.
- [x] Simplify shader capture and Cook-only provider pinning where supported by
  the fixed host lifetime. Keep compiler inputs explicit and shared provider APIs
  intact unless every consumer is migrated.
- [x] Remove now-unused hooks/exports only after consumer checks, including
  CoreDObject and runtime mutation paths. Do not retain an old/new mode toggle.

Completion condition: the standalone path uses ordinary object residency and
resources; dependency correctness survives without a private Cook object graph
or a runtime input-mutation enforcement protocol.

Stage 2 evidence: real process coverage passed after ordinary-loader migration;
`AssetPackageTests FPackageAssetTests.Cook*` passed 11 cases. The registered
`@domain=asset-package+asset-cook+shader,kind=contract+feature+integration` selection
passed all 10 targets, including real shader library production/runtime loading.
Deleted private input capture and its object/resource ownership, active-capture
exports, whole-run source/bulk storage, reflection freeze hooks, native recipe
pin/validator nesting, and shader capture TLS/copy wrappers. Shared private linker,
live-load guard, modular feature lifetime, and explicit fixed shader artifact APIs
remain. Shader provider interface version is 4; content identity framing and
Cook-state v2 are unchanged. Offline preparation is an explicit derived-work
policy with no input ownership or mutation admission behavior.

### Stage 3: Consolidate coordinator and compatibility surfaces

- [x] Separate Cook-state encoding and loose output-store implementation from
  coordinator orchestration without changing their wire or transaction behavior.
- [x] Remove the unused save-plan `FingerprintVersion` field if the inventory
  confirms no contract consumer. Review reachability and compatibility Publish
  entry points; migrate/remove redundancy or document a distinct retained role.
- [x] Preserve per-run hash reuse, bounded dependency expansion and retained output
  limits; update memory reporting to describe the new ownership accurately.
- [x] Preserve typed operational failures and audit CLI status consumers before
  removing capture-only outcomes. Retire tests for unsupported mid-run edits,
  provider replacement and editor-resident conflicts; preserve shared API tests.

Completion condition: one production Cook flow remains, with dependency analysis,
loading/preparation and output publication having clear ownership; no dead
capture-only compatibility surface remains.

Stage 3 evidence: state v2 encode/decode moved unchanged to `CookState.cpp`;
transaction staging/rollback/manifest-last publication moved to `CookOutputStore.cpp`.
`FingerprintVersion`, resident-conflict/input-changed statuses and dead shader
capture failure branches are removed. JSON schema v1 fields remain intact;
`peakCapturedBytes` explicitly measures accounted dependency values and detached
outputs, excluding codec/compiler transient buffers and ordinary loader resources.
`BuildCookReachability` remains a standalone query; `FCookContext::Publish` remains
a lower-level direct-family API, not a second production Cook path. The same
10-target bounded native selection passed after extraction.

### Stage 4: Qualify and publish the revised contracts

- [x] Run the actual executable on representative package families, aliases,
  cycles, lazy bulk and shader-library production; verify runtime output loads.
- [x] Verify full and incremental Cook, unchanged hits, source/bulk/external/config
  changes between runs, direct/transitive invalidation, and unrelated Registry
  publication. Missing declarations must never produce an unjustified hit.
- [x] Compare stable-input outputs against the baseline fixtures, explaining any
  recipe/version changes. Record input file inventories/content hashes before
  and after fresh, hit, failure, cancellation and dry-run scenarios in tests.
- [x] Verify missing/corrupt inputs, corrupt cached outputs, cancellation and
  injected publication failure retain existing typed errors and prior-manifest
  behavior. Requalify shutdown ownership and non-Cook load/mutation consumers.
- [x] Update the authoritative asset lifecycle/catalog and tool workflow docs to
  replace sealed-generation guarantees with the selected stable-input contract.
  Record exact native/doc evidence, deletion scope, remaining limits and measured
  memory observations; commit each validated stage with Plan/Stage provenance.

Completion condition: stable-input builds preserve functional/cache/publication
behavior, Cook never writes authored inputs, and docs/tests no longer promise
protection against edits made during Cook. Mark the plan complete only after
required registered validation passes.

Stage 4 evidence (2026-09-08, macos-xcode-arm64 / MacOS-arm64-Debug-DurinEditor):

- `StandaloneCookProcessTests` invokes the actual tool on saved generic,
  Texture2D, TextureCube, VolumeTexture, Material, StaticMesh and
  EnvironmentLighting packages plus a redirect alias. Fresh, repeated and forced
  builds preserve source inventories and hashes; cooked package/companion hashes
  match across the three runs. All seven outputs load through the runtime path,
  including texture/mesh readiness and an actual external EnvironmentLighting
  lazy range read. Shader library production and post-provider-unload runtime
  loading also pass `RenderShaderCookIntegrationTests`.
- Process failures cover missing input, corrupt source and redirect cycles,
  preserving source bytes and the previous manifest where one exists. Fresh
  SIGINT cancellation returns 130 without publishing a manifest. Nested output
  symlinks cannot redirect package/cache/log writes outside the writable tree;
  overlap checks also cover direct family publication. Existing direct-family
  fixtures now mount the source during publication and the output only during
  runtime loading. Valid graph/dependency cycles retain their registered loader
  and incremental graph coverage; the real-process cycle case is a rejected
  redirect loop.
- `AssetPackageTests` preserves unchanged hits, source/bulk/external/config
  invalidation between runs, direct/transitive build-only dependencies and
  unrelated Registry publications. A contributor with its declarations removed
  rebuilds twice despite existing reusable state. Undeclared reads, cancellation
  and injected store failures preserve prior-manifest behavior. Shared private
  loader, mutation admission, modular lifetime and retirement tests remain.
- Baseline checked-in golden Cook encoding assertions remain unchanged and pass;
  forced versus incremental family output hashes match. No authored/runtime
  format or output recipe was migrated. Cook-state v2 and JSON schema v1 remain;
  ShaderBuild's native provider interface version changes from 3 to 4 because its
  source identity query replaces the capture API. This is not a persisted format
  bump or a claim of a separate old-binary comparison for every asset family.
- Final `./DevTool test affected --base 4a6acea45` resolved to all routine targets:
  **76/76 passed**, 28.28 seconds test time. Evidence:
  `Build/.agent-state/logs/20260908-070533-771756-47688-ctest.log`.
  `./DevTool build` (`all`) passed in 6.94 seconds; evidence:
  `Build/.agent-state/logs/20260908-070613-215580-49804-cmake.log`.
  GPU qualification and application smoke were not selected by this CPU process
  refactor; these results do not claim their execution.
- Relative to `4a6acea45`, `git diff --numstat -- Engine/Source` totals 848 added
  and 1045 removed lines, a net removal of 197 source lines, including the new
  output safety and offline-preparation code. The deletion is the private Cook
  object/resource universe, capture TLS, reflection freezes, repeated participant
  probes, nested provider capture and shader redirection; ordinary shared loader
  and lifetime code remains. Tests and documentation are outside this count.
- The same small process fixture reported `peakCapturedBytes` 446223 before the
  loading migration and 445290 after it. The seven-package family fixture reports
  1000082. These are accounted dependency-value/detached-output peaks, not RSS;
  transient codec/compiler buffers and ordinary loaded resources are excluded.
  No total-process memory or throughput improvement is inferred from them.
- Authoritative lifecycle/catalog/tool docs now describe stable saved inputs and
  ordinary file-backed loading. Concurrent edits remain unsupported with no
  detection guarantee. Custom contributors remain responsible for declaring
  external/native inputs and for avoiding authored writes; shared lifetime
  ownership remains necessary. Changed-document and all-plan lifecycle checks
  pass before the Stage 4 handoff commit.

## Validation and Handoff

Follow [Build and Run](../Agents/BuildAndRun.md) and
[Testing](../Agents/Testing.md) before selecting or running native validation.
Query registered tool/integration targets; do not infer target names from source
folders. Use focused tests during migration and affected native selection at
handoff. Existing Registry, dependency/state, runtime load and mutation tests
remain relevant; process integration must exercise the real executable as well
as unit fixtures. No GPU change is selected; determine additional requirements
from actual touched behavior.

Implementation is authorized. Run changed-document and all-plan lifecycle
validation with stage commits, and the registered native gates for runtime changes.

## Related Code and Contracts

- `Engine/Source/Programs/DurinAssetTool/Private/AssetToolMain.cpp`
- `Engine/Source/Runtime/Engine/Private/Asset/CookCoordinator.cpp`
- `Engine/Source/Runtime/Engine/Private/Asset/CookDependencyDiscovery.cpp`
- `Engine/Source/Runtime/Engine/Private/Asset/CookDependencies.cpp`
- `Engine/Source/Runtime/Engine/Private/Asset/AssetLiveLoadGuard.h`
- `Engine/Source/Runtime/Engine/Private/Asset/AssetRuntime.cpp`
- `Engine/Source/Runtime/Engine/Private/Asset/AssetPackageLinkerLoader.cpp`
- `Engine/Source/Runtime/Engine/Private/Asset/EngineCookContributors.cpp`
- `Engine/Source/Runtime/Engine/Public/Asset/Cook.h`
- [Asset data lifecycle](../Runtime/Assets/AssetDataLifecycle.md)
- [Asset catalog and mutation](../Runtime/Assets/AssetCatalogAndMutation.md)
