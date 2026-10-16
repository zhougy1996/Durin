# Cook Reuse Precision and Async Preparation Plan

Summary: Separate dependency projection reuse from cooked output reuse, narrow proven dependency invalidation, and prepare Mesh/Material data asynchronously within one package while retaining the PreSave graph freeze.

Last reviewed: 2026-10-16

Status: Active
Completed:

## Current Status

Planning only. No implementation stage has started. This plan follows the
completed [registry removal plan](CookObjectLifecycleAndRegistryRemoval.md) and
its follow-up review. The implementation baseline is `5829f77d6`.

Current findings:

- Instance-computed configuration/version declarations disable projection reuse;
  that flag also disables output reuse and can propagate to dependents.
- Every registered custom version participates in every package's projection and
  output inputs. Every material configuration participates in projection settings.
- Common preparation begins all selected objects before joining operations, but
  Mesh and Material still do their expensive work synchronously inside Begin.
- Preparation precedes PreSave. Capture freezes persistent object membership
  before PreSave and verifies it before/after archive passes.

Only the plan document is authorized in this planning change. Implementation,
source migration, output cleanup, builds and native tests belong to later work.

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

- [ ] Confirm names/storage for the three reuse decisions and the rules for
  propagating genuine dependency incompleteness through direct/transitive edges.
- [ ] Inventory built-in class/policy/serializer versions, nested structs,
  constructed classes and request keys; identify incomplete fallback users.
- [ ] Audit Mesh/Material capture ownership, provider lifetime/thread affinity,
  completion delivery and cancellation granularity. Select existing task/provider
  primitives and explicit concurrent-operation/working-memory budgets.
- [ ] Establish focused fixtures for stable/changing instance values, unrelated
  versions/configurations, build-only dependencies, multiple objects/configurations,
  and prohibited PreSave mutations. Record cold/DDC-warm/Cook-warm counts.
- [ ] Choose the state schema transition and miss-reason/report policy. Update
  this plan with concrete choices before implementation depends on them.

Gate: baseline is reproducible; no unknown coverage is treated as precise; async
ownership and admission have a documented path to progress and teardown.

### Stage 1: Separate projection validation from output reuse

Dependencies: Stage 0. Outcome: loaded reevaluation can still produce an output hit.

- [ ] Introduce independent projection validation and dependency completeness state;
  stop deriving completeness from the old projection reusable flag.
- [ ] Reevaluate instance-computed values after loading, then compare full output
  inputs; propagate only genuine incompleteness to dependents.
- [ ] Preserve declaration read checks and eligibility/dependency reevaluation when
  their governing inputs change. Unsupported descriptors safely miss or fail with
  a typed reason, never reuse stale opaque values.
- [ ] Persist the selected new state semantics and remove retired cooked-state
  fallback code. Migrate affected report/CLI consumers if their contract changes.
- [ ] Verify no-load hit, loaded output hit, changed-value miss and incomplete-input
  rejection, including direct and transitive dependents and corrupt/old state.

Gate: unchanged instance values cause no preparation/output rewrite after required
reevaluation; changed values invalidate all and only justified dependents.

### Stage 2: Narrow dependency invalidation with coverage fallback

Dependencies: Stage 1. Outcome: qualified inputs stop responding to unrelated changes.

- [ ] Add type-owned current-version/constructed-class coverage, including inherited,
  nested, policy and Cook-only usage. Keep global fallback for incomplete coverage.
- [ ] Replace global version hashing for qualified types in both projection and
  output paths; validate required declarations before any no-load decision.
- [ ] Split governing request inputs from consumed output inputs. Stop putting
  material configurations into unrelated qualified packages' projection identities.
- [ ] Audit automatic package edges; narrow proven cases with tests and document
  why remaining conservative edges are required. Preserve build-only and alias edges.
- [ ] Test relevant/unrelated version bumps, newly introduced Cook-only versions,
  nested serializers, constructed classes and selection/declaration branch changes.
- [ ] Verify missing coverage retains conservative invalidation, and configuration
  changes rebuild affected material variants without forcing unrelated projection loads.

Gate: each removed global/transitive dependency has a positive invalidation test
and an unrelated-change hit test; incomplete paths retain safe fallback behavior.

### Stage 3: Add bounded package preparation operations

Dependencies: Stage 1; Stage 2 must pass before final integration. Outcome: shared
operation ownership supports real package-local overlap without lifecycle leaks.

- [ ] Implement operation state, readiness/result separation, synchronized adoption
  and provider lifetime retention using the selected existing primitives.
