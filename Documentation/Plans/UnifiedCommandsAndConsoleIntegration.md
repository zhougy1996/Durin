# Unified Commands and Console Integration Plan

Summary: Establish module-owned Toolsets with structured calls and an editor invocation service, then integrate Console without implementing MCP endpoints.

Last reviewed: 2026-10-15

Status: Completed
Completed: 2026-10-15

## Current Status

All stages are complete. Native Toolsets and Console share structured discovery
and invocation, with bounded lifecycle handling.
At completion, 52 contract cases also passed in isolated processes against the
then-unchanged Stage 4 implementation, alongside its 244-case regression and
cross-project all-build evidence.
Core, editor-service, Console, and future-adapter contracts have authoritative
owning documents. Documentation/lifecycle validation and diff checks pass.
DurinMCP still contains only its editor-module skeleton. Completion evidence is
recorded below; physical archival is separate from this handoff.

Post-completion single-user cleanup removed the four PIE Console aliases and
`toggle_pause`, made Console dispatch domain-independent, and consolidated native
argument preparation and editor safe-point checks. Earlier stage checklists and
validation evidence describe the original delivery; current behavior is defined
by the linked Core/editor/Console contract documents.
Cleanup validation: CoreToolTests 19/19, EditorHostToolTests 23/23,
EditorOperationTests 81/81, and CoreUtilityTests 122/122 passed, along with the
cross-project `all` build. The 14 Console cases and new prepared-binding case
also passed in isolated processes. Documentation and lifecycle checks passed.


## Goal

Provide one structured Toolset definition and invocation path usable by native
callers and Console, with explicit arguments, owned results, deterministic
discovery, safe provider unregistration, and GameThread editor execution. Preserve
existing Console behavior while migrating a bounded set of PIE commands.
Future MCP integration must consume this service without implementing another
business capability registry.

## Scope and Ownership

| Owner | Responsibility |
| --- | --- |
| Core `Public/Tools` and `Private/Tools` | Values, schema subset, validation, descriptors, requests, diagnostics, results, and provider contract; no editor or protocol dependency |
| Core `Modules` | Main-thread modular-feature registration, safe-point checks, and exact-registration invocation |
| DurinEd `Public/Tools` and `Private/Tools` | Editor tool subsystem, grouped toolset catalog, invocation queue, routing, host context, result retention, and shutdown |
| Domain modules | Command descriptions and typed handlers calling existing domain services; PIE starts in DurinEd |
| MainFrame Console | Text input conversion, merged help/completion, aliases, submission, and result presentation |
| Existing native test infrastructure | Core contract/lifecycle coverage and editor-service/Console adapter coverage in suitable registered targets |

Do not add a standalone Tools module. Keep public Core types independent of
DurinEd, DObject, UI, JSON parser implementation types, transport libraries, and
MCP SDKs. Provider modules depend on the command contracts, not DurinMCP.

Out of scope: Operator interaction frameworks, general function reflection, embedded Python, MCP transports/tools/resources,
DevTool Python command migration, broad scene/material/asset command migration,
automatic conversion of all UI actions, generic asynchronous job scheduling,
persistent request deduplication, and a new Undo system.

## Selected Technical Design

### Toolsets, descriptions, and dispatch

The implemented contracts live in [Toolset Contracts](../Runtime/Core/Tools.md)
and [Editor Tool Service](../Editor/Architecture/EditorTools.md). Stage 3 revises
the original flat Command vocabulary into explicit Toolsets before Console work.
`IToolset` owns one named group, whose Tools have local names. `FToolAddress`
keeps Toolset and Tool separate; no ambiguous dotted-name splitting is allowed.
Duplicate group namespaces and duplicate local tool names fail closed. Exact
registration routing, bounded detached data, trusted host context, cooperative
cancellation, and the existing editor safe point remain authoritative.

Authors can provide explicit `FToolBinding` definitions through `FNativeToolset`
or implement `IToolset` for dynamic descriptions. `BindTool` derives supported
schemas and conversion from an explicitly bound C++ function signature. Names,
descriptions, defaults, and policy stay explicit. General DHT function reflection,
Python runtime integration, and automatic discovery are deliberately deferred;
future adapters produce these same contracts rather than another registry.

### Domain and Console integration

