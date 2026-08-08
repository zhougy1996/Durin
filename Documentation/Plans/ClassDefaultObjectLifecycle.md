# Class Default Object Lifecycle Plan

Summary: Give eligible reflected classes one immutable deterministic default object without triggering ordinary runtime publication or activation.

Last reviewed: 2026-08-08

Status: Completed
Completed: 2026-08-08

## Current Status

- All stages and acceptance gates are complete. The final stage started from
  implementation baseline `98826af0`; module-aware teardown, lasting docs,
  validation, editor restart smoke, and unchanged-package evidence are recorded
  in the final handoff below.
- This plan is a prerequisite for compact asset serialization. The DAST v4
  measurement/wire-contract plan is deliberately not active until this plan's
  exit gate passes.
- Construction purpose and template flags now distinguish runtime, load,
  duplication, class-default, and bounded default-subobject construction.
  The compact-serialization roadmap now records this prerequisite as complete;
  its v4 measurement and wire-contract child milestone is ready to activate but
  no v4 implementation or package-format change was made here.
- `DMaterial` proves both feasibility and risk: its durable defaults are explicit
  and deterministic, including 56 canonical parameter definitions, but ordinary
  construction also allocates a render proxy and calls publication. A class
  default must retain the former without performing the latter.
- The design takes the useful CDO/template boundary from Unreal-style object
  systems but does not introduce editable defaults, Blueprint regeneration,
  arbitrary archetype chains, hot reload, or general default-subobject graphs.

## Goal

Provide one immutable, deterministic default object for every reflected class
that is eligible to participate as a concrete authored object. The object must
run the same durable default initialization as an ordinary instance while being
unambiguously marked as a template and prevented from causing runtime
publication, registration side effects, I/O, tasks, dirty state, or
world/render/physics activation.

The completed lifecycle must give later serialization work a stable logical
comparison baseline, preserve existing ordinary-object behavior, keep template
objects and their references safe under GC, and release module-owned instances
before their code unloads.

## Scope

- Exact eligibility rules for classes that own a class default object, including
  abstract, intrinsic/bootstrap, package, and ordinary authored classes.
- A dedicated construction purpose carried by `FStaticConstructObjectParameters`
  and `FObjectInitializer`.
- Explicit class-default/template object flags and query helpers.
- `DClass` ownership, creation state, immutable access, deterministic naming,
  base-before-derived creation, reentrancy diagnostics, and registration-batch
  integration.
- Constructor audits and bounded repairs needed to separate durable defaults
  from runtime side effects, beginning with Material render-proxy publication.
- Template filtering for broad `GDObjectArray` queries and systems that mean
  live instances rather than reflection templates.
- GC reachability, default-object references, garbage rejection, teardown, and
  module-shutdown ordering.
- Logical parity tests proving a newly constructed ordinary object begins with
  the same authored reflected values as its class default.
- Lasting Reflection System, Garbage Collection, and Runtime Lifecycle
  documentation after implementation.

## Non-Goals

- Defining DAST v4 headers, tables, opcodes, byte accounting, default-relative
  encoding, forced-override provenance, unknown retention, or migration.
- Implementing struct default storage or changing the completed `DStruct`
  operations contract.
- Mutable or editor-authored class defaults, Blueprint-generated classes,
  arbitrary archetypes, class reinstancing, hot reload, config overlays, or
  inheritance-time default editing.
- A general default-subobject instancing graph. Constructor-created owned inners
  are an audit blocker unless this plan explicitly selects and tests a bounded
  contract for them.
- Treating C++ padding or raw object memory as a default snapshot.
- Automatically allowing hard asset references in defaults without an explicit
  GC and later authored-dependency disposition.
- Changing ordinary asset load, duplication, or package wire bytes.

## Design Decisions and Invariants

### Ownership and Identity

- `DClass` is the authoritative owner and exposes its ready default object as a
  `const DObject*`. Callers cannot mutate class defaults through the public API.
- A class default is a real `DObject` with explicit `ClassDefaultObject` and
  `Transient` identity. It uses normal reflected field access and GC traversal;
  raw allocation snapshots and detached unregistered pseudo-objects are not
  allowed.
- The class default remains registered in `GDObjectArray` so existing object
  validity, handle, reference, and destruction machinery remains authoritative.
  Generic scans that mean live instances exclude class defaults by default;
  explicit reflection/lifecycle diagnostics may opt in.
- The existing `DClass::GetDefaultObjectName()` metadata continues to name
  ordinary spawned objects. The internal class-default name is a separate
  deterministic identity and cannot change actor/component naming behavior.
- Class defaults are immutable after successful construction. They cannot be
  placed in an asset package, marked dirty, saved, migrated, duplicated as an
  ordinary object, or selected as a live editor/runtime instance.

### Eligibility and Creation Order

- Stage 0 freezes an explicit eligibility predicate. `ClassConstructor !=
  nullptr` alone is insufficient because reflection bootstrap types and packages
  have construction behavior but are not authored instance templates.
- Abstract classes do not receive class defaults in the first contract. Every
  concrete class that can occur in an authored object graph must either receive
  one or produce a deterministic ineligibility diagnostic before v4 planning.
- Creation occurs on the game thread only after the class, superclass,
  generated properties, struct dependencies, and GC reference schema are fully
  registered. It is never an implicit lazy side effect of a worker-thread query.
