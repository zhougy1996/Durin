# Durin Math API Facade Plan

Summary: Introduce a Durin-owned math operation surface over the current GLM backend and migrate engine call sites without changing math value types, reflection identities, or serialized data.

Last reviewed: 2026-08-05

Status: Completed
Completed: 2026-08-05

## Current Status

- The plan is complete. `Math/Operations.h` owns the tested `Durin::Math`
  operation surface, and covered Core, renderer, engine, editor, program, and
  production-test callers use it instead of direct GLM algorithms.
- GLM remains the storage/backend implementation. Existing aliases and ABI are
  unchanged, and `FMatrix4f` adds a Durin spelling for the existing GLM float
  4x4 type used by import and shader-layout code.
- Lasting contracts now live in [Core Math](../Runtime/Core/Math.md), including
  header ownership, exceptional-value rules, coordinate/matrix conventions,
  the direct-backend boundary, and future value-type replacement scope.
- The checked boundary is 9 files, 65 direct symbols, and 7 direct includes:
  one alias declaration, three backend files, one third-party interop file, one
  shader-layout calculation, and three independent reference-test files. It has
  no deferred or migration-debt entry.
- Stage 0 completed on 2026-08-05 from baseline `e1e5a5d1`. The 55-file
  direct-GLM inventory, first facade surface, exceptional-value rules,
  implementation boundary, checked-allowlist format, and concurrent ownership
  exclusions are frozen below.
- Stages 1-4 completed on 2026-08-05 and were compressed into one math
  completion commit after rebasing onto `dev` commit `c7d5053c`. The rebased
  reflection baseline ends at `0ebf7375` and retains its three separate commits.
- The rebase introduced spline control-point box projection and hit-testing
  calculations from `dev`; those new calls were migrated to `Durin::Math`
  before the boundary and validation were requalified.

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

## Stage 0 Handoff

### Baseline and Working Set

- Baseline commit: `e1e5a5d1` (`docs(math): plan backend facade migration`).
- Inventory roots: repository-owned `*.h`, `*.hpp`, and `*.cpp` files below
  `Engine/Source` and `Engine/Tests`; generated and third-party trees are not
  scanned.
- The baseline contains 55 files with a direct `glm::` spelling or GLM include:
  Core 4, DurinEd 1, Engine 8, LevelEditor 9, MaterialEditor 1, MonaImGui 1,
  RenderCore 1, Renderer 10, RHI 1, StandardAssetImport 3, and native tests 16.
- The most common direct symbols are `dot` (82), `normalize` (79), GLM vector
  type spellings (75), `length` (46), `radians` (27), `cross` (26), `mat4`
  (25), `angleAxis` (22), `identity` (14), `inverse` (12), and `mix` (11).
- Five repository public headers expose GLM spelling: `MathFwd.h`, `Vector.h`,
  `Box.h`, `ImportedScene.h`, and `LevelEditorTransformTargets.h`.

The classification below applies to every direct occurrence in each file.
`Migrate` means all direct algorithms and replaceable type spellings are owned
by this plan. `Mixed interop` means algorithms migrate but the named float
matrix, generic-vector, or shader-layout spelling remains a bounded exception.
`Deferred` identifies a different active-plan integration gate.

