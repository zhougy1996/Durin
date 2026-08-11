# Native Graybox Scene Authoring Plan

Summary: Add repository-native, transaction-backed graybox placement and recipe reconciliation without temporary build targets or external editor-control services.

Last reviewed: 2026-08-11

Status: Active
Completed:

## Current Status

Durin can already author ordinary level Actors through the Level Editor. The
World Outliner creates reflected Actor classes, the Scene Viewport places
dragged StaticMesh assets, Details and the transform gizmo edit selected
objects, and `FLevelDocumentController` saves the active `DLevel` package.
These paths are direct panel mutations rather than one reusable level-authoring
service. Actor creation, deletion, and rename do not share the complete
transaction behavior already used by reflected properties, transforms,
visibility, spline edits, and primary-camera changes.

The Sandbox `ThirdPersonTest` graybox was bootstrapped at revision `f12c9113`
by temporarily compiling a one-use executable, loading `/Engine/Models/Box`,
creating ordinary `AStaticMeshActor` instances, and saving a new Level asset.
The temporary target was removed after generation. The resulting Actors are
individually editable, but repeating or reconciling the layout requires either
manual UI work or another bespoke executable. That workflow is evidence for a
missing reusable authoring boundary, not an accepted long-term workflow.

Stage 1 is implemented. `FStaticMeshLevelAuthoringService` now plans and
executes bounded create/update/rename/remove batches for ordinary unattached
`AStaticMeshActor` graphs. It uses exact package and hierarchy revisions,
typed diagnostics, one structural transaction, source-state validation on
Undo/Redo, collision refusal, no-op suppression, and existing saved-revision
tracking. StaticMesh viewport placement and the corresponding Outliner
create/rename/delete paths use the service; unsupported Actor graphs retain
explicit legacy behavior. `LevelAuthoringTests` covers atomic batches,
Undo/Redo, saved state, stale plans, read-only/document transitions,
unsupported graphs, no-op execution, and Redo collision refusal.
Test-only failure injection also verifies rollback after temporary rename,
remove, create, final rename, and state update without publishing history or
leaving the package dirty.
Selection remains workspace state rather than transaction history: interactive
callers select the exact names returned by Execute, document synchronization
removes destroyed selections after Undo, and Redo does not overwrite whatever
the user selected after the original operation. This avoids retaining a panel
or workspace-context pointer in long-lived transaction history.

The next selected increment is Stage 1.5: an initial repository-native
`graybox-build` command. It generates a new open-air Box arena at one explicit
unused Level path and exits; it does not inspect or patch an existing Level,
accept JSON, or expose the later general recipe workflow. Existing output paths
are rejected. Initial publication can reuse AssetCore's tokenized relocation
to move a verified temporary candidate into an absent destination, but
destructive replacement remains deferred until replacement of an occupied
package has its own qualified publication operation. Clearing a live Level and
hoping the later save succeeds is not an accepted replacement strategy.

Stage 1.5 is implemented. Launch now carries one opaque startup-command
envelope until a feature module registers its handler, with bounded refusal for
unknown commands and deterministic result-code shutdown. LevelEditor registers
`graybox-build`, derives the open-arena transforms from the loaded Box bounds,
applies one Stage 1 StaticMesh batch to an unpublished candidate, adds
PlayerStart and DirectionalLight, verifies save/reload state, and publishes the
candidate through AssetCore relocation. DurinDevTool owns argument validation,
profile selection, heartbeat, timeout, cancellation, and diagnostics. A
project-wide process mutex rejects concurrent visible or command Editor
authoring.

The Sandbox smoke published `/Game/Levels/GrayboxStage15`, refused an identical
rerun, refused a second command while another Editor owned Sandbox, left no
conflict output, and passed the compatibility audit with no incompatible,
unsupported, failed, or stale packages. An initial failed verification exposed
that a newly saved candidate can temporarily be absent from the live registry;
cleanup now removes that exact command-owned physical package when and only
when no registry entry exists. The observed orphan was removed after its exact
path was verified. Command-local save/reload injection was not added to the
production command surface: focused geometry/startup/CLI tests combine with the
existing Stage 1 mutation and AssetCore relocation failure suites, while live
smokes qualify occupied-output, ownership, persistence, cleanup, and shutdown.