- Each newly loaded registration batch creates eligible defaults base-first,
  then by qualified class name for peers. A derived default cannot be ready
  before its superclass disposition is ready.
- `DClass` tracks at least `Uninitialized`, `Constructing`, `Ready`, and `Failed`
  states. Recursive construction, duplicate construction, missing dependencies,
  and failed constructor invariants fail deterministically without publishing a
  partially ready default.

### Construction Purpose and Side Effects

- `FStaticConstructObjectParameters` and `FObjectInitializer` carry one explicit
  construction purpose. At minimum it distinguishes `RuntimeObject`,
  `ClassDefaultObject`, `AssetLoad`, and `Duplication`; callers cannot infer
  purpose from name, Outer, or thread state.
- The ordinary most-derived C++ constructor still runs for a class default so
  member initializers, superclass defaults, native fields, and canonical durable
  values remain identical to normal construction.
- Constructors may initialize local durable state and deterministic value-owned
  storage for a class default. They may not publish to global systems, submit
  render/RHI work, start tasks, register with a world or editor service, touch
  dirty/package state, inspect project content, perform I/O, or read time/random
  process state.
- Runtime resources and publication move to an existing later lifecycle boundary
  or a narrowly introduced activation boundary. A constructor-purpose check may
  guard unavoidable local setup, but scattered class-name or default-name checks
  are forbidden.
- `DMaterial` class-default construction keeps schema version, static
  properties, and canonical parameter definitions while allocating/publishing
  no render proxy. Ordinary construction, load, edit, and destruction retain
  their current render-proxy guarantees.

### Authored Default Parity

- Default comparison is logical and field-based. `memcmp`, C++ object bytes,
  vtable pointers, padding, container capacity, registration order, and runtime
  resources never participate.
- A new ordinary instance must be logically equal to its class default across
  every inherited field that the authored Archive exposes before caller edits or
  load data are applied. Transient runtime-only fields are excluded by the same
  explicit authored-field policy, not by ad hoc type checks.
- Constructor defaults that depend on a later repair hook are not deterministic
  class defaults. The audit must move durable initialization earlier or record a
  precise blocker.
- Non-null hard references and constructor-created owned inners are reported
  explicitly. GC safety may be implemented here, but their future omitted-value
  dependency semantics remain a gate for the v4 measurement/wire contract.

### Global Queries, GC, and Shutdown

- Template identity is centralized through object flags/query helpers. Asset,
  material, world, editor, duplication, reference-discovery, and diagnostic
  scans must state whether templates are included; default behavior preserves
  current live-instance results.
- A ready class default is retained for the lifetime of its owning class and its
  reflected plus declared hidden hard references remain visible to GC. Ordinary
  GC and hierarchy garbage requests cannot collect it.
- The class-default object itself is not made permanent past module safety.
  Teardown clears class ownership and releases defaults derived-first while all
  owning module code and referenced runtime services are still available.
- Default destruction completes before module unload. No default object's
  `BeginDestroy`, readiness polling, `FinishDestroy`, destructor, or referenced
  resource release may call already-unloaded code.
- Startup, dynamic module registration, failed initialization, normal shutdown,
  and test reset each have one explicit ownership path; leaked roots and
  process-lifetime intentional leaks are not accepted.

### Failure and Thread Policy

- Creation, lookup that may mutate state, release, and constructor-side-effect
  validation are game-thread operations. Read-only access to an already ready
  immutable default may occur on worker threads only after publication through
  the selected synchronization boundary.
- A missing, failed, or ineligible default returns a stable status/diagnostic;
  callers cannot silently fall back to zeroed memory or an arbitrary newly
  constructed instance.
- Failure before readiness removes any partial object from class ownership,
  object-array registration, roots, and owned temporary state before reporting
  the first failure.

## Current Foundations and Gaps

### Foundations

- Generated classes already expose stable qualified names, superclass links,
  class constructors, sizes, alignments, flags, and complete generated property
  chains.
- `ProcessNewlyLoadedDObjects()` already provides a registration-batch boundary
  after compiled-in classes, properties, enums, C++ packages, and GC schemas are
  finalized.
- `FObjectInitializer` already reaches every generated constructor, and
  `StaticConstructObject(...)` centralizes allocation, most-derived construction,
  Outer assignment, and object-array registration.
- `GDObjectArray`, object handles, roots, logical property equality, reflected
  GC schemas, and phased destruction provide reusable identity and lifetime
  machinery.
- The completed struct and Archive prerequisites provide logical equality and an
  exact authored-field view for nested values.

### Gaps to Close in Stage 0

- No eligibility contract distinguishes authored concrete classes from abstract,
  reflection-bootstrap, package, or other infrastructure classes.
- `FObjectInitializer` has no purpose, template flag, archetype, or publication
  state.
- `DClass` has no default-object pointer, creation state, ownership, immutable
  accessor, or release path.
- `EObjectFlags` cannot identify class defaults, and broad object-array scans
  treat every registered object as a live instance.
- The exact creation point for late-loaded module classes and the derived-first
  shutdown point are not selected.
- Material construction publishes runtime state; other current constructors have
  not yet been audited for I/O, tasks, global publication, time/random state,
  hard references, or owned inners.