| Module | File | Baseline symbols/include role | Classification |
| --- | --- | --- | --- |
| Core | `Public/Math/MathFwd.h` | GLM forward include and all Durin aliases | Alias declaration; frozen |
| Core | `Public/Math/Vector.h` | GLM extension include for alias construction/constants | Backend include; move to facade boundary |
| Core | `Public/Math/Box.h` | `all`, `lessThanEqual`, `min`, `max` | Migrate |
| Core | `Private/Math/Transform.cpp` | vector/quaternion/matrix algorithms and experimental `decompose` | Migrate; retain `decompose` as backend implementation |
| RenderCore | `Private/SceneViewProjection.cpp` | determinant, inverse, length | Migrate |
| RHI | `Private/RHIResources.cpp` | length | Migrate |
| Renderer | `Private/PBRLighting.cpp` | geometric include, dot, cross, max | Migrate |
| Renderer | `Private/EditorGridRendering.h` | shader-facing `glm::mat4` | Shader-layout interop |
| Renderer | `Private/EditorGridRendering.cpp` | shader float types plus determinant, inverse, transpose | Mixed interop |
| Renderer | `Private/SkyBoxRendering.h` | shader-facing `glm::mat4` | Shader-layout interop |
| Renderer | `Private/SkyBoxRendering.cpp` | shader float types plus determinant, inverse, quaternion and transpose operations | Mixed interop |
| Renderer | `Private/Renderers/StaticMeshRenderer.cpp` | shader float matrices plus determinant, inverse, transpose | Mixed interop |
| Renderer | `Private/Renderers/EditorAssistance/GizmoRenderer.cpp` | shader float matrix plus transpose and constants | Mixed interop |
| Renderer | `Private/Renderers/EditorAssistance/OverlayIconRenderer.cpp` | mix | Migrate |
| Renderer | `Private/Renderers/EditorAssistance/OverlayLineRenderer.cpp` | length, mix | Migrate |
| Renderer | `Private/Renderers/TextureCubeThumbnailRenderer.cpp` | radians | Migrate |
| Engine | `Private/Components/CameraComponent.cpp` | quaternion include, vector/quaternion algorithms, radians | Migrate |
| Engine | `Private/Components/DirectionalLightComponent.cpp` | normalize | Migrate |
| Engine | `Private/Components/SceneComponent.cpp` | normalize | Migrate |
| Engine | `Private/Components/SplineComponent.cpp` | dot, matrix inverse | Migrate |
| Engine | `Private/EnvironmentLighting/EnvironmentLightingBuild.cpp` | GLM vector includes/types and vector algorithms | Migrate types and algorithms |
| Engine | `Private/Materials/MaterialInterface.cpp` | dot | Migrate |
| Engine | `Private/Spline/SplineCurve.cpp` | clamp, dot, length | Migrate |
| Engine | `Private/StaticMesh/StaticMesh.cpp` | dot, cross | Migrate |
| MonaImGui | `Private/MonaImGuiPropertyTable.cpp` | quaternion/Euler conversions | Migrate |
| DurinEd | `Private/Thumbnail/RenderedAssetThumbnailPreviewScene.cpp` | vector/quaternion algorithms, angles/constants | Migrate |
| LevelEditor | `Public/LevelEditorTransformTargets.h` | quaternion identity | Migrate |
| LevelEditor | `Private/Viewport/ViewportCameraTransform.cpp` | vector algorithms and angle conversions | Migrate |
| LevelEditor | `Private/Viewport/LevelEditorViewportClient.cpp` | vector/matrix algorithms and transforms | Migrate |
| LevelEditor | `Private/Viewport/TransformGizmo.cpp` | vector/quaternion/matrix algorithms and constants | Migrate |
| LevelEditor | `Private/Widgets/MLevelEditor.cpp` | radians | Migrate |
| LevelEditor | `Private/Customizations/LevelEditorCustomizations.cpp` | vector abs/dot/length/mix | Migrate |
| LevelEditor | `Private/Customizations/SplineEditorCustomizations.cpp` | inverse, length, mix, normalize | Migrate |
| LevelEditor | `Private/Customizations/CameraEditorCustomizations.cpp` | normalize, radians | Deferred to Editor Icon Atlas integration |
| LevelEditor | `Private/Customizations/DirectionalLightEditorCustomizations.cpp` | length, normalize | Deferred to Editor Icon Atlas integration |
| MaterialEditor | `Private/Widgets/MaterialPreview.cpp` | vector/quaternion algorithms and angles/constants | Migrate |
| StandardAssetImport | `Public/ImportedScene.h` | public float vectors and `glm::mat4` source conversion | Mixed interop; replace vectors, retain float matrix |
| StandardAssetImport | `Private/AssimpSceneGeometry.cpp` | `glm::mat4` to Assimp conversion | Third-party matrix interop |
| StandardAssetImport | `Private/GltfSceneAdapter.cpp` | generic `glm::vec<Length, float>` adapter | Third-party generic-vector interop |
| CoreTests | `Private/TransformTests.cpp` | quaternion reference construction/comparison | Migrate to independent facade expectations |
| RenderCoreTests | `Private/RHITextureTests.cpp` | length, normalize | Migrate |
| AssetCoreTests | `Private/AssetImportTests.cpp` | imported GLM types plus vector algorithms | Deferred until Reflected Struct Operations integrates |
| EngineTests | `Private/Editor/ReflectedPropertyContainerTests.cpp` | quaternion length | Deferred until Reflected Struct Operations integrates |
| EngineTests | `Private/EditorGridRenderingTests.cpp` | shader float-matrix transpose/reference | Independent shader-layout reference test |
| EngineTests | `Private/SkyBox/SkyBoxTestSupport.h` | shader float types and matrix/vector reference algorithms | Independent shader-layout reference support |
| EngineTests | `Private/SkyBox/SkyBoxComponentTests.cpp` | angle-axis/radians | Migrate |
| EngineTests | `Private/SkyBox/SkyBoxEditorTests.cpp` | angle-axis/radians | Migrate |
| EngineTests | `Private/SkyBox/SkyBoxRenderingTests.cpp` | quaternion/matrix algorithms | Migrate production-oriented calls |
| EngineTests | `Private/SkyBox/SkyBoxVulkanTests.cpp` | matrix/quaternion construction and angle constants | Retain only shader-layout reference operations |
| EngineTests | `Private/SplineTests.cpp` | angle-axis/radians | Migrate |
| EngineTests | `Private/Viewport/ViewportCustomizationTests.cpp` | dot, length | Migrate |
| EngineTests | `Private/Viewport/ViewportFoundationTests.cpp` | dot, length | Migrate |
| EngineTests | `Private/Viewport/ViewportInteractionTests.cpp` | quaternion/vector algorithms and constants | Migrate |
| EngineTests | `Private/Viewport/ViewportProjectionTests.cpp` | length | Migrate |
| EngineTests | `Private/World/WorldComponentTests.cpp` | angle-axis, dot, radians | Migrate |

