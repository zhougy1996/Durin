# Cook Reuse Precision and Async Preparation Plan

Summary: Separate dependency projection reuse from cooked output reuse, narrow proven dependency invalidation, and prepare Mesh/Material data asynchronously within one package while retaining the PreSave graph freeze.

Last reviewed: 2026-10-16

Status: Archived
Completed: 2026-10-16

## Current Status

All seven stages and required gates are complete. Runtime code was qualified at
`54267e5cd`; the completion commit changes only a pre-existing test expectation
and acceptance documentation. This plan follows the
completed [registry removal plan](CookObjectLifecycleAndRegistryRemoval.md) and
its follow-up review. The implementation baseline is `5829f77d6`.

Baseline findings:

- Instance-computed configuration/version declarations disable projection reuse;
  that flag also disables output reuse and can propagate to dependents.
- Every registered custom version participates in every package's projection and
  output inputs. Every material configuration participates in projection settings.
- Common preparation begins all selected objects before joining operations, but
  Mesh and Material still do their expensive work synchronously inside Begin.
- Preparation precedes PreSave. Capture freezes persistent object membership
  before PreSave and verifies it before/after archive passes.

Stage 0 decisions (2026-10-16):

- Persist `bCanReuseWithoutLoading` on projections; evaluate current dependency
  completeness independently and let the coordinator compare output fingerprints.
- Use CookState v5, reject v4 without a compatibility reader, retain report v2.
- Qualified classes explicitly identify their exact covered class and used version
  GUIDs through class-owned hooks. Unqualified classes retain global versions;
  inherited coverage cannot silently qualify a new subclass. Start with audited
  texture leaves and mesh/material owners; packages with unaudited children retain
  fallback. Automatic runtime-import transitive edges remain conservative.
- Use existing Core tasks and detached material build operations. Default package
  admission is four operations and a 512 MiB working reservation, with a 1 GiB
  retained product bound. Admission drains earlier work on the owner thread before
  capturing another snapshot when needed. Single operations exceeding the working
  budget fail explicitly. No owner-thread completion pumping is required by workers.
- Mesh source/settings and physics capture are owner-thread work; detached build
  and encoding are worker work. Material snapshot already requires the game thread;
  its build operation accepts immutable inputs and supports cancellation and join.
- Baseline at `fa1565d39`: CookFunctionalTests 27/27, AssetCookTests 37/37.
  Reports: `Build/NativeTestResults/CookPrecisionBaseline.xml` and
  `Build/NativeTestResults/CookPrecisionAssetBaseline.xml`. Existing functional
  fixtures cover cold/warm output hits, configuration changes, stored declarations,
  class schemas, build-only inputs and forbidden graph mutation; new cases extend
  them for loaded-value hits, scoped versions and controlled async overlap.

Stage 1 validation: CookFunctionalTests 28/28 and AssetCookTests 37/37 passed.
Reports: `Build/NativeTestResults/CookPrecisionStage1.xml` and
`Build/NativeTestResults/CookPrecisionState5.xml`. Stable loaded values skip
preparation; changed values and explicit incomplete declarations recapture.
CookState v5 rejects earlier headers without a legacy reader. Shared API `all`
build and full qualification remain Stage 6 gates.

Stage 2 validation: CookFunctionalTests 29/29 passed; report
`Build/NativeTestResults/CookPrecisionStage2.xml`. Exact class coverage plus current
used GUID definitions now governs both projection and output fingerprints. Explicit
constructed-class closure contributes reflected and native schemas. An incomplete
class keeps global versions and conservative request settings. Qualified unrelated
version/configuration changes preserve output hits without constructing objects;
Cook-only version and constructed-schema changes recapture. Material owners retain
all four audited material-domain versions; unaudited expression/BodySetup/import
classes keep package fallback. Inherited/nested reflected fields remain in schema
hashes and source-recorded GUIDs remain current-version inputs. Automatic runtime
imports stay transitive: native transformations can consume imported derived state,
so there is no demonstrated narrower contract. Existing explicit direct/transitive,
build-only, alias and omission tests remain passing. No automatic edge was weakened.