- GC does not know that a `DClass` owns a default object, and current permanent-
  object rules have no releasable-template category.
- No test proves constructor/authored-field parity or class-default immutability.

## Implementation Stages

### Stage 0: Freeze eligibility, construction, ownership, and audit contracts

- [x] Inventory every current reflected class with its abstract/constructible,
  infrastructure/authored, superclass, constructor, authored-field, hard-
  reference, owned-inner, and runtime-side-effect disposition.
- [x] Freeze the exact eligibility predicate and deterministic diagnostic for
  every class that cannot own a default.
- [x] Freeze construction-purpose values, object flags, internal naming, Outer,
  object-array membership, creation states, base-before-derived ordering,
  registration-batch publication, and reentrancy/failure rollback.
- [x] Freeze logical authored-field parity, immutability, hard-reference, and
  owned-inner policies without defining DAST bytes.
- [x] Audit broad `GDObjectArray` consumers and classify each as live-only,
  template-aware, or diagnostics-only.
- [x] Audit startup, late module load, GC, object drain, render flush, module
  shutdown, and test reset to select one creation and one derived-first release
  boundary.
- [x] Audit `DMaterial` and all other eligible constructors for publication,
  allocation, world/editor registration, I/O, tasks, dirty state, time/random
  state, hard references, and owned inners; record the bounded repair owner.
- [x] Record baseline, working set, selected decisions, inventory, open
  questions, and validation in the stage handoff.

#### Acceptance Gate

- Every current reflected class and broad object-array consumer has one explicit
  disposition; no eligibility or template-filter behavior is inferred from
  names or incidental flags.
- Creation, publication, rollback, GC retention, and shutdown each have one
  selected owner and order, including late-loaded classes and failed batches.
- No unresolved decision can change public construction APIs, object identity,
  lifecycle ordering, authored parity, or the constructor repair working set.
- DAST v4 code and package bytes remain untouched.

#### Stage 0 Handoff

Baseline: `b8dbe9fa`.

Working set inspected: the CoreDObject construction, class, object-array,
Archive, GC, package, and registration-batch paths; AssetCore package discovery,
load, save, and object-graph paths; Launch and module startup/shutdown; every
production `DCLASS`; and every production constructor for an eligible class.
No DAST implementation or package-format code was changed.

##### Frozen Core Contract

- `EClassFlags` gains `Intrinsic` and `NoClassDefaultObject`. Intrinsic macros
  set the former; infrastructure declarations use the explicit
  `DCLASS(NoClassDefaultObject)` specifier. Eligibility is exactly: a valid
  class constructor and object layout, with none of `Abstract`, `Intrinsic`, or
  `NoClassDefaultObject`. Names, superclass identity, package, and constructor
  presence alone never infer authored eligibility.
- `DClass` exposes `EClassDefaultObjectState` with `Uninitialized`,
  `Constructing`, `Ready`, `Ineligible`, and `Failed`, plus a stable reason:
  `Abstract`, `Intrinsic`, `NoClassDefaultObject`, `MissingConstructor`,
  `InvalidLayout`, `MissingSuperclassDisposition`, `RecursiveConstruction`, or
  `ConstructionFailed`. Public access returns only `const DObject*`; only the
  CoreDObject lifecycle owner can mutate ownership state.
- `EObjectConstructionPurpose` is carried by both construction parameter
  structures and contains `RuntimeObject`, `ClassDefaultObject`,
  `ClassDefaultSubobject`, `AssetLoad`, and `Duplication`. `NewObject` selects
  `RuntimeObject`; AssetCore load and Archive duplication select their explicit
  purposes. A nested `CreateDefaultComponent` inherits
  `ClassDefaultSubobject` only from an active class-default transaction.
- `EObjectFlags` gains `ClassDefaultObject` and `DefaultSubobject`; both imply
  `Transient` and satisfy the centralized `IsTemplateObject()` query. A class
  default has Outer equal to its owning `DClass` and name
  `Default__<ShortClassName>`. A bounded default subobject has the class default
  as Outer and retains its constructor-requested fixed name. Both remain in
  `GDObjectArray`; neither is an asset-package member or a live instance.
- `ProcessNewlyLoadedDObjects()` remains the sole creation boundary. After
  classes, properties, reference schemas, enums, and C++ packages finish, the
  exact new batch is sorted by inheritance depth and then qualified class name.
  A superclass may be ready or have a final ineligible disposition, but may not
  still be uninitialized/constructing. Late module loads use the same path
  before `StartupModule()`.
- A batch construction transaction records every top-level default and bounded
  default subobject. Objects register normally for handle and reference
  validity, but `DClass` publishes no pointer until the entire batch succeeds.
  Ready state is release-published after the immutable pointer; worker lookup is
  acquire-only and never creates. Recursive lookup fails the owning class and
  its dependent construction. Any failure clears all new class pointers, marks
  the recorded hierarchy for derived-first disposal, collects it, and leaves
  no batch object or root published; pre-existing ready batches are unchanged.
- `DClass::AddReferencedObjects` reports its ready default as a native strong
  edge. Reflected hard references and the four bounded actor subobject graphs
  then use the existing reference schema. Ordinary garbage and hierarchy
  requests reject ready templates; only the CoreDObject release transaction may
  clear class ownership and mark them.