### Concurrent Ownership Exclusions

- The active Reflected Struct Operations worktree currently owns DHT reflection
  generation, `CoreDObject` struct descriptors/operations, and
  `CoreDObjectTests`. This plan must not edit `MathFwd.h`,
  `CoreDObject/Private/DObject/MathStructs.cpp`, generated reflection output,
  `CoreDObjectTests`, `AssetCoreTests/Private/PackageTests.cpp`, or
  `EngineTests/Private/Editor/ReflectedPropertyContainerTests.cpp` until that
  baseline lands. `AssetImportTests.cpp` is also deferred under the plan's
  AssetCore-test collision rule.
- Typed Struct Property Registration is ordered after Reflected Struct
  Operations and owns the same DHT/property/`MathStructs.cpp` bridge surface;
  no facade stage depends on or edits that surface.
- Texture Support currently owns texture, material-type, texture-editor, and
  `MMaterialEditor.cpp` work. None of the 55 inventoried files overlaps its
  recorded working set.
- Editor Icon Atlas names `CameraEditorCustomizations.cpp` and
  `DirectionalLightEditorCustomizations.cpp`; those two migration sites are
  deferred until that plan is inactive or its owning baseline is integrated.
- Renderer float matrix structs used for CPU-to-shader transfer are not shared
  plan collisions, but remain explicit shader-layout interop exceptions because
  no existing Durin float-matrix alias represents their ABI.

### Selected Facade Surface

The first header-inline `Durin::Math` surface is:

- vector/quaternion primitives: `Dot`, `LengthSquared`, `Length`, `Cross`,
  `Normalize`, `TryNormalize`, `NormalizeOr`, `IsFinite`, `Abs`, `Min`, `Max`,
  `Clamp`, and `Lerp`;
- angles/constants: `DegreesToRadians`, `RadiansToDegrees`, `Pi`, `HalfPi`, and
  `TwoPi`, preserving the input scalar or vector precision;
