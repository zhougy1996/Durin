# Runtime Startup And Scene Activation Plan

Summary: Separate process service startup from project and scene opening, keep one responsive frame loop, and make activation, rendering readiness, and recovery explicit.

Last reviewed: 2026-10-26

Status: Active
Completed:

## Current Status

Execution authorized. Stage 1 is complete; Stages 2-5 are being integrated and
validated. The user additionally reported whole-computer slowdown throughout
Editor runtime, so Stage 6 now includes steady/background work pacing and hidden
viewport demand. Remaining stage acceptance gates stay open.
The current implemented contracts still live in [Runtime lifecycle](../Runtime/Application/RuntimeLifecycle.md),
[Asset packages](../Runtime/Assets/Packages/AssetPackages.md),
[Load preparation](../Runtime/Rendering/Pipeline/LoadPreparation.md), and
[Subsystems](../Runtime/Engine/Subsystems.md).

Review baseline: `3baf1f8b3`, Windows Debug Editor, RTX 3090/Vulkan. Diagnostic
experiments were reverted, and the Engine DLL was rebuilt from unchanged source.
Raw local evidence is under ignored `Build/EditorStartupDiagnosis/`; it is not a
portable acceptance receipt. Two 10-second, 3840x2064 Sandbox samples measured
median/P95 frame time of 9.63/14.39 ms with repeated preparation and 7.87/11.85 ms
with the old Ready shortcut temporarily restored. These are diagnostic samples,
not performance qualification or a production fix.

Stage 1 validation (Windows Debug Editor, same GPU/backend): `AssetPackageTests`
207/207 and `MaterialCookTests` 6/6 passed. New regressions cover all eight enum
underlying types, width mismatch rejection, native bool 0/1 validation, same-path
retry with GC deferred, and late failed-generation retirement after replacement.
MainFrame/LevelEditor consumers built, and Sandbox and RoadWeaver each completed
60 running ticks with successful default-Level activation. Evidence:
`Build/EditorStartupDiagnosis/stage1-packages.xml`, `stage1-material-cook.xml`,
and DevTool run logs `20261025-221403-141798-72312-DurinEditor.log` and
`20261025-221409-510793-25024-DurinEditor.log` under `Build/.agent-state/logs/`.
Validation covers this stage's exact inputs; later startup migrations require
their own behavior coverage.

Stage 2 implementation now has viewport-local presentation receipts independent
of profiling, explicit host Pending/Ready/Failed outcomes, separate interactive
restore policy/history, and initial/default-document failure propagation. The
ordinary-loop integration from Stage 3 was migrated with this interface to avoid
retaining two readiness owners. Project content and the first World now follow
the shell receipt; immutable launch namespaces remain configured before the UI
for the ShaderBuild reason documented below. Stage 3 cancellation and service
coverage remain open.
Engine `Init` no longer contains the nested Editor startup pump. Stage 4's shared
candidate-World operation is now implemented and under validation. The independent single-traversal
Renderer plumbing from Stage 5 is implemented ahead of scene-operation migration
to give that migration one preparation/draw entry point; presentation receipts
and the Stage 5 workload matrix remain under validation.

Validation so far: the Editor incremental `all` build passed for the viewport ABI
and host-loop migration; `ViewportTests` passed 116/116 for those inputs. Subsequent
history/error propagation passed five real-process startup characterizations and
`CoreFileSystemTests` (55 passed, two skipped). Reports are under
`Build/EditorStartupDiagnosis/stage2-*.xml`. These results do not establish a
first-scene presentation gate or a nonblocking Level decoder. The Debug Game
incremental `all` build also passed after the shared interface/history changes;
Game runtime behavior and Metal execution remain outstanding. The four Launch
contract targets passed in `stage2-launch-contracts.xml` for the same final inputs.

