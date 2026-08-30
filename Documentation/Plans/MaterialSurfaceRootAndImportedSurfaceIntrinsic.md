# Material Surface Root and Imported Surface Intrinsic Plan

Summary: Replace expanded surface fallbacks and the canonical ImportedSurface graph with explicit surface-root and Standard Surface semantics while preserving authored material behavior.

Last reviewed: 2026-08-30

Status: Active
Completed:

## Current Status

The selected design is ready for implementation. A fresh material currently
owns no authored expression nodes, but normalization appends one Constant IR
node for each of its eight unconnected surface fallbacks. The editor's derived
Material Output terminal is already presentation-only and is not the source of
those nodes.

`/Engine/Materials/ImportedSurface` is a separate problem. Its package persists
the complete program returned by `MakeCanonicalMaterialProgram`, so opening the
asset correctly exposes the complete sampling, channel extraction, clamping,
normal decoding, and composition DAG. It is not expanded transiently by the
editor. That representation is correct but leaks the implementation of an
Engine-owned import parent and makes the common graph unnecessarily large.

This plan selects two explicit semantic boundaries:

1. normalized IR owns one special Surface Root terminal whose inputs are either
   expression references or inline typed literals; and
2. authored programs may use one `StandardSurface` intrinsic returning an
   aggregate `Surface` value whose implementation is lowered inside the
   compiler rather than persisted as primitive expression nodes.

No implementation work has started.

## Goal

Make the material program express its actual semantic structure:

- a default material contains zero authored and zero ordinary normalized
  expression nodes plus one special Surface Root;
- the derived Material Output remains one editor terminal and does not become a
  deletable, copyable, or persisted ordinary node;
- the built-in ImportedSurface material opens as one Standard Surface node
  connected to Material Output instead of exposing its implementation DAG;
- existing per-property material graphs remain editable and compile with the
  same surface behavior;
- canonical parameter GUIDs, imported material-instance overrides, static
  properties, render layout v3, and renderer/pass consumers remain unchanged.

## Scope

- Versioned authored-program support for an aggregate `Surface` value and a
  closed-domain `StandardSurface` intrinsic.
- A special normalized Surface Root with literal-or-expression property inputs
  and an optional aggregate Surface source.
- Deterministic validation, normalization, canonical encoding, identity,
  generated Slang, dependency inspection, diagnostics, Cook, and memory
  accounting for the new forms.
- Material graph inspection, catalog, connection, layout, clipboard, and canvas
  behavior for the aggregate node and Surface link.
- A bounded schema-2 to schema-3 transition for existing material programs.
- Migration and qualification of `/Engine/Materials/ImportedSurface` and its
  generated instances without changing the stable asset path or parameter IDs.

## Non-Goals

- A general material-function, subgraph, macro, or arbitrary multi-output node
  system.
- Automatic recognition or collapse of arbitrary user-authored graphs that
  merely resemble Standard Surface.
- Surface layering, Substrate-style BSDF composition, Clear Coat, Subsurface,
  or changes to the metallic/roughness PBR surface ABI.
- Changing texture bindings, sampler policy, UV-transform meaning, dynamic
  material-instance ownership, or Renderer pass integration.
- Treating a presentation-only collapsed group as sufficient. Presentation may
  improve independently, but the target node-count reduction must exist in the
  authored program and normalized IR.
- Preserving the old material-program or shader identity digest. Versioned
  semantic representation changes must invalidate affected derived data.

## Selected Design

### Surface Root is a terminal, not an ordinary value node

Keep ordinary `FMaterialIRNode` values single-result and strongly typed. Add a
dedicated `FMaterialIRSurfaceRoot` owned directly by `FMaterialIR`; do not add a
`MaterialOutput` opcode whose missing result type and heterogeneous inputs would
weaken every ordinary-node invariant.

Each per-property root input has exactly one normalized form:

- `Expression`: a valid earlier numeric IR-node index of the property's exact
  required type; or