This plan selects an entirely repository-native solution. It does not use Echo
SceneBox, MCP, a remote service, a network listener, or code from another
project. The first slice combines a LevelEditor-owned mutation service, a
Graybox panel, a versioned YAML recipe, and an optional non-interactive Editor
command wrapped by DurinDevTool.

## Goal

- Let a user build and revise open or enclosed graybox test spaces from the
  Level Editor without compiling a purpose-built executable.
- Make each generated piece an ordinary selectable and editable
  `AStaticMeshActor` using existing Outliner, Details, and gizmo workflows.
- Apply a source-controlled scene recipe repeatedly as one atomic, undoable
  create/update/prune operation with deterministic results.
- Give local automation a permanent repository-owned entry point that uses the
  same validation and mutation semantics as the interactive editor.

## Scope

- A game-thread-only level-authoring service in `LevelEditor` for preflighting
  and transactionally mutating supported static-mesh Actors.
- An initial fixed-contract `DevTool scene graybox-build` command that creates
  one new open-air Box Level through a bounded non-interactive DurinEditor
  process before the general recipe workflow is available.
- Create, update, rename, and remove operations needed by manual graybox
  placement and recipe reconciliation.
- Box and triangular-prism built-in graybox assets, with a rotated Box preset
  retained as the default rectangular ramp.
- A dockable Graybox panel for primitive placement, dimensions, transforms,
  enclosure/platform/stair/ramp presets, selection, and recipe preview/apply.
- A versioned project-owned YAML recipe format containing a namespace, stable
  entry IDs, primitive or StaticMesh references, transforms, and optional
  prune policy.
- Deterministic incremental reconciliation against an open editable Level.
- A permanent `DevTool scene plan|apply` workflow backed by a bounded
  non-interactive DurinEditor command, after the interactive service and recipe
  contract are qualified.
- Focused native tests, Editor rendering evidence, asset audit, documentation,
  and bounded interactive and non-interactive runtime validation.

## Non-Goals

- Echo SceneBox, MCP, HTTP, sockets, remote procedure calls, or any dependency
  on company-internal editor tooling.
- A general scripting language, Blueprint equivalent, Python runtime embedded
  in the Editor, or arbitrary reflected-object mutation from untrusted text.
- Replacing the Outliner, Details panel, transform gizmo, Content Browser,
  package serializer, or editor transaction manager.
- Procedural CSG/BSP, boolean operations, mesh editing, UV authoring, material
  painting, terrain, foliage, navigation, or production level-design tooling.
- Arbitrary Actor-class serialization in the first slice. Recipe ownership and
  structural Undo/Redo are limited to supported `AStaticMeshActor` layouts.
- Production collision, character sweeps, slope movement, step solving,
  physics bodies, or camera obstruction. Graybox geometry remains visual until
  those runtime capabilities receive their own plan.
- Silent background saving or concurrent mutation of a Level already owned by
  another Editor process.
- Initial `graybox-build` replacement of an existing Level. The first command
  is create-only and rejects an occupied output path; `--replace` requires a
  separately qualified atomic candidate-package publication boundary.

## Design Decisions and Invariants

### Ownership and layering

- `LevelEditor` owns the authoring service, recipe model, reconciliation,
  structural transactions, Graybox panel, and non-interactive command handler.
  Runtime `Engine` keeps only the Actor, Level, component, and asset contracts
  needed by ordinary authored content.
- `DurinEd` continues to own the generic transaction manager and workspace
  framework. It gains no Graybox or Sandbox policy.
- `MainFrame` may expose the panel through the established workspace-panel
  registration surface, but it does not parse recipes or mutate Levels.
- DurinDevTool validates arguments, starts the correct Editor profile, captures
  output, and reports the result. It does not parse or rewrite `.dasset` files.
- Sandbox owns its concrete recipes and level-design choices. Engine and
  LevelEditor own only reusable primitives and authoring behavior.