- The CoreDObject lifecycle owner exposes one derived-first release operation.
  Normal Launch calls it after engine/asset consumer detachment and before the
  first shutdown collection; initialization-failure paths call it before their
  collection. Tool hosts and test fixtures use a scoped/reset owner that invokes
  the same release before final collection. A ModuleManager pre-shutdown
  callback releases a still-owned module batch before `ShutdownModule()` and
  rejects unload if its objects cannot drain. Full process exit normally reaches
  that callback only as an empty validation because all defaults were released
  before `GC -> render flush -> GC`.

##### Authored Parity And Immutability

- Parity walks superclass-first, non-`Transient` reflected properties and uses
  `ArePropertyValuesIdentical`; arrays, maps, nested structs, hard/soft
  references, Names, and Guids therefore share the existing logical semantics.
  A bounded actor template graph is paired with an ordinary graph by
  Outer-relative name and class before its authored fields are compared.
  Runtime caches, handles, registration state, allocation identity, and native
  fields not exposed by the authored Archive are excluded.
- All audited hard-reference defaults are null/empty. This includes redirector
  destination, material-instance parent and texture parameters, static-mesh
  default material, component asset/material references, scene attachments,
  Level actors/camera, and Actor component collections before derived
  construction. A future non-null default is a deterministic eligibility
  failure until its authored-dependency policy is reviewed.
- Constructor-created inners are allowed only for the four inventoried actor
  defaults and only through `CreateDefaultComponent`: `AStaticMeshActor`,
  `ASkyBoxActor`, `ADirectionalLightActor`, and `ACameraActor`. Their fixed-name
  component templates carry `DefaultSubobject`, participate in transaction
  rollback/GC/release, and are not independently editable defaults. Any other
  constructor-created inner is a deterministic construction failure; this does
  not establish a general archetype or default-subobject graph.
- After Ready, mutable lookup is unavailable. Package dirtying, reparenting,
  load, save, duplication as an ordinary object, asset registration, and
  ordinary garbage requests reject templates with a stable diagnostic. Debug
  mutation guards cover the shared mutation entry points; direct native state
  is protected by the public const-only ownership boundary.

##### Reflected Class Inventory

Every production reflected class has one explicit disposition below. "Clean"
means member initialization, deterministic value-owned allocation, or an empty
constructor only; it does not exempt later lifecycle methods from template
guards.

| Class | Disposition | Authored/reference, inner, and constructor audit |
| --- | --- | --- |
| `DObject`, `DType`, `DStructBase`, `DClass`, `DStruct`, `DEnum` | Intrinsic; ineligible | Bootstrap/reflection metadata; intrinsic construction only. |
| `DPackage` | Infrastructure; ineligible | Package/registry owner, not an authored instance template; constructor clean. |
| `DEngine`, `DGameEngine`, `DEditorEngine` | Infrastructure; ineligible | Process/editor service owners. `DEditorEngine` allocates managers and publishes `GEditor`; later Init paths publish worlds, windows, modules, and render state. |
| `DWorld` | Infrastructure; ineligible | Runtime/session owner with transient Level reference; constructor clean and activation remains in later lifecycle calls. |
| `DTextureCubePreviewComponent` | Infrastructure; ineligible | Editor preview-only component with transient texture reference; constructor clean. |
| `DTexture` | Abstract; ineligible | Abstract asset base; constructor clean. |
| `DAssetRedirector` | Eligible | Authored destination hard reference defaults null; clean. |
| `DTexture2D`, `DTextureCube` | Eligible | Authored value data, no non-null hard default or inner; constructors perform one-time asset-delete-contributor publication and require the module-startup repair below. |
| `DStaticMesh` | Eligible | Authored mesh data and null default-material hard reference, no inner; constructor performs the same global contributor publication. |
| `DEnvironmentLighting` | Eligible | Authored payload metadata, no hard default or inner; clean. |
| `DMaterialInterface` | Eligible | No non-null authored hard default or inner; constructor allocates a render proxy and requires deferred runtime-resource creation. |
| `DMaterial` | Eligible | Deterministic schema/static properties and 56 canonical parameter definitions; constructor publishes proxy state. |
| `DMaterialInstance` | Eligible | Null parent hard reference and deterministic override containers; constructor publishes proxy state. |
| `DLevel` | Eligible | Authored Actor collection and primary-camera hard references default empty/null; clean. |
| `AActor` | Eligible | Authored root/owned/instance component references default empty/null; base constructor clean. |
| `AStaticMeshActor`, `ASkyBoxActor`, `ADirectionalLightActor`, `ACameraActor` | Eligible with bounded subobjects | Each constructs one fixed-name owned component and assigns its reflected hard reference/root; covered by the bounded contract above. |
| `DActorComponent` | Eligible | Native owner derives from Outer; no authored hard default/inner or publication. |
| `DSceneComponent` | Eligible | Authored attachment references default null/empty; implicit clean construction. |
| `DPrimitiveComponent`, `DMeshComponent`, `DDirectionalLightComponent`, `DCameraComponent` | Eligible | Deterministic member defaults; no hard default, owned inner, or constructor publication. |
| `DPhysicsComponent` | Eligible | Deterministic authored physics defaults; constructor changes local tick state and an owning actor's tick state when used as a subobject, with no external registration. |
| `DStaticMeshComponent` | Eligible | Static-mesh and material hard references default null/empty; clean. |
| `DSplineComponent` | Eligible | Builds deterministic value-owned evaluation data only; no task, I/O, registration, or hard default. |
| `DSkyBoxComponent` | Eligible | Texture hard reference defaults null; constructor generates Guid/atomic runtime IDs and requires deferred activation identity. |
| `DImportRecord` | Eligible | Authored fields contain no non-null hard reference or inner; constructor generates an authored `RecordId` and requires explicit factory assignment. |