Stage 3 validation: CookFunctionalTests 31/31 passed; report
`Build/NativeTestResults/CookPrecisionStage3.xml`. Controlled two-worker barriers
prove overlap; one-slot/one-reservation capture proves admission drains and makes
progress; oversized admission launches no work. Failure tests prove other started
work is canceled/joined and clears run before returning without publication. Common
operations capture owned results and publish only on the owner thread; request
cancellation is polled by the owner, not invoked by worker code. Working reservation
and retained product limits are distinct. Existing lifecycle/cancellation, Texture,
nested/direct capture and graph guards remain passing. Provider cancellation remains
cooperative; joins are not claimed to be hard timeouts.

Stage 4 validation: CookFunctionalTests 32/32 passed; report
`Build/NativeTestResults/CookPrecisionStage4.xml`. A package with render-only,
convex-hull and triangle-mesh assets has byte-identical captures at default and
single-operation budgets and leaves authored/live products unchanged. Existing
cold/DDC-warm/Cook-warm family, target rejection and cancellation cases pass.
Mesh Begin now reserves before capture and launches detached render/physics work;
physics input tasks retain immutable source values and captured mode/policy, never
BodySetup. Synchronous preparation uses the same detached work function. A compile
check caught a borrowed material-slot span; the final snapshot owns a vector.

Stage 5 validation: CookFunctionalTests 35/35 passed; report
`Build/NativeTestResults/CookPrecisionStage5.xml`. A controlled delivery hook forces
High completion ahead of Low and proves independently progressing variants. Mixed
Mesh/Material captures, multiple material owners and parent instances produce the
same bytes at default and single-owner admission. Translation game-thread producer
counts do not increase. Cancellation while a result delivery is still active joins
requests before returning empty output. Existing function, collection, selected
configuration, negative compile, cold/warm and cooked-only cases pass. PreSave tests
use separate captures to prove both revision mutation and graph growth still fail.
Prepared Mesh source/metadata/render revisions and Material parent/function revisions
are rechecked before consuming products and after capture; ordinary saves retain
existing graph checks. Shader/compiler recipes and the CLI report schema are unchanged.

Stage 6 integration review: direct package edges no longer inherit the target's
native-input incompleteness; transitive edges still do. A focused fixture proves
both behaviors and resumed reuse after complete reevaluation. Class-hook metadata
is bounded and invocation-only readers/values are discarded after collection.
A missing constructed-class schema refreshes the immutable catalog after lazy
registration; isolation exposed and now covers this case. Owner-thread metrics
measure outstanding reservation peaks independently from physical memory.

Integration qualification: CookFunctionalTests 36/36 passed as a shared process;
36/36 also passed isolated after the lazy-registration fix. Cook CLI tests passed
7/7 using the repository virtual environment; only pytest cache persistence was
blocked, not test/report output. Reports are `Build/NativeTestResults/` files
`CookPrecisionIntegration.xml`, `CookPrecisionIsolated.xml`, and
`CookPrecisionCli.xml`. Retired-symbol scan is
`Build/CookPrecisionRetiredSymbolScan.txt`.

### Final acceptance

| Gate | Result | Evidence under `Build/` |
| --- | --- | --- |
| Shared API/workspace build | Editor `all` passed, including Engine/Sandbox/RoadWeaver consumers | `.agent-state/logs/20261016-152857-708645-40976-cmake.log` |
| Non-editor consumers | Game `all` passed | `.agent-state/logs/20261016-153237-377307-40272-cmake.log` |
| Functional/isolation | 36/36 shared-process; 36/36 isolated, one scheduling slot | `NativeTestResults/CookPrecisionIntegration.xml`, `NativeTestResults/CookPrecisionIsolated.xml` |
| Full native coverage | 109/110 targets passed initially; the sole obsolete expectation was migrated, then the entire AssetPackageTests target passed 190/190 cases | `NativeTestResults/CookPrecisionFullNative.xml`, `NativeTestResults/CookPrecisionAssetPackageRecheck.xml` |
| CLI consumers | 7/7 passed, unchanged report v2 | `NativeTestResults/CookPrecisionCli.xml` |
| Standalone Cook | Cold 7 captured/1 omitted; DDC-warm forced 7 captured/1 omitted; Cook-warm 7 hits/1 omitted | `CookPrecisionCold.json`, `CookPrecisionDdcWarm.json`, `CookPrecisionWarm.json` |
| Determinism/source preservation | All 11 package/segment SHA-256 digests match across three runs; all 27 authored asset/segment hashes unchanged | `CookPrecisionSmokeEvidence.json` |
| Source scan | No retired independent-registry/legacy-producer compatibility symbols | `CookPrecisionRetiredSymbolScan.txt` |