Latest Stage 2 inputs passed six real-process startup characterizations
(`stage2-coordinator-final.xml`) and four Launch contract targets
(`stage2-coordinator-contracts.xml`). Editor `all` rebuilt the changed Editor
layout and project consumers; the Game launcher built the shared content boundary.
WorldTests passed 189/189 and EditorShellTests 61/61 during this migration.
Sandbox and RoadWeaver then completed 60 running ticks each; logs
`20261025-225525-850280-40436-DurinEditor.log` and
`20261025-225536-584034-58296-DurinEditor.log` show shell presentation before
workspace/default-document readiness. RoadWeaver still spends about 545 ms in
synchronous asset loading: Stage 4/6 must remove/measure that remaining stall.

Single-traversal plumbing passed RendererSceneContractTests 88/88
(`stage5-single-traversal.xml`) and Editor/Game incremental `all` builds for the
shared render-request ABI. Sandbox completed 60 running ticks and reached
resource-ready drawing in `20261025-225920-979027-62928-DurinEditor.log`.
This proves the covered draw path works; it does not qualify the full workload
matrix or establish first-scene presentation.

Scene-operation and runtime-work checkpoint (Windows Debug/Vulkan):
`WorldTests` passed 190/190, including cancellation before activation preserving
the active World/Level; `MonaViewportTests` passed 10/10 for output generation,
viewport identity and visible display demand; `ViewportTests` passed 116/116;
`RendererSceneContractTests` passed 88/88; `ThumbnailTests` passed 74/74 including
deferred optional admission. Six process characterizations and four Launch
contract targets passed. Reports are `stage4-*.xml` and
`stage6-thumbnail-admission.xml` under `Build/EditorStartupDiagnosis/`.

Editor/Game `all` builds passed for the shared Engine/host/UI interfaces. Later
implementation-only changes rebuilt their owning targets and affected launchers.
Sandbox confirmed primary scene presentation and completed 180 running ticks in
`20261026-012447-222710-50200-DurinEditor.log`; RoadWeaver completed 60 in
`20261026-013859-820426-66380-DurinEditor.log`. The final commit guard also passed
WorldTests and the two-tick Sandbox run `20261026-014159-822272-65704-DurinEditor.log`.
The initial receipt check incorrectly observed command recording; it now brackets
native Present during RHI replay without a wait. Automated cancellation before
readiness returns failure rather than counting event-only iterations as ticks.

The short paced Sandbox sample (`steady-paced-samples.csv`) used about 0.73 of
20 logical CPU cores in its steady portion; total GPU utilization varied from
11 to 28 percent. Other desktop applications were active, and GPU utilization
was already substantial with the Engine closed. This is diagnostic evidence,
not quiet-lane median/P95/P99 qualification or proof every runtime stall is gone.
RoadWeaver's asynchronous asset phase still takes about 636 ms wall time; live
decode/object application and World registration remain atomic and need the
remaining Stage 6 work. Raw authored-project Cook compatibility, cancellation at later phases,
the full workload matrix and Metal execution are still acceptance gaps.

API refinement selected after caller review: scene opening takes one immutable
request (asset path, optional play policy and initial-view builder). The ordinary
Engine frame owns advancement and candidate rendering; clients observe one
progress snapshot, then cancel or commit. Initial editor view builders capture
camera settings and output extent by value rather than borrowing document UI.
Engine exposes typed progress; presentation text belongs to MainFrame. Candidate
publication and view construction stay protected implementation capabilities.
This removes phase-order obligations from Editor and Game without changing the
explicit readiness/presentation gates. Foreground-idle load remains under review.

API refinement validation (working tree based on `a9fcd8bc3`): Editor and Game
`all` builds passed with four compile jobs, including Sandbox and RoadWeaver
consumers. WorldTests passed 190 cases (including cancellation, progress visibility,
no view callback after cancellation, and closed admission returning one failed
handle); EditorShellTests passed 60. Launch startup characterization passed all
six cases. XML reports are `Build/EditorStartupDiagnosis/api-{world,editor-shell,startup}.xml`.
Sandbox and RoadWeaver each completed three running ticks after the primary
presentation gate in logs `20261026-021852-031979-50152-DurinEditor.log` and
`20261026-021839-484261-75700-DurinEditor.log`. The refreshed isolated cooked
Game bundle also completed three ticks and exited 0 (`api-game.stdout.log` and
`api-game.stderr.log` in its existing runtime directory). These short runs verify
startup behavior, not sustained foreground-idle performance.