##### Bounded Constructor Repairs

| Owner | Frozen repair |
| --- | --- |
| Engine material classes | The constructors use the explicit purpose: `DMaterialInterface` leaves its proxy null only for templates, while ordinary/load/duplication construction preserves current allocation. `DMaterial` and `DMaterialInstance` likewise skip constructor publication only for templates. |
| Engine texture/static-mesh module | Move the three asset-delete-contributor registrations out of instance constructors into one module-owned startup registration; object constructors retain only durable values. |
| `DSkyBoxComponent` | Use the explicit purpose to leave runtime Guid/instance ID empty only for templates; ordinary construction preserves current allocation. They remain excluded from authored parity. |
| AssetImportCore | Make the constructor's authored `RecordId` deterministically invalid. A dedicated import-record asset creation helper assigns `FGuid::NewGuid()` after ordinary construction; AssetLoad leaves it invalid until deserialization preserves the serialized identity. |
| Actor defaults | Route only `CreateDefaultComponent` through the bounded `ClassDefaultSubobject` transaction described above; ordinary construction/duplication keeps its existing fixed-name inner reuse. |

No audited constructor performs I/O, starts a task, marks a package dirty, or
registers with a World/editor service beyond the specific publications above.

##### Broad Object-Array Consumer Inventory

| Consumer | Disposition |
| --- | --- |
| `ObjectLifecycle.cpp`: mark, sweep, destruction ordering, deferred diagnostics | Template-aware lifecycle; includes templates, and only the release transaction may select a ready template. |
| `Class.cpp`: derived-class and reflected-type path lookup; `Archive.cpp` class-name lookup; `AssetPackageArchive.cpp` reflected-property lookup; `Package.cpp` intrinsic attachment | Reflection-metadata-only; explicitly selects reflected metadata and cannot return a template. |
| `Archive.cpp`: snapshot discovery and duplication gather/reuse | Live-only. Reject a template root/source and exclude templates from inner discovery. Duplication-created ordinary default components remain live and reusable. |
| `AssetSystem.cpp`: package graph gather and load-time existing-inner lookup | Live-only; class defaults/default subobjects never enter authored package discovery or collision checks. |
| `AssetPackageArchive.cpp`: package graph gather/freeze validation | Live-only; templates cannot affect saved object counts, dependencies, or graph stability. |

The centralized query policy is `EObjectQueryScope::{LiveOnly,
IncludeTemplates}` with no inferred class/name checks. Lifecycle/reflection code
states `IncludeTemplates`; asset, duplication, world, and editor code states
`LiveOnly`. Existing raw registry traversal remains an internal lifecycle
primitive rather than the default application query.

Open questions: none that can change Stage 1 public APIs, identity, ordering,
parity, or the bounded repair set. Stage 1 may choose private container types and
test-hook mechanics without changing this contract.

Validation: targeted symbol searches found 38 production reflected classes and
all production `GDObjectArray.GetAll/GetObjectsWithOuter` call sites. Constructor
definitions, generated registration order, AssetLoad/Duplication construction,
Launch success/failure shutdown, ModuleManager late-load/shutdown ordering, and
the GC mark/sweep rules were inspected against the tables above. Documentation-
only Stage 0 changed no C++ or package bytes.

### Stage 1: Implement core class-default construction and lifetime

- [x] Add construction purpose and class-default/template flags with focused
  generated-constructor and manual-constructor coverage.
- [x] Add `DClass` eligibility/status, immutable access, ownership, creation, and
  failure diagnostics without exposing mutable defaults.
- [x] Create eligible defaults after registration finalization in deterministic
  base-before-derived order and prove repeated lookup returns one identity.
- [x] Register defaults through the selected `GDObjectArray` path, retain them
  through normal GC, reject ordinary garbage/dirty/save operations, and clean up
  every injected partial-construction failure.
- [x] Add focused tests for abstract/infrastructure exclusions, late module
  registration, recursive construction, duplicate creation, failed construction,
  worker read access after publication, and mutation rejection.
- [x] Record baseline, working set, symbols, decisions, open questions, and
  validation in the stage handoff.

#### Acceptance Gate

- Every eligible class owns exactly one ready immutable default after its
  registration batch; every ineligible or failed class reports the frozen status.
- Class defaults carry unambiguous template identity, never appear as ordinary
  asset/world/editor instances, and survive ordinary GC with all hard references
  intact.
- Construction ordering and injected failures leave no partial class pointer,
  object-array entry, root, owned inner, or leaked allocation.

#### Stage 1 Handoff

Baseline: `bbb7ecf3` (`docs(cdo): freeze lifecycle contracts`).