The initial full-native batch exposed the old assertion in
`FPackageAssetTests.CookReusesDeclaredInputsAndLoadsOrdinaryPackages`: it required
a miss even after stable loaded instance values were reevaluated. The migrated test
now requires initial capture after its version bump, then a warm output hit, and
checks ordinary loaded packages are unloaded afterward. The original failed report
is retained; it is not relabeled as a clean batch. All 110 targets have current
passing coverage by combining the 109 unchanged target results with the complete
190-case recheck. No runtime implementation changed after that batch.

Standalone reproduction uses `Sandbox/Sandbox.dproject`, target `win64`, profile
`game`, fresh output `Build/CookPrecisionSmoke`, roots `/Engine/Models/Box` and
`/Engine/Materials/Functions/UVTransform`, plus configured project/built-in roots.
The second run adds `--no-incremental`; the third uses normal incremental Cook.
Changed/reused bytes are 6,015,299/0, 6,015,299/0 and 0/6,015,299 respectively.
The root owns its initially empty DDC; no shared cache was deleted. Prepared bytes,
reservation and compile counters are covered by deterministic functional assertions;
the single-slot reservation observation (1,024 bytes) is also in GoogleTest XML.
Raw reports and summary are `CookPrecisionValidationSummary.json` and
`CookPrecisionSmokeEvidence.json`. No wall-time speedup is claimed.

Retained limits: package-local overlap only; owner-thread capture and ordinary
load behavior; conservative coverage fallback and automatic transitive imports;
cooperative cancellation/join; no persistent PreSave expansion or captured-input
mutation. Without a running task scheduler, direct adapters retain synchronous
execution. General dependency-function registration, automatic config interception,
server/Blueprint cases and a cross-package scheduler remain outside this plan.

## Goal

Preserve correct incremental Cook while avoiding unnecessary output rebuilds and
overlapping independent Mesh/Material preparation inside a package. Retain object
ownership of lifecycle and dependency declarations; do not add a Cook handler
registry, centralized family dispatch, or an independent evaluator registry.

Success requires both positive reuse cases and negative invalidation cases.
Fewer rebuilds alone are not proof of correctness.

## Scope and Selected Decisions

### Independent reuse decisions

Replace the overloaded reusable/declared state with three explicit decisions;
the final field/type names are chosen in Stage 0:

| Decision | Meaning | Effect |
| --- | --- | --- |
| Projection can be revalidated without loading | Persisted selection/declaration descriptors and their governing inputs can be evaluated without constructing package objects | Enables the no-load path |
| Evaluated dependencies are complete | Current declarations cover all known inputs and use deterministic, supported identities | Permits comparing output fingerprints |
| Output can be reused | Complete current inputs, target, serializer identity and validated output match previous state | Skips preparation/capture/publication as appropriate |

An instance-computed value can require loading without making its output
uncacheable. Reload the object, rerun eligibility/dependency events, then compare
current inputs. Do not reuse an old opaque value without reevaluation. Truly
incomplete inputs continue to prohibit reuse and propagate conservatively along
the applicable build edges. Preserve declaration enforcement on actual reads.

### Dependency precision with explicit coverage

- Track current local custom versions used by class/policy/build/save operations,
  including inherited and nested native serializers and Cook-only paths. Stored
  source versions identify usage candidates; their old numeric values do not
  replace current local definitions.
- Class-owned metadata must expose versions affecting eligibility, dependency
  events or PreSave before attempting projection reuse. Save-time discovery alone
  cannot authorize an earlier incremental skip.
- Maintain explicit, reviewed coverage for a type and its relevant operations.
  An empty declaration list is not evidence of complete coverage. Unknown types
  and incomplete native serializers retain the global-version fallback. Coverage
  belongs to reflected type metadata/hooks, not a separate dispatch registry.
- Record fixed-kind named request/configuration descriptors and evaluate their
  current values through the existing request/type context. Arbitrary instance
  computations may continue to require loading. General named callback dispatch
  and UE-style dependency-function registration are outside this plan.
