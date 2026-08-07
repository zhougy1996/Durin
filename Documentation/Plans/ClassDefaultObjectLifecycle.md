# Class Default Object Lifecycle Plan

Summary: Give eligible reflected classes one immutable deterministic default object without triggering ordinary runtime publication or activation.

Last reviewed: 2026-08-08

Status: Active
Completed:

## Current Status

- Stage 0 is active from baseline commit `b8dbe9fa`. No class-default-object
  implementation exists yet; `DClass` stores only a display/instance default
  name and `FObjectInitializer` carries only class, Outer, name, and storage.
- This plan is a prerequisite for compact asset serialization. The DAST v4
  measurement/wire-contract plan is deliberately not active until this plan's
  exit gate passes.
- Current construction always enters `StaticConstructObject(...)`, runs the
  generated class constructor, assigns Outer, and registers the object in
  `GDObjectArray`. There is no template construction purpose or flag, and broad
  object-array scans cannot distinguish a default template from a live instance.
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

- [ ] Inventory every current reflected class with its abstract/constructible,
  infrastructure/authored, superclass, constructor, authored-field, hard-
  reference, owned-inner, and runtime-side-effect disposition.
- [ ] Freeze the exact eligibility predicate and deterministic diagnostic for
  every class that cannot own a default.
- [ ] Freeze construction-purpose values, object flags, internal naming, Outer,
  object-array membership, creation states, base-before-derived ordering,
  registration-batch publication, and reentrancy/failure rollback.
- [ ] Freeze logical authored-field parity, immutability, hard-reference, and
  owned-inner policies without defining DAST bytes.
- [ ] Audit broad `GDObjectArray` consumers and classify each as live-only,
  template-aware, or diagnostics-only.
- [ ] Audit startup, late module load, GC, object drain, render flush, module
  shutdown, and test reset to select one creation and one derived-first release
  boundary.
- [ ] Audit `DMaterial` and all other eligible constructors for publication,
  allocation, world/editor registration, I/O, tasks, dirty state, time/random
  state, hard references, and owned inners; record the bounded repair owner.
- [ ] Record baseline, working set, selected decisions, inventory, open
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

### Stage 1: Implement core class-default construction and lifetime

- [ ] Add construction purpose and class-default/template flags with focused
  generated-constructor and manual-constructor coverage.
- [ ] Add `DClass` eligibility/status, immutable access, ownership, creation, and
  failure diagnostics without exposing mutable defaults.
- [ ] Create eligible defaults after registration finalization in deterministic
  base-before-derived order and prove repeated lookup returns one identity.
- [ ] Register defaults through the selected `GDObjectArray` path, retain them
  through normal GC, reject ordinary garbage/dirty/save operations, and clean up
  every injected partial-construction failure.
- [ ] Add focused tests for abstract/infrastructure exclusions, late module
  registration, recursive construction, duplicate creation, failed construction,
  worker read access after publication, and mutation rejection.
- [ ] Record baseline, working set, symbols, decisions, open questions, and
  validation in the stage handoff.

#### Acceptance Gate

- Every eligible class owns exactly one ready immutable default after its
  registration batch; every ineligible or failed class reports the frozen status.
- Class defaults carry unambiguous template identity, never appear as ordinary
  asset/world/editor instances, and survive ordinary GC with all hard references
  intact.
- Construction ordering and injected failures leave no partial class pointer,
  object-array entry, root, owned inner, or leaked allocation.

### Stage 2: Separate runtime side effects and prove default parity

- [ ] Repair `DMaterialInterface`/`DMaterial` so class-default construction keeps
  canonical authored data but creates or publishes no render proxy; preserve
  ordinary create/load/edit/destroy behavior.
- [ ] Apply only the bounded repairs identified by the Stage 0 constructor audit
  to other eligible classes.
- [ ] Update every audited broad object query to apply the centralized template
  policy and add regression tests for unchanged live-instance counts and results.
- [ ] Add recursive logical parity tests for inherited scalars, enums, strings,
  Names, Guids, structs, arrays, maps, hard/soft references, and native named
  fields exposed to authored Archives.
- [ ] Add immutability, deterministic repeated-startup, different registration-
  order, and constructor-side-effect counter tests.
- [ ] Record baseline, working set, symbols, decisions, open questions, and
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

### Stage 3: Complete teardown, document, and qualify the prerequisite

- [ ] Implement derived-first class-default release at the Stage 0-selected
  shutdown boundary; clear class ownership, release roots, drain destruction,
  and prove no virtual cleanup runs after module unload.
- [ ] Cover normal shutdown, initialization failure, late-loaded classes, module
  shutdown ordering, deferred destruction, render-resource release, and test
  reset with deterministic diagnostics.
- [ ] Update Reflection System, Garbage Collection, and Runtime Lifecycle with
  the lasting eligibility, construction, template-query, ownership, GC, and
  release contracts.
- [ ] Update the compact-serialization roadmap with completion evidence and the
  measured/default-audit inputs required to activate the v4 measurement and wire
  contract child plan.
- [ ] Run focused DHT, CoreDObject construction/Archive/GC/duplication, AssetCore
  package, Engine material/world/component, and editor workflow suites under the
  documented Agent Build Profile.
- [ ] Complete a successful full `all` build because the CoreDObject public ABI,
  generated constructors, Engine runtime objects, GC, and editor consumers change
  together.
- [ ] Perform a representative editor open/create/load/save/unload/restart smoke
  and verify class-default creation causes no package or registry rewrite.
- [ ] Record final baseline, working set, symbols, decisions, constructor audit,
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

- [Compact Asset Serialization Roadmap](../Roadmaps/CompactAssetSerialization.md)
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