- `Literal`: the finite typed fallback stored directly in the root input.

The root also supports an aggregate mode containing one valid earlier
`Surface` expression index. Aggregate mode and per-property expression mode are
mutually exclusive. Per-property retained fallbacks remain present in authored
state so disconnecting an aggregate or ordinary source has deterministic
behavior.

The root participates in canonical encoding and program identity but is not
included in `IR.Nodes`. Generated source resolves root literals inline and must
accept an empty ordinary-node vector.

### Standard Surface is a closed intrinsic

Add `Surface` to the authored and normalized value domains and add one
`StandardSurface` opcode. It has no arbitrary shader source and no hidden
user-selected parameter IDs. Its dependency and lowering contract comes from
one Engine-owned descriptor over the existing eight canonical material roles.

The intrinsic reproduces the current `MakeCanonicalMaterialProgram` behavior:

- Base Color combines the canonical value and RGB texture sample with the
  current saturation policy;
- Normal combines the canonical tangent-space value with decoded RG texture
  data using the current RNM path;
- Metallic, Roughness, Ambient Occlusion, Opacity, and Opacity Mask retain the
  current canonical channel and finite-range policies;
- Emissive retains the current non-negative value and texture composition; and
- every texture role retains its canonical UV channel, scale, offset, rotation,
  sampler, and fallback behavior.

The descriptor is the single authority consumed by validation, parameter
dependency inspection, normalization/lowering, and editor inspection. No
consumer may recover intrinsic dependencies by duplicating a switch over role
names.

### Authored output modes

Program schema 3 adds an optional aggregate Surface source to
`FMaterialSurfaceOutputs` while retaining the eight existing optional typed
links and fallbacks.

- Aggregate mode: the aggregate source is valid and all eight per-property
  links are disconnected.
- Per-property mode: the aggregate source is invalid and the existing eight
  links/fallbacks behave as they do today.
- Mixed mode is rejected rather than defining an implicit precedence rule.

Material Output displays one Surface input row when aggregate mode is active or
being connected, and the eight property rows in per-property mode. Switching
modes is an explicit atomic graph command; it never silently destroys a
connected branch.

### Version and compatibility policy

- Bump the authored material-program schema, normalized IR version, generator
  version, compiler envelope where required, and all affected Cook/DDC keys.
- Provide one bounded value-owned schema-2 to schema-3 upgrade: initialize the
  aggregate source as disconnected and preserve all old nodes, links,
  fallbacks, GUIDs, presentation, and static properties.
- Schema 3 is the only saved form after a successful edit/save. Unknown future
  schemas continue to fail before residency.
- Do not reinterpret a legacy expanded canonical DAG as Standard Surface during
  ordinary package load. The checked-in ImportedSurface asset is migrated
  explicitly and exact legacy recognition is confined to the built-in asset
  migration/validation path.

## Implementation Stages

### Stage 0: Freeze baselines and transition contracts

- [ ] Add focused assertions for the current default-material authored-node,
  normalized-node, root-output, generated-source, identity, and compiled-stage
  baselines before changing representation.
- [ ] Record the exact current canonical ImportedSurface node/opcode count,
  reachable parameter dependencies, generated-source behavior, render identity,
  and representative Vulkan output.
- [ ] Define the schema-2 input fixtures used to qualify value-preserving
  upgrade, including disconnected fallbacks, connected property graphs,
  presentation positions, and malformed/unknown versions.
- [ ] Define stable diagnostic locations for aggregate output errors and
  Standard Surface failures without changing existing node/input/surface-output
  locations.
- [ ] Confirm the Standard Surface descriptor reproduces every operation and
  texture-role dependency currently created by `MakeCanonicalMaterialProgram`.

Completion condition: tests pin the old behavior and the schema, identity,
diagnostic, and migration rules above have no unresolved representation choice.

### Stage 1: Introduce the normalized Surface Root