- Distinguish selection/declaration inputs from output inputs. A key that can
  change which dependencies are declared must invalidate the projection even if
  the previous declaration did not consume its resulting value. Target, profile
  and retention semantics remain mandatory where they govern selection/save.
- Preserve build-only assets and class schemas even when their objects are
  stripped from runtime output. Record explicitly constructed classes not visible
  in source inspection when their schema affects the result.
- Audit automatic transitive package edges separately from explicit direct and
  transitive declarations. Narrow only edges with a demonstrated byte/recipe
  contract; retain conservative edges elsewhere. Do not replace source identities
  with cooked identities or remove alias/redirect dependencies casually.

Native behavior changes still require a deliberate relevant version/schema
change. This plan does not infer native code changes from source or binary hashes.

### Package-local asynchronous preparation

Retain the existing Begin/IsCached/Clear lifecycle and scoped detached products.
Begin captures immutable inputs on the owner thread and starts/acquires work;
queries report readiness without doing expensive work; save consumes completed
products; Clear releases operation state after all work has ended.

- Mesh: capture source/settings, material-slot metadata, collision inputs/policy
  and target before dispatch. Reuse worker-safe mesh construction and physics
  capture/cook boundaries. Copy or retain immutable data for paths that currently
  borrow live RenderData. Background encoding must not read the asset or BodySetup.
- Material: capture each requested configuration, static properties, function and
  collection inputs on the game thread. Reuse/adapt `FMaterialBuildOperation` for
  detached compilation, cancellation and ownership. Do not route Cook through an
  editor publication path that installs accepted live programs. Aggregate variants
  in canonical configuration order, independent of completion order.
- Common preparation: preserve begin-all-before-wait behavior. Bound admission
  and working memory, including queued snapshots, active intermediates and retained
  products; the final payload limit alone is insufficient. Admission must make
  progress without waiting for a completion that only the blocked owner can process.
- Separate result readiness from full request termination. Successful serialization
  requires ready products; releasing state requires all callbacks/tasks to end.
  Failure and cancellation are terminal completion outcomes, not perpetual pending.
- Workers own their input/result lifetime, not raw asset/context pointers. Define
  synchronized result publication and owner-thread adoption. No DObject traversal,
  reflection loading or mutation on workers. Keep providers resident until work ends.
- Cancel and join every started operation on every exit path. A deadline may report
  timeout but cannot detach work still referencing capture state. Cancellation must
  reach underlying providers; do not describe a blocking join as a hard timeout.

The first implementation needs no cross-package scheduler. Synchronous cache-hit
or trivial fast paths remain valid. Cross-package overlap, multiple simultaneous
targets on one object and a full nonblocking cooker state machine are deferred.

### Restrictions retained

- PreSave cannot add/delete persistent exports, change their ownership, or generate
  packages. Keep the graph checks through harvest/write for authored and cooked
  capture. Do not weaken normal save behavior as a side effect of this refactor.
- PreSave may adjust supported transient save state, but must not mutate authored
  inputs, accepted live render state, or build inputs already captured for async
  preparation. Membership equality alone does not prove those inputs unchanged;
  validate existing revision/identity coverage and add targeted guards where needed.
- Preparation and serialization cannot introduce undeclared build inputs. Heavy
  construction/DDC work stays outside serialization and PreSave.
- Keep exact target/profile/configuration identity, source stability, object/product
  budgets, strict missing/wrong-type reference errors and existing stripping rules.
- Keep the supported Game profile. Server/Blueprint special cases and arbitrary
  PreSave graph expansion are outside this plan. Expansion would require dependency
  and class-schema recollection, preparation of new objects and package closure
  updates before a later freeze; deleting the current guard is not sufficient.

### State and compatibility

Version persisted CookState/dependency descriptors when their interpretation
changes. Reject incompatible state for reuse and perform a cold rebuild; do not
retain a legacy migration reader solely to preserve cooked outputs. Use one
coherent new schema per migration rather than parallel old/new runtime paths.

Existing authored assets are still inputs. Do not remove authored package readers
because old cooked outputs are disposable. Preserve payload/DDC contracts unless
semantic inputs or encoding actually change; scheduling alone must not change
recipe identities. Delete obsolete cooked state/output compatibility only when
its lack of authored consumers is demonstrated. Any cleanup stays within verified,
task-owned output paths and never deletes shared DDC or source assets by default.