The selected migration is PIE status, play, stop, and explicit pause state, with
an execution-time toggle compatibility tool. Remaining legacy Console commands
stay in their original registry. The implemented syntax, compatibility messages,
merged discovery, bounded argument files, and receipt presentation are owned by
[Console Toolset Integration](../Editor/Architecture/ConsoleTools.md). Domain
handlers retain live-state validation and transaction ownership; protocol and UI
adapters share those handlers through the editor service.

## Implementation Stages

### Stage 0: Contract and migration inventory

Depends on: no implementation stages.

- [x] Inventory existing Console registrations and aliases, plus their owning
  modules and host lifetimes; identify the exact PIE migration set.
- [x] Confirm public type names and signatures, value/schema representation,
  Core serialization reuse, catalog revision mechanism, and tick safe point.
- [x] Specify Console grammar, compatibility aliases, collision behavior,
  result states, cancellation semantics, and finite resource limits.
- [x] Select registered native test targets and fixtures using repository test
  discovery; cover editor code without implicitly enabling application/GPU tests.

Completion condition: decisions and affected source/test roots are recorded
here, with no unresolved issue that changes ownership or public contracts.

### Stage 1: Core command contracts and protected provider invocation

Depends on: Stage 0.

- [x] Implement owned values, bounded schema validation, descriptors, requests,
  trusted context, results, diagnostics, and `ICommandProvider`.
- [x] Add exact-registration ModularFeature visitation and the minimal catalog
  invalidation signal required by Stage 0, using main-thread dispatch scopes and safe-point mutation checks.
- [x] Test schema/default/error paths, owned snapshot lifetime, exact identity,
  missing/unregistered entries, replacement identities, exceptions, wrong-thread
  rejection, and mutation/unload rejection during nested dispatch.
- [x] Audit consumers across every project in `Durin.dworkspace` for shared API
  changes; keep existing Console and feature-provider behavior compatible.

Completion condition: Core contracts and lifecycle tests pass, without new
editor, transport, or MCP dependencies in Core.

### Stage 2: Editor command catalog and invocation service

Depends on: Stage 1.

- [x] Register and implement `DEditorCommandSubsystem` with atomic catalog
  snapshots, duplicate diagnostics, and exact-provider routing.
- [x] Implement GameThread execution and the bounded cross-thread submission,
  completion lookup, retention, queued cancellation, and shutdown path.
- [x] Integrate the explicit tick safe point and host callback scopes; keep
  editor context resolution inside protected execution.
- [x] Test discovery ordering, catalog invalidation, wrong-thread rejection,
  queue saturation, expired results, stale queued routes, output validation,
  cancellation, and host shutdown without dangling work or deadlock.

Completion condition: native callers can discover and execute a test provider
through immediate and queued paths, with equivalent results and safe unregistration.

### Stage 3: Toolset contracts and explicit C++ authoring

Depends on: Stage 2.

- [x] Replace structured Command contracts with Tool/Toolset contracts and
  migrate every workspace consumer without changing the legacy Console API.
- [x] Publish named toolsets with local tool names and two-part call addresses;
  reject ambiguous toolset/tool ownership and preserve exact registration routing.
- [x] Provide explicit typed C++ function bindings that infer supported value
  schemas without DHT function reflection; retain manual descriptions for dynamic
  tools and future Python adapters.
- [x] Validate schema generation, grouped discovery, address collisions, native
  and queued invocation, and all existing service lifecycle contracts; build all.

Completion condition: native authors register and call a grouped toolset through
explicit bindings. No general DFunction system, embedded Python runtime, Console
adapter, or MCP endpoint is introduced. Python signature inspection is deferred
until a Python runtime bridge exists; it will publish the same owned contracts.

### Stage 4: Console adapter and PIE migration

Depends on: Stage 3.

- [x] Add typed Console conversion and bounded JSON argument-file input, merged
  help/completion, collision detection, aliases, and structured result display.
- [x] Migrate the selected PIE commands into a DurinEd-owned provider calling
  existing PIE services. Remove migrated legacy callbacks, preserving aliases.
- [x] Preserve legacy Console parsing and remaining command behavior; resolve
  the service from the owning host and detach cleanly during host shutdown.
- [x] Test typed parsing and argument files, unchanged legacy commands, help,
  completion, collision refusal, compatibility aliases, pause toggle semantics,
  and equivalent native/Console results and failures.

