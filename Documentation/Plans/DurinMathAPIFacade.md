# Durin Math API Facade Plan

Summary: Introduce a Durin-owned math operation surface over the current GLM backend and migrate engine call sites without changing math value types, reflection identities, or serialized data.

Last reviewed: 2026-08-05

Status: Active
Completed:

## Current Status

- Core math value names such as `FVector3`, `FQuat`, and `FMatrix` are aliases
  of GLM types declared in `MathFwd.h`.
- Engine code commonly calls `glm::dot`, `glm::normalize`, `glm::length`,
  `glm::cross`, `glm::angleAxis`, matrix transforms, conversions, constants,
  and related algorithms directly.
- A targeted scan currently finds 55 C++ source/header files under
  `Engine/Source` and `Engine/Tests` containing a direct GLM symbol or include.
  The use spans Core, Renderer, Engine, editor code, and tests, with five
  repository public headers exposing direct GLM spelling.
- Existing `Math/Vector.h` supplies constants but no engine-owned algorithm
  facade. `FTransform` and its tests call GLM directly for quaternion and matrix
  behavior.
- Direct use is functional, but call sites encode GLM naming, overloads,
  exceptional-value behavior, angle units, quaternion conventions, and matrix
  assumptions. Replacing the backend later would therefore require a broad,
  high-risk edit rather than a bounded implementation change.
- This plan intentionally retains GLM-backed aliases and their current ABI. It
  isolates algorithm usage first; it does not introduce engine-owned vector,
  quaternion, or matrix storage.
- The facade foundation and non-reflection call-site migration may execute in a
  separate worktree alongside
  [Reflected Struct Operations](ReflectedStructOperations.md). Shared reflection
  bridge files and type aliases remain frozen until that plan's owning changes
  are integrated.
- No implementation stage has started.

## Goal

Make Durin's source-level math behavior depend on a small, tested
`Durin::Math` API instead of scattered direct GLM algorithms, while retaining
GLM as the implementation backend and preserving all existing value types,
ABI, reflection schemas, transform conventions, and serialized representations.

After completion, a future engine-owned math type migration can evaluate and
replace one documented backend boundary rather than first discovering semantic
contracts across renderer, engine, editor, and test call sites.

## Scope

- A Core-owned `Durin::Math` namespace for the vector, quaternion, matrix,
  angle, interpolation, and finite/near-zero operations demonstrated by current
  engine use.
- Explicit contracts for precision, NaN/infinity handling, zero-length input,
  normalization failure, angle units, quaternion identity/sign equivalence,
  handedness, matrix indexing, and transform composition order where relevant.
- A documented GLM backend boundary and a small allowlist for code that must
  directly interoperate with GLM while aliases remain in use.
- Focused Core tests that freeze current intended behavior before call-site
  migration.
- Bounded migration of repository-owned Core, Renderer, Engine, editor, and
  native-test algorithm calls from `glm::*` to the facade.
- Replacement of explicit GLM type spellings in repository public APIs with
  existing Durin aliases where the underlying type and ABI remain identical.
- Include cleanup that removes unnecessary direct GLM extension headers from
  migrated call sites while retaining the backend includes required by the
  current aliases and facade implementation.
- Cross-plan collision rules for `MathFwd.h`, intrinsic reflection descriptors,
  generated code, and reflection/serialization tests.
- Focused math, renderer, engine, editor, and compatibility validation plus
  lasting Core math documentation.

## Non-Goals

- Replacing GLM or introducing engine-owned vector, quaternion, or matrix
  storage types.
- Changing `FReal`, `FVector*`, `FQuat`, `FMatrix`, `FTransform`, or their C++
  ABI, size, alignment, constructors, operators, or component member access.
- Changing reflected qualified names, `FVector3`'s `x/y/z: Double` schema,
  intrinsic `DStruct` registration, StructOps capabilities, or property
  registration parameters.
- Changing Archive behavior, DAST v2/v3 representation, asset packages,
  network formats, shader layouts, or GPU constant-buffer contracts.
- Wrapping every GLM symbol mechanically. Operations enter the facade only when
  repository use demonstrates a stable Durin semantic contract.