Slow-phase instrumentation stays internal to asset loading and scene operations.
Timing samples above 16 ms use Debug severity, not Warning: a frame-duration
sampling threshold does not establish a loading fault. Asset, package and scene
timing messages use neutral wording; actual failure diagnostics retain their
existing severity. No new public tuning option is needed.
The Engine target rebuilt successfully with four jobs. The same Sandbox startup
completed three running ticks and exited 0 in
`20261026-025230-630260-55176-DurinEditor.log`, with Debug timing samples retained
and no Warning/Error entries. This logging-only change does not alter loading or
frame scheduling; no additional native test suite was needed.
The RoadWeaver follow-up measured a 508.83 ms nonpreemptible owner-thread asset
application and 86.46 ms World registration call; the initial-view phase took
1741.08 ms across frames. Earlier split diagnostics in
`20261026-020311-737605-66848-DurinEditor.log` measured 31.84 ms detached decode,
160.21 ms live linker application and 317.24 ms PostLoad. Loading UI now reports
an indeterminate bar and actual phase/work counts rather than a phase-derived
percentage that reaches completion before presentation. These results do not
establish the cause of persistent foreground-idle desktop lag.

Cooked Game follow-up: a fresh isolated Sandbox copy was canonical-resaved and
cooked successfully (seven packages). Before resave, strict reference discovery
rejected the source Level's obsolete `AActor::AuthoredComponents` field. The
isolated resave preserved all 21 object path/class identities and restored a Ready
reference index with nine edges. Tracked content was not modified. The current
Game executable and DLLs, standard configuration template and cooked data were
assembled under `Build/EditorStartupDiagnosis/GameBundle/Runtime/DurinGame/`.
That application passed the native BeginPlay/resource/presentation gate, completed
three running ticks and exited 0; `game-startup.stdout.log` and stderr are in that
directory. Sandbox reports its existing optional graybox pawn visual is absent
from the registry; this receipt qualifies lifecycle/readiness, not complete
gameplay visuals or raw authored-project Cook compatibility. The original
project's stale reference schema still needs its own reviewed content migration.

## Goal

Give every startup phase one owner, a concrete completion condition, and an
explicit failure path. A usable editor shell must survive a bad project or Level;
opening a Level must not depend on forced physical GC; a completed preparation
must not cause a second full view traversal every steady frame. Preserve object,
module, World, render-thread, and RHI lifetime contracts.

Initial scope includes Editor startup, Project Browser, initial/default Level,
later Level opening, Game startup, automation readiness, cancellation, and partial
shutdown. Preserve process relaunch for changing projects: in-process project
module unloading/remounting is outside this plan. Multi-document editing,
arbitrary future PSO coverage, and a general task-graph framework are also outside
scope.

## Current Flow And Structural Problems

| Boundary | Current behavior | Consequence |
|---|---|---|
| [Process runner](../../Engine/Source/Runtime/Launch/Private/ApplicationRunner.cpp) | `PreInit`, blocking `Init`, then ordinary ticks | Startup and normal execution have different completion/safe-point behavior. |
| [Launch initialization](../../Engine/Source/Runtime/Launch/Private/EngineLoop.cpp) | Project selection/publication precedes window/RHI; common services and host initialization are coupled | Project errors and browser startup are difficult to isolate from process failure. |
| [Editor initialization](../../Engine/Source/Editor/DurinEd/Private/Editor/EditorEngine.cpp) | One host-operation/GC-deferral scope covers base services, preview warmup, shell, and a nested bootstrap loop | Failed preview packages can retain their path until after default-Level loading retries it. |
| [MainFrame bootstrap](../../Engine/Source/Editor/MainFrame/Private/MainFrameModule.cpp) | UI owns workspace and synchronous default-document advancement | A named phase can still freeze the last submitted loading frame. |
| [Level document controller](../../Engine/Source/Editor/LevelEditor/Private/Documents/LevelDocumentController.cpp) | Synchronous load followed by immediate World/document mutation | Object load, registration, document publication, and render readiness have no shared operation outcome. |
| [Game startup](../../Engine/Source/Runtime/Engine/Private/Engine/GameEngine.cpp) | Authored and cooked startup advance differently; some failures leave `Init` successful; cooked preparation continues in `Tick` | Service success, gameplay success, and scene visibility are different implicit states. |
| [Viewport preparation](../../Engine/Source/Runtime/Engine/Private/Client/SceneViewport.cpp) | A Ready request is fully repumped before normal rendering | The same view is prepared twice per frame to preserve freshness. |