Completion condition: Console and native callers reach the same PIE handlers;
existing Console commands remain usable and no MCP implementation is introduced.

### Stage 5: Integration validation and contract handoff

Depends on: Stages 1 through 4.

- [x] Run selected Core, editor-service, and Console regression targets following
  [agent testing guidance](../Agents/Testing.md); record exact coverage/results.
- [x] Build affected project targets and complete an `all` build for the shared
  Core API changes following [agent build guidance](../Agents/BuildAndRun.md).
- [x] Document implemented Core contracts under Runtime/Core and editor/Console
  behavior under Editor/Architecture; update relevant module/routing entries
  and keep this plan from becoming competing contract documentation.
- [x] Validate changed documentation and the all-plan lifecycle, review the
  final diff, and commit implementation stages with exact Plan/Stage trailers.
- [x] Confirm DurinMCP remains a skeleton and document the future adapter seam:
  list/describe, structured submission, result lookup, and exposure metadata.

Completion condition: required tests/builds and documentation checks pass with
recorded evidence; every earlier stage is complete before marking this plan
Completed. Application-hosted execution is not an implicit gate.

## Required References

- [Code module ownership](../Workspace/CodeModules.md)
- [Modular features and module retirement](../Runtime/Core/ModularFeaturesAndModuleRetirement.md)
- [Subsystem lifecycle](../Runtime/Core/Subsystems.md)
- [Editor subsystems](../Editor/Architecture/EditorSubsystems.md)
- [Editor transactor](../Editor/Architecture/Transactors.md)
- [Agent documentation workflow](../Agents/Documentation.md)
- [Existing Console contract](../../Engine/Source/Runtime/Core/Public/Console/ConsoleCommand.h)
- [Existing modular-feature API](../../Engine/Source/Runtime/Core/Public/Modules/ModularFeature.h)

## Main-Thread Simplification Decision

On 2026-10-15 the selected feature lifecycle was simplified at the user's request.
All feature registry operations run on the module-control/GameThread. Reset is
immediate, idempotent unregistration, without waiting, timeout, in-flight counts,
or retirement states. A dispatch-depth scope permits nested calls but forbids
registration changes until the whole call stack returns. Module unload is
rejected before shutdown when called from feature dispatch; callers defer it to
a safe point. Modules independently stop producers and synchronize workers,
callbacks, and code lifetime before unloading.

Feature registration no longer depends on FModuleStartup or test owners.
FModuleStartup retains only startup module-name discovery for subsystem providers.
FModuleTestOwner is removed; FModuleTestHarness remains separate support for
concrete module lifecycle tests and is not referenced by ModularFeature.
The Command plan stays at Stage 2; this revision updates its completed Core
foundation and does not implement the editor service or MCP endpoints.

## Stage 0 Execution Decisions

### Inventory and migration boundary

Source inspection covers Engine, Sandbox, and RoadWeaver, the three projects in
`Durin.dworkspace`. No Console or modular-feature consumers exist in the two
project source roots. Existing Engine Console registrations are:

| Provider / lifetime | Commands | Selected action |
| --- | --- | --- |
| Core registry / process | `help` | Preserve, merge structured help in the Console adapter |
| DurinEd EditorEngine / editor Init to shutdown | `pie.play`, `pie.stop`, `pie.pause`, `pie.status`, `pie.step`, `pie.physics`, `pie.apply` | Migrate the first four; preserve the last three |
| MainFrame ConsolePanel / panel construction to destruction | `clear`, `asset.fixup_redirectors` | Preserve |
| LevelEditor SceneViewportPanel / panel construction to destruction | `viewport.camera.speed`, `viewport.camera.move`, `viewport.camera.position`, `viewport.specular-aa` | Preserve |
| Renderer resource coordinator / Start to Stop | `renderer.reload-shaders`, `renderer.retry-resources` | Preserve |

There is no separate alias registry today. Stage 4 exposes the `editor.pie` toolset with local `status`, `play`, `stop`,
and `pause` tools, using canonical Console addresses such as
`editor.pie/pause --paused <true|false>`. The existing short names become Console
aliases. `pie.pause on|off` maps to explicit state; empty input or `toggle` first
reads state and submits the opposite explicit value on GameThread. Mark that
alias non-idempotent. Preserve stopped-state failures and existing messages;
structured status additionally reports stopped/playing/paused as a field.
PIE targets the admitting editor host, never an arbitrary global editor.