- Eliminating ordinary arithmetic operators or `.x/.y/.z/.w` access while the
  current aliases remain the public value types.
- Redesigning `FTransform`, coordinate systems, camera conventions, Euler
  rotation policy, or matrix decomposition algorithms.
- Adding SIMD dispatch or accepting performance regressions merely to remove a
  direct `glm::` spelling.

## Design Decisions and Invariants

### Public API Ownership

- Core owns the facade under `Engine/Source/Runtime/Core/Public/Math/` in the
  `Durin::Math` namespace. Higher modules do not define competing wrappers.
- Public names express operation semantics rather than backend names. The first
  surface is limited to operations found in the Stage 0 inventory and required
  to migrate selected call sites.
- The facade accepts and returns Durin aliases such as `FVector3`, `FQuat`, and
  `FMatrix`; public signatures do not spell GLM types.
- Scalar standard-library operations remain standard-library operations when
  they need no Durin-specific semantics. The facade does not wrap `std` for
  stylistic uniformity.
- Constants with engine meaning remain under Durin-owned namespaces. Backend
  constants are replaced only when their units and precision are explicitly
  fixed.

### Semantic Contract

- Each normalization API states whether zero, near-zero, NaN, and infinite
  input fails, returns a fallback, or propagates a value. Call sites may not
  inherit behavior accidentally from a backend overload.
- Unsafe algebraic operations may remain available only when their preconditions
  are explicit. Runtime paths accepting untrusted or authored values use safe
  variants that report failure or name their fallback.
- Angle conversion and construction APIs include `Radians` or `Degrees` in the
  type/function contract where ambiguity would otherwise exist.
- Quaternion comparisons and tests account for `q` and `-q` representing the
  same rotation; raw component equality is used only when exact representation
  is intentionally significant.
- Matrix storage/indexing, multiplication order, handedness, transform order,
  and quaternion-vector application retain current Durin behavior and receive
  characterization tests before migration.
- Facade results preserve the current precision of the Durin alias used by the
  call. Float and double overloads do not silently narrow through a shared
  implementation.

### Backend Boundary

- GLM remains an approved implementation detail of Core math headers/private
  sources and narrowly identified third-party interoperability sites.
- Repository modules outside the backend boundary call `Durin::Math` rather
  than new direct GLM algorithms. Direct type construction through Durin aliases,
  component access, and arithmetic operators remain permitted.
- Direct GLM uses that cannot yet migrate are recorded in a checked allowlist
  with an owner and rationale. The allowlist distinguishes alias declarations,
  backend implementation, external API interop, reference tests, and true
  migration debt.
- The plan measures compile, runtime, and code-size impact before selecting
  header-inline versus exported implementations for each operation family.
  Performance-sensitive operations do not cross a module boundary without
  evidence that the cost is acceptable.
- Facade implementation may use GLM internally, but facade tests specify Durin
  behavior and must not treat “matches whatever the installed GLM does” as the
  complete contract.

### Parallel Execution Contract

- During concurrent Reflected Struct Operations work, this plan does not change
  the aliases in `MathFwd.h`, `CoreDObject/Private/DObject/MathStructs.cpp`, DHT
  built-in type resolution, reflected qualified names, or math field schemas.
- The initial facade is added in new Core math files and exposed through
  `DurinMath.h` or targeted includes. Editing `MathFwd.h` is unnecessary for
  the backend-preserving facade and remains outside concurrent stages.
- Reflection and AssetCore tests may consume the facade only after their owning
  reflection stage is integrated. This plan does not churn those tests merely
  to remove direct GLM reference calculations.
- If another active plan owns a renderer, engine, editor, or test file, that
  file is deferred to a later migration batch rather than edited concurrently
  in the same checkout.
- Parallel implementation uses separate worktrees because repository build and
  generated-output ownership is single-writer per checkout. Final integration
  performs one coherent regeneration/build after both baselines are merged.

### Compatibility and Future Replacement

- Keeping the aliases means this plan improves source dependency isolation, not
  C++ ABI isolation. Public mangled names and layouts still depend on GLM until
  an explicit future type-replacement plan executes.