### Supported mutation boundary

- The first public service accepts an immutable mutation request and returns a
  typed plan/result. It does not expose panel widgets, retain caller-owned
  pointers, or mutate during preflight.
- Supported operations are create supported StaticMesh Actor, update its mesh
  and transform, rename it, and remove a recipe-managed StaticMesh Actor.
  Unsupported classes or component graphs fail preflight without mutation.
- Every request names one exact open `DLevel`, expected level/package identity,
  and expected editor document revision. World replacement, PIE/read-only
  transition, incompatible package state, or revision change makes the plan
  stale and rejects execution.
- All mutation and transaction execution occurs on the game thread. Recipe
  file reading and parsing may occur off-thread, but it publishes an immutable
  detached model before touching assets or objects.

### Transactions, dirty state, and failure

- One interactive placement or one recipe application produces at most one
  `IEditorTransaction`. It reports the exact Level package through
  `GetAffectedPackages()` and participates in the existing saved-revision
  model.
- Structural transactions retain detached before/after descriptions sufficient
  to recreate the supported Actor class, requested stable name, mesh reference,
  transform, visibility, attachment state allowed by the first slice, and
  attachment state allowed by the first slice. The first slice supports only
  unattached Actors. They do not retain ephemeral component addresses or a
  workspace selection pointer as their restoration authority.
- Preflight resolves every class and asset, validates every finite transform,
  proves unique identities, checks prune eligibility, and computes the full
  before/after set before the first live mutation.
- Execution is atomic at the supported authoring boundary. Any failed create,
  bind, transform, rename, destroy, notification, or transaction publication
  restores the exact pre-request supported Actor set and does not enter history.
- Undo/Redo revalidates Level ownership and name collisions before mutation.
  Failure leaves the history head and live Level unchanged and reports one
  actionable error.
- Saving remains explicit through the existing Level document controller.
  Interactive Apply dirties the Level but never saves automatically.

### Recipe format and stable identity

- Recipes are UTF-8 YAML files below a project-owned configuration root such
  as `Configs/SceneRecipes/`; they are source inputs, not `.dasset` packages.
- Schema version 1 has one Level package path, one recipe namespace, and an
  ordered list of entries. Each entry has a unique ASCII identifier, a
  primitive alias or exact mounted `DStaticMesh` path, and a finite transform.
  Optional preset-specific parameters must lower deterministically to that
  canonical entry list before reconciliation.
- A managed Actor name is derived exactly from validated
  `<namespace>__<entry-id>`. The service never accepts the auto-suffixed name
  returned by `DLevel::SpawnActor` as success. A collision with an unmanaged or
  wrong-class Actor fails preflight.
- Actor names are the selected first-slice persistent reconciliation identity.
  Manually renaming a managed Actor intentionally detaches it from the recipe;
  preview reports the old entry as missing and the renamed Actor as unmanaged.
  Persistent Actor GUIDs or serialized editor metadata remain deferred until a
  demonstrated rename-preservation requirement justifies the runtime-format
  cost.
- Preview classifies entries as unchanged, create, update, conflict, and prune.
  Apply is disabled while any conflict exists.
- Prune is opt-in for each apply. It may remove only supported Actors whose
  names are inside the exact recipe namespace. Default Apply never deletes an
  Actor absent from the recipe.
- Recipe ordering defines creation and report order. Reapplying an unchanged
  recipe is a no-op: it creates no transaction, does not dirty the package, and
  does not reorder the Level's existing Actors.

### Graybox assets and panel behavior

- `/Engine/Models/Box` remains the canonical Box. Stage 0 qualifies its source
  bounds and transform convention rather than duplicating a Sandbox cube.
- The triangular prism is one small Engine-owned built-in StaticMesh with
  documented axis, unit bounds, normals, UVs, source provenance, and asset
  audit coverage. It is not generated procedurally per Level.
- The Graybox panel places ordinary `AStaticMeshActor` instances through the
  authoring service. It does not introduce an `AGrayboxActor` runtime type or
  hide many pieces inside one component-owning aggregate.