Stage 4 executes short calls immediately when the service is at its dispatch safe
point, otherwise submits once and displays a receipt; a provider failure is never
retried automatically. The MainFrame host passes its editor explicitly to the
Console adapter. For deferred empty/toggle pause aliases, a native-only
`editor.pie/toggle_pause` compatibility tool reads state at execution and calls
the same pause setter. It is non-idempotent and not externally exposed; this
avoids freezing the opposite state while an invocation is still queued.

### Public contracts and catalog invalidation

Core tool values, schemas, explicit native bindings, and `IToolset` live under
`Core/Public/Tools` and `Core/Private/Tools`. DurinEd owns grouped catalogs and
GameThread invocation under its `Tools` roots. The registry publication revision
is global; unrelated features can conservatively invalidate a catalog. Feature
mutation is forbidden during synchronous visitation, so publication does not
need a concurrent retirement/retry protocol. Exact registration remains
independently authoritative at dispatch.

The Console adapter reuses `FJsonDocument` privately for argument files;
its document-bound views must be copied into owned tool values. No competing
JSON parser or unused serialization framework is introduced.

### Implemented contract authorities

The execution decisions above explain the selected migration. Maintain the
implemented rules in their owning documents rather than extending this plan as
a parallel API specification:

- [Toolset Contracts](../Runtime/Core/Tools.md): owned values, schema vocabulary,
  input/output validation, explicit native bindings, and provider lifetime.
- [Editor Tool Service](../Editor/Architecture/EditorTools.md): grouped catalogs,
  exact routes, host context, queue/result limits, cancellation, shutdown, and
  the future external/MCP adapter seam.
- [Console Toolset Integration](../Editor/Architecture/ConsoleTools.md): text
  syntax, typed/file conversion, help/completion, aliases, and presentation.
- [Modular Features and Module Shutdown](../Runtime/Core/ModularFeaturesAndModuleRetirement.md):
  main-thread safe points and module-owned unload responsibility.

### Test ownership and affected roots

Registry discovery confirms CoreConcurrencyTests and CoreUtilityTests are direct
contract targets; EditorOperationTests is a direct editor-operation contract
target and EditorHostToolTests is a direct editor-shell feature target owning
MainFrame private sources. No application-hosted or GPU gate is required here.
Add CoreToolTests (contract, tools domain, Core only; renamed in Stage 3) for command values,
schemas, provider snapshots, and cancellation. Extend CoreConcurrencyTests for
exact visitation, publication revisions, and main-thread safe-point checks. Run CoreUtilityTests
unchanged for legacy Console regression. Stages 2/3 extend EditorOperationTests and Stage 4 extends
EditorHostToolTests rather than broad module catch-all targets.

Affected implementation roots are Core Tools/Modules and
`Engine/Tests/Native/CoreTests`; documentation lives in Runtime/Core and this
plan. Editor stages use DurinEd Tools/Editor and MainFrame Panels, with PIE
handlers owned by DurinEd. No existing feature or Console consumer requires an
API migration: the ModularFeature API extension preserves existing entrypoints.

## Validation Evidence

Stage 0 source/registry inventory and Stage 1 validation completed on 2026-10-15
using the windows-msvc-x64 profile and Win64-Debug-DurinEditor preset:

- `test CoreCommandTests`: 12/12 passed on final code, including schema defaults,
  raw/normalized limits, exact 64-bit bounds, metadata/output error categories,
  owned provider snapshots, and cross-thread cooperative cancellation.
- `test CoreConcurrencyTests`: 176/176 passed on final code, including four new
  exact-invocation/publication cases and existing lifecycle coverage.
- `test CoreUtilityTests`: 122/122 passed, including unchanged legacy Console
  tests. Subsequent changes only refined new schema validation and exact-route
  allocation; the tested legacy Console implementation and contracts are unchanged.
- `build --target all`: passed for Engine, Sandbox, and RoadWeaver; the final
  incremental build also passed after the validation-category correction.
- Builds/tests used authorized execution outside the sandbox under the established
  MSVC PDB service workaround. No application-hosted or GPU execution was needed.
- Existing ModularFeature/Console symbols were searched across source and test
  roots of all three workspace projects. No consumer migration was necessary;
  existing calls compile against the extended API.