Working set: DurinHeaderTool class parsing/source emission and fixtures;
CoreDObject construction, class metadata, object flags, Archive, package and GC
lifecycle; AssetCore load construction; actor default-component construction;
the bounded material, sky-box, and import-record constructor guards; Launch
shutdown integration; and `ReflectionTypeTests.cpp`.

Key symbols and decisions:

- `EObjectConstructionPurpose`, `EObjectFlags::ClassDefaultObject`,
  `EObjectFlags::DefaultSubobject`, and `DObject::IsTemplateObject()` carry the
  frozen identity through generated, manual, load, duplication, and nested
  component construction.
- `DCLASS(NoClassDefaultObject)` and intrinsic macros emit the explicit
  eligibility flags. The six inventoried service/infrastructure classes,
  including `DTextureCubePreviewComponent`, use the opt-out rather than name
  inference.
- `DClass::GetDefaultObject()` is const-only and acquire-published. The private
  batch creator expands superclass chains, sorts base-first then qualified name,
  publishes only after the batch completes, and rolls back the complete template
  hierarchy on recursive or dependent failure.
- Ready defaults remain ordinary `GDObjectArray` entries and are retained by
  `DClass::AddReferencedObjects`. Ordinary root, garbage, hierarchy-garbage,
  rename, reparent, package-dirty, save, duplicate, and package-asset operations
  reject templates; only the lifecycle owner can release and collect them.
- `ReleaseClassDefaultObjects()` clears ownership derived-first. Launch invokes
  it on success and initialization failure; Stage 3 still owns module-unload
  integration and full shutdown-order qualification.

Focused validation: all 189 DurinHeaderTool tests passed; all 77
`CoreObjectTests` passed; all 77 `MaterialTests` passed with its declared
900-second allowance; and the `Win64-Debug-DurinEditor-Tests` full `all` build
passed. The recursive failure fixture proved removal and destruction of both the
top-level default and its registered default subobject.

Open questions: none that change Stage 2's frozen parity or side-effect
contract. Stage 2 must still move the remaining audited registrations/identity
generation out of template construction, centralize broad live-instance query
filtering, and prove authored-field/default-subobject parity.

### Stage 2: Separate runtime side effects and prove default parity

- [x] Repair `DMaterialInterface`/`DMaterial` so class-default construction keeps
  canonical authored data but creates or publishes no render proxy; preserve
  ordinary create/load/edit/destroy behavior.
- [x] Apply only the bounded repairs identified by the Stage 0 constructor audit
  to other eligible classes.
- [x] Update every audited broad object query to apply the centralized template
  policy and add regression tests for unchanged live-instance counts and results.
- [x] Add recursive logical parity tests for inherited scalars, enums, strings,
  Names, Guids, structs, arrays, maps, hard/soft references, and native named
  fields exposed to authored Archives.
- [x] Add immutability, deterministic repeated-startup, different registration-
  order, and constructor-side-effect counter tests.
- [x] Record baseline, working set, symbols, decisions, open questions, and
  validation in the stage handoff.

#### Acceptance Gate

- Every eligible new ordinary object is logically equal to its class default
  across the complete authored field view before caller edits or load data.
- Creating all class defaults produces zero render/RHI submissions, tasks, I/O,
  dirty packages, asset registrations, world/component registrations, and
  editor publications.
- Material runtime proxies and every other repaired runtime resource retain
  existing ordinary create/load/edit/release behavior and focused tests.
- Live-instance queries return the same results as the pre-CDO baseline unless
  their documented purpose explicitly includes templates.

#### Stage 2 Handoff

Baseline: `d56b4de5` (`feat(cdo): add class default object lifecycle`).

Working set: CoreDObject object-array query APIs and all audited consumers;
Engine material, texture, static-mesh, spline, and sky-box construction;
AssetImportCore import-record creation; Launch, DurinAssetTool, and Engine test
initialization; CoreDObject, AssetCore, material, and sky-box regression tests.

Key symbols and decisions:

- `EObjectQueryScope::{LiveOnly, IncludeTemplates}` is mandatory on broad
  `GDObjectArray` enumeration and Outer queries. Asset graphs, duplication,
  loaded-material queries, and render-state recreation use `LiveOnly`;
  reflection metadata and lifecycle/GC explicitly use `IncludeTemplates`.
- Template material interfaces own no render proxy and publish no proxy state;
  ordinary create/load/duplicate construction is unchanged. Loaded-material
  dependency queries cannot return or count class defaults.
- `InitializeEngineAssetServices()` owns the three texture/static-mesh delete
  contributor registrations. Their instance constructors are now value-only.
- `DImportRecord` starts with an invalid identifier. The dedicated
  `CreateImportRecordAsset()` assigns a fresh Guid after ordinary asset
  construction, while load and duplication restore serialized identity.
- Sky-box template identities remain empty; ordinary identities remain unique.
  The production parity scan also exposed a Stage 0 audit error: default spline
  points generated random Guids. The bounded correction gives the two built-in
  default points stable curve-local identities; subsequently added points keep
  their existing unique Guid generation.
- Batch construction now permits a successfully constructed, unpublished
  superclass from the same transaction and checks recursive access across every
  constructed class. All intrinsic metadata classes receive a final explicit
  ineligible disposition on every registration boundary.