Verified immediate defects must be fixed independently: structured loading rejects
the existing unsigned enum representation when C++ requests signed storage, and
native bool serializers can use U8 descriptors. Failed Box warmup leaves a garbage
package registered during deferred GC; the default Level then fails on that same
path. The architecture must prevent a similar optional failure from poisoning a
required operation, but it cannot substitute for correct serialization.

## Selected Design Direction

### 1. Process Services And Project Sessions Have Different Lifetimes

Launch owns crash handling, arguments, logging, scheduler/executors, platform
application, the primary native window, presentation/RHI, rendering admission,
the minimal UI backend, host lifetime, and final unwind. Engine owns runtime asset
and compilation services. Engine content and minimal UI rendering must work
without a project. Split default-material/scene services from the minimum needed
to display the shell; preview meshes and thumbnails are not shell dependencies.

Resolve the descriptor and freeze its mount/shader/cache namespace as explicit
launch configuration before creating the minimum rendering services. This is a
refinement selected during Stage 2: ShaderBuild finalizes immutable source mounts
before the shell can compile UI shaders, so initializing an empty namespace and
rebinding it afterwards would invalidate accepted work. The selected descriptor
returned by legacy `GetCurrentProject()` is not proof of project admission.
Editor mutation/tool capabilities remain closed until host readiness.

Native project modules, schema providers, catalog admission, default materials,
project Engine/World subsystems, and the initial World are initialized after the
shell's presentation receipt. Browser startup omits that content/World work.
Namespace resolution/ownership failure is retained for the error shell using an
Engine-only configuration. Do not silently reuse cache bindings from a different
project; changing projects still requires process relaunch.

Keep one process lifecycle and one owned project-open operation. DurinEd owns
Editor orchestration; MainFrame renders snapshots and user actions. LevelEditor
owns document policy and binds a completed scene operation. Engine owns the
shared scene-load operation used by Editor and Game. Renderer owns preparation
and rendering results. Do not introduce a parallel scheduler or a second global
service registry.

### 2. One Frame Loop Advances Startup And Normal Work

After minimum host services are ready, `RunApplicationProcess` enters its only
frame loop. Remove the nested `PumpStartupFrame` loop from `DEditorEngine::Init`.
Host initialization no longer synchronously opens a project or Level.

Every frame follows this order:

1. Process native events and close/cancel requests; reject recursive advancement.
2. Deliver bounded deferred, package-load, compiler, geometry/program, and render
   result completions through their existing services.
3. Advance project/scene operations at most once using the admitted completions.
4. Run explicit World service work for candidate/stopped Worlds, including PSO
   request delivery and proxy publication. Enable gameplay simulation, timers,
   tools/scripts, PIE, and editing only for their declared capability gates.
5. Build the shell/loading/error UI and snapshot actual viewport requirements.
6. Submit eligible candidate/active scene work and UI through normal render/RHI
   framing; deliver presentation receipts on a later owner-thread frame.
7. Leave mutation scopes and run the existing GC safe point. Publish diagnostics.

Do not run arbitrary World or gameplay ticks merely to make a loader progress.
Split necessary World services from simulation where they currently share a tick.
During loading, game time does not advance and user mutation of the candidate is
disabled. Native module startup and object/serializer callbacks remain on their
required owner thread.

Use one monotonic host-frame identity for service/UI/render work. Keep simulation
and post-startup automated tick counts distinct. Preserve `--exit-after-ticks`
as completed running ticks after the requested startup outcome; loading frames
must not exhaust the smoke budget or claim success after a failed default Level.
Modal resize continuation may service a safe frame but cannot reenter project or
scene transitions or advance simulation twice.

