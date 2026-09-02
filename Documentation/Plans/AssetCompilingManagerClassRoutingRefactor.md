# Asset Compiling Manager Class Routing Refactor Plan

Summary: Replace asset-compilation domains with Engine-owned typed compiling managers registered by reflected asset class while preserving asynchronous behavior and the TextureBuild recipe boundary.

Last reviewed: 2026-09-02

Status: Completed
Completed: 2026-09-02

## Current Status

The class-routing architecture is implemented and validated:

- `FAssetCompilingManager` remains the process aggregate for frame pumping,
  selected-object finish and cancellation, aggregate progress, successful
  post-compile events, and shutdown placement.
- `IAssetCompilationDomain`, domain dependency declarations, domain-name
  lookup, and the domain dependency graph are removed.
- A narrow `IAssetCompilingManager` lifecycle and dispatch contract remains so
  the aggregate does not hard-code every asset family.
- Registrations map one or more reflected asset classes to one typed compiling
  manager. Object operations resolve the nearest registered class in the
  object's superclass chain and dispatch only to that manager.
- Engine owns `FTextureCompilingManager` and `FMaterialCompilingManager`.
  `DTexture2D` and `DMaterial` are their initial registered classes.
- TextureBuild remains a synchronous value-only recipe provider. This refactor
  does not move task ownership, DDC orchestration, live-object access, or
  result publication into TextureBuild.
- `AssetCompilingManagerTests` and the complete `test affected` selection pass
  on `MacOS-arm64-Debug-DurinEditor`; the same preset's full `all` target builds.
- Engine retains only its existing optional `DerivedDataCache` edge and no
  `TextureBuild` dependency, so runtime/game module selection remains unchanged.

## Goal

Make asset-compilation routing express the actual ownership relationship:
asset class to typed compiling manager. Remove the artificial domain model and
its broadcast filtering while retaining one aggregate lifecycle, bounded frame
processing, module-safe registration, and family-specific compiler state.

## Scope

This plan changes the Engine asset-compilation aggregate, Material compilation,
Texture2D compilation, their tests, and the authoritative asset-compilation
contract. It establishes the registration seam that later Engine-owned mesh or
terrain compiling managers may use.

The following are not part of this plan:

- changing Texture2D derived-data keys, payloads, cache policy, recipe output,
  or provider feature names;
- moving Texture algorithms, asynchronous queues, DDC orchestration, or object
  publication across the Engine/TextureBuild boundary;
- making TextureCube or VolumeTexture asynchronous;
- migrating StaticMesh, SkeletalMesh, AnimationClip, or Terrain build paths;
- creating a typeless job record, shared compiler state bag, or global asset
  status enum;
- preserving general cross-compiler dependencies that have no production
  caller.

## Selected Architecture

### Registry model

The aggregate keeps two distinct registries:

- a compiler registry keyed by a canonical compiler name, containing the
  `IAssetCompilingManager`, lifecycle state, generation, and optional module
  callback gate/resource lease;
- a class route table mapping `DClass*` to the owning compiler registration.

One registration describes one compiler and all of its root asset classes.
Registering several classes for one compiler must not create several lifecycle
entries: that compiler starts, receives each process pass, finishes all work,
and shuts down exactly once.

Initial built-in registrations are equivalent to:

```cpp
RegisterBuiltInCompiler({
	.Name = FName("Durin.Material"),
	.AssetClasses = {DMaterial::StaticClass()},
	.Manager = MaterialCompilingManager});

RegisterBuiltInCompiler({
	.Name = FName("Durin.Texture"),
	.AssetClasses = {DTexture2D::StaticClass()},
	.Manager = TextureCompilingManager});
```

TextureCube and VolumeTexture are not registered until they own pending
compilation work. If they later become asynchronous, their exact classes may
be added to the existing Texture compiler registration without adding another
Texture compiler lifecycle.

### Registration rules

- Compiler names are canonical, unique diagnostic identities. They no longer
  define routing or dependency domains.
- Every registered class is non-null and unique within the submitted
  registration.
- Two live registrations cannot claim the same exact class. A conflicting
  registration fails atomically without changing existing routes.