- Primitive dimensions lower to Actor scale using the qualified source bounds.
  Negative, zero, non-finite, or unrepresentable dimensions are rejected.
- A rectangular ramp is a rotated, scaled Box preset. A triangular-prism ramp
  is an explicit alternative, not an automatic replacement.
- Enclosure presets expose floor and wall dimensions separately and default to
  no ceiling. A ceiling is added only when the user explicitly enables it.
- Newly created Actors become the shared Level selection and remain editable
  through the existing gizmo and Details panel. Panel-local selection never
  competes with `FLevelEditorContext`. Selection is published by the caller
  after initial Execute; Undo removes invalid selections through ordinary
  context synchronization, while Redo preserves the user's current selection.

### Non-interactive execution

- Automation uses a permanent DurinEditor command mode registered by
  LevelEditor after ordinary project, module, mount, reflection, asset, and
  renderer initialization. It does not use a newly generated CMake target.
- `DevTool scene plan` performs parse, load, compatibility, and reconciliation
  preview without mutation. `DevTool scene apply` requires an explicit recipe
  and Level, executes one batch, saves only after successful execution, audits
  the saved package, and exits with a stable result code.
- Command mode rejects dirty input, an incompatible Level, target mismatch,
  concurrent project authoring ownership, prompts, or any request requiring UI
  choice. It never starts PIE.
- The command emits a versioned machine-readable summary plus concise human
  output. DurinDevTool forwards paths and diagnostics without interpreting
  binary asset contents.

### Initial graybox-build boundary

- Stage 1.5 adds one narrower command before the recipe system: `DevTool scene
  graybox-build --project <project> --output <mounted-level-path>`. DevTool
  starts the selected DurinEditor profile with its window suppressed and one
  bounded `graybox-build` startup request.
- The initial command supports one generic `open-arena` preset using
  `/Engine/Models/Box`. Parameters are width, depth, floor thickness, wall
  height, wall thickness, and an explicit ceiling switch. Dimensions are
  finite positive values with documented bounds. Ceiling defaults to false.
- Width and depth mean clear walkable distance between opposing inner wall
  faces. The floor spans the complete outer wall footprint with its top at
  `Z=0`; wall height is measured above that plane. North/south walls cover the
  outer width, east/west walls cover the clear depth, and every wall penetrates
  the floor by one small derived seam overlap bounded by both floor and wall
  thickness. An explicit ceiling reuses floor thickness and caps the outer
  footprint at wall height. This convention prevents floor, edge, and corner
  gaps without relying on assumed unit-cube scale.
- The startup command handler owns no placement math. A LevelEditor-owned
  preset builder resolves Box bounds, lowers the complete arena to one Stage 1
  mutation request, and names every generated Actor deterministically. The
  generic preset also describes one centered `APlayerStart` and one standard
  `ADirectionalLightActor` so a newly published Sandbox Level can enter native
  third-person play and render without manual baseline setup. These two
  non-StaticMesh Actors are created only inside the disposable unpublished
  candidate; they do not expand Stage 1's interactive transaction boundary.
  Sandbox-specific topology and coordinates are not compiled into Engine or
  LevelEditor.
- Initial publication is create-only. The command proves that the exact output
  path is absent from both the registry and loaded packages, creates a
  command-owned temporary `DLevel` below the same writable project mount,
  applies the complete batch, saves once, unloads and reloads the candidate,
  and verifies its Actor states. It then analyzes, revalidates, and applies one
  AssetCore relocation token into the still-absent output path and reloads the
  published Level. Pre-publication failure deletes only the exact temporary
  candidate; post-publication verification failure restores the relocation
  token before cleanup and reports cleanup failure separately.
- The process refuses an occupied output path, an incompatible project, an
  unavailable or invalid Box asset, invalid dimensions, another active project
  authoring owner, prompts, PIE, extra commands, or save/reload verification
  failure. One process handles one project, one output Level, and one build
  request.
- Stage 1.5 produces concise human diagnostics and stable exit codes. It does
  not add scene JSON, YAML parsing, arbitrary Actor operations, inspection,
  incremental reconciliation, pruning, or replacement. Stage 4 may add a
  machine-readable summary while extending the same startup-command shell.