- [ ] Add `FMaterialIRSurfaceInput` and `FMaterialIRSurfaceRoot` with explicit
  literal, per-property expression, and aggregate expression invariants.
- [ ] Replace `FMaterialIR::SurfaceOutputs` index storage with the special root
  while keeping `FMaterialIRNode` a single-result value node.
- [ ] Normalize unconnected authored outputs directly into root literals instead
  of appending Constant IR nodes.
- [ ] Update canonical encoding, bounds checking, equality, hashing, identity,
  memory accounting, compile-result ownership, and failure diagnostics.
- [ ] Update Slang generation to accept zero ordinary nodes, resolve inline root
  literals, and assemble `FMaterialSurface` only at the terminal boundary.
- [ ] Update Cooked Program validation/versioning so cooked payloads remain free
  of authored graph state and reject mismatched IR/compiler versions.
- [ ] Replace the default-material test expectation of eight constant IR nodes
  with zero ordinary nodes and one fully literal Surface Root.

Completion condition: default and existing per-property materials compile,
render, Cook, cache, and diagnose through the new root; unconnected fallbacks no
longer appear in the ordinary IR node count.

### Stage 2: Add aggregate Surface and Standard Surface semantics

- [ ] Add the `Surface` value category and `StandardSurface` opcode without
  allowing Surface values in arithmetic, conversion, texture, or ordinary
  property-link positions.
- [ ] Add the aggregate source to authored surface outputs and enforce the
  aggregate/per-property exclusivity rule in bounded validation.
- [ ] Implement the one-version schema-2 to schema-3 upgrade before current
  schema validation and keep save output canonical at schema 3.
- [ ] Implement the shared Standard Surface descriptor and use it for opcode
  validation, result typing, canonical built-in parameter dependencies, and
  deterministic normalization.
- [ ] Lower Standard Surface as one aggregate IR expression consumed by the
  Surface Root; do not materialize its implementation as authored nodes or
  generic normalized nodes.
- [ ] Generate the existing PBR computations and bindings from the intrinsic,
  retaining exact finite-value, channel, normal, roughness, emissive, UV, and
  sampler semantics.
- [ ] Update dependency inspection so base Details and material instances expose
  precisely the canonical declarations reachable through the intrinsic.
- [ ] Add invalid-graph coverage for mixed output modes, Surface type misuse,
  multiple/invalid aggregate sources, malformed schema upgrades, and bounds.

Completion condition: a one-node Standard Surface program is semantically and
visually equivalent to the legacy canonical expanded program across generated
source behavior, parameter resolution, and renderer output.

### Stage 3: Integrate the aggregate node into MaterialEditor

- [ ] Add Standard Surface to graph inspection and the closed creation catalog
  with a distinct aggregate Surface pin type and deterministic title/category.
- [ ] Extend graph commands with atomic connect, disconnect, and explicit mode
  switching for the aggregate Material Output input.
- [ ] Preserve the derived, non-copyable Material Output terminal while showing
  either the aggregate Surface row or the eight per-property rows according to
  the current mode and connection gesture.
- [ ] Extend geometry, link hit testing, semantic zoom, framing, deterministic
  layout, selection adjacency, and diagnostic navigation for Surface links.
- [ ] Extend clipboard, duplicate, replace, removal, Undo/Redo, and structured
  inspection tests without serializing the derived terminal itself.
- [ ] Ensure disconnecting or deleting Standard Surface returns atomically to
  the retained eight per-property fallbacks rather than creating hidden nodes.
- [ ] Add canvas rendering and save/reload coverage showing that the compact
  representation survives editor lifecycle operations.

Completion condition: users and structured callers can create and edit the
aggregate form without canvas-only behavior, semantic drift, or fabricated
primitive branches.

### Stage 4: Migrate and harden ImportedSurface

- [ ] Replace `MakeCanonicalMaterialProgram` as the creation path for
  `/Engine/Materials/ImportedSurface` with a deterministic one-node Standard
  Surface program while retaining the asset path and canonical definitions.
