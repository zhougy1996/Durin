# Editor Agent Toolset Expansion Plan

Summary: Extend editor Toolsets beyond PIE with inspection, reversible authoring, explicit persistence, and material graph operations through existing domain services.

Last reviewed: 2026-10-17

Status: Active
Completed:

## Current Status

Planning only; no new tools are implemented by this document. The inspected
baseline is 3e7ebac85. Production exposure currently consists of editor.pie
status/play/pause/stop. The existing MCP adapter already discovers externally
exposed Toolsets; new domains should not require domain-specific MCP handlers.

Recommended first implementation: Stage 0 and Stage 1, then Stage 2 to establish
an inspect/edit/undo loop. Later stages build scene and asset authoring on that
foundation. Compile, import, screenshots, and automatic function exposure remain
separate follow-ups with explicit prerequisites below.

## Goal

Let an Agent determine the actual editor state, find assets and scene objects,
perform bounded edits, inspect their effects, undo a specific change, and save an
explicit target. Reuse the same tools from Console and MCP, with structured
results and diagnostics. Keep provider implementations in their domain modules;
DurinMCP continues to own only protocol adaptation.

## Verified Reuse and Gaps

| Capability | Existing implementation | Remaining work and priority |
| --- | --- | --- |
| Project/editor state | DEditorEngine world, PIE and subsystem state | Owned context snapshot and explicit host identity; first batch |
| Documents | FWorkspaceManager document enumeration/activation and IWorkspace save policy | MainFrame owns the host manager; provide a narrow host-scoped access path, not a widget pointer; first batch reads |
| Logs | FLogger::ReadRecords, sequence bounds and eviction reporting | Bounded filters and result conversion; first batch |
| Asset metadata/references | AssetRegistry catalog/reference snapshots and exact path resolution | Query DTOs, filters, revision checks; put editor-facing tools in AssetTools; first batch |
| Scene/selection | DWorld/DLevel actor enumeration and private FLevelEditorContext selection | Headless scene queries plus a narrow LevelEditor context bridge; first batch reads |
| Properties | Reflection metadata, FPropertyEditTarget and FPropertyEditSession | Typed value conversion and an explicit supported-property policy; first authoring batch |
| Undo/Redo | DTransactor expected-ID Undo/Redo and package revision tracking | Tool descriptors and stable wire IDs; first authoring batch |
| Scene structure | FStaticMeshLevelMutations CaptureTarget/Plan/Execute | Adapt ordinary unattached StaticMeshActor operations first; general actors/components need separate domain support |
| Asset authoring | IAssetTools create/duplicate/save/import/relocate/fix-up/deletion | Start with bounded creation, duplication and explicit saves; import and irreversible workflows later |
| Material graphs | FMaterialGraphDocument inspection and transactional graph commands | Resolve the actual working document, expose typed graph values, preserve working-copy semantics; later stage |
| Reflected actions | Native function reflection and ExecuteEditorFunction | Reflection supports only void/no-argument methods; CallInEditor does not grant external exposure; defer automatic generation |

Source anchors: [workspace manager](../../Engine/Source/Editor/DurinEd/Public/Workspace/WorkspaceManager.h),
[property editing](../../Engine/Source/Editor/DurinEd/Public/PropertyEditor/PropertyEditing.h),
[transactor](../../Engine/Source/Editor/DurinEd/Public/Transactions/Transactor.h),
[scene mutations](../../Engine/Source/Editor/LevelEditor/Public/StaticMeshLevelMutations.h),
[asset tools](../../Engine/Source/Editor/AssetTools/Public/AssetTools/IAssetTools.h),
and [material document](../../Engine/Source/Editor/MaterialEditor/Public/MaterialGraphDocument.h).

## Selected Design

### Ownership and invocation

- DurinEd owns editor-wide context, object/property adapters, and history tools.
  MainFrame registers the document provider with its host's FWorkspaceManager.
  LevelEditor owns scene, selection, and viewport tools; AssetTools owns asset
  queries and operations; MaterialEditor owns graph tools.
- Use FNativeToolset and explicit descriptors/bindings. Register/unregister
  through existing module/host lifetimes. Stateless module providers resolve the
  admitting editor with GetToolEditor(Context); host-bound providers must reject
  other hosts and release borrowed context before host teardown.