## Current Foundations and Gaps

| Area | Current foundation | Required gap closure |
| --- | --- | --- |
| Level mutation | `DLevel` creates, destroys, finds, and renames Actors | Reusable preflight, atomic batch, and supported structural snapshots |
| Transactions | Reflected properties, transforms, visibility, spline edits, and primary camera use editor history | Create/delete/rename/static-mesh placement are not one complete transaction path |
| Selection | `FLevelEditorContext` owns shared Actor/component selection | Authoring result must publish selection through that authority |
| Placement | Outliner adds Actor classes; viewport drag/drop places mesh Actors | Logic is embedded in panels and cannot be reused by presets or recipes |
| Saving | `FLevelDocumentController` saves and advances saved state | Non-interactive apply needs the same guarded save result and post-save audit |
| Assets | Engine supplies `/Engine/Models/Box` | No qualified triangular-prism built-in or primitive catalog |
| Repeatability | Ordinary Level serialization preserves generated Actors | No source recipe, preview, deterministic identity, or reconciliation |
| Automation | DurinDevTool selects profiles and captures child output | No permanent scene command or bounded Editor command mode |
| Initial generation | Content Browser can create an empty Level and Stage 1 can populate supported Actors | No permanent create-only open-arena command, startup request, or verified post-save reload |

## Implementation Stages

### Stage 0: Freeze the authoring and recipe contracts

Dependencies: existing Level, workspace, reflected editing, transform gizmo,
saved-revision, and asset compatibility contracts.

- [ ] Record the source revision and focused LevelEditor, transaction, Level
  serialization, asset audit, and hidden Editor startup baseline.
- [ ] Characterize current create, drag/drop, rename, delete, save, Undo/Redo,
  selection, document replacement, PIE read-only, and failure behavior.
- [ ] Freeze the supported StaticMesh Actor snapshot, request/result types,
  game-thread boundary, stale-plan checks, and rollback semantics.
- [ ] Freeze YAML schema version 1, namespace/name grammar, canonical transform
  representation, recipe path policy, prune policy, and result codes.
- [ ] Qualify Box source bounds and define the triangular-prism geometry,
  orientation, normals, UVs, source path, and mounted asset path.
- [ ] Add failing fixtures for atomic create/update/prune, no-op apply,
  conflicts, stale plans, Undo/Redo, dirty state, selection, and serialization.

#### Acceptance Gate

- The first-slice classes, identities, schema, mutation ordering, rollback,
  threading, save boundary, primitive conventions, and exclusions are explicit,
  and the unchanged focused baseline is green.

### Stage 1: Centralize transaction-backed level mutation

Dependencies: Stage 0.

- [x] Add the LevelEditor-owned planning and execution service with immutable
  requests, typed diagnostics, supported Actor snapshots, and stale-plan checks.
- [x] Implement atomic create, update, rename, and remove for supported
  `AStaticMeshActor` graphs.
- [x] Add one structural batch transaction with exact package reporting,
  rollback, Undo/Redo collision checks, saved-revision integration, and no-op
  suppression.
- [x] Route existing StaticMesh viewport drop through the service without
  changing placement position, selection, asset loading, or error behavior.
- [x] Route the relevant Outliner StaticMesh create/rename/delete paths through
  the service; retain explicit behavior for unsupported Actor classes until a
  separate generic snapshot contract exists.
- [x] Prove document replacement, PIE/read-only transition, actor/component
  destruction, asset replacement, failed rollback, and editor shutdown safety.

#### Acceptance Gate

- Every supported StaticMesh structural edit uses one reusable service and is
  atomic, undoable, selection-correct, and saved-revision-correct; unsupported
  Actor edits cannot be mistaken for transaction-backed support.

### Stage 1.5: Add an initial create-only graybox-build command

Dependencies: Stage 1, existing project initialization, asset creation/save,
and DurinDevTool profile selection. This stage does not depend on the Graybox
panel, triangular-prism asset, or recipe parser.