Steady-state refinement from the user's runtime feedback: VSync alone is not a
work budget. Pace focused Editor running frames at 60 Hz, startup/error frames
at 30 Hz, unfocused frames at 10 Hz and minimized services at 5 Hz. Native events
wake waits without bypassing the next render deadline. Consume visible offscreen
display demand once per UI frame; hidden/clipped views retain their last texture
without continuously drawing full scenes. Game running presentation policy is
unchanged. This bounds work admission, not the duration of a single GPU frame or
native callback, and requires measurement alongside serialization work.

Use a shared completion time/count/byte budget instead of allowing each startup
consumer an independent full-frame budget. A nominal budget is checked between
units; it cannot preempt a module constructor, serializer, native driver call, or
one large object component. Record such units explicitly. Moving file I/O to a
worker alone is not a responsiveness guarantee.

### 3. Readiness Is Explicit Runtime State

| Milestone | Meaning and owner |
|---|---|
| `HostServicesReady` | Launch/Engine can service events, completions, and teardown. |
| `ShellPresented` | The primary shell presentation path has accepted a successful frame. |
| `ProjectReady` | Required project ownership, mounts, schemas/modules, settings, catalog, and workspace services are admitted. |
| `AssetsReady` | Required packages and selected objects passed normal restoration/validation/PostLoad; compilation/GPU readiness is separate. |
| `WorldRegistered` | A stopped candidate World/scene contains the Level and required registrations; it is not the active document. |
| `RequiredResourcesReady` | Required geometry, programs, publications, and the actual view's rendering routes are usable for one current request. |
| `DocumentCommitted` | Editor has atomically bound the candidate World, document, history, selection, and viewport state. |
| `FirstScenePresented` | A successful primary presentation consumed scene output from the committed operation's generation. |
| `GameplayReady` | Game BeginPlay succeeded and the post-BeginPlay initial scene passed its required view/presentation gate; simulation/input may start. |

These are owned operation facts, not independent global booleans or profiling
timestamps. Store operation identity, generation, phase, typed terminal outcome,
required-work status, and the original diagnostic. Derived capabilities such as
editing and simulation are centralized policy. Profiling observes transitions;
disabling Tracy must not change control flow. A frame receipt identifies its
window/viewport and operation generation; an unrelated shell or thumbnail present
cannot satisfy `FirstScenePresented`. Presentation acceptance does not promise
physical monitor scanout or require a GPU-idle wait.

Project Browser terminates at `ShellPresented`. A deliberately empty document has
an explicit successful `NoSceneConfigured` outcome; an invalid configured Level
is a failure. Document visibility remains pending while minimized or without a
drawable extent; do not fabricate a presentation receipt.

### 4. Open A Scene As An Owned, Cancellable Operation

The shared Engine operation has a small ordered phase sequence:

`Resolve -> LoadAssets -> PrepareAssetProducts -> RegisterStoppedWorld -> PrepareView -> CanCommit`

Editor policy wraps this with dirty-document resolution before admission and
`CommitDocument -> AwaitScenePresentation -> Complete` afterwards. Each phase can
report Pending, success, or a typed failure. Cancellation retires the operation
before cleanup; stale completions can only release their own results.

Use the existing async-load, compiling, cooked mesh, material-program, PSO, and
render-resource services. The operation owns package residency, candidate World
and render scene, request handles, subscriptions, output targets, and module/code
lifetime needed by callbacks. Transfer successful ownership exactly once to the
document/play session; cancel and drain owned work before releasing providers.

Build and register the candidate World without replacing the active editor World.
Retain the previous document until the candidate can commit. Keep candidate
context explicit rather than temporarily replacing `GWorld` or editor globals.
The current initial/main-World assumptions and every consumer of those accessors
must be migrated. Opening an already-active identical document is an explicit
reuse operation, not a second World claiming the same Level.