- A derived class may register a different compiler from an ancestor. Exact
  class lookup wins, followed by the nearest registered superclass.
- A registration either publishes all class routes after its compiler starts
  successfully or publishes none of them.
- External registrations retain the existing module-owned callback gate and
  resource-lease guarantees. Retirement removes routes from future snapshots,
  stops admission, finishes accepted work, shuts the compiler down, destroys
  retained values, and only then releases the owner resource.
- Route resolution initially walks `DClass::GetSuperClass()` under the registry
  lock. No derived-class cache is introduced until measurement justifies its
  invalidation complexity.

This follows the reflected-class routing pattern already used by Engine cook
contributors while adding compiler lifecycle and module-retirement ownership.

### Aggregate dispatch

`ProcessAsyncTasks`, aggregate progress, and `FinishAllCompilation` iterate a
snapshot of unique live compilers rather than the class route table. The
existing bounded first opportunity, round-robin fairness, unused-quota
reclamation, and completion deadline behavior remain unchanged.

Selected-object finish and cancellation use this sequence:

1. Ignore null or invalid objects.
2. Resolve each object's exact class and then its superclass chain.
3. Group resolved objects by compiler while preserving their input order.
4. Invoke each resolved compiler once with only the objects routed to it.
5. Coalesce successful weak-object reports and publish events outside compiler
   callbacks and the registry mutex.

An object with no registered compiler is a no-op for finish and cancellation.
Compilers no longer receive every object and no longer perform top-level
family filtering merely to discover ownership.

Compilers are independent. Their deterministic canonical-name order replaces
the unused production dependency graph; shutdown uses reverse order. A future
real cross-compiler ordering requirement must define an explicit aggregate
policy rather than reintroducing a general domain DAG speculatively.

### Public contract and terminology

The shared interface is named `IAssetCompilingManager`. It contains only the
operations required by `FAssetCompilingManager`: start, stop admission,
remaining count, bounded completion processing, selected finish, selected
cancellation, finish all, and shutdown.

Domain terminology is replaced consistently:

| Current | Target |
| --- | --- |
| `IAssetCompilationDomain` | `IAssetCompilingManager` |
| `FAssetCompilationDomainRegistration` | `FAssetCompilingManagerRegistration` |
| `RegisterDomain` | `RegisterCompiler` |
| `FAssetCompileDomainDiagnostics` | `FAssetCompilerDiagnostics` |
| `DomainCount`, `Domains` | `CompilerCount`, `Compilers` |
| `FAssetPostCompileData::DomainName` | `CompilerName` |
| `FindDomain` | removed |

The compiler name remains in diagnostics and post-compile events because one
compiler may serve multiple reflected classes. A `DClass*` route is not used as
compiler identity.

### Typed manager ownership

`FTexture2DCompilationDomain` becomes `FTextureCompilingManager`. Its current
Texture2D queue, object records, worker admission, priority fairness, byte
budget, cancellation, mailbox, request serials, completion callbacks,
GameThread result application, and diagnostics remain typed and behaviorally
unchanged. Public `FTexture2DCompilation*` request/result APIs retain their
family-specific names.

Texture submission and diagnostics use a private typed Texture-manager access
path established during aggregate initialization. They do not recover the
manager through a string lookup or `dynamic_cast`. The aggregate owns the
built-in manager lifetime; callers observe unavailable/not-started state
without constructing a second manager.

`FMaterialCompilationDomain` similarly becomes
`FMaterialCompilingManager`. Material submission and cancellation access that
typed manager directly, removing the current `FindDomain` plus `dynamic_cast`
path while preserving program-identity single-flight, consumer accounting,
last-known-good visibility, and Renderer publication.

### TextureBuild boundary

`FTextureCompilingManager` invokes the existing synchronous
`ITexture2DBuildProvider` from an Engine-owned worker. Engine continues to own
the request serial, input identity, DDC lookup/store orchestration, provider
invocation gate, completion admission, and object publication. TextureBuild
continues to own pixel-format selection, mip generation, alpha processing,
compression, recipe metrics, and producer version identity.

No provider callback, task, object pointer, module-owned deleter, or borrowed
request value may escape the synchronous provider invocation.