- [ ] Add admission and peak working-memory accounting for snapshots, active work
  and retained products. Define oversized single-operation failure explicitly.
- [ ] Preserve all-Begin-before-join behavior where admission allows; make progress
  under a one-operation budget and avoid owner-thread completion deadlocks.
- [ ] Wire cancellation to active providers and cancel/join/clear all started work
  on success, launch failure, later-object failure, capture failure and cancellation.
- [ ] Keep Texture behavior and nested/direct captures correct; no heavy work in
  Serialize, polling queries or PreSave.
- [ ] Use controlled worker barriers to prove overlap, bounded admission, failure
  completion and cleanup; timeouts fail tests rather than strand process trees.

Gate: deterministic fixtures demonstrate overlap and bounded progress; all exit
paths end tasks before destroying their state. No cross-package scheduler required.

### Stage 4: Prepare Mesh products asynchronously

Dependencies: Stage 3. Outcome: Mesh Begin captures inputs and returns while admitted
render/collision preparation runs independently.

- [ ] Split object capture from render/collision build and payload encoding; remove
  worker reads of Mesh, BodySetup and borrowed live RenderData.
- [ ] Retain immutable source/bulk inputs and exact settings/target identities;
  reuse worker-safe builders without publishing live editor resources.
- [ ] Integrate typed completion/cancellation and detached product adoption with
  common operations. Preserve pending-source-mutation rejection.
- [ ] Test render-only, simple/complex collision, source and supported fallback
  paths, cold/DDC-warm results, multiple meshes and mixed Texture/Mesh packages.
- [ ] Compare deterministic payloads/load behavior and recipe identities with the
  baseline; prove cancellation and rejection leave source/live state unchanged.

Gate: independent mesh operations overlap within budget, bytes remain equivalent
for unchanged semantics, and runtime loading does not require build providers.

### Stage 5: Prepare Material variants asynchronously

Dependencies: Stage 3; integrate with Stage 4 for mixed-package acceptance. Outcome:
Material Begin snapshots configurations and submits detached compilation work.

- [ ] Capture all required graph/function/collection/static-property data on the
  game thread; submit owned immutable compiler inputs per canonical configuration.
- [ ] Reuse/adapt build operations without editor live-program publication, retry
  queue side effects or callbacks that require an unavailable game-thread pump.
- [ ] Define cancellation/failure across variants, reserve snapshot memory before
  dispatch, and aggregate successful variants in deterministic order.
- [ ] Test parent/static overrides, function and collection dependencies, multiple
  quality/feature configurations, duplicate work, DDC hits and compiler failures.
- [ ] Prove shuffled completion order preserves bytes/identities and that cancellation
  during translation/compile ends requests before cleanup. Cover mixed Mesh/Material
  packages and unsupported targets/configurations.

Gate: expensive compilation is outside Begin/Serialize/PreSave, independent work
overlaps, and accepted live programs and authored object graphs remain unchanged.

### Stage 6: Qualify integration and publish contracts

Dependencies: Stages 1-5. Outcome: complete consumer migration and documented behavior.

- [ ] Run the acceptance matrix below and investigate unexplained regressions.
- [ ] Verify PreSave expansion, removal/reparenting and prepared-input mutation are
  still rejected appropriately; keep normal transient-save hooks functional.
- [ ] Scan all workspace projects/tools for obsolete flags, cooked-state readers,
  synchronous heavy Begin paths and accidental registry/central dispatch additions.
- [ ] Update [Cooking](../Runtime/Assets/Cooking.md) and affected owning contracts;
  keep implemented invariants there and stage/evidence records in this plan.
- [ ] Record tested revision, exact selections, reproducible settings, counts and
  raw evidence paths; close only passed gates and complete plan lifecycle validation.

Gate: every mandatory matrix row passes or an explicit scope decision is recorded
before handoff; no required validation is replaced by an unsupported performance claim.

## Acceptance Matrix and Evidence

Follow [Testing](../Agents/Testing.md), [Build and Run](../Agents/BuildAndRun.md)
and [Documentation workflow](../Agents/Documentation.md) for supported commands,
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

- `Plan: Documentation/Plans/CookReusePrecisionAndAsyncPreparation.md`
- `Stage: Stage <N>: <exact stage title above>`

The initial planning-only commit carries only the Plan trailer and claims no
implementation stage completion. Preserve the completed predecessor's historical
acceptance record. Archive this plan only after its own gates and lasting contract
updates are complete.