- Reflection uses qualified Durin identities and logical component fields; this
  plan neither changes nor re-registers them.
- Serialization continues to traverse the same reflected fields. No facade
  operation participates in wire encoding or default construction.
- A later engine-owned type plan must preserve or deliberately version the
  documented math semantics, C++ conversions, reflection identities, and
  serialized fields. It is not implicitly authorized by completion of this
  facade.
- The migration is behavior-preserving. Any desirable semantic correction
  discovered during characterization is recorded separately rather than hidden
  inside a wrapper rename.

## Current Foundations and Gaps

### Foundations

- `MathFwd.h` already provides Durin names for the primary scalar, vector,
  quaternion, matrix, and integer-vector types.
- `Math/Vector.h`, `Math/Constants.h`, `Math/NumericalOperations.h`, and
  `Math/DurinMath.h` provide an existing Core-owned location and umbrella for a
  public math surface.
- `FTransform` centralizes important composition and relative-transform
  behavior and already has focused native tests.
- Native Core, renderer, and engine test targets can characterize CPU math and
  downstream rendering behavior independently.
- Current direct GLM use provides a concrete inventory from which to select the
  minimum facade surface.

### Gaps

- No Durin-owned algorithm namespace exists; constants alone do not isolate
  direct calls to backend operations.
- Direct GLM calls are spread across modules and tests, so behavior such as safe
  normalization and angle units is inconsistent or implicit.
- Public repository headers outside Core math expose direct GLM spelling even
  where an existing Durin alias represents the same type.
- There is no explicit exception inventory separating backend implementation
  from accidental architectural leakage.
- Existing tests often compute expectations through the same GLM operation as
  production code, which can reproduce a backend behavior without independently
  freezing the intended Durin contract.
- Replacing the GLM aliases today would still require broad changes to direct
  constructors, operators, component access, matrix indexing, and external
  interop even after algorithm calls are wrapped.

## Implementation Stages

### Stage 0: Inventory Usage and Freeze Math Semantics

- [ ] Inventory direct GLM symbols and includes by module, public/private
  header, operation family, execution frequency, and third-party boundary.
- [ ] Classify each use as alias declaration, Durin-facade candidate, backend
  implementation, third-party interop, reference test, or deferred debt.
- [ ] Record the exact concurrent ownership exclusions for active reflection,
  asset, renderer, and editor plans before selecting migration batches.
- [ ] Characterize vector length/dot/cross, normalization, interpolation, angle
  conversion, quaternion construction/composition/inversion, vector rotation,
  matrix construction/inversion/transposition, and transform decomposition as
  currently used.
- [ ] Freeze zero/near-zero, NaN, infinity, precision, quaternion sign,
  handedness, matrix indexing, and transform-order behavior for the first API
  surface.
- [ ] Select API names and signatures in `Durin::Math`, including explicit safe
  and preconditioned variants where current callers need both.
- [ ] Select inline versus exported implementation per operation family using
  representative build/codegen evidence and module-boundary cost.
- [ ] Establish the direct-GLM allowlist format and a targeted validation check
  that reports new unclassified uses without scanning third-party code.

#### Acceptance Gate

- Every current direct GLM use is classified with a migration owner or an
  explicit bounded exception.
- The first facade surface has one set of names, signatures, precision rules,
  failure semantics, angle units, and transform conventions.
- Concurrent file ownership excludes reflection bridge/type-identity changes
  and identifies any temporarily deferred call sites.
- Characterization tests can distinguish intended Durin behavior from an
  accidental backend pass-through.

### Stage 1: Establish the Core Facade

- [ ] Add the selected public facade declarations and backend implementations
  under Core math without changing the existing type aliases.
- [ ] Add Durin-owned constants and explicit degree/radian conversions required
  by the inventory.
- [ ] Implement safe and preconditioned vector/quaternion operations according
  to the frozen failure contracts.
- [ ] Implement the selected matrix and transform-adjacent operations while
  preserving current multiplication and storage conventions.
- [ ] Add focused Core tests for float/double precision, ordinary inputs,
  zero/near-zero values, NaN, infinities, signed zero where significant,
  quaternion sign equivalence, and matrix/transform order.