## Implementation Stages

### Stage 0: Freeze routing and behavior baselines

Dependencies: none.

- [x] Inventory every production and test use of `IAssetCompilationDomain`,
  registration, dependency declarations, domain lookup, domain diagnostics,
  and post-compile domain identity.
- [x] Confirm that no production compiler declares a dependency and no
  production external module registers a compilation domain; record any
  exception before removing the graph.
- [x] Freeze existing Material and Texture2D pending-count, bounded-pump,
  finish-selected, cancellation, finish-all, success-event, startup, and
  shutdown behavior with focused tests.
- [x] Add or identify test object subclasses needed to prove exact-class,
  superclass fallback, derived override, and unregistered-class behavior.
- [x] Record the current runtime/game and editor/developer module closures so
  the refactor cannot introduce a TextureBuild or DerivedDataCache dependency
  into runtime/game configurations.

#### Acceptance Gate

- Every removed domain feature has a known caller or explicit no-caller
  result, and every behavior retained by later stages has executable baseline
  evidence.

### Stage 1: Introduce compiler and class-route registries

Dependencies: Stage 0.

- [x] Replace the domain interface and registration vocabulary with the narrow
  `IAssetCompilingManager` contract and compiler registration descriptor.
- [x] Split internal compiler lifecycle entries from `DClass*` routes, with
  atomic validation, generation-safe removal, and one lifecycle per compiler.
- [x] Implement exact-class then nearest-superclass resolution and batch
  selected objects by resolved compiler.
- [x] Preserve bounded process fairness, deadline handling, result coalescing,
  callback-gate entry, post-compile dispatch outside locks, and terminal
  shutdown behavior over unique compiler snapshots.
- [x] Remove dependency collection, missing-dependency diagnostics, cycle
  checks, dependency levels, and topological ordering.
- [x] Rewrite aggregate tests for duplicate classes, duplicate names,
  superclass fallback, derived override, unregistered objects, multi-class
  deduplication, registration rollback, and module retirement.

#### Acceptance Gate

- Object operations invoke only their resolved compiler; a compiler serving
  multiple classes receives one lifecycle and process invocation; conflicts
  fail without partial routes; and aggregate scheduling/lifetime tests pass.

### Stage 2: Establish `FTextureCompilingManager`

Dependencies: Stage 1.

- [x] Rename the Texture2D domain implementation, configuration, private files,
  forward declarations, friend declarations, factories, and diagnostics to
  the Texture compiling-manager model without renaming public family-specific
  request/result types.
- [x] Register `DTexture2D::StaticClass()` with compiler identity
  `Durin.Texture`; do not register `DTexture`, `DTextureCube`, or
  `DVolumeTexture` speculatively.
- [x] Replace the global domain lookup/factory path with one aggregate-owned,
  private typed manager access path used by submit, wait, cancel, and
  diagnostic operations.
- [x] Preserve worker detachment, request-serial latest-wins admission,
  completion ordering, exactly-once terminal callbacks, bounded retained
  diagnostics, and shutdown quiescence.
- [x] Prove unchanged cold build, warm DDC hit, failure, cancellation,
  supersession, provider unavailable/ambiguous, object destruction, and
  TextureBuild unload/reload behavior.

#### Acceptance Gate

- Texture2D behavior and derived-data compatibility are unchanged, all Texture
  object-aware asynchronous ownership remains in Engine, and no production
  Texture symbol uses domain terminology.

### Stage 3: Establish `FMaterialCompilingManager`

Dependencies: Stage 1.

- [x] Rename the private Material domain to `FMaterialCompilingManager` and
  register `DMaterial::StaticClass()` with compiler identity
  `Durin.Material`.
- [x] Replace `FindDomain` and `dynamic_cast` state recovery with the private
  typed Material-manager access path.
- [x] Preserve shared-program single-flight, consumer accounting, authored and
  dependency freshness, cancellation, reload requests, last-known-good
  behavior, cooked-program rules, and Renderer publication.
- [x] Prove Material subclasses route through the registered base class and an
  unrelated object never enters Material finish or cancellation code.

#### Acceptance Gate