Admit candidate memory and render-owner capacity before promising preservation of
the old document. If the old and candidate scenes cannot coexist within the
budget, report the resource limit and offer an explicit close-then-open operation
after the normal dirty-document decision. Do not silently evict the old document
or exhaust the renderer's active-owner limit. Limit admitted required work to the
initial primary view; camera previews and thumbnails request their own work later.

Commit changes the document/World bindings as one short owner-thread mutation.
Only then retire the previous document and release its residency. The previous
scene output remains visible during loading where valid; after commit, show an
explicit preparing/error view until the new generation is presented. Resizing or
changing the required view invalidates the old readiness receipt.

Game uses the same asset and candidate-World stages. Run BeginPlay exactly once
after pre-play required resources are admitted, then prepare the actual initial
view again if BeginPlay spawned or changed content. BeginPlay is a side-effect
boundary, not a transaction that promises general rollback. On failure stop and
retire that play session; do not rerun BeginPlay on a partially started World.
Enable simulation/input only after the selected gameplay-ready gate. Future
streamed or gameplay-generated content remains ordinary asynchronous demand.

Game supplies its own presentation/loading policy without MainFrame, LevelEditor,
or MonaImGui dependencies. Sharing the Engine scene operation does not add Editor
modules to the Game build graph.

### 5. Resource Preparation And Drawing Share One Traversal

Replace the perpetual external `PrepareViewResources -> RenderView` preflight
pair with one Renderer pipeline invocation for each actual scene/view/output
request in a frame. It prepares required routes once, observes request ownership,
and either returns Pending/Failed or continues directly into graph authoring and
drawing with that same prepared context.

Keep PrepareOnly mode for offscreen preparation and tools. Do not retain borrowed
frame/proxy pointers for reuse on a later frame. Across frames, retain immutable
products, request identities, and bounded resource ownership only. Scene, output,
view/features, material/program, and device changes are checked against the actual
request and existing scoped invalidation mechanisms. A permanent Ready shortcut
that misses changed requirements is not an acceptable fix.

Required failures stay observable; optional previews/thumbnails do not block the
main view. The first visible-scene gate covers the requested routes, not every
future camera, material, or PSO. Cold shaders/pipelines remain asynchronous and
must not be joined on the render thread. Preserve component proxy admission and
its owner subscriptions; do not replace them with global component scans.

### 6. Logical Rollback Does Not Require Physical GC

An unsuccessful load must remove the failed generation from public package and
object-path admission, withdraw its resource registrations/subscriptions, and
make same-path retry well-defined before promising recoverability. Physical
destruction can then wait for normal GC/render fences.

Audit both package and object lookups/naming, pending-load records, resource
manager identities, weak caches, and retirement callbacks. Late destruction of
generation A must never unregister or release replacement generation B. Do not
merely ignore the duplicate-package check or collect inside a protected callback.
If an in-flight operation still owns admission, return explicit Busy/Pending
until its logical retirement completes instead of treating its garbage graph as
a resident package.

Narrow GC-deferral/host-operation scopes to actual mutation and callback regions.
No scope may span frames, wait for I/O, or cover the full startup sequence. Keep
the existing safe-point collector and code-lifetime protections. Optional preview
warmup runs after required admission or on demand, with its own failure boundary.

### 7. Failures, Project Selection, And Automation Are Policy

| Failure | Interactive Editor behavior | Game/automation behavior |
|---|---|---|
| Arguments or essential process/window/RHI services | Existing fatal diagnostic and partial unwind | Deterministic failure using the launch result contract. |
| Project descriptor, edit ownership, required modules/mounts/catalog | Keep shell and show project admission error; offer retry/Project Browser | Fail requested project startup; do not silently run a browser or another project. |
| Configured Level or required scene resources | Keep admitted project shell; retain previous document when available; show original phase/package/object cause | Fail requested scene/gameplay readiness. |
| Thumbnail, preview, optional tool | Report degraded capability; isolate failure and retry on demand | Does not satisfy or revoke an unrelated required gate. |
| Close/cancel | Stop operation admission, reject late completion, retire owned work, then unwind | Explicit cancellation result; no hidden success after a required failure. |