- Add only narrow domain accessors/services where existing operations are tied
  to widgets. Do not expose widgets or private headers across module boundaries,
  make DurinEd depend on MainFrame/LevelEditor/MaterialEditor, or add a universal
  command dispatcher. UI and Toolset adapters should share domain operations.
- Execute through the existing safe point and detached tool endpoint. Keep Core
  and MCP free of editor business rules. Console and MCP use the same names,
  schemas, exposure flags, effects, and retry policy; add no compatibility aliases.

### Target identity and results

- Require explicit targets for mutation. Queries may offer active-document or
  current-selection convenience, but mutation requests use the returned target.
  Distinguish editor world from PIE world; initial authoring targets editor state
  only and reports unavailable for unsupported play/read-only states.
- Use canonical asset/object paths for persistent identities. Runtime object
  references also carry an editor-session identity and serial-aware weak object
  identity. Encode IDs and revision tokens as strings; expose no addresses,
  array indices as object identities, or JSON numbers requiring exact uint64.
  Paths/labels are useful descriptions, not fallback routes for stale handles.
- Resolve and validate on GameThread immediately before execution. Reject stale
  references after deletion, reload, world replacement or host shutdown; never
  silently resolve a new object with an old name. Preserve only weak identity,
  not a new unbounded object-retention/cache layer.
- Mutations take observed package/document revision tokens, or an expected
  transaction ID for Undo/Redo. Reuse existing package edit/hierarchy revisions;
  Stage 0 resolves domains that lack a sufficient revision. Do not use disk
  timestamps as authored-state versions.
- Results contain domain data, target/revision, changed status, and available
  diagnostics. Mutation results identify the affected package and history entry
  when recorded. A successful query may be empty; an invalid target is an error.

### Bounds, editing and completion

- Queries accept bounded limits (initially default 50, maximum 200), filters,
  and opt-in detail. Report truncation explicitly. For continued asset/scene
  enumeration use offset plus an observed snapshot revision and reject stale
  continuation. This is domain querying, not MCP tools/list pagination.
- Read logs by sequence, maximum 200 records and 64 KiB total text per call.
  Preserve NextSequence, oldest/newest bounds and loss reporting; filtering must
  advance across scanned records. Do not consume global history/transaction
  events that another editor consumer needs.
- One edit call owns one existing domain transaction, with bounded targets and
  authoritative revalidation. Start property writes with one target/member per
  call; no remote begin/commit session or generic multi-call transaction API.
  Reuse existing edit validation, change notifications, dirty tracking and replay.
- Finish or reject conflicting interactive edits through existing admission
  policy. Do not steal a gesture or block on a modal dialog. Undo/Redo requires
  the expected top history ID, so an intervening manual edit cannot be undone
  accidentally. No automatic Undo as a substitute for cancellation.
- Edits remain in memory until an explicit save. Save is a disk effect and is
  not covered by Undo. Use the owning document/asset persistence policy; avoid
  overwriting an unapplied material working copy with a package-only save.
- Publish a tool only when it can return its final domain outcome. The current
  synchronous handler does not yield across ticks, and a 30-second MCP deadline
  cannot preempt it. Do not block GameThread waiting for future editor ticks,
  return a queued job as success, or increase timeouts to hide this mismatch.
- Initially accept only bounded synchronous domain operations. Check cancellation
  before effects and between supported batch items. If a domain operation can
  partially succeed, preserve per-item effects/failures; promise atomic rollback
  only where the reused domain contract provides it.

## Implementation Stages

### Stage 0: Freeze target contracts and provider ownership

Outcome: the first tool schemas and host integration seams are concrete before
business exposure. Depends on the existing Core/DurinEd Toolset contracts.

- [ ] Define shared wire shapes for editor-session identity, object references,
  canonical asset paths, revision tokens, query bounds and mutation summaries.
- [ ] Select serial-aware object identity and existing revision sources; specify
  stale-reference behavior across rename, deletion, Undo, reload and PIE.
- [ ] Define host registration ownership for MainFrame documents and private
  LevelEditor context; specify teardown and wrong-host rejection.
- [ ] Freeze Stage 1/2 schemas, initial property types, effects, exposure and retry
  policies. Keep helpers limited to actual shared conversions/resolution.