- quaternions: `MakeQuaternionFromAxisAngleRadians`,
  `MakeQuaternionFromAxisAngleDegrees`, `MakeQuaternionFromEulerRadians`,
  `MakeQuaternionFromEulerDegrees`, `QuaternionToEulerRadians`,
  `QuaternionToEulerDegrees`, `QuaternionFromMatrix`, `Inverse`,
  `RotateVector`, and `AreRotationsEquivalent`;
- matrices: `Determinant`, `Inverse`, `TryInverse`, `Transpose`,
  `TranslationMatrix`, `ScaleMatrix`, and `RotationMatrix`;
- transform decomposition remains the exported
  `TryMakeTransformFromMatrix` operation because the experimental GLM
  decomposition extension belongs in a Core private source file.

Ordinary scalar `std::abs`, `std::clamp`, trigonometry, square root, and power
calls remain standard-library operations. Ordinary alias construction,
arithmetic operators, component access, and matrix indexing do not enter the
facade.

### Frozen Semantics

- Facade templates return the exact scalar/vector/quaternion type implied by
  their Durin-alias input. Float inputs remain float and double inputs remain
  double; mixed-precision overloads are not provided.
- `Normalize` and `Inverse` are preconditioned algebraic operations. Their
  input must be finite and respectively non-zero or nonsingular. They do not
  sanitize invalid input; NaN/infinity and violated preconditions may produce
  non-finite backend results.
- `TryNormalize` rejects any non-finite component, squared length less than or
  equal to the caller-selected threshold, or a non-finite result. It leaves its
  output unchanged on failure. `NormalizeOr` returns the caller-provided
  fallback under the same conditions. Signed zero is a zero-length failure.
- `TryInverse` rejects a non-finite matrix, a non-finite determinant, absolute
  determinant less than or equal to the caller-selected threshold, or a
  non-finite inverse. It leaves its output unchanged on failure.
- `IsFinite` means every component is finite. Component-wise `Abs`, `Min`,
  `Max`, `Clamp`, and `Lerp` require finite operands when callers need portable
  exceptional-value behavior; they do not silently replace NaN or infinity.
  `Lerp` is not clamped.
- Public angle constructors/conversions state radians or degrees in their
  names. Positive axis-angle rotation follows the current right-handed GLM
  convention and the axis must be finite and normalized for the unchecked
  constructor.
- Quaternion multiplication remains ordered `Parent * Relative`, and vector
  rotation remains `Quaternion * Vector`. Rotation equivalence uses the
  absolute normalized dot product so `q` and `-q` compare as the same rotation;
  invalid or non-normalizable inputs are not equivalent.
- Matrices retain column-vector multiplication and `[column][row]` indexing.
  `FTransform::ToMatrix` remains `Translation * Rotation * Scale`; Durin uses
  +X forward, +Y right, +Z up, and the existing right-handed cross-product
  orientation. Shader upload transposition remains an explicit interop step.
- `TryMakeTransformFromMatrix` retains finite-input validation, rejects a
  failed decomposition or non-normalizable rotation, and commits the output
  only after the complete candidate is valid.

### Implementation and Allowlist Decisions

- The selected vector, quaternion, angle, and ordinary matrix functions are
  templates or overloads over the existing GLM aliases and remain header-inline.
  This preserves the current call-site code-generation boundary and avoids a
  Core DLL call in renderer and geometry hot paths. Exported implementation is
  reserved for decomposition and any future operation whose backend include or
  measured code size justifies one.
- Representative MSVC code-generation evidence from the baseline Debug
  `Transform.cpp.obj` (193,533 bytes) places `glm::dot`, `normalize`, `inverse`,
  `determinant`, `transpose`, and related template bodies in caller-local COMDAT
  sections rather than a GLM runtime library. An exported facade would therefore
  add a module call boundary where none exists today; the header-inline choice
  retains current Debug linkage and allows Release inlining. Stage 1 still
  verifies the optimized wrapper output before hot-path migration.