## Change Inventory

Paths below are relative to `Engine/Source/Runtime/` unless stated otherwise.
Confirm exact callers and test selections before editing.

| Area | Expected changes |
| --- | --- |
| `Engine/Private/Asset/CookDependencyDiscovery.h/.cpp` | Split projection/completeness/output eligibility; revalidate loaded values; scoped versions/configuration and qualified edge precision |
| `Engine/Public/Asset/CookDependencies.h`, `Engine/Private/Asset/CookDependencies.cpp` | Deterministic descriptors, coverage and explicit direct/transitive propagation |
| `Engine/Private/Asset/CookState.cpp`, coordinator and report consumers | Persist new semantics, validate state, expose useful miss reasons without treating projection loads as output misses |
| `CoreDObject/Public/DObject/CookLifecycle.h`, class/reflection bindings if needed | Type-owned version/constructed-class coverage and request dependencies; migrate all callers if shared API changes |
| `Engine/Private/Asset/CookedPreparation.h/.cpp` | Owned operations, admission/memory accounting, readiness, cancellation/join and safe result adoption |
| StaticMesh `*Cook*`, build and physics capture helpers | Detached request capture and asynchronous render/collision product construction |
| Materials `*Cook*`, snapshot and `MaterialBuildOperation.*` | Owner-thread capture, detached async variants and deterministic family encoding |
| `CoreDObject/Private/DObject/PackageCapture.cpp` | Preserve graph freeze; targeted checks for prohibited PreSave input changes, not expansion support |
| Engine tests, `Tools/DurinDevTool` tests/schemas if reports change | Incremental precision, failure cleanup, concurrency, state rejection and CLI consumer migration |

Search shared symbols across Engine, Sandbox and RoadWeaver source/test roots
declared by `Durin.dworkspace`; migrate all consumers. Existing build managers may
provide primitives, but their editor publication/acceptance semantics are not Cook
semantics and must not be copied wholesale.

## Implementation Stages

### Stage 0: Freeze contracts and baseline fixtures

Dependencies: none. Outcome: explicit coverage and operation contracts with a
reproducible baseline before changing reuse behavior.

- [x] Confirm names/storage for the three reuse decisions and the rules for
  propagating genuine dependency incompleteness through direct/transitive edges.
- [x] Inventory built-in class/policy/serializer versions, nested structs,
  constructed classes and request keys; identify incomplete fallback users.
- [x] Audit Mesh/Material capture ownership, provider lifetime/thread affinity,
  completion delivery and cancellation granularity. Select existing task/provider
  primitives and explicit concurrent-operation/working-memory budgets.
- [x] Establish focused fixtures for stable/changing instance values, unrelated
  versions/configurations, build-only dependencies, multiple objects/configurations,
  and prohibited PreSave mutations. Record cold/DDC-warm/Cook-warm counts.
- [x] Choose the state schema transition and miss-reason/report policy. Update
  this plan with concrete choices before implementation depends on them.

Gate: baseline is reproducible; no unknown coverage is treated as precise; async
ownership and admission have a documented path to progress and teardown.

### Stage 1: Separate projection validation from output reuse

Dependencies: Stage 0. Outcome: loaded reevaluation can still produce an output hit.

- [x] Introduce independent projection validation and dependency completeness state;
  stop deriving completeness from the old projection reusable flag.
- [x] Reevaluate instance-computed values after loading, then compare full output
  inputs; propagate only genuine incompleteness to dependents.
- [x] Preserve declaration read checks and eligibility/dependency reevaluation when
  their governing inputs change. Unsupported descriptors safely miss or fail with
  a typed reason, never reuse stale opaque values.
- [x] Persist the selected new state semantics and remove retired cooked-state
  fallback code. Migrate affected report/CLI consumers if their contract changes.
- [x] Verify no-load hit, loaded output hit, changed-value miss and incomplete-input
  rejection, including direct and transitive dependents and corrupt/old state.

Gate: unchanged instance values cause no preparation/output rewrite after required
reevaluation; changed values invalidate all and only justified dependents.

### Stage 2: Narrow dependency invalidation with coverage fallback

Dependencies: Stage 1. Outcome: qualified inputs stop responding to unrelated changes.

