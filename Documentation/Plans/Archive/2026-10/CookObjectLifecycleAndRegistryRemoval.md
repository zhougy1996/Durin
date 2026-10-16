# Cook Object Lifecycle and Registry Removal Plan

Summary: Replace the independent Cook contributor registry with object lifecycle hooks, class schema fingerprints, persisted dependency declarations, and target-aware object stripping.

Last reviewed: 2026-10-16

Status: Archived
Completed: 2026-10-16

## Current Status

All original stages and mandatory gates are complete. For the subsequent audit,
see [Follow-up review](#follow-up-review). Original implementation commits are
`213774157` (object/type foundations), `1256c2a38` (registry removal and migration)
and `ad9f75f0a` (omitted-result presentation). The original qualification tested
`ad9f75f0a`; its completion commit changed only this acceptance record. The clean starting
point was planning commit `b0b36c8e7`, code baseline
`ed82411474a27b3af248f71355fb9bc355337d5f`.

The independent registry, registration/snapshot APIs, central family dispatch and
module registration scopes are removed across Engine, Sandbox, RoadWeaver and
build/tool consumers. Native schemas are bound through reflection; preparation,
dependency events, PreSave and eligibility are owned by object types. Final
source/test/build-description scans contain no obsolete registration infrastructure.

Registry section v3 persists separate authored build-package edges. New readers
accept canonical v2 packages without resaving sources; public saves emit v3.
CookState v3 and publisher `engine-package` version 3 intentionally invalidate
all pre-migration Cook state/output reuse once. Existing family payload/recipe/DDC
identities remain explicit and unchanged where semantic inputs are unchanged.

### Final validation

All native reports are under `Build/NativeTestResults/Win64-Debug-DurinEditor/`;
raw command logs are under `Build/.agent-state/logs/`.

| Gate | Result and reproducible selection | Final log/report |
| --- | --- | --- |
| Shared API and workspace | Editor `build --target all --jobs 8`, including Engine/Sandbox/RoadWeaver | `20261016-125336-523425-35336-cmake.log` |
| Runtime variant | `build --preset Win64-Debug-DurinGame --target all --jobs 8` | `20261016-125343-535531-27972-cmake.log` |
| Full native selection | `test all --test-jobs 4 --report --timeout 600`: 108/108 targets passed | `20261016-125351-358475-38652-ctest.log`, `all.xml` |
| Per-case setup/cleanup | `test CookFunctionalTests --isolate --test-jobs 1 --report`: 25/25 cases passed | `20261016-124507-724916-35636-ctest.log`, `CookFunctionalIsolated.xml` |
| Exact package compatibility | `test PackageWriterContractTests --report`: 21/21 passed; included in final full selection | `20261016-124350-538609-22948-PackageWriterContractTests.log` |
| DHT and CLI | Python pytest over `Engine/Source/Programs/DurinHeaderTool/tests` and `Tools/DurinDevTool/tests/test_cook.py`: 254 passed, 1 skipped | `Build/NativeTestResults/cook-registry-python.xml` |
| Documentation | Changed-document validation and all-plan validation passed before closure; closure revalidates both | DevTool validation receipt in completion turn |

The Python run used `.venv/Scripts/python.exe` with an explicitly created
`Build/Pytest` parent and `--basetemp=Build/Pytest/CookRegistryFinal2`. Earlier
sandbox/default-temp attempts failed during setup and are not passing evidence.
The one skip is the existing directory-symlink case: this Windows account lacks
the symlink-creation privilege (WinError 1314). An earlier full native
run had one Vulkan process crash; its independent target run passed 11/11 and two
subsequent full selections passed 108/108. The old-v2 canonicalization failures
found during qualification were fixed and retained as regression coverage.

Baseline selections passed CookFunctionalTests 20/20 and AssetCookTests 44/44
before implementation (logs `20261016-111239-177103-5896-CookFunctionalTests.log`
and `20261016-111402-360122-10636-AssetCookTests.log`). Registry-only registration
and retirement tests were removed; object operation lifetime and teardown cases
replace their useful coverage. New cases cover inherited/no-CDO schemas, frozen
request inputs, no-construction warm hits, begin-before-wait, cancellation join,
incomplete operations, graph growth, target/ancestor exclusion, null reference
projection, omission, dependency conflicts and legacy/corrupt projection records.
Existing full suites cover family outputs, rollback, cooked-only loads without
providers and authored persistence.

### Standalone process and reuse evidence

Ran `DevTool.bat cook` against `Sandbox/Sandbox.dproject`, target `win64`, profile
`game`, output `Build/CookRegistryRemovalSmoke`, explicit roots
`/Engine/Models/Box` and `/Engine/Materials/Functions/UVTransform`. The configured
project Level and ordinary built-in roots also participated. Every process exited
successfully with both worker pools drained. The function root is omitted and has
no published runtime file; seven selected runtime packages are published.

| Run | Runtime packages / omitted | Changed / reused bytes | Cook time | DerivedDataBuild task admissions |
| --- | --- | --- | --- | --- |
| Cold output/DDC | 7 captured / 1 omitted | 6,015,299 / 0 | 11.913 s | 19 |
| DDC warm, `--no-incremental` | 7 captured / 1 omitted | 6,015,299 / 0 | 1.606 s | 10 |
| Cook warm | 7 hits / 1 omitted | 0 / 6,015,299 | 0.603 s | 0 |

Forced capture intentionally republishes files. Repeating it produced identical
SHA-256 digests for all 14 published files, including CookState and manifest.
The native family case also checks cold/recaptured output equivalence. The
object-hook fixture verifies two eligible exports begin/clear on the first run
and warm reuse adds no constructors or preparation calls. Task admissions above
are scheduler events, not algorithm execution counts; no blanket zero-DDC-rebuild
claim is made. The warm process admitted no tasks. There is no unexplained
counter/timing regression in this measured fixture; these samples are not a
cross-machine performance threshold.

Reports, raw timings and task counters are in
`Build/CookRegistryRemovalSmokeCold.json`, `Build/CookRegistryRemovalSmokeDdcWarm.json`,
`Build/CookRegistryRemovalSmokeWarmFinal.json` and
`Build/CookRegistryRemovalSamples.json`; process logs are under
`Build/CookRegistryRemovalSmoke/Logs/`. Digest evidence is
`Build/CookRegistryRemovalOutputsBeforeForcedRepeat.json` and the repeat report
`Build/CookRegistryRemovalSmokeDdcWarmRepeat.json`. All nine shipped Engine asset
files retained their original bytes and timestamps, checked against
`Build/CookRegistryRemovalSourceBefore.json`. A missing-root negative invocation
returned an input failure with zero changed bytes; its report is
`Build/CookRegistryRemovalSmokeMissingRoot.json`.

Execution host coverage is Windows/MSVC, Win64 Editor/Game. MacOS selection is
covered by direct policy/serialization tests; execution on a MacOS host and
application-hosted tests were not required or run because no host-specific code
changed. Authoritative contracts are now in Cooking, TargetPlatforms,
DerivedDataBuild, AssetPackages, PackagePersistence, ReflectionGeneration,
CodeModules and CLI documentation.

This plan succeeds the completed
[Cook Object Lifecycle Simplification plan](CookObjectLifecycleSimplification.md).
It deliberately replaces that plan's registration ownership and excluded-reference
contracts. Runtime documentation now owns the implemented contracts; this completed plan
retains migration decisions and qualification evidence.

## Follow-up review

Reviewed again on 2026-10-16 at the user's request, based on `125109843`.
The user explicitly allowed discarding previously cooked artifacts and removing
compatibility maintained only for those outputs. The review patch and this
record are committed together. The original acceptance record above remains
historical; current contracts are in the owning runtime/tool documentation.

Findings and corrections:

- **P1: stale instance projection after custom-version changes.** The output
  dependency fingerprint included local custom versions, but the reusable
  object-selection/dependency projection did not. A version change could recook
  bytes with old declarations or selection. A new regression reproduced the
  missing event/new dependency before the fix. Registered version definitions
  now participate in projection identity as well as output identity.
- **P2: class schema could capture package-scoped dependencies.** Schema hooks
  received the first encountered package identity, and declarations were cached
  for every later package containing the class. Class collection now clears the
  package identity and accepts only shared configuration/producer-version values.
  Package/file declarations belong to object dependency events. Existing native
  hooks use package identity only for diagnostics and were migrated accordingly.
- **Cleanup: retired registration-era metadata.** No independent registration
  map, handles, registration APIs, snapshots, central dispatch or module scopes
  remain. Removed the dummy family-producer word/constant and renamed remaining
  writer identity fields to `Serializer`/`SerializerVersion`. CookState v4 decodes
  only the current layout; old state safely misses. Cook report v2 uses
  `packages[].serializer`; the v1 schema and contributor-field fallback are absent.

Scope boundaries confirmed against the Epic reference:

- Object begin/completion/clear and PreSave remain type-owned. Cleanup joins all
  started operations on success, cancellation and error. Durin uses a bounded
  completion adapter rather than UE's complete cooker tick/scheduling machinery;
  mesh/material preparation remains synchronous.
- Game is the supported Cook profile. Client/server policies, cooked-editor and
  Blueprint exceptions are not implemented. PreSave graph growth is rejected;
  newly generated persistent objects are not recursively prepared during save.
- Dependency declaration uses class values and object events; Durin does not
  implement UE's complete save-harvest dependency context. All local custom
  versions conservatively affect reuse, rather than tracking only per-class
  used versions. These remain explicit adaptations, not claims of full UE parity.
- Common target/ancestor policy still controls capture, dependency reachability
  and reference projection. Same/cross-package hard references to policy-stripped
  objects become null; soft identities remain paths; empty packages are omitted.
- Registry section v2 canonical reading is retained for existing **authored**
  assets, which still use it. This is distinct from disposable CookState/output
  compatibility. Current cooked payload decoders already require current format
  versions and have no retired payload-version fallback to delete.

Validation for the reviewed patch:

- Editor and Game `all` builds passed. Final logs under
  `Build/.agent-state/logs/` are `20261016-132127-033341-26904-cmake.log`
  and `20261016-132128-794568-30996-cmake.log`.
- Full native selection `test all --test-jobs 4 --report --timeout 600` passed
  **108/108 targets**, log `20261016-131826-482101-8456-ctest.log`.
- CookFunctionalTests passed **27/27** and AssetCookTests **37/37**. New version
  projection and class-schema boundary cases also passed independently, with
  reports `Build/NativeTestResults/CookReviewVersionAfterFix.xml` and
  `Build/NativeTestResults/CookReviewClassSchema.xml`. Pre-fix failure evidence is
  `Build/NativeTestResults/CookReviewBeforeFix.xml`.
- Cook CLI tests passed **7/7**, including rejection of the retired v1 contributor
  report; report `Build/NativeTestResults/CookReviewCli.xml`.
- Standalone cold/warm Cook used the same Sandbox roots as the original smoke,
  with output `Build/CookReviewSmoke`. Cold captured seven runtime packages and
  omitted one function package. Warm hit all seven, wrote zero bytes and reused
  6,015,299 bytes. Actual state/report versions were checked as 4/2. Reports are
  `Build/CookReviewCold.json`, `Build/CookReviewWarm.json` and
  `Build/CookReviewAcceptance.json`.
- Removed **14** old task-owned published files under
  `Build/CookRegistryRemovalSmoke` after validating every absolute path and its
  saved digest. DDC and historical logs remain; the exact deletion inventory is
  `Build/CookReviewRemovedOldOutputs.txt`. All nine shipped Engine authored asset
  hashes/timestamps remained unchanged.
- Changed-document/all-plan validation and final diff checks passed. Host coverage
  remains Windows/MSVC; no new host-specific behavior or GPU logic was added.

## Goal

Remove the independent, mutable Cook contributor registry completely. Asset types
own their preparation, dependency declarations, and target eligibility through
the object/type system. The common Cooker continues to own package traversal,
incremental validation, scheduling, serialization, and transactional publication.

The final implementation must have no contributor registration API, handles,
registration mutex, registration snapshot, built-in registration scope, duplicate
registration diagnostics, or centralized asset-type preparation dispatch. Do not
replace these with another global Cook handler map or a manually maintained table.

Scope includes Engine, CoreDObject, reflection generation, package persistence,
AssetRegistry dependency projection where needed, all workspace consumers, and
tests. Source roots are those of every project in `Durin.dworkspace`, currently
Engine, Sandbox, and RoadWeaver.

Not included: live editor cooking, concurrent mutation support, new target
platforms or server profiles, new payload formats solely for API cleanup, a generic
artifact store, or replacement of DDC/provider registration mechanisms unrelated
to Cook contributor dispatch. Keep the existing `SerializeCooked` entrypoint;
merging it into `Serialize` is unnecessary for this outcome.

## UE Reference and Deliberate Adaptations

References checked on 2026-10-16:

- [Epic C++ Cooking Development Reference](https://dev.epicgames.com/documentation/unreal-engine/cplusplus-cooking-development-reference)
- [Epic UObject API](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/CoreUObject/UObject)

UE exposes object hooks for beginning, observing, and clearing cooked platform
data; save hooks handle small transformations and serialization. Its class schema
and Cook events support incremental dependency tracking. Target eligibility can
strip objects, null references to them, and omit packages without public exports.

Durin will follow those responsibility boundaries while retaining typed failures,
bounded detached products, and its common transactional publisher. It will use
existing platform/profile identities rather than introducing UE target types.
Durin's unavailable CDO states require class schema access that does not assume
every class has a default object. No client/server hooks are required until such
profiles exist. UE-specific Blueprint exceptions and chunk assignment are outside
this plan.

## Selected Architecture

### Ownership and interfaces

| Owner | Selected responsibility |
| --- | --- |
| CoreDObject | Generic lifecycle contexts, object defaults, class schema binding, dependency declaration vocabulary, save-time selection and reference projection |
| DClass and reflection generation | Immutable class-owned schema hook binding and inheritance; no separate Cook registration |
| Engine asset classes | Platform preparation, completion/error state, cleanup, eligibility overrides, native schema values, instance declarations |
| Engine Cooker | Input validation, persisted dependency evaluation, loading, scheduling, package outcomes and publication |
| AssetRegistry | Read-only authored dependency metadata projection; no asset construction or compilation |
| Developer providers | Existing texture, mesh, physics and shader algorithms and DDC contracts |

Add default object hooks with UE-aligned names/responsibilities:

- `BeginCacheForCookedPlatformData`: start or acquire a target-specific operation.
- `IsCachedCookedPlatformDataLoaded`: report completion, including failed or
  cancelled operations; a separate typed result carries success/failure.
- `ClearCachedCookedPlatformData`: release that operation's temporary state.
- `IsEditorOnly` and `NeedsLoadForTargetPlatform`: side-effect-free object
  eligibility queries, with target/profile available through a generic context.
- `OnCookEvent`: collect per-object dependencies through a bounded context;
  initially support the platform-dependency event actually needed by this plan.
- `PreSave`: a shared authored/cooked save hook receiving explicit save context.

Exact signatures are fixed in Stage 0. Defaults are no-op preparation, completed
success, safe cleanup, runtime eligibility, and no additional dependencies.
Overrides chain to `Super` where parent contributions are required. Ordinary
objects need no generic-family registration or marker callback.

Contexts must not introduce a CoreDObject dependency on Engine, Material,
ShaderBuild, or DDC. Move only generic identities, declarations and result types
needed at this boundary; keep material variant configuration and family products
in Engine. Use a narrow generic context/service boundary implemented by Engine,
not arbitrary untyped casts or a new service registry.

### Class schema without a second registry

Bind `AppendToClassSchema`-style native hooks through existing `FClassParams` /
`DClass` construction and generated/intrinsic class machinery. Hooks contribute
deterministically framed bytes under qualified class identity. Evaluate once per
class and exact target/request projection per run; include inherited contributions
once in a defined order. Run-local memoization stores evaluated values only.

This is immutable type metadata with the class's lifetime, not a separately
registered Cook route. No startup calls, handles, replacement, or CDO construction
may be needed. Cover abstract, intrinsic, and `NoClassDefaultObject` classes.

Move contributor behavior versions into this schema, preserving native recipe,
payload, compiler and relevant configuration identities. A change to custom
serialization, preparation, eligibility or declaration behavior requires an
explicit schema/version change. Reflected schema hashes alone are insufficient.
Do not hash function addresses, timestamps, run handles, or absolute paths.

### Preparation and save lifecycle

The common Engine save entrypoint must work for both the coordinator and direct
cooked saves. On a miss it loads the package, selects candidates, begins their
platform operations, waits/yields through existing task facilities, checks typed
results, invokes save hooks and captures once, then clears platform state on every
exit path. Freeze target/configuration before any callback. `PreSave` executes
once per object per logical save, before archive discovery/emission; it must not
repeat for every serialization pass. Objects created during preparation/save
hooks must be discovered to a bounded fixed point or rejected with a typed error;
never silently omit their lifecycle work.

Begin is idempotent for an identical active request. Completion is distinct from
success. Cancellation joins/retires owned work before destroying its state; no
busy polling, infinite wait, or callback after module unload. Nested captures
and different targets cannot share mutable operation state accidentally.

Keep `FScopedCookedPreparation`'s bounded detached-product storage initially,
keyed by object handle, package revision and exact target/request. Asset hooks
populate/access it through the selected context. Remove registry ownership from
it. `SerializeCooked` consumes validated values and starts no expensive work.
Material/mesh products must not replace accepted live rendering state. Texture
completion may retain its existing transient CPU-data behavior. Cooked-only
resaves validate existing target products and do not invoke authored builders.

Do not perform Cook-specific transformations in `PostLoad`. Ordinary loading,
authored saves, runtime loading and direct raw-archive failure behavior retain
their existing responsibilities. Host shutdown joins tasks and releases objects
before unloading native providers; deleting `LifetimeOwner` does not authorize
unloading modules while their objects or operations remain live.

### Dependency declarations and incremental reuse

Separate class-wide schema values from per-instance declaration records. Preserve
direct/transitive package inputs, external files, configuration values and native
producer versions, including existing declared-input access enforcement.

During authored saves, call the dependency event with `IsCooking == false` and
persist/project package build dependencies separately from runtime references.
During Cook, call it with the exact target and collect all supported declaration
kinds. Stripped build inputs, including material functions, still participate in
build invalidation; they do not become runtime roots.

Persist validated declaration descriptors plus their evaluated identities in
Cook state. Hashes alone cannot describe which external file or package to
reevaluate next run. Keep acquisition locators separate from persistent semantic
identity. The next run validates source/bulk bytes, relevant class schemas, exact
target/configuration, and every recorded dependency before authorizing reuse.
If source bytes and declaration code are unchanged, recorded instance declarations
may be reevaluated without constructing assets. Missing, unknown, incomplete,
corrupt, or non-reevaluable declarations force a miss, never a hit.

First runs and legacy authored packages without declaration metadata load as
needed. Cook must not resave/repair authored inputs. Authored metadata is an
up-front scheduling aid, not sufficient evidence by itself for cache acceptance.
Only complete, successfully captured dependency records become reusable state.
Target-dependent instance eligibility likewise requires a validated recorded
projection or loading; class defaults are not evidence for instance decisions.

Retain declaration limits, canonical ordering, duplicate/conflict validation,
cycle termination, automatic source/bulk inputs and relevant schemas. Avoid
adding every family configuration or shader identity to unrelated packages.
Runtime edges and build edges remain distinct, including soft-reference handling.

### Object stripping and reference projection

Replace `SourceOnly` registration disposition with type/object eligibility.
Material function classes and their authored-only graphs become editor-only.
Evaluate ancestor exclusion consistently for nested objects. Transient/template
rules and explicit save omissions keep their separate meanings.

| Situation | Selected Cook behavior |
| --- | --- |
| Object is editor-only or rejected for the exact target | Omit its export and excluded descendants |
| Kept object hard-references a valid, policy-stripped object | Serialize a null reference, within or across packages |
| Runtime reference is missing, corrupt, wrong-type, or otherwise invalid | Keep typed failure; do not disguise as policy stripping |
| Soft reference names an excluded target | Do not promote target into runtime output; define path preservation/loading behavior in Stage 0 |
| Field is editor-only | Strip normally; diagnostic retention must not promote excluded objects or dependencies |
| Package has no surviving public runtime exports | Omit normally, including an explicit Cook root; report a typed omitted outcome |
| Mixed package | Keep eligible exports independently of export ordering |
| Stripped object/package is a declared build input | Retain build identity without runtime publication |

The explicit-root omission outcome intentionally replaces `EmptyRuntimeRoot` as
a run failure. An all-omitted run is successful if other input checks pass; it
publishes a valid manifest/state even if there are no package outputs. Produce
shader auxiliary output only when required by retained runtime content.

Use one target selection/projection model for preparation, capture, runtime edge
traversal and incremental state. Cross-package nulling must resolve the referenced
object's policy even when the target package is omitted or unloaded. Unknown
policy requires loading or conservative miss/rejection, never speculative nulling.
Reference projection must not mutate the live pointer. Update emitted dependencies
and linker imports so stripped targets leave no dangling runtime import.

## Change Inventory

Paths below identify baseline locations; recheck ownership before implementation.

| Area | Concrete changes |
| --- | --- |
| `CoreDObject/Public/DObject/Object.h` and implementation | Object lifecycle, dependency/save events, eligibility defaults |
| `CoreDObject/Public/DObject/Class.h`, `DObjectGlobals.h`, class construction, object macros and DurinHeaderTool generation | Class-owned native schema hooks, inheritance and non-CDO support |
| CoreDObject package capture/save contexts and persistence codecs | Shared selection, stripped-reference nulling, authored dependency records and compatibility |
| AssetRegistry metadata/reference projection | Expose authored build edges without constructing assets or changing runtime edge meaning |
| `Engine/Public/Asset/Cook.h` | Remove registration API/types/errors, contributor snapshot setters/storage; retain Cook operation results and publication |
| `CookRegistry.h` | Delete completely |
| `EngineCookContributors.cpp` | Move family behavior into types; delete centralized dispatch and registration scopes; relocate unrelated formatting/save helpers |
| `EngineModule.cpp` | Remove Cook registration startup/shutdown state |
| `CookCoordinator.cpp` | Remove registry map/mutex/handles/snapshot; orchestrate lifecycle, omission results and dependency validation |
| `CookDependencyDiscovery.h/.cpp` | Remove `FResolveContributor`; consume class schema and persisted instance declarations/projections |
| `CookDependencies.h`, `CookState.*`, Cook result/manifest integration | Generic declaration split, declaration descriptors, state migration and omitted-package bookkeeping |
| `CookedPreparation.h/.cpp` | Replace callback resolution with object hooks; retain scoped products and failure cleanup |
| `PackageSerialization.h`, `CookedAsset.cpp`, `AssetPackageLinkerCapture.cpp` | Remove `CookSnapshot` plumbing; use common lifecycle and selection |
| Texture, StaticMesh and Material public classes and `*CookIdentity.cpp` | Per-type hooks and schema contributions; preserve existing builders and payload contracts |
| MaterialFunctionInterface and descendants | Editor-only eligibility and build-only dependency behavior |
| DurinAssetTool and all workspace consumers | Omitted results/JSON diagnostics, host lifetimes, obsolete API removal |
| Native tests and DHT tests | Replace injected registrations with reflected fixture overrides and schema/dependency fixtures |

## Implementation Stages

### Stage 0 contract decisions

- Generic `FObjectCookContext` owns target, package identity, cancellation,
  canonical named request values, declarations and a typed `FObjectCookResult`.
  Class hooks are static functions bound in `FClassParams`/intrinsic construction;
  `DClass::AppendCookSchema` invokes distinct inherited hooks root-to-leaf once.
  Class hooks do not call their parent explicitly; object virtual overrides do.
- Engine-specific detached products remain in the invocation-scoped Engine table.
  Asset overrides access a checked active object/context pair through an Engine
  helper, without downcasting the CoreDObject context or adding a service registry.
- Authored build-package edges use a versioned Registry section extension, read
  alongside existing hard/soft edges. New readers accept the existing section;
  old readers reject the new section explicitly. Do not change asset payloads.
- CookState gains a new version for instance declaration descriptors and selection
  evidence. Old state safely misses. Descriptor count/value/aggregate bounds reuse
  the existing Cook dependency limits; partial descriptors never authorize reuse.
- Preserve soft object paths as authored identities, while excluding proven
  stripped targets from runtime reachability. Runtime resolution of an omitted
  target returns unavailable through ordinary loading; do not invent a null soft
  path or load a stripped target solely because its path is serialized.
- Only proven Cook-policy exclusion nulls hard references. Explicit omission and
  malformed reference failures retain their separate contracts. All-omitted runs
  publish empty package selections successfully and no unnecessary shader library.
- The standalone host continues to join asset/provider work, release objects and
  then unload modules. The current Engine module already rejects dynamic reload.
- Confirmed registry selections: AssetCookTests, CookFunctionalTests,
  CoreObjectTests, CoreObjectReplacementTests, CorePropertyChangeTests,
  CorePropertyValueSnapshotTests, PackageLinkerContractTests,
  PackagePersistenceTests and PackageWriterContractTests. DHT tests are Python
  tests rather than a native registry target; discover their host launcher before
  executing them.

### Stage 0: Freeze contracts and migration fixtures

Dependencies: none. Outcome: approved-by-review concrete signatures and reproducible
baseline fixtures, with no uncertainty about persistent-state or stripping rules.

- [x] Inventory symbols across source/test roots of all `Durin.dworkspace` projects;
  identify any new production registration consumers and all applicable guidance.
- [x] Fix generic contexts, typed completion/error protocol, lifecycle order,
  inheritance and class-hook binding, including direct saves and nested captures.
- [x] Specify optional authored dependency metadata placement and old/new reader
  behavior. Select a versioned CookState extension/new version for descriptors and
  selection records; document bounds, validation and transactional publication.
- [x] Fix the soft-reference matrix, cross-package object lookup, diagnostic editor
  retention, explicit omissions and all-omitted output/auxiliary-library behavior.
- [x] Establish native/provider shutdown ordering after removal of callback owners.
- [x] Generate cold/warm/legacy-state, mixed-export, nested-object, stripped-target
  and material-function fixtures under ignored `Build/`; record schema identities,
  outputs and expected deliberate behavior changes.
- [x] Query the test registry and record exact selections for each later stage.

Gate: review signatures, wire compatibility and an executable acceptance matrix;
baseline tests characterize current behavior without declaring it the new contract.
Do not require a second user approval merely to begin already-authorized work.

### Stage 1: Add object lifecycle and class schema primitives

Dependencies: Stage 0. Outcome: type-owned extension points with no CoreDObject
dependency inversion and no reliance on CDO availability.

- [x] Add generic contexts/results, default object hooks and class schema binding.
- [x] Update DHT generation and intrinsic macros; test inherited contributions,
  stable ordering, abstract/intrinsic/no-CDO types and cross-module construction.
- [x] Add the common save lifecycle, operation cleanup and bounded candidate
  discovery; keep the existing registry only as a temporary migration adapter.
- [x] Keep migration adapters internal and assign their deletion to Stage 5;
  do not publish a second registration API.

Gate: CoreDObject/DHT focused tests, affected target builds, authored-save regression
coverage and editor/game compile coverage for the new API boundary.

### Stage 2: Migrate assets to platform lifecycle hooks

Dependencies: Stage 1. Outcome: texture, mesh and material preparation dispatches
through object overrides and works outside the coordinator.

- [x] Move texture start/completion/error handling and mesh/material preparation
  from `PrepareBuiltin` into asset-owned overrides using existing builders.
- [x] Connect exact-target detached products and deterministic cleanup, including
  cancellation, failure, nested saves, target changes and cooked-only resaves.
- [x] Add shared authored/cooked `PreSave` dispatch; keep compiler/DDC requests
  outside serialization and preserve live authored/render state.
- [x] Prove every eligible secondary/nested object is prepared, without preparing
  CDOs/templates or accepting stale products.

Gate: family Cook/serialization suites and direct-save tests pass independently
and sequentially; cold/warm DDC behavior and output equivalence hold where semantics
have not intentionally changed.

### Stage 3: Migrate dependency declarations and incremental state

Dependencies: Stages 1-2. Outcome: incremental decisions use class schemas and
reevaluable object declarations, with no registration-derived identity.

- [x] Move native recipe/version/configuration declarations into class schemas;
  preserve producer and payload identities rather than copying family-name tables.
- [x] Implement object dependency events in authored and cooked save flows;
  persist/project authored package build edges and Cook declaration descriptors.
- [x] Implement no-load warm validation and safe fallback loading for missing,
  legacy, target-dependent or incomplete records; keep declared-input enforcement.
- [x] Remove contributor resolution from dependency discovery, including the
  top-level generic fallback and nested-family special cases.
- [x] Handle cycles, duplicate/conflicting declarations, stripped build-only inputs
  and transitive invalidation through the existing bounded dependency graph.

Gate: changed source/bulk/configuration/external file/class code/producer version
invalidates exactly the affected closure; unchanged warm hits perform no asset
construction or builder work where the stored projection is complete. Verify
newly added/removed declarations, stale authored metadata and legacy-state misses.

### Stage 4: Align target stripping and package omission

Dependencies: Stage 3. Outcome: shared object policy controls exported objects,
runtime dependencies and reference bytes consistently.

- [x] Implement object/ancestor eligibility and MaterialFunction editor-only policy.
- [x] Replace registry filtering in discovery, preparation and capture with the
  common selection model; handle instance decisions without assuming CDO policy.
- [x] Null only references proven to target policy-stripped objects, including
  cross-package targets; prune linker imports/runtime edges and preserve live data.
- [x] Implement mixed, empty and all-omitted package outcomes throughout results,
  JSON diagnostics, manifests/state and auxiliary output selection.
- [x] Ensure subsequent incremental runs omit stale outputs from manifests and
  runtime discovery; use the existing store ownership/transaction rules for any
  physical cleanup, never delete unrelated files.

Gate: same/cross-package hard-reference round trips produce null, malformed
references still fail, soft references match Stage 0, editor retention does not
resurrect source-only content, and omitted roots are successful and deterministic.

### Stage 5: Delete the independent registry and migrate every consumer

Dependencies: Stages 2-4. Outcome: no production or test path needs registration.

- [x] Delete `CookRegistry.h`, registration APIs/types/errors, map/mutex/handles,
  captured registration ownership and Engine built-in registration scopes.
- [x] Delete `PrepareBuiltin` type dispatch and `CookSnapshot` propagation through
  Cook contexts and serialization options; relocate remaining unrelated helpers.
- [x] Remove Engine module registration lifetime state and temporary adapters.
- [x] Replace test callback injection with reflected object fixtures and class
  schema hooks; remove duplicate-registration/owner-retirement tests and preserve
  equivalent operation/module lifetime and cleanup coverage.
- [x] Search all workspace projects and build descriptions for obsolete symbols,
  includes, diagnostics and source-file references; migrate every consumer.

Gate: complete an `all` build for the shared API migration. Source/test scans show
zero independent Cook registry infrastructure or centralized family dispatch;
existing DClass/reflection and unrelated provider registries are explicitly allowed.

### Stage 6: Qualify compatibility and publish lasting contracts

Dependencies: Stage 5. Outcome: reproducible behavior, performance characteristics
and documentation for the final architecture.

- [x] Run the validation matrix below against final code; resolve failures and
  record exact revision, preset, selections, results and remaining limitations.
- [x] Qualify DurinAssetTool process shutdown, direct saves and workspace game/editor
  target integration; verify runtime cooked loads need no Developer services.
- [x] Update Cooking, TargetPlatforms, DerivedDataBuild, CoreDObject/save contracts,
  CodeModules and tooling documentation where their implemented behavior changes.
- [x] Run changed-document and all-plan validation; close only evidence-backed
  checklists and complete this plan after every mandatory gate passes.

Gate: no required validation remains outstanding; old registry contracts have been
replaced in authoritative documentation. Historical plans remain historical.

## Validation Matrix and Compatibility

Follow [Testing](../../../Agents/Testing.md) and
[Build And Run](../../../Agents/BuildAndRun.md); discover actual targets through the test
registry. Do not infer target names from filenames. Likely existing entrypoints
include AssetCookTests, CookFunctionalTests, MaterialFunctionTests and family
material/texture/mesh suites; Stage 0 must confirm the exact registered selections.

| Concern | Required evidence |
| --- | --- |
| Reflection/API boundary | DHT generation tests; intrinsic/no-CDO/inherited hooks; all workspace consumers compile |
| Lifecycle | Begin/query/clear order; typed failure completes; cancellation joins; direct/nested saves; exact target/configuration isolation; bounded new-object discovery |
| Family behavior | Cold/warm/forced build; texture completion; mesh collision; material variants; no accepted live-program replacement; existing unsupported targets remain unsupported |
| Dependencies | Class code/schema/recipe and instance inputs; direct/transitive/soft separation; cycles; new/removed declarations; conflicts; undeclared reads; corrupt/oversized records |
| Incremental | Warm hit without construction when eligible; output digest repair; legacy-state miss; old authored input loading without resave; unrelated packages retain reuse |
| Stripping | Mixed export order, nested exclusion, same/cross-package nulling, missing/wrong-type target failures, soft paths, retained editor fields, build-only functions, explicit/all-omitted roots |
| Runtime | Cooked load and cooked-only resave; no build provider requirement; no dangling imports or discarded-target references |
| Publication | Failure/cancellation at each stage publishes no partial state; package/companion/shader/manifest/state rollback; omitted outputs leave no manifest entry |
| Lifetime | Sequential suite cleanup, isolated cases, module/provider task shutdown and no use after operation teardown |
| Integration | Final affected native selection, CoreDObject/DHT shared-infrastructure coverage, and full `all` build including affected Engine/Sandbox/RoadWeaver targets |

Run new cases alone and affected suites sequentially to expose setup and cleanup
issues. Because CoreDObject save/reference behavior is shared infrastructure, run
the full native test selection for final qualification; preserve existing locks
and host constraints. Application-hosted and GPU runs are conditional on changed
behavior or explicit existing gates, not implicit requirements of API cleanup.
Use registry-selected CPU Cook/load tests for these semantics. Cross-host execution
is required only if host-specific code changes; otherwise compile affected target
variants available on the executing host and report unexecuted host coverage.

Keep asset payload/DDC formats and keys unchanged when semantic inputs are
unchanged. Version Cook state/dependency metadata when needed, with explicit old
reader/new reader behavior and safe misses. Stripping and omission changes require
a package-publisher/selection identity bump so pre-change cached bytes cannot be
accepted. Quantify the resulting one-time miss scope and prove subsequent warm
reuse. Do not claim no DDC rebuilds without measured provider-call/key evidence.

Store generated packages, states, logs and timing/counter samples under ignored
`Build/` or CI artifacts. Record concise acceptance evidence here with revision,
commands/selections, settings, counts and retrievable artifact locations. Compare
builder and asset-load counts for cold, DDC-warm and Cook-warm runs; investigate
unexplained regressions rather than inventing a wall-clock threshold.

## Commit and Handoff

Implementation commits update this plan's date, status and relevant checklists.
Use exact provenance trailers:

- `Plan: Documentation/Plans/Archive/2026-10/CookObjectLifecycleAndRegistryRemoval.md`
- `Stage: Stage <N>: <exact stage title above>`

The initial planning-only commit carries the Plan trailer without claiming a
completed implementation stage. Follow the repository's documentation validation
workflow; no source build or native test run is needed for the plan-only change.