- [ ] Migrate the checked-in ImportedSurface package explicitly and validate
  that all generated material-instance parent references remain unchanged.
- [ ] Restrict legacy canonical-graph recognition to an exact, versioned
  fingerprint in the built-in migration tool; never collapse similar or edited
  user graphs.
- [ ] Make `EnsureImportedSurfaceMaterial` validate the expected template
  revision and fail with an actionable diagnostic on modified or stale built-in
  content rather than silently overwriting it during import.
- [ ] Verify scene import/reimport, opaque/masked/translucent instances, every
  texture semantic and derivation, thumbnails, preview, StaticMesh,
  SkeletalMesh, Terrain, forward, GBuffer, and shadow paths.
- [ ] Verify opening ImportedSurface presents exactly one editable Standard
  Surface node plus the derived Material Output terminal and does not synthesize
  the legacy DAG.
- [ ] Remove the expanded canonical builder only after all production and test
  callers use either the default program, Standard Surface, or an explicit
  focused fixture.

Completion condition: imported content uses the compact built-in parent without
path, parameter, appearance, import, Cook, or renderer regressions, and no
production path reconstructs the old canonical DAG.

### Stage 5: Qualification and lasting documentation

- [ ] Run focused material schema/compiler, graph operations, material instance,
  scene import, Cook, thumbnail, and Vulkan rendering tests according to the
  repository testing workflow.
- [ ] Run the affected Engine, MaterialEditor, AssetForgeBuiltins, Renderer, and
  RenderCore build targets according to the repository build workflow.
- [ ] Record before/after authored-node count, normalized ordinary-node count,
  canonical bytes, generated-source bytes, compile timings, cooked bytes, and
  ImportedSurface package size; treat measurements as evidence rather than a
  promise of GPU speedup.
- [ ] Update the authoritative Material System contract with the Surface Root,
  schema transition, intrinsic lowering, dependency, identity, and Cook rules.
- [ ] Update Material Graph Operations with aggregate pin, command, layout,
  clipboard, diagnostic, and presentation behavior.
- [ ] Update the Material System roadmap only if milestone status or child-plan
  sequencing changes; do not duplicate the implementation contract there.
- [ ] Close this plan only after all acceptance gates pass and lasting behavior
  has moved to the authoritative contracts.

Completion condition: all required builds/tests pass, measurements and migration
evidence are recorded, authoritative documentation owns the landed behavior,
and the plan can be marked completed.

## Acceptance Gates

### Representation

- [ ] A fresh material has zero authored expression nodes, zero ordinary IR
  nodes, and one literal Surface Root covering all eight outputs.
- [ ] A legacy per-property graph upgrades without changing node GUIDs, links,
  fallback literals, presentation, static properties, or rendered behavior.
- [ ] Standard Surface persists and normalizes as one aggregate expression; its
  implementation is not reconstructed as ordinary authored or IR nodes.
- [ ] Aggregate and per-property output sources cannot coexist in a valid
  program.

### ImportedSurface

- [ ] The stable `/Engine/Materials/ImportedSurface` path and canonical
  parameter GUIDs are unchanged.
- [ ] Existing generated material instances resolve the same parent and retain
  all value, texture, UV, sampler, alpha, and static-property overrides.
- [ ] Opening ImportedSurface shows one Standard Surface node and one derived
  Material Output terminal.
- [ ] Only the exact recognized built-in legacy graph is eligible for migration;
  arbitrary user graphs are never rewritten.

### Compiler and runtime

- [ ] Canonical encoding is deterministic across authored node order and GUID
  changes where the existing contract requires it.
- [ ] Version changes invalidate stale DDC and cooked artifacts, while warm
  compiles reuse the new identity normally.
- [ ] Default and Standard Surface generated shaders expose no unused material
  resource bindings after backend reflection.