- [x] Add type-owned current-version/constructed-class coverage, including inherited,
  nested, policy and Cook-only usage. Keep global fallback for incomplete coverage.
- [x] Replace global version hashing for qualified types in both projection and
  output paths; validate required declarations before any no-load decision.
- [x] Split governing request inputs from consumed output inputs. Stop putting
  material configurations into unrelated qualified packages' projection identities.
- [x] Audit automatic package edges; narrow proven cases with tests and document
  why remaining conservative edges are required. Preserve build-only and alias edges.
- [x] Test relevant/unrelated version bumps, newly introduced Cook-only versions,
  nested serializers, constructed classes and selection/declaration branch changes.
- [x] Verify missing coverage retains conservative invalidation, and configuration
  changes rebuild affected material variants without forcing unrelated projection loads.

Gate: each removed global/transitive dependency has a positive invalidation test
and an unrelated-change hit test; incomplete paths retain safe fallback behavior.

### Stage 3: Add bounded package preparation operations

Dependencies: Stage 1; Stage 2 must pass before final integration. Outcome: shared
operation ownership supports real package-local overlap without lifecycle leaks.

- [x] Implement operation state, readiness/result separation, synchronized adoption
  and provider lifetime retention using the selected existing primitives.
- [x] Add admission and peak working-memory accounting for snapshots, active work
  and retained products. Define oversized single-operation failure explicitly.
- [x] Preserve all-Begin-before-join behavior where admission allows; make progress
  under a one-operation budget and avoid owner-thread completion deadlocks.
- [x] Wire cancellation to active providers and cancel/join/clear all started work
  on success, launch failure, later-object failure, capture failure and cancellation.
- [x] Keep Texture behavior and nested/direct captures correct; no heavy work in
  Serialize, polling queries or PreSave.
- [x] Use controlled worker barriers to prove overlap, bounded admission, failure
  completion and cleanup; timeouts fail tests rather than strand process trees.

Gate: deterministic fixtures demonstrate overlap and bounded progress; all exit
paths end tasks before destroying their state. No cross-package scheduler required.

### Stage 4: Prepare Mesh products asynchronously

Dependencies: Stage 3. Outcome: Mesh Begin captures inputs and returns while admitted
render/collision preparation runs independently.

- [x] Split object capture from render/collision build and payload encoding; remove
  worker reads of Mesh, BodySetup and borrowed live RenderData.
- [x] Retain immutable source/bulk inputs and exact settings/target identities;
  reuse worker-safe builders without publishing live editor resources.
- [x] Integrate typed completion/cancellation and detached product adoption with
  common operations. Preserve pending-source-mutation rejection.
- [x] Test render-only, simple/complex collision, source and supported fallback
  paths, cold/DDC-warm results, multiple meshes and mixed Texture/Mesh packages.
- [x] Compare deterministic payloads/load behavior and recipe identities with the
  baseline; prove cancellation and rejection leave source/live state unchanged.

Gate: independent mesh operations overlap within budget, bytes remain equivalent
for unchanged semantics, and runtime loading does not require build providers.

### Stage 5: Prepare Material variants asynchronously

Dependencies: Stage 3; integrate with Stage 4 for mixed-package acceptance. Outcome:
Material Begin snapshots configurations and submits detached compilation work.

- [x] Capture all required graph/function/collection/static-property data on the
  game thread; submit owned immutable compiler inputs per canonical configuration.
- [x] Reuse/adapt build operations without editor live-program publication, retry
  queue side effects or callbacks that require an unavailable game-thread pump.
- [x] Define cancellation/failure across variants, reserve snapshot memory before
  dispatch, and aggregate successful variants in deterministic order.
- [x] Test parent/static overrides, function and collection dependencies, multiple
  quality/feature configurations, duplicate work, DDC hits and compiler failures.
- [x] Prove shuffled completion order preserves bytes/identities and that cancellation
  during translation/compile ends requests before cleanup. Cover mixed Mesh/Material
  packages and unsupported targets/configurations.

Gate: expensive compilation is outside Begin/Serialize/PreSave, independent work
overlaps, and accepted live programs and authored object graphs remain unchanged.

### Stage 6: Qualify integration and publish contracts

Dependencies: Stages 1-5. Outcome: complete consumer migration and documented behavior.