Implemented Core contracts are authoritative in
[Structured Command Contracts](../Runtime/Core/Tools.md) and
[Modular Features and Module Shutdown](../Runtime/Core/ModularFeaturesAndModuleRetirement.md).
The editor contract is now documented in
[Editor Command Service](../Editor/Architecture/EditorTools.md). Console
decisions remain here until Stage 4 implements them; MCP endpoints remain deferred.

### Main-thread revision validation

Validated on 2026-10-15 with Win64-Debug-DurinEditor: CoreConcurrencyTests
(176 cases), CoreCommandTests (12), and AssetSaveReadinessTests (3) passed.
DynamicDllUnloadQualificationTests and DynamicDllUnloadFailureQualificationTests
passed with explicit qualification admission, including physical unload/reload,
module-owned worker cleanup, failed cleanup keeping code mapped, and reentrant
unload refusal before Shutdown. The shared API `all` build passed for all three
workspace projects. Plan and changed-document validation also passed.

FModuleTestOwner and all feature retirement symbols are removed from production
and test source roots. Registration uses the public typed registry API. Registry
header dependencies on FModuleStartup/FModuleTestOwner/FModuleTestHarness are
removed; startup-name lookup and the module lifecycle test harness have separate
remaining callers. FeatureVersion, exact registration identity, publication
revision, and exception-contained synchronous visitation are preserved.

### Stage 2 validation

Validated on 2026-10-15 with windows-msvc-x64 / Win64-Debug-DurinEditor:

- `test EditorOperationTests`: 76/76 passed, including 15 new command service
  cases for discovery/collisions, native and worker parity, exact provider
  replacement, multiple editor contexts, bounded queues/catalog/results,
  reserved container capacity, cancellation, transaction deferral, and shutdown
  during availability/execution and after host GC.
- `test CoreCommandTests`: 13/13 passed, including schema-independent raw value
  preflight before copying external input.
- `test WorldTests 'FWorldSubsystemTests.*'`: 32/32 passed for existing
  subsystem lifecycle behavior.
- `build --target all`: passed for Engine, Sandbox, and RoadWeaver after final
  production changes. Builds/tests used the established authorized MSVC PDB
  service workaround outside the sandbox; no application-hosted/GPU gate applied.
- `doc plan validate --scope all`, `doc validate --scope changed`, and
  `git diff --check`: passed.

The native endpoint retains only owned data and a work/code gate, and remains
safe after host GC. Immediate execution is independent of queue slots and result
retention. Shutdown clears old results and publishes bounded cancellation receipts
for queued work. Console and MCP adapters are unchanged.

### Stage 3 validation

Validated on 2026-10-15 with windows-msvc-x64 / Win64-Debug-DurinEditor:

- `test CoreToolTests`: 18/18 passed. The renamed target retains all prior Core
  command cases and adds five cases for explicit typed schema generation, exact
  and narrow integers, defaults, strings/arrays/const references, void functions,
  invalid bindings, and group metadata/address validation.
- `test EditorOperationTests`: 81/81 passed. Five additional cases cover grouped
  ordering, identical local names in separate toolsets, duplicate local tools,
  invalid duplicate group metadata in either registration order, empty-group
  capacity, and native typed bindings through immediate and worker submission.
  Existing cancellation, stale routing, transaction deferral, and shutdown cases
  pass against the new contracts.
- `build --target all`: passed for Engine, Sandbox, and RoadWeaver. Builds/tests
  used the established authorized MSVC PDB service workaround outside the sandbox.
- Shared symbols were searched across every declared project source/test root;
  all implementation consumers were migrated. Legacy Console APIs and DurinMCP
  remain unchanged. Old Command identifiers in earlier evidence are historical.
- `test affected --explain` conservatively selects module-wide Core/DurinEd
  coverage. The focused CoreToolTests/EditorOperationTests contract targets cover
  this isolated API migration and behavior; unrelated runtime/GPU paths were not
  executed. The cross-project compilation gate remains `all`.
- `doc plan validate --scope all`, `doc validate --scope changed`, and
  `git diff --check`: passed.

Implemented contracts now live under Core/DurinEd `Tools` and the linked Toolset
documentation. Stage 4 consumes grouped addresses through Console; Python
runtime/signature discovery and general function reflection remain deferred.