- [ ] StaticMesh, SkeletalMesh, Terrain, preview, thumbnail, forward, GBuffer,
  masked shadow, and translucent behavior remain qualified.
- [ ] Failures remain bounded and identify the aggregate root, intrinsic node,
  input, or program without partial publication.

### Editor and automation

- [ ] Canvas and structured commands share the same aggregate creation,
  connection, disconnection, replacement, deletion, layout, and inspection
  semantics.
- [ ] Copy/paste and Undo/Redo preserve aggregate programs without copying or
  persisting the derived Material Output terminal.
- [ ] Frame, semantic zoom, hit testing, diagnostics, and save/reload work with
  the Surface link and compact ImportedSurface graph.

## Risks and Controls

- **Hidden dependency drift:** Standard Surface implicitly consumes canonical
  roles. Control this with one descriptor shared by validation, dependency
  inspection, lowering, and editor inspection, plus an exact dependency-set
  test.
- **Silent shader behavior change:** replacing a visible DAG with intrinsic
  lowering can change saturation, channel, or fallback details. Control this
  with operation-level generated-source assertions and rendered qualification
  before migrating the asset.
- **Schema rejection of existing assets:** current loading expects the current
  program schema. Land and qualify the bounded schema-2 upgrade before writing
  any schema-3 asset.
- **Ambiguous aggregate overrides:** allowing aggregate and property sources
  together creates hidden precedence. Reject mixed mode and require one atomic
  explicit switch.
- **Over-specialization:** ImportedSurface must not become an importer-owned
  shader shortcut. Keep Standard Surface in Engine as a closed PBR semantic and
  keep AssetForgeBuiltins responsible only for the stable built-in asset.
- **Misleading performance claims:** fewer nodes primarily improve semantic
  size, serialization, inspection, and compiler bookkeeping. Report GPU or
  compile performance only from measurements.

## Related Documentation and Code

- [Material System](../Runtime/Rendering/MaterialSystem.md)
- [Material Graph Operations](../Editor/Architecture/MaterialGraphOperations.md)
- [Material System Roadmap](../Roadmaps/MaterialSystem.md)
- [`MaterialProgramTypes.h`](../../Engine/Source/Runtime/Engine/Public/Materials/MaterialProgramTypes.h)
- [`MaterialProgramTypes.cpp`](../../Engine/Source/Runtime/Engine/Private/Materials/MaterialProgramTypes.cpp)
- [`MaterialProgramCompiler.h`](../../Engine/Source/Runtime/Engine/Public/Materials/MaterialProgramCompiler.h)
- [`MaterialProgramCompiler.cpp`](../../Engine/Source/Runtime/Engine/Private/Materials/MaterialProgramCompiler.cpp)
- [`MaterialProgramGenerator.cpp`](../../Engine/Source/Runtime/Engine/Private/Materials/MaterialProgramGenerator.cpp)
- [`MaterialCookedProgram.cpp`](../../Engine/Source/Runtime/Engine/Private/Materials/MaterialCookedProgram.cpp)
- [`MaterialGraphOperations.h`](../../Engine/Source/Editor/MaterialEditor/Public/MaterialGraphOperations.h)
- [`MaterialGraphCanvas.cpp`](../../Engine/Source/Editor/MaterialEditor/Private/Graph/MaterialGraphCanvas.cpp)
- [`ImportedSurfaceMaterial.cpp`](../../Engine/Source/Editor/AssetForgeBuiltins/Private/ImportedSurfaceMaterial.cpp)
- [`SceneImport.cpp`](../../Engine/Source/Editor/AssetForgeBuiltins/Private/SceneImport.cpp)
- [`MaterialSchemaAndEditingTests.cpp`](../../Engine/Tests/Native/EngineTests/Private/Materials/MaterialSchemaAndEditingTests.cpp)
- [`MaterialGraphOperationsTests.cpp`](../../Engine/Tests/Native/EngineTests/Private/Materials/MaterialGraphOperationsTests.cpp)