- [ ] Establish CPU fixtures and a clean Sandbox acceptance fixture without
  writing production content.

Gate: schema review plus tests for identity invalidation and host isolation;
no pointer-backed protocol values or UI dependency cycles.

### Stage 1: Deliver editor observation tools

Outcome: an Agent can inspect an editor before changing it. Depends on Stage 0.

- [ ] DurinEd: editor.context/status returns project, host/session, worlds, PIE,
  readiness, and transaction availability with explicit unavailable fields.
- [ ] MainFrame: editor.documents/list and inspect return open/active documents,
  resource paths and dirty state using the host manager.
- [ ] DurinEd: editor.logs/read exposes bounded logger history and loss metadata.
- [ ] AssetTools: editor.assets/search, inspect and references use registry
  snapshots without loading each asset; distinguish metadata from loaded state.
- [ ] LevelEditor: editor.scene/list, inspect and selection return actor/component
  summaries, transforms, target references and revisions; full property
  schemas/values follow in Stage 2.
- [ ] Verify external discovery and Console invocation for these same providers.

Gate: CPU tests for bounds, filters, empty results, stale continuation and wrong
host. Codex discovers the new providers and can report project, selected objects,
an asset reference and recent diagnostics without changing authored state.

### Stage 2: Deliver property edits and precise history control

Outcome: inspect/edit/verify/undo/redo works for one supported object. Depends on
Stage 1 and the Stage 0 property policy.

- [ ] DurinEd: editor.objects/inspect returns supported property schemas/values;
  editor.objects/set edits one explicitly editable member through the property
  editing boundary and host transactor.
- [ ] Start with bool, bounded integers/numbers, strings/names, enums, and an
  explicit set of vector/rotation/color structs. Reject unsupported properties,
  non-finite values, read-only/template targets and unsafe reference assignment.
  Defer arbitrary container mutation and raw serialization-based replacement.
- [ ] DurinEd: editor.history/status, undo and redo expose top IDs/descriptions
  and availability; mutation calls require the expected top ID.
- [ ] Return actual canonical values, changed/dirty state, revision and history
  IDs. Support no-op edits without manufacturing history entries.

Gate: validation failures leave supported targets unchanged; change notifications,
dirty state and undo/redo match the UI path. Intervening manual edits reject stale
requests. Codex changes one fixture property, reads it, undoes and redoes it.

### Stage 3: Deliver bounded scene authoring

Outcome: an Agent can place and adjust supported content in a fixture level.
Depends on Stage 2.

- [ ] LevelEditor: editor.scene/create_static_mesh, update_static_mesh, rename
  and remove adapt FStaticMeshLevelMutations with its existing restrictions.
- [ ] Require level identity and observed revisions; use explicit asset paths and
  transforms with documented units/rotation convention. Return created targets.
- [ ] Add editor.scene/set_selection and focus through the owning workspace,
  separate from authored changes; report unavailable without a level viewport.
- [ ] Keep generic actor spawning, arbitrary components, attachment changes and
  special RoadWeaver entities outside this first structural adapter.

Gate: plan rejection, stale revisions, transactional create/update/remove,
undo/redo and selection invalidation pass. Codex places a mesh, moves it, focuses
it, verifies the result and undoes the authored changes.

### Stage 4: Deliver explicit asset creation and persistence

Outcome: supported authored edits can be saved deliberately. Depends on Stage 2;
scene save acceptance also depends on Stage 3.

- [ ] AssetTools: editor.assets/create and duplicate wrap a bounded initial set
  of synchronous factories and existing collision/publication policy. Advertise
  supported classes/options, not unrestricted DClass construction.
- [ ] AssetTools: editor.assets/save accepts an explicit bounded package set and
  observed revisions; report saved, unchanged and failed targets separately.
- [ ] MainFrame: editor.documents/save uses the owning workspace for supported
  synchronous documents. Check final completion semantics before exposure;
  keep material compilation-backed saves unavailable at this stage.
- [ ] Add editor.documents/activate for an existing document after admission.
  Defer deferred opens, modal close/discard and project switching.
- [ ] Reuse existing save/revision bookkeeping and collision validation; do not
  add save-all, implicit save on edit or generic filesystem access tools.

Gate: fixture asset creation/duplication, collision handling, explicit save and
disk reload agree. Unsaved edits stay unsaved until requested; unsupported
document saves are rejected before starting deferred work.