- [x] Freeze the `scene graybox-build` CLI, bounded numeric ranges, mounted
  output-path policy, deterministic Actor names, human diagnostics, and stable
  exit codes. Do not expose `--replace` in the initial contract.
- [x] Add a generic Launch-to-Editor startup-command envelope that preserves
  opaque command arguments through ordinary initialization, admits exactly one
  handler after module startup, requests exit on completion, and returns its
  result code without embedding Graybox policy in Launch.
- [x] Add the LevelEditor-owned `open-arena` preset builder. Qualify actual Box
  local bounds, derive floor and four-wall transforms from requested dimensions,
  add a ceiling only when explicitly requested, lower the geometry to one
  Stage 1 create batch, and add deterministic centered PlayerStart plus standard
  DirectionalLight descriptors for the unpublished candidate.
- [x] Add create-only command publication: reject registry or loaded-package
  collisions, create a command-owned temporary `DLevel` in the same project
  mount, execute the batch, save once, unload and reload the candidate, compare
  its Actor snapshots, and publish it to the absent output using AssetCore's
  analyze/revalidate/apply relocation token. Restore a failed published
  verification and delete only the exact command-owned candidate during cleanup.
- [x] Add project-authoring ownership exclusion and reject concurrent command
  execution, a visible Editor authoring the same project, prompts, PIE, extra
  commands, invalid project/output/asset state, and cancellation before save.
- [x] Add `DevTool scene graybox-build` argument validation, profile and editor
  executable selection, hidden child lifecycle, heartbeat, cancellation,
  timeout, concise output forwarding, and exit-code mapping. DevTool must not
  create or edit `.dasset` files itself.
- [x] Add focused native and Python coverage for preset geometry, open-top
  default, explicit ceiling, bounds conversion, invalid dimensions, and CLI
  lifecycle. Reuse the qualified Stage 1 mutation and AssetCore relocation
  failure suites, and add live command smokes for occupied output, ownership
  conflict, cleanup, deterministic re-run refusal, persistence, and shutdown.
- [x] Document the create-only workflow and validate one Sandbox command smoke
  that publishes a new Level, passes asset audit, opens in DurinEditor, contains
  floor plus four connected walls, and has no ceiling by default.

#### Acceptance Gate

- A user or agent can create and verify a new open-air Box arena at an unused
  Sandbox Level path with one permanent DevTool command. The workflow creates
  no temporary target, emits no scene JSON, never overwrites an existing Level,
  leaves no partial published asset after a reported failure, and exits its
  non-interactive DurinEditor process deterministically. The published Level
  contains floor, four connected walls, PlayerStart, DirectionalLight, and no
  ceiling by default, and can enter the Sandbox native third-person play path.

### Stage 2: Add native Graybox assets and interactive tools

Dependencies: Stages 1 and 1.5. Stage 2 reuses the qualified Box convention and
open-arena lowering but does not depend on the command for interactive edits.

- [ ] Add and audit the Engine triangular-prism source and StaticMesh asset;
  expose Box and prism through a fixed Graybox primitive catalog.
- [ ] Register a dockable Graybox panel in the Level workspace with primitive,
  dimension, transform, and placement controls.
- [ ] Add deterministic floor, four-wall enclosure, platform, stair, rotated-Box
  ramp, prism-ramp, cover, and pillar presets; ceiling remains off by default
  and requires an explicit control.
- [ ] Lower every action to the Stage 1 mutation request and publish results to
  shared selection, Details, Outliner, viewport rendering, and history.
- [ ] Preserve ordinary Actor editing after placement and prove that save,
  close/reopen, Undo/Redo, duplication, manual rename, and deletion remain
  understandable.
- [ ] Add rendered Editor evidence for Box/prism orientation, winding, normals,
  transforms, open-top enclosure layout, selection, and gizmo interaction.

#### Acceptance Gate

- A user can build, edit, undo, save, reopen, and continue editing an open-air
  graybox arena without compiling code, and every piece remains an ordinary
  `AStaticMeshActor`.

### Stage 3: Add recipe preview and incremental reconciliation

Dependencies: Stages 1-2.