Parity and determinism evidence: the production sweep requires exactly 38
`Durin::` reflected classes and 25 Ready eligible defaults, creates fresh
ordinary objects in reverse qualified-name order, pairs bounded subobjects by
Outer-relative name/class, and compares every non-transient reflected field.
Core tests separately cover native named Archive fields, reverse batch input,
repeat creation identity, template query exclusion, mutation rejection, and
recursive top-level-plus-inner rollback.

Validation: `CoreObjectTests` 79/79, `MaterialTests` 78/78,
`AssetImportCoreTests` 23/23, `AssetPackageTests` 81/81, `TextureTests` 62/62,
`StaticMeshTests` 44/44, and `SkyBoxTests` 10/10 passed. The
`Win64-Debug-DurinEditor-Tests` full `all` build passed. A mistakenly parallel
Texture/StaticMesh invocation was rejected by the checkout lock; the reported
Texture recovery completed successfully before both suites were rerun serially.

Open questions: none for the Stage 2 contract. Stage 3 still owns module batch
release before unload, shutdown/reset coverage, lasting architecture docs, the
complete focused/full validation matrix, and editor smoke evidence.

### Stage 3: Complete teardown, document, and qualify the prerequisite

- [x] Implement derived-first class-default release at the Stage 0-selected
  shutdown boundary; clear class ownership, release roots, drain destruction,
  and prove no virtual cleanup runs after module unload.
- [x] Cover normal shutdown, initialization failure, late-loaded classes, module
  shutdown ordering, deferred destruction, render-resource release, and test
  reset with deterministic diagnostics.
- [x] Update Reflection System, Garbage Collection, and Runtime Lifecycle with
  the lasting eligibility, construction, template-query, ownership, GC, and
  release contracts.
- [x] Update the compact-serialization roadmap with completion evidence and the
  measured/default-audit inputs required to activate the v4 measurement and wire
  contract child plan.
- [x] Run focused DHT, CoreDObject construction/Archive/GC/duplication, AssetCore
  package, Engine material/world/component, and editor workflow suites under the
  documented Agent Build Profile.
- [x] Complete a successful full `all` build because the CoreDObject public ABI,
  generated constructors, Engine runtime objects, GC, and editor consumers change
  together.
- [x] Perform a representative editor open/create/load/save/unload/restart smoke
  and verify class-default creation causes no package or registry rewrite.
- [x] Record final baseline, working set, symbols, decisions, constructor audit,
  open questions, focused/full validation, editor smoke, and next-plan handoff.

#### Acceptance Gate

- All eligible classes have deterministic immutable defaults; all excluded
  classes have stable evidence-backed diagnostics, and no constructor-side-effect
  blocker remains for v4 measurement.
- GC and derived-first shutdown release every default and referenced resource
  before module unload with no roots, objects, tasks, render work, or deferred
  destruction left behind.
- Lasting documentation, focused suites, full build, and editor smoke agree, and
  the v4 measurement/wire-contract entry gate is satisfied without changing any
  package byte.

#### Stage 3 Final Handoff

Baseline: `98826af0` (`fix(cdo): isolate template construction side effects`).

Working set: Core module-manager pre-shutdown hooks; CoreDObject module-scoped
release/drain and late/deferred lifecycle tests; Launch success/failure shutdown;
Reflection System, Garbage Collection, and Runtime Lifecycle documentation; this
plan; and the Compact Asset Serialization roadmap.

Final symbols and ordering:

- `FModuleManager::SetPreShutdownModuleCallback(...)` runs before a ready
  module changes state or enters `ShutdownModule()`. CoreDObject installs
  `ReleaseClassDefaultObjectsForModule(...)`, which selects classes through
  `/Cpp/<Module>`, clears ownership derived-first, marks the template hierarchy,
  collects synchronously, and rejects shutdown/unload while any module template
  remains registered.
- A rejected deferred drain is retryable even after `DClass` ownership was
  cleared: the module callback finds remaining template objects through their
  Outer-to-`DClass` chain. Tests exercise release rejection, readiness
  completion, retry, and complete removal; ModuleManager keeps a rejected module
  Ready and returns before its shutdown callback or native unload.
- Normal and initialization-failure Launch paths release all defaults before the
  first shutdown collection. Reverse module shutdown therefore normally performs
  an empty validation; direct late unload uses the same scoped transaction.
- Lasting eligibility, construction-purpose, query-scope, GC ownership,
  immutability, and shutdown contracts now live in their owning Runtime docs.
  The compact roadmap records 38 production classes, 25 eligible Ready defaults,
  and a satisfied v4 measurement-plan entry gate.

Final validation under `Win64-Debug-DurinEditor-Tests`: DurinHeaderTool 189/189;
`CoreObjectTests` 79/79; `AssetImportCoreTests` 23/23;
`AssetPackageTests` 81/81; `MaterialTests` 78/78; `TextureTests` 62/62;
`StaticMeshTests` 44/44; `SkyBoxTests` 10/10; `WorldTests` 62/62;
`EditorPropertyTests` 27/27; `EditorAssetWorkflowTests` 69 passed of 70 with one
intentional skip; and `RHIInitializationTests` 4/4. The full `all` build passed.

Editor smoke: `Sandbox/Sandbox.dproject` completed two hidden three-tick editor
runs through normal `FEngineLoop::Exit()`, with no error, assertion,
class-default failure, or deferred-object diagnostic in either log. SHA-256 for
all 17 authored `.dasset` files plus `Sandbox`'s asset `Registry.bin` matched
before and after both runs. Editor create/load/save/unload behavior was also
covered by `EditorAssetWorkflowTests`; no tracked or hashed package byte changed.