- [x] Run the acceptance matrix below and investigate unexplained regressions.
- [x] Verify PreSave expansion, removal/reparenting and prepared-input mutation are
  still rejected appropriately; keep normal transient-save hooks functional.
- [x] Scan all workspace projects/tools for obsolete flags, cooked-state readers,
  synchronous heavy Begin paths and accidental registry/central dispatch additions.
- [x] Update [Cooking](../../../Runtime/Assets/Build/Cooking.md) and affected owning contracts;
  keep implemented invariants there and stage/evidence records in this plan.
- [x] Record tested revision, exact selections, reproducible settings, counts and
  raw evidence paths; close only passed gates and complete plan lifecycle validation.

Gate: every mandatory matrix row passes or an explicit scope decision is recorded
before handoff; no required validation is replaced by an unsupported performance claim.

## Acceptance Matrix and Evidence

Follow [Testing](../../../Agents/Testing.md), [Build and Run](../../../Agents/BuildAndRun.md)
and [Documentation workflow](../../../Agents/Documentation.md) for supported commands,
test selection, build serialization and receipts. Discover exact test selectors
from the current registry rather than copying historical case counts.

| Area | Required evidence |
| --- | --- |
| Incremental state | No-load hit; loaded-value output hit; relevant-value miss; incomplete-input propagation; old/corrupt descriptor rejection; output integrity validation |
| Precision | Relevant/unrelated custom versions and request keys; policy branch changes; inherited/nested/Cook-only versions; constructed classes; direct/transitive/build-only/alias edges |
| Scheduling | Controlled overlap without wall-clock guesses; one-slot progress; concurrency/memory cap enforcement; deterministic order; mixed families; no cross-package requirement |
| Failure lifetime | Cancellation before dispatch/during build/after readiness; launch/provider/compiler failure; save failure; all tasks/callbacks ended and every started operation cleared |
| Save semantics | Once-per-selected-object PreSave; retained graph freeze; no stale prepared inputs; unchanged authored/live state; existing stripping/omission/reference errors |
| Asset families | Focused AssetCook/CookFunctional and current Mesh/Physics/Material/Texture suites; direct capture/save and headless CPU fixtures; unchanged cooked-load behavior |
| Workspace | Affected Engine/Sandbox/RoadWeaver consumers validated; required `all` build for shared Engine API migrations; full native qualification at integration |
| Tooling/docs | CLI/report/schema tests if touched; changed-document and all-plan validation; source scans confirming no independent Cook registry |

Collect cold, DDC-warm and Cook-warm runs with identical roots, target and material
configurations. Record projection loads, output hits/captures, builder invocations,
in-flight counts, peak accounted memory and output hashes. Keep DDC and Cook-output
reuse distinct. Stable loaded-value cases must have zero output captures; qualified
unrelated-change cases must retain the expected hits. Async acceptance requires
demonstrated overlapping work when budget permits, not a universal timing ratio.

Use matched release/debug settings and repeat timings only when making a timing
claim. Keep raw logs/reports/timing samples under ignored `Build/`; commit concise
acceptance summaries and remaining gates here. Do not run builds/native tests for
the initial documentation-only planning change.

## UE References and Deliberate Limits

- [Cooking development reference](https://dev.epicgames.com/documentation/unreal-engine/cplusplus-cooking-development-reference): object-owned cached transforms, dependency declarations, native schema/version responsibility, and harvest/write separation.
- [UObject PreSave](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/CoreUObject/UObject/PreSave): objects created inside PreSave do not automatically receive PreSave.

Durin retains a stronger pre-save persistent graph freeze and package-local waits.
This plan aligns ownership and input semantics without claiming UE's full scheduler,
automatic config instrumentation or dynamic save dependency harvesting. Those are
separate capabilities and do not justify weakening current correctness checks.

## Commit and Handoff

Implementation commits update this plan's date, current status and evidence-backed
checklists. Use exact provenance trailers:

- `Plan: Documentation/Plans/Archive/2026-10/CookReusePrecisionAndAsyncPreparation.md`
- `Stage: Stage <N>: <exact stage title above>`

The initial planning-only commit carries only the Plan trailer and claims no
implementation stage completion. Preserve the completed predecessor's historical
acceptance record. Archive this plan only after its own gates and lasting contract
updates are complete.