- [ ] Implement the versioned YAML parser with bounded counts, strict unknown
  fields, finite numeric validation, canonical diagnostics, and detached output.
- [ ] Implement deterministic lowering of primitive and preset entries to the
  canonical Actor-entry model.
- [ ] Add recipe preview with unchanged/create/update/conflict/prune categories,
  exact target Level checks, and explicit prune opt-in.
- [ ] Apply the complete reconciliation as one Stage 1 transaction; preserve
  unmanaged Actors and Level order for unchanged managed Actors.
- [ ] Add panel actions to open, reload, preview, apply, and reveal recipe
  errors without automatically saving the Level.
- [ ] Add a Sandbox-owned recipe matching `ThirdPersonTest`, reconcile the
  existing Level without changing its intended open-top layout, and document
  which manual edits are overwritten or detached on later apply.
- [ ] Prove identical reapply, reordered YAML, missing assets, name conflicts,
  manual rename detachment, explicit prune, parse cancellation, rollback,
  Undo/Redo, save/reload, and asset audit behavior.

#### Acceptance Gate

- `ThirdPersonTest` and equivalent project levels can be regenerated or
  incrementally updated from source-controlled YAML with deterministic preview,
  one-step Undo, no implicit deletion, and no temporary executable.

### Stage 4: Add permanent local automation and qualify the workflow

Dependencies: Stages 1.5 and 3. Stage 4 extends the startup-command shell and
DevTool scene group instead of creating a second automation path.

- [ ] Add bounded non-interactive DurinEditor scene-plan and scene-apply command
  handling that reuses the same recipe and mutation services.
- [ ] Add `DevTool scene plan|apply` argument validation, profile selection,
  child lifecycle, cancellation, timeout, machine summary, and exit-code mapping.
- [ ] Reject concurrent authoring ownership, dirty/incompatible inputs, prompts,
  target mismatches, PIE, and save or post-save audit failure.
- [ ] Ensure Apply saves only the requested Level after successful transaction
  execution and verifies the published package before reporting success.
- [ ] Publish lasting Editor architecture and user-guide documentation; update
  Sandbox gameplay documentation to point to its maintained recipe.
- [ ] Run focused native/Python tests, asset audits, complete Editor build, and
  bounded interactive plus command-mode smokes using the root validation rules.

#### Acceptance Gate

- Humans and local automation use the same repository-native authoring
  semantics; no workflow creates a temporary target, directly edits `.dasset`
  bytes, starts a service, or depends on external project tooling.

## Validation Matrix

| Boundary | Required evidence |
| --- | --- |
| Planning | Parse and reconciliation are mutation-free, deterministic, bounded, and stale-aware |
| Structural mutation | Create/update/rename/remove success plus injected failure at every live step |
| Initial command | New-path success, occupied-path refusal, open-top default, explicit ceiling, ownership exclusion, save/reload verification, failure cleanup, bounded exit |
| Transactions | One history entry, exact Undo/Redo, collision refusal, no-op suppression, saved-revision correctness |
| Selection and UI | Shared selection, Outliner/Details refresh, gizmo editing, read-only and document transition behavior |
| Assets | Box bounds, prism geometry/normals/UVs, asset compatibility, save/reload references |
| Recipes | Schema errors, unique IDs, target mismatch, conflicts, manual detachment, prune opt-in, idempotent reapply |
| Persistence | Explicit interactive save, command-mode guarded save, reload equivalence, post-save asset audit |
| Rendering | Box/prism orientation, open-top enclosure, transform updates, selection visualization |
| Automation | Argument errors, child cancellation/timeout, concurrent ownership rejection, stable JSON and exit codes |
| Scope | No Echo SceneBox/MCP/network dependency, no temporary build target, no arbitrary object scripting |

## Definition of Done

- LevelEditor owns one reusable, tested StaticMesh level-authoring service used
  by existing placement paths, Graybox tools, recipes, and local automation.
- Supported structural edits are atomic, undoable, dirty-state-correct,
  selection-correct, and safe across document/PIE/lifecycle transitions.