### Stage 5: Deliver material graph inspection and bounded edits

Outcome: an Agent can inspect and edit a material draft through the same graph
commands as the editor. Depends on Stage 2 and a verified working-document bridge.

- [ ] MaterialEditor: editor.material/catalog and inspect describe supported
  nodes, GUID-addressed pins/links, values, draft revision and diagnostic state.
- [ ] Expose create_node, connect, disconnect, set_constant, remove_nodes and
  layout using FMaterialGraphDocument and its transactor.
- [ ] Target the actual open working copy when one exists, with explicit draft
  identity/revision. If no document exists, require an admitted document before
  editing; do not silently create an unsynchronized second draft or mutate the
  source asset behind an existing material document.
- [ ] Keep compile/apply/save separate from draft edits. Report draft validity,
  unapplied changes, compiler state and saved source state separately; no claim
  of successful compilation or persistence after a graph edit.

Gate: graph schema validation, GUID identity, links/constants, UI observation and
undo/redo pass. Codex builds and inspects a small draft graph. If document access
requires deferred creation, resolve that prerequisite before exposing edits.

### Stage 6: Integration acceptance and contract handoff

Outcome: the implemented surface is documented and verified end to end.
Depends on Stages 0-5.

- [ ] Verify provider registration/unregistration, module/host shutdown, stale
  targets, cancellation, malformed requests and domain diagnostics.
- [ ] Run domain CPU tests plus MCP codec/HTTP and official SDK regressions;
  validate affected targets under the repository's
  [shared API coverage policy](../../AGENTS.md#task-routing), including an `all`
  build when that policy requires it.
- [ ] Use Codex's MCP client against the visible Sandbox editor to inspect,
  edit, undo/redo, save/reload supported fixture content and inspect a material
  draft. Keep test-generated assets/logs in ignored fixture storage.
- [ ] Update owning Toolset/domain contracts and the MCP connection guide with
  actual schemas, restrictions and examples; record exact tested revisions,
  results and deferred gates here before completing the plan.

## Deferred Candidates

| Candidate | Why useful | Prerequisite before a separate implementation plan |
| --- | --- | --- |
| Material compile/apply/save | Close the draft-to-rendered-asset loop | Nonblocking final-result integration with existing compile/publication lifecycle and cancellation |
| Asset import/reimport | Bring Agent-generated assets into a project | Bounded source/factory policy, long-operation completion and partial-success semantics |
| Viewport camera/capture | Visual verification of scene changes | Host viewport targeting; frame/GPU readback completion, bounded image/file results and cancellation |
| Asset relocation/deletion/fix-up | Content organization | Preserve existing preflight/reference/staging policies, explicit irreversible effects and partial-failure reporting |
| General actor/component authoring | Broader scene construction | Shared structural mutation and transaction services beyond the current StaticMeshActor scope |
| Explicit reflected actions | Reuse suitable editor actions | Separate external opt-in and domain outcome policy; CallInEditor alone is insufficient |
| RoadWeaver tools | Road/domain authoring | RoadWeaver-owned Toolset after its mutation/revision contracts are selected |

Resources, Prompts, arbitrary C++/Python evaluation, shell/build wrappers, generic
console execution, automatic reflection-generated tools, new protocol versions,
RBAC/approval frameworks, and a generic job system are outside this plan. Build
and test commands remain DevTool tasks. Add asynchronous service support only for
a concrete follow-up operation, rather than introducing it speculatively here.

## Required References

- [Core Toolset contracts](../Runtime/Core/Tools.md),
  [editor service](../Editor/Architecture/EditorTools.md),
  [MCP adapter](../Editor/Architecture/MCPTools.md).
- [Transactor core](../Editor/Architecture/Transactors.md),
  [material graph operations](../Editor/Architecture/MaterialGraphOperations.md),
  [material lifecycle](../Editor/Architecture/MaterialEditorLifecycle.md).
- [Function reflection](../Runtime/Core/FunctionReflection.md) and
  [editor actions](../Editor/Architecture/EditorFunctionActions.md).
- [Build/run](../Agents/BuildAndRun.md), [testing](../Agents/Testing.md), and
  [documentation maintenance](../Agents/Documentation.md).