- [ ] Add compile-time signature/return-type checks proving overloads do not
  narrow Durin aliases.
- [ ] Add the allowlist validation test/tool with only audited baseline entries.

#### Acceptance Gate

- The facade covers the operation families selected in Stage 0 and exposes no
  GLM spelling in caller-facing signatures.
- Exceptional inputs follow documented Durin behavior and focused tests do not
  rely solely on the same backend call for expected results.
- Existing aliases, sizes, alignments, constructors, operators, and reflection
  identities are unchanged.
- The allowlist distinguishes legitimate backend/interoperability use from
  unowned direct-call debt.

### Stage 2: Migrate Core and Runtime Modules in Bounded Batches

- [ ] Migrate Core math implementations such as `FTransform` and `FBox` to the
  facade where doing so does not make the backend implementation recursively
  depend on itself.
- [ ] Migrate Renderer, RenderCore, RHI, and Engine call sites by operation
  family, preserving hot-path behavior and module dependency direction.
- [ ] Replace explicit GLM type spellings in repository public APIs with
  existing Durin aliases when source and binary type identity remain unchanged.
- [ ] Remove direct GLM extension includes made unnecessary by each migrated
  batch and verify the remaining transitive include requirements explicitly.
- [ ] Add or update focused module tests for camera, geometry, lighting,
  transforms, mesh processing, and renderer calculations affected by each
  batch.
- [ ] Benchmark or inspect generated code for identified hot operations and
  keep an evidence-backed allowlist entry when facade indirection would regress
  the path.
- [ ] Defer files owned by another active plan and record their exact follow-up
  integration point rather than creating overlapping edits.

#### Acceptance Gate

- Migrated runtime modules introduce no new unclassified direct GLM algorithms
  or includes.
- Behavior, precision, coordinate conventions, and performance-sensitive paths
  match their frozen baselines.
- Public API type-spelling cleanup does not alter ABI or reflected/serialized
  identities.
- Every deferred shared file has a named owning plan and integration condition.

### Stage 3: Migrate Editor and Test Callers, Then Close Exceptions

- [ ] Migrate editor and program call sites that are not owned by another active
  plan.
- [ ] Convert production-oriented native tests to the facade while retaining a
  small independent reference-test set where backend comparison adds value.
- [ ] Review all remaining direct GLM occurrences and remove obsolete allowlist
  entries.
- [ ] Confirm remaining public GLM spellings are limited to the alias/backend
  boundary or documented external interoperability.
- [ ] Integrate call sites deferred during reflection work only after their
  owning baseline lands, without changing reflection descriptors or schemas.
- [ ] Run focused Core, renderer, engine, editor, and asset compatibility tests
  affected by the migrated calculations.

#### Acceptance Gate

- Repository production call sites use `Durin::Math` for every covered operation
  unless a reviewed allowlist entry states why direct backend access remains.
- Remaining test-side GLM calls are independent references or backend tests,
  not copied production behavior.
- Reflection registrations, field schemas, Archive traversal, and authored
  asset bytes remain unchanged after shared-file integration.
- The allowlist is small, categorized, mechanically checked, and contains no
  entry without an owner and rationale.

### Stage 4: Document the Boundary and Qualify the Baseline

- [ ] Add lasting Core math documentation covering the facade surface,
  exceptional-value rules, coordinate/matrix conventions, backend allowlist,
  and correct header ownership.
- [ ] Document explicitly that GLM aliases and ABI remain and that engine-owned
  value types require a separate future plan.
- [ ] Run all focused Core, renderer, engine, editor, reflection compatibility,
  and asset serialization suites identified by the migration batches under the
  documented Agent Build Profile.
- [ ] Complete one successful full `all` build from the integrated reflection
  and math-facade baseline.
- [ ] Re-run the direct-GLM inventory and record reductions, remaining exception
  categories, build/code-size observations, validation, and open replacement
  questions in the stage handoff.

#### Acceptance Gate

- Lasting documentation, tests, and the checked allowlist define the Durin math
  behavior and backend boundary without relying on this active plan.