- Stage 1 will add a versioned JSON allowlist and a repository tool that scans
  only the inventory roots. Each entry records a normalized file path,
  category, owner, rationale, direct include counts, and per-symbol occurrence
  counts. A new file, symbol, include, or changed count is an error, so adding a
  use of an already-allowed symbol cannot silently expand the boundary.
- Allowlist categories are `alias-declaration`, `backend-implementation`,
  `third-party-interop`, `shader-layout-interop`, `reference-test`,
  `deferred-plan`, and `migration-debt`. Migration stages remove entries or
  narrow their symbol/count sets; exceptions require an owner and rationale.

### Open Questions

- Stage 1 confirmed representative Release code generation for `Dot`,
  `Normalize`, quaternion-vector rotation, and matrix inverse. Direct GLM and
  facade probe pairs produced instruction-identical function bodies; the
  temporary probe source was removed after inspection.
- The two Editor Icon Atlas-owned customization files and reflection-owned test
  files remain integration questions until their owning baselines land.

## Stage 1 Handoff

- Squashed integration baseline: `0ebf7375` (`feat(reflection): make struct
  consumers capability-aware`). Stage-specific math commits were intentionally
  collapsed into the plan's single completion commit after validation.
- Working set: `Math/Operations.h`, the `DurinMath.h` umbrella, the explicit
  quaternion identity constant in `Vector.h`, `MathFacadeTests.cpp`, the
  `CoreUtilityTests` source list, and `Tools/Architecture` direct-GLM boundary
  checker/allowlist.
- Key symbols: `Durin::Math::{Dot,LengthSquared,Length,Cross,Normalize,
  TryNormalize,NormalizeOr,IsFinite,Abs,Min,Max,Clamp,Lerp}`; explicit
  degree/radian and quaternion construction/conversion operations; quaternion
  equivalence/inversion/vector rotation; and finite checked matrix inversion,
  determinant, transpose, translation, rotation, and scale construction.
- Safe normalization and inversion leave output unchanged on failure. Unchecked
  algebra retains explicit finite/non-zero/nonsingular preconditions. Templates
  accept only the selected Durin aliases and preserve their scalar precision.
- The version-1 JSON allowlist contains exact direct-include and per-symbol
  counts. The checker validates 56 audited files, 535 direct symbol uses, and 20
  direct includes at the Stage 1 baseline; a temporary unclassified test file
  was correctly rejected.
- `CoreUtilityTests` passed, including the five new facade tests.
  A `Win64-Release-DurinEditor` Core probe showed instruction-identical direct
  and facade output for the four representative hot operations and introduced
  no facade DLL call.
- Open questions are limited to the recorded cross-plan deferred files. No
  reflection identity, type alias, layout, field schema, or serialization code
  changed.

## Stage 2 Handoff

- Squashed integration baseline: `0ebf7375`; the Stage 2 checkpoint and its
  validation evidence are retained here rather than as a separate commit.
- Working set: Core `FTransform`/`FBox`; RenderCore scene projection; RHI cube
  direction resolution; Engine camera, scene, spline, static-mesh, material,
  and environment-lighting implementations; Renderer lighting, grid, sky-box,
  mesh-transform, overlay, gizmo, and thumbnail calculations; and
  `ImportedScene.h` public vector spelling.
- Core retains one private `glm::decompose` backend call. Renderer retains six
  shader-layout entries for `glm::mat4` uniform storage/conversion and the
  float `mat3` determinant used by static-mesh normal orientation. Explicit
  float-matrix aliases are not introduced because `MathFwd.h` remains frozen.
- CPU-to-shader matrix conversion is now written as an explicit
  `[column][row] = float(Source[row][column])` boundary. This preserves the
  existing cast-then-transpose result without routing Durin matrices through a
  direct GLM algorithm at call sites.
- `ImportedScene` public vectors now spell `FVector2f`, `FVector3f`, and
  `FVector4f`; the source-conversion `glm::mat4` remains because there is no
  existing Durin float-matrix alias. Alias identity preserves layout and ABI.
- The checked boundary fell from the Stage 1 baseline of 56 files, 535 symbols,
  and 20 includes to 41 files, 367 symbols, and 12 includes. Runtime migration
  debt is limited to editor-facing MonaImGui work assigned to Stage 3.