- Users can place Box and triangular-prism pieces and build common open-air
  graybox structures from a native panel while retaining ordinary Actor edits.
- A versioned YAML recipe previews and reconciles a Level deterministically,
  never deletes implicitly, and leaves an unchanged reapply as a true no-op.
- Sandbox `ThirdPersonTest` has a maintained recipe and no longer depends on a
  one-use generator for future layout changes.
- DurinDevTool can plan and apply recipes through a bounded Editor command mode
  with guarded save and audit, without an external control service.
- Lasting contracts and workflows are published under Editor architecture and
  guides, all required validation passes, and no temporary generator remains.

## Deferred Follow-ups

- Persistent Actor GUIDs or serialized recipe metadata if real workflows prove
  that managed identity must survive arbitrary manual rename.
- Generic structural transactions for arbitrary reflected Actor/component
  graphs beyond the first supported StaticMesh boundary.
- Procedural CSG, editable primitive topology, grid-surface snapping, vertex
  snapping, material palettes, grouping, prefabs, and reusable sublevels.
- Collision generation and visualization, character sweeps, walkable slopes,
  step solving, camera collision, and physics-backed gameplay qualification.
- Live inter-process control of an already running Editor. Any future proposal
  requires a separate security and ownership plan and may not assume MCP or a
  company-internal service.
- Destructive `graybox-build --replace`. Existing relocation safely publishes
  a verified candidate only to an absent path; replacement stays deferred until
  a separate change qualifies occupied-destination publication that preserves
  the previous Level on mutation, save, audit, cancellation, crash, or
  process-exit failure.

## Related Documentation

- [Viewport Editing Architecture](../Editor/Architecture/ViewportEditing.md)
- [Static Mesh Level Authoring](../Editor/Architecture/StaticMeshLevelAuthoring.md)
- [Reflected Property Editing](../Editor/Architecture/ReflectedPropertyEditing.md)
- [Editor Workspace Framework](../Editor/Architecture/WorkspaceFramework.md)
- [Sandbox Gameplay](../Runtime/Gameplay/SandboxGameplay.md)
- [Level System](../Runtime/World/LevelSystem.md)
- [Static Mesh Rendering](../Runtime/Rendering/StaticMeshRendering.md)
- [Build And Run](../Development/Build/BuildAndRun.md)
- [Native Tests](../Development/Build/NativeTests.md)

## Related Code

- `Engine/Source/Runtime/Engine/Public/Engine/Level.h`
- `Engine/Source/Runtime/Engine/Private/Engine/Level.cpp`
- `Engine/Source/Runtime/Engine/Public/Actors/StaticMeshActor.h`
- `Engine/Source/Editor/LevelEditor/Public/StaticMeshLevelAuthoring.h`
- `Engine/Source/Editor/LevelEditor/Public/GrayboxSceneAuthoring.h`
- `Engine/Source/Editor/LevelEditor/Private/Authoring/StaticMeshLevelAuthoring.cpp`
- `Engine/Tests/Native/EngineTests/Private/Editor/StaticMeshLevelAuthoringTests.cpp`
- `Engine/Source/Editor/DurinEd/Public/Editor/EditorTransaction.h`
- `Engine/Source/Editor/LevelEditor/Private/Workspace/LevelEditorContext.h`
- `Engine/Source/Editor/LevelEditor/Private/Panels/SceneViewportPanel.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Panels/WorldOutlinerPanel.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Documents/LevelDocumentController.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Widgets/MLevelEditor.cpp`
- `Engine/Source/Runtime/Launch/Private/Launch.cpp`
- `Engine/Source/Runtime/Launch/Public/LaunchEngineLoop.h`
- `Engine/Source/Editor/LevelEditor/Private/Panels/ContentBrowserOperations.cpp`
- `Tools/DurinDevTool/durin_dev_tool/registry.py`
- `Tools/DurinDevTool/durin_dev_tool/build/handler.py`
- `Tools/DurinDevTool/durin_dev_tool/scene.py`
- `Sandbox/Content/Levels/ThirdPersonTest.dasset`
- `Sandbox/Configs/Project.yaml`