Constructor audit correction: the only Stage 0 miss was the two random default
spline-point Guids, repaired in Stage 2 with stable curve-local defaults. The
final production parity sweep and side-effect suites found no remaining blocker.

Open questions: none for this prerequisite. Next work is a new bounded v4
measurement and wire-contract plan; production v4 reader/writer work remains
deferred until that plan freezes recursive byte accounting and the wire model.

## Validation Matrix

| Area | Required evidence |
| --- | --- |
| Eligibility | Complete reflected-class inventory; deterministic authored/infrastructure, abstract/concrete, constructor, reference, inner, and side-effect dispositions |
| Construction context | Purpose reaches generated/manual constructors; no name/Outer/thread inference; base-before-derived and late-batch ordering |
| Identity | One immutable default per eligible class, stable internal name/Outer/flags, repeated lookup identity, no collision with ordinary default instance names |
| Failure | Recursive/duplicate construction, missing superclass, injected failure, rollback, and stable status without partial publication |
| Parity | Complete inherited authored-field logical equality across scalar, enum, string, Name, Guid, struct, container, reference, and native field kinds |
| Side effects | Zero render/RHI, task, I/O, dirty, package/asset, world/component, or editor publication during class-default creation |
| Queries | Live-only scans exclude templates; explicit diagnostics can include them; pre-CDO result/count parity |
| GC | Default and hard-reference reachability, garbage rejection, handles, roots/ownership, hidden references, and no ordinary collection |
| Shutdown | Derived-first release, initialization failure, deferred destruction, render flush, module order, and zero leaked roots/objects/resources |
| Integration | DHT, CoreDObject, AssetCore packages, Engine materials/world/components, editor create/load/save/restart, and unchanged DAST v3 bytes |

Build and test execution follows [Build and Run](../Development/Build/BuildAndRun.md)
and [Native Tests](../Development/Build/NativeTests.md).

## Definition of Done

- Every eligible concrete reflected class owns one immutable deterministic class
  default object created after complete reflection finalization.
- Construction purpose and template identity are explicit; constructors retain
  durable defaults while class-default creation performs no runtime publication
  or external side effect.
- Ordinary objects begin with authored values logically equal to their class
  defaults, and all broad object queries preserve their intended live/template
  semantics.
- Class defaults and their references are GC-safe, cannot enter ordinary asset
  workflows, and are released derived-first before owning modules unload.
- Focused suites, full build, editor smoke, lasting documentation, and the compact
  roadmap handoff pass without changing DAST v3 bytes or activating v4 code.

## Deferred Follow-ups

- Recursive struct default storage, no-delta comparison, forced-override runtime
  provenance, and default-relative package behavior.
- DAST v4 recursive byte accounting, exact wire contract, reference fixtures,
  production writer/reader, compatibility, migration, and content rollout.
- Mutable/editor-authored defaults, arbitrary archetype chains, default-subobject
  instancing, hot reload, class reinstancing, and Blueprint-style generated
  classes.
- Non-null hard-reference defaults whose implicit dependency semantics require a
  reviewed authored-format contract.

## Related Documentation

- [Compact Asset Serialization Roadmap](../Roadmaps/Archive/2026-08/CompactAssetSerialization.md)
- [Reflection System](../Runtime/Core/ReflectionSystem.md)
- [Garbage Collection](../Runtime/Core/GarbageCollection.md)
- [Runtime Lifecycle](../Runtime/Core/RuntimeLifecycle.md)
- [Build and Run](../Development/Build/BuildAndRun.md)
- [Native Tests](../Development/Build/NativeTests.md)
- [Unified Archive Serialization Plan](Archive/2026-08/UnifiedArchiveSerialization.md)
- [Reflected Struct Operations Plan](Archive/2026-08/ReflectedStructOperations.md)

## Related Code

- `Engine/Source/Runtime/CoreDObject/Public/DObject/DObjectGlobals.h`
- `Engine/Source/Runtime/CoreDObject/Private/DObject/DObjectGlobals.cpp`
- `Engine/Source/Runtime/CoreDObject/Public/DObject/ObjectMacros.h`
- `Engine/Source/Runtime/CoreDObject/Public/DObject/Object.h`
- `Engine/Source/Runtime/CoreDObject/Private/DObject/Object.cpp`
- `Engine/Source/Runtime/CoreDObject/Public/DObject/Class.h`
- `Engine/Source/Runtime/CoreDObject/Private/DObject/Class.cpp`
- `Engine/Source/Runtime/CoreDObject/Private/DObject/ObjectLifecycle.cpp`
- `Engine/Source/Runtime/Engine/Public/Materials/MaterialInterface.h`
- `Engine/Source/Runtime/Engine/Private/Materials/MaterialInterface.cpp`
- `Engine/Source/Runtime/Engine/Private/Materials/Material.cpp`
- `Engine/Tests/Native/CoreDObjectTests/Private/ReflectionTypeTests.cpp`
- `Engine/Tests/Native/CoreDObjectTests/Private/GarbageCollectionTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/Materials/MaterialRenderingTests.cpp`