- Focused suites and the full build pass from one coherent integrated baseline.
- No reflection identity, math field schema, serialized representation, or
  public value-type ABI changed.
- A future type-replacement proposal can enumerate its remaining work from the
  documented alias/operator/component/interoperability boundary rather than a
  repository-wide direct-algorithm search.

## Validation Matrix

| Area | Required evidence |
| --- | --- |
| Inventory | Every direct GLM symbol/include classified by module, operation, ownership, and exception category |
| API surface | Durin-owned signatures, explicit units/failure rules, precision preservation, and no caller-facing GLM spelling |
| Vector math | Dot, cross, length, normalization, interpolation, finite values, zero/near-zero, NaN, and infinity |
| Quaternion math | Identity, normalization, inverse, angle construction, composition, vector rotation, and sign equivalence |
| Matrix/transform | Indexing, multiplication, translation/rotation/scale order, inverse, decomposition, and invalid inputs |
| Module migration | Core, Renderer, RenderCore, RHI, Engine, editor, programs, and tests introduce no unclassified direct calls |
| Performance | Hot operations preserve acceptable generated code/runtime cost or retain a justified bounded exception |
| Reflection compatibility | Stable qualified math identities, component fields, StructOps, property descriptors, and GC traversal |
| Serialization compatibility | Unchanged object-graph and DAST v2 behavior for vectors, quaternions, matrices where supported, and transforms |
| Integration | Focused suites, checked allowlist, direct-use rescan, and full `all` build from one baseline |

Build and test execution follows [Build and Run](../Development/Build/BuildAndRun.md)
and [Native Tests](../Development/Build/NativeTests.md).

## Definition of Done

- `Durin::Math` owns a tested operation surface for the math behavior used by
  repository production code.
- Covered production call sites no longer depend directly on GLM algorithm
  names or extension includes outside the documented backend/interoperability
  boundary.
- Exceptional-value, angle-unit, quaternion, matrix, and transform conventions
  are explicit and independently tested.
- GLM remains the value-type/backend implementation, and existing aliases,
  ABI, reflection identities, field schemas, and serialized bytes are unchanged.
- Concurrent reflection work integrates without overlapping ownership or a
  second math/reflection bridge.
- The remaining direct GLM allowlist is checked, small, categorized, and
  justified.
- Focused tests and a full build pass, and lasting Core math documentation owns
  the implemented contract.

## Deferred Follow-ups

- Engine-owned vector, quaternion, and matrix storage types with an explicit C++
  ABI and whole-repository rebuild/migration plan.
- Strong angle types if measured defects show that function naming does not
  prevent degree/radian mistakes.
- SIMD/backend selection, deterministic cross-platform floating-point modes,
  or specialized high-performance vector batches.
- Shader-language math facade or generated CPU/GPU shared math contracts.
- Removal of ordinary operator, constructor, component, and indexing dependence
  on GLM after engine-owned value types are selected.

## Related Documentation

- [Reflected Struct Operations](ReflectedStructOperations.md)
- [Typed Struct Property Registration](TypedStructPropertyRegistration.md)
- [Reflection System](../Runtime/Core/ReflectionSystem.md)
- [Build and Run](../Development/Build/BuildAndRun.md)
- [Native Tests](../Development/Build/NativeTests.md)

## Related Code

- `Engine/Source/Runtime/Core/Public/Math/MathFwd.h`
- `Engine/Source/Runtime/Core/Public/Math/DurinMath.h`
- `Engine/Source/Runtime/Core/Public/Math/Vector.h`
- `Engine/Source/Runtime/Core/Public/Math/Box.h`
- `Engine/Source/Runtime/Core/Public/Math/Transform.h`
- `Engine/Source/Runtime/Core/Private/Math/Transform.cpp`
- `Engine/Source/Editor/StandardAssetImport/Public/ImportedScene.h`
- `Engine/Source/Editor/LevelEditor/Public/LevelEditorTransformTargets.h`
- `Engine/Tests/Native/CoreTests/Private/TransformTests.cpp`
- `Engine/Source/Runtime/CoreDObject/Private/DObject/MathStructs.cpp`