### Stage 4 validation

Validated on 2026-10-15 with windows-msvc-x64 / Win64-Debug-DurinEditor:

- `test EditorHostToolTests`: 23/23 passed, including 14 new Console adapter cases
  for typed/default/enum inputs, quotes and Windows paths, exact 64-bit JSON
  conversion, duplicate/invalid/oversized files, merged help/completion and
  collisions, legacy parser parity, admitting-host routing, pause toggle timing,
  provider Busy without retry, queued completion/cancellation, adapter destruction,
  host shutdown/GC, and a real CPU-only PIE start/pause/stop cycle.
- `test EditorOperationTests`: 81/81 passed. Catalog tests now explicitly separate
  their test namespaces from the built-in PIE Toolset; the new Console cases
  validate built-in discovery and dispatch. Existing service lifetime coverage
  passes with the module-owned provider present.
- `test CoreUtilityTests`: 122/122 passed, including unchanged legacy Console
  parsing/registry regression. `test CoreToolTests`: 18/18 passed.
- `build --target all`: passed for Engine, Sandbox, and RoadWeaver after the final
  address-escaping correction. Builds/tests used the established authorized MSVC
  PDB service workaround outside the sandbox. No application-hosted or GPU
  execution was required; the PIE test uses a native host without rendering.
- Shared host-interface and Console consumers were searched across all declared
  workspace projects and migrated. The selected regression targets cover the
  changed behavior; module-wide affected selection is conservative for DurinEd.
- `doc plan validate --scope all`, `doc validate --scope changed`, and
  `git diff --check`: passed.

Lasting behavior is documented in
[Console Toolset Integration](../Editor/Architecture/ConsoleTools.md). DurinMCP
remains a skeleton, and the three unmigrated PIE commands retain their legacy
callbacks. Stage 5 remains open for final integration contract handoff.

### Stage 5 integration validation and handoff

Validated on 2026-10-15 with windows-msvc-x64 / Win64-Debug-DurinEditor against
the Stage 4 implementation commit `30dbdb823`:

- `test EditorHostToolTests 'FConsoleToolTests.*' --isolate --test-jobs 4`:
  14/14 passed, including native PIE lifecycle, queued toggles, strict conversion,
  collisions, cancellation, and shutdown/GC in fresh processes.
- `test EditorOperationTests 'FEditorToolTests.*' --isolate --test-jobs 4`:
  20/20 passed for discovery, native bindings, exact routes, cross-thread calls,
  budgets, cancellation, transaction deferral, and host lifetime.
- `test CoreToolTests --isolate --test-jobs 4`: 18/18 passed for schemas, value
  ownership, provider invocation, explicit bindings, and cancellation contracts.
- Reused the final Stage 4 whole-target results: EditorHostToolTests 23/23,
  EditorOperationTests 81/81, CoreUtilityTests 122/122, CoreToolTests 18/18
  (244 cases), and the successful `build --target all` across Engine, Sandbox,
  and RoadWeaver. `git diff 30dbdb823 --exit-code -- Engine Sandbox RoadWeaver CMake`
  confirms the implementation/build inputs are unchanged; this stage changes
  documentation only. The additional 52 isolated cases verify process independence.
- Builds/tests used the established authorized MSVC PDB service workaround
  outside the sandbox. No application-hosted or GPU execution gate applies.
- Audited DurinMCP's descriptor, module implementation, and source inventory:
  Core-only dependency, empty module implementation, no protocol/transport or
  Toolset registration. The editor contract documents the future discovery,
  External-source submission, exposure filtering, receipt/result, cancellation,
  detached ownership, and shutdown seams without adding an unused SDK/API.
- Reviewed Core, editor, and Console contracts against current public interfaces
  and adapters. Updated module ownership and routing, and replaced duplicate
  syntax/budget specifications in this plan with links to their owning contracts.
- `doc plan validate --scope all`, `doc validate --scope changed`, and
  `git diff --check`: passed. Plan lifecycle is Completed with completion date
  2026-10-15; no archive move or unrelated plan change is included.

The delivered scope includes explicit native Toolset authoring, editor discovery
and invocation, and Console/PIE integration. Generic DFunction reflection, Python
runtime/signature discovery, MCP transport/endpoints, broad domain migration,
long-operation scheduling, and a new Undo system remain outside this plan.