Retain errors across cleanup and preserve cause chains rather than replacing them
with only "Could not open default Level". Report named phases, completed/known
work, pending owners, elapsed time, and cancellation status. Unknown work remains
indeterminate; phase indices are not a truthful global percentage. A slow-step
watchdog diagnoses pending ownership; it does not kill arbitrary work or turn a
timeout into success.

Explicit project arguments win. Without them, apply a stored interactive policy
(Project Browser or last successfully admitted interactive project). Required
project failure never silently selects a different recent entry. Separate recent
history from the automatic restore target, and update both only at their declared
successful admission boundary. Automation/smoke launch intent does not update the
interactive restore target. Preserve existing process relaunch for selecting a
different project, including ownership release and tracked child process behavior.

Before native-module admission, a rejected descriptor can return to the same
process's Browser. After project-native admission begins, keep the minimal shell
and error visible, but implement Retry/Project Browser through clean relaunch.
Do not promise in-process rollback of module startup or arbitrary project code
side effects. A Level-only retry within an admitted project remains in-process.

Startup commands declare the capability/readiness they require; dispatch on that
gate, not an arbitrary 120-tick fallback. Record the requested and actual project,
document, operation outcome, and first-scene receipt in smoke results. Interactive
recovery to a usable shell must still fail an automated request for a bad scene.

## Implementation Stages

### Stage 0: Define startup ownership and readiness

- [x] Trace current Launch, Editor, Game, document, resource, and GC boundaries.
- [x] Record proposed ownership, ordered phases, readiness, failure policy, and scope.
- [x] Separate verified defects and diagnostic timings from architecture recommendations.
- [x] Validate this design document and record the Stage 0 baseline.

### Stage 1: Repair loading contracts and failure isolation

Depends on Stage 0. Outcome: existing startup can load tracked content correctly,
and a failed optional load cannot poison a required retry.

- [x] Unify enum/native-bool storage and logical-read contracts without blanket numeric coercion; cover all enum widths/signedness and native/reflected booleans.
- [x] Establish generation-safe logical retirement and same-path retry under a GC deferral scope; retain cycle/SCC and callback lifetime guarantees.
- [x] Preserve the original failure path/type and prevent preview warmup from masking it.
- [x] Validate tracked Engine meshes plus Sandbox and RoadWeaver Levels, failed/retried loads, and late retirement after replacement.

### Stage 2: Introduce explicit host readiness and project admission

Depends on Stage 1. Outcome: the shell works independently of an admitted project,
and no control decision reads profiling milestones.

- [x] Split minimum host/UI services from project-dependent paths, caches, modules, catalog, default materials, and World creation.
- [x] Add owned phase/result snapshots and generation-scoped shell presentation receipts; make MainFrame the presenter rather than the project startup owner.
- [x] Specify and implement project selection/history policies and initial catalog failure policy.
- [ ] Preserve Browser, project relaunch, ownership, module-provider ordering, and partial-init teardown.

### Stage 3: Advance startup through the single frame loop

Depends on Stage 2. Outcome: startup processes completions and GC safe points
without nested startup pumping or premature simulation.

- [x] Remove `PumpStartupFrame` and the blocking Editor bootstrap loop after migrating their consumers.
- [ ] Centralize bounded completion/service advancement and phase capability gates; audit PSO/proxy and candidate-World services normally driven by World ticks.
- [x] Narrow mutation/GC scopes; define startup, running, modal, minimized, and stopping frame semantics and counters.
- [ ] Test close/cancel/retry during every pending phase, modal reentry, minimized pending presentation, and automation tick/readiness behavior.

### Stage 4: Share scene loading and explicit document/play commit

Depends on Stage 3. Outcome: initial opening and later Level opening use the same
owned scene operation, with previous-document preservation and typed outcomes.

- [x] Introduce the Engine scene operation using existing service handles, candidate World/scene ownership, and scoped residency.
- [x] Reduce callers to one request and progress snapshot plus explicit cancel/commit; keep advancement, candidate rendering and publication internals behind Engine ownership.
- [ ] Migrate Editor initial/default/open-Level paths and all active-World consumers; retain dirty-document and transaction policy at the document boundary.
- [x] Migrate authored and cooked Game startup; separate registration, BeginPlay, initial-view preparation, and simulation/input enablement.
- [ ] Cover explicit empty documents, required failure, stale completion, commit cancellation, scene changes, and exactly-once BeginPlay/retirement.