- The complete native-test aggregate and the focused `AssetImportTests` target
  passed. A Release Renderer build passed, and disassembly of PBR lighting,
  static-mesh processing, spline evaluation, camera, and scene-projection
  objects contained no calls to `Durin::Math` wrapper functions.
- Deferred ownership remains unchanged: the two Editor Icon Atlas
  customization files, reflection-owned editor/AssetCore tests, `MathFwd.h`,
  intrinsic math registration, generated reflection code, and serialization
  compatibility files were not edited.

## Stage 3 Handoff

- Squashed integration baseline: `0ebf7375`. The reflection baseline was
  replayed on `dev` before shared tests were migrated, and `dev`'s new spline
  box projection and hit-testing calculations were included in the facade
  migration during rebase.
- Working set: MonaImGui numeric property editing; thumbnail preview math;
  Level Editor customization, viewport, gizmo, camera-transform, widget, and
  transform-target code; Material Editor preview math; and production-oriented
  Core, RenderCore, sky-box, spline, viewport, and world tests.
- Covered editor calculations now use the same explicit angle-unit,
  normalization, quaternion-equivalence, and matrix-order facade contracts as
  runtime code. Production-oriented tests consume the facade while the three
  shader-layout reference tests remain independent GLM comparisons.
- `FMatrix4f` now aliases the existing GLM float 4x4 type. Imported-scene and
  shader-uniform code uses that Durin spelling, preserving storage and ABI while
  removing the prior float-matrix spelling exceptions.
- The checked boundary fell from the Stage 2 baseline of 41 files, 367 symbols,
  and 12 includes to 9 files, 65 symbols, and 7 includes. It contains one alias
  declaration, three backend files, one third-party interop file, one bounded
  shader-layout calculation, and three independent reference-test files; no
  `deferred-plan` or `migration-debt` entry remains.
- Remaining public GLM spelling is limited to `MathFwd.h` alias declarations and
  the Core math backend. No existing value type, reflection descriptor, field
  schema, archive traversal, or serialized representation changed in the facade
  integration.
- The complete native-test aggregate passed after the final alias and shared
  call-site integration. The earlier non-conflicting batch also passed on an
  immediate unchanged rerun after one unrelated intermittent TextureCube
  derived-data-cache failure.
- Open questions: none for Stage 3. The checked boundary records every remaining
  backend, third-party, shader-layout, and reference-test use with an owner and
  exact symbol/include count.

## Stage 4 Handoff

- Squashed integration baseline: `0ebf7375`. Working set: the final shared and
  rebased-`dev` call sites, `FMatrix4f` alias propagation, exact allowlist
  closure, and lasting Core math documentation and routing.
- The final inventory is 9 files, 65 symbols, and 7 direct includes, down from
  the 55-file Stage 0 source/test baseline and the 56-file, 535-symbol,
  20-include Stage 1 checked boundary. Remaining categories are one alias
  declaration, three backend implementations, one third-party interop, one
  shader-layout calculation, and three independent reference tests.
- The facade remains header-inline. Stage 1 Release inspection showed
  instruction-identical representative direct/facade operations with no Core
  DLL call, and Stage 2 Release caller objects contained no facade wrapper
  calls. `FMatrix4f` is a source alias and adds no storage, conversion, symbol,
  or runtime call.
- The complete native-test aggregate passed after reflection-baseline and final
  shared-call-site integration. A full `all` build then passed under the
  `windows-msvc-x64` Agent Build Profile using the
  `Win64-Debug-DurinEditor-Tests` preset.
- Math-facade integration changed no existing alias identity or ABI, intrinsic
  math reflection identity, field schema, Archive traversal, or serialized
  representation. The independently integrated reflection baseline retains its
  own plan provenance and validation.
- Open replacement work is explicit in [Core Math](../Runtime/Core/Math.md):
  GLM storage, constructors/operators, component/indexing access, generic-vector
  third-party interop, the float 3x3 shader calculation, reflection, and
  serialization all belong to a separate future value-type plan.