- Material compilation passes its lifecycle and rendering tests without
  generic lookup, runtime cast, broadcast filtering, or domain terminology.

### Stage 4: Remove compatibility residue and qualify the architecture

Dependencies: Stages 2 and 3.

- [x] Remove `FindDomain`, old registration handles, domain factories, domain
  globals, dependency helpers, compatibility aliases, obsolete tests, and all
  remaining production references to compilation domains.
- [x] Rename aggregate diagnostics and post-compile fields, then update every
  listener and test to use compiler identity.
- [x] Update the authoritative Asset Compilation contract and directly related
  runtime-lifecycle text to describe class routing and typed managers; keep
  completed historical plans unchanged except for required link maintenance.
- [x] Run focused aggregate, Texture, Material, cook, and affected rendering
  tests according to the repository testing workflow.
- [x] Build affected Engine, Launch, editor/developer, and runtime/game closures
  according to the repository build workflow, confirming runtime/game still
  excludes TextureBuild and DerivedDataCache.
- [x] Search production sources and current documentation for obsolete
  `IAssetCompilationDomain`, `CompilationDomain`, `RegisterDomain`,
  `FindDomain`, domain dependency, and `DomainName` usage.

#### Acceptance Gate

- Class routing is the only generic object-to-compiler dispatch mechanism;
  Material and Texture are Engine-owned typed managers; TextureBuild remains a
  pure synchronous provider; no obsolete domain API or concept remains in
  production/current contracts; and all affected tests and module closures
  pass.

## Validation Matrix

| Concern | Required evidence |
| --- | --- |
| Exact route | An exact registered class resolves to its compiler. |
| Inheritance | An unregistered derived class resolves to the nearest registered ancestor. |
| Derived override | A derived exact registration wins over an ancestor registration. |
| Conflict atomicity | Duplicate compiler name or exact class registration leaves all previous routes intact. |
| No route | Finish and cancellation of an unrelated object invoke no compiler. |
| Batching | Mixed objects are grouped by compiler, retain per-compiler input order, and are not broadcast. |
| Lifecycle deduplication | One compiler registered for multiple classes starts, pumps, finishes all, and shuts down once. |
| Frame fairness | Every live compiler gets a bounded first opportunity and idle quota is reclaimed. |
| Success events | Only current successful object applications emit `CompilerName` and coalesced weak objects. |
| Retirement | Route removal, admission closure, finish, shutdown, destruction, and module-resource release remain ordered. |
| Texture compatibility | Keys, cache results, payloads, request freshness, callbacks, and module closure remain unchanged. |
| Material compatibility | Single-flight, freshness, cancellation, reload, cooked behavior, and publication remain unchanged. |

## Related Documentation

- [Asset Compilation](../Runtime/Assets/AssetCompilation.md)
- [Asset Data Lifecycle and Storage](../Runtime/Assets/AssetDataLifecycle.md)
- [Runtime Lifecycle](../Runtime/Core/RuntimeLifecycle.md)
- [Texture Build Object Boundary Completion](TextureBuildObjectBoundaryCompletion.md)
- [Non-Texture Asset Build DDC Decoupling](NonTextureAssetBuildDdcDecoupling.md)

## Related Code

- `Engine/Source/Runtime/Engine/Public/Asset/AssetCompilingManager.h`
- `Engine/Source/Runtime/Engine/Private/Asset/AssetCompilingManager.cpp`
- `Engine/Source/Runtime/Engine/Public/Texture/Texture2DCompilation.h`
- `Engine/Source/Runtime/Engine/Private/Texture/TextureCompilingManager.h`
- `Engine/Source/Runtime/Engine/Private/Texture/TextureCompilingManager.cpp`
- `Engine/Source/Runtime/Engine/Private/Texture/Texture2DCompilation.cpp`
- `Engine/Source/Runtime/Engine/Private/Texture/Texture2DBuildProvider.cpp`
- `Engine/Source/Runtime/Engine/Private/Materials/MaterialCompileLifecycle.cpp`
- `Engine/Source/Developer/TextureBuild/Private/TextureBuildModule.cpp`
- `Engine/Tests/Native/EngineTests/Private/AssetCompilingManagerTests.cpp`