### Stage 5: Prepare and render each view once

Depends on Stages 2 and 4. Outcome: current-view correctness no longer requires a
second complete resource-preparation traversal on steady frames.

- [x] Separate preparation observation/ownership from PrepareOnly execution mode; continue from a ready prepared frame directly into drawing.
- [x] Produce operation/generation-scoped successful scene-output and primary-composition receipts.
- [ ] Preserve pending/failure visibility, scoped invalidation, retention limits, component gates, and tools/thumbnail/hit-proxy coverage.
- [ ] Capture cold/warm, resized, camera/feature/material-changed, device-invalidated, and two-view workloads; verify one preparation traversal per actual view invocation.

### Stage 6: Bound remaining startup stalls and qualify the complete flow

Depends on Stages 1-5. Outcome: remaining nonpreemptible work is measured and
startup responsiveness/termination has explicit acceptance evidence.

- [x] Bound focused/background/minimized Editor frame admission, render offscreen views only on visible demand, and defer new optional thumbnail work during required scene loading.
- [ ] Move detached immutable package decode/validation off GameThread where safe; keep reflection registration, live construction, serializers, and publication on their required owner thread.
- [ ] Split large publication/registration units only at valid unpublished-state boundaries with explicit pins; preserve SCC completion and rollback semantics.
- [ ] Keep preview meshes, thumbnails, and optional tooling outside first-scene admission and measure their background impact.
- [ ] Update owning runtime/editor contracts and remove superseded bootstrap/readiness paths.
- [ ] Complete the acceptance matrix below and record final-input evidence and remaining platform limits.

## Acceptance And Validation

Follow [build selection](../Agents/BuildAndRun.md) and
[test selection and evidence reuse](../Agents/Testing.md). Discover current targets
before execution; existing anchors include `LaunchArgumentTests`,
`EngineFrameTimingTests`, `LaunchProcessBoundaryTests`,
`LaunchStartupCharacterizationTests`, `AssetPackageTests`,
`PackageLinkerContractTests`, `ViewportTests`, and `WorldTests`. Add behavior tests
to owning targets rather than duplicating the implementation as a test.

Required scenarios: Browser, explicit/restore project, invalid/missing/owned
project, no default Level, corrupt default Level/dependency, deferred-GC retry,
warm/cold cache, large asset closure, canceled/stale completion, optional preview
failure, resize/minimize/restore, close during every phase, startup command and
bounded-tick smoke, authored/cooked Game, BeginPlay-created initial content, and
shutdown with accepted worker/render work. Verify first-present identity and
required-scene failure without mistaking shell recovery for smoke success.

For shared API changes, search and migrate consumers across every project in
`Durin.dworkspace`, including Sandbox and RoadWeaver. Bound stage validation to
owning and affected targets. One final incremental `all` Editor build is required
because startup changes cross Launch, Engine, Core/object lifetime, editor hosts,
and rendering boundaries; a single module cannot cover that build graph. Build
the corresponding Game preset for changed Game/shared runtime paths and test its
cooked boundary. Windows Vulkan execution is required; qualify Metal on an
available macOS host before claiming cross-platform completion.

Performance evidence must record startup phase durations, shell and first-scene
presentation, cancellation latency, main-thread nonpreemptible steps, and steady
frame median/P95/P99 with identical scene/view/extent/cache/instrumentation. Timing
acceptance follows the [quiet-lane requirements](../Development/Build/NativeTestQualification.md#performance-qualification-and-concurrent-agents).
Choose numerical responsiveness targets after Stage 0 characterization; do not
claim a 2 ms completion budget bounds an uninterruptible serializer. Required
structural gates are no nested startup loop, no load retry depending on physical
GC, no false required readiness, no render-thread compile join, and one logical
preparation per actual view/frame request.