## Implementation Stages

### Stage 0: Inventory Usage and Freeze Math Semantics

- [x] Inventory direct GLM symbols and includes by module, public/private
  header, operation family, execution frequency, and third-party boundary.
- [x] Classify each use as alias declaration, Durin-facade candidate, backend
  implementation, third-party interop, reference test, or deferred debt.
- [x] Record the exact concurrent ownership exclusions for active reflection,
  asset, renderer, and editor plans before selecting migration batches.
- [x] Characterize vector length/dot/cross, normalization, interpolation, angle
  conversion, quaternion construction/composition/inversion, vector rotation,
  matrix construction/inversion/transposition, and transform decomposition as
  currently used.
- [x] Freeze zero/near-zero, NaN, infinity, precision, quaternion sign,
  handedness, matrix indexing, and transform-order behavior for the first API
  surface.
- [x] Select API names and signatures in `Durin::Math`, including explicit safe
  and preconditioned variants where current callers need both.
- [x] Select inline versus exported implementation per operation family using
  representative build/codegen evidence and module-boundary cost.
- [x] Establish the direct-GLM allowlist format and a targeted validation check
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

- [x] Add the selected public facade declarations and backend implementations
  under Core math without changing the existing type aliases.
- [x] Add Durin-owned constants and explicit degree/radian conversions required
  by the inventory.
- [x] Implement safe and preconditioned vector/quaternion operations according
  to the frozen failure contracts.
- [x] Implement the selected matrix and transform-adjacent operations while
  preserving current multiplication and storage conventions.
- [x] Add focused Core tests for float/double precision, ordinary inputs,
  zero/near-zero values, NaN, infinities, signed zero where significant,
  quaternion sign equivalence, and matrix/transform order.
- [x] Add compile-time signature/return-type checks proving overloads do not
  narrow Durin aliases.
- [x] Add the allowlist validation test/tool with only audited baseline entries.

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

- [x] Migrate Core math implementations such as `FTransform` and `FBox` to the
  facade where doing so does not make the backend implementation recursively
  depend on itself.
- [x] Migrate Renderer, RenderCore, RHI, and Engine call sites by operation
  family, preserving hot-path behavior and module dependency direction.
- [x] Replace explicit GLM type spellings in repository public APIs with
  existing Durin aliases when source and binary type identity remain unchanged.
- [x] Remove direct GLM extension includes made unnecessary by each migrated
  batch and verify the remaining transitive include requirements explicitly.
- [x] Add or update focused module tests for camera, geometry, lighting,
  transforms, mesh processing, and renderer calculations affected by each
  batch.
- [x] Benchmark or inspect generated code for identified hot operations and
  keep an evidence-backed allowlist entry when facade indirection would regress
  the path.
- [x] Defer files owned by another active plan and record their exact follow-up
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

- [x] Migrate editor and program call sites that are not owned by another active
  plan.
- [x] Convert production-oriented native tests to the facade while retaining a
  small independent reference-test set where backend comparison adds value.
- [x] Review all remaining direct GLM occurrences and remove obsolete allowlist
  entries.
- [x] Confirm remaining public GLM spellings are limited to the alias/backend
  boundary or documented external interoperability.
- [x] Integrate call sites deferred during reflection work only after their
  owning baseline lands, without changing reflection descriptors or schemas.
- [x] Run focused Core, renderer, engine, editor, and asset compatibility tests
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

- [x] Add lasting Core math documentation covering the facade surface,
  exceptional-value rules, coordinate/matrix conventions, backend allowlist,
  and correct header ownership.
- [x] Document explicitly that GLM aliases and ABI remain and that engine-owned
  value types require a separate future plan.
- [x] Run all focused Core, renderer, engine, editor, reflection compatibility,
  and asset serialization suites identified by the migration batches under the
  documented Agent Build Profile.
- [x] Complete one successful full `all` build from the integrated reflection
  and math-facade baseline.
- [x] Re-run the direct-GLM inventory and record reductions, remaining exception
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

- [Core Math](../Runtime/Core/Math.md)
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
