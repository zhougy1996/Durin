# Non-Mutating Cook Package Serialization Plan

Summary: Replace live-asset Cook mutation with recursive EditorOnly filtering and non-mutating per-save value overrides

Last reviewed: 2026-08-23

Status: Completed
Completed: 2026-08-23

## Current Status

Implementation is complete. CoreDObject now applies one recursive reflected
save predicate for `Transient`, deprecated, and `EditorOnly` fields. AssetCore
owns authored/cooked save domains, exact Cook target state, owned object/property
save overrides, frozen-graph validation, and an explicit captured-package to
DAST-encoding boundary. The repeated-encode contract test encodes one capture
twice without returning to reflected field traversal.

Texture2D, TextureCube, VolumeTexture, TerrainHeightmap, StaticMesh,
SkeletalMesh, AnimationClip, and EnvironmentLighting now build Cook packages
through `FCookContext` policy and owned descriptor replacements. Stable source
provenance and diagnostics are `EditorOnly`; StaticMesh collision is built into
a detached candidate and both mesh and BodySetup descriptors are overridden
without live writes. Source audit finds none of the migrated retention booleans,
Cook `PropertyFilter` lambdas, descriptor setters, or save/clear/restore blocks.

Validation on Win64-Debug-DurinEditor passed: full `all` build;
DurinHeaderTool pytest 197/197; CoreObjectTests 80/80;
AssetPackageTests 108/108; AssetCookTests 13/13;
AssetBulkContainerTests 11/11; TextureTests 80/80; StaticMeshTests 73/73;
SkeletalAssetTests 34/34; TerrainHeightmapCookTests 1/1;
EnvironmentLightingTests 3/3; TextureCookIntegrationTests 1/1; `fast-all`
58/58 targets; and the repository-wide native gate 78/78 targets. Serialization,
Asset Packages, and Asset Data Lifecycle own the lasting contracts.

## Goal

Make Cook package serialization a read-only operation over live assets. A Cook
must produce the same runtime package and bulk-payload semantics without
temporarily changing authored fields, generated payload descriptors, package
dirty state, revisions, diagnostics, or observable object state.

Adopt Unreal-compatible terminology and behavior:

- `EditorOnly` is a reflected property flag whose values remain in ordinary
  editable packages and are recursively excluded when an archive filters
  editor-only data.
- `DURIN_WITH_EDITOR` remains a compile-time behavior boundary and does not
  substitute for serialization filtering.
- A future `DURIN_WITH_EDITORONLY_DATA` layout optimization may complement
  `EditorOnly`, but it is not required to make Cook serialization correct.
- Cook-generated reflected values are supplied through an owned per-save
  override rather than installed on the live object.

## Scope

- Add `EPropertyFlags::EditorOnly`, DurinHeaderTool annotation support, and
  reflection tests for `DPROPERTY(EditorOnly)`.
- Apply one shared property-selection rule to reflected object fields and
  recursively visited reflected struct fields.
- Give package capture an explicit authored-versus-cooked save domain, target
  facts, and editor-only filtering state.
- Add an AssetCore-owned per-save override model, analogous in purpose to
  Unreal's `FObjectSaveOverride`, that can omit an object/property for one save
  or serialize an owned replacement value without mutating its source object.
- Migrate the eight known Engine Cook asset families and StaticMesh BodySetup
  collision metadata away from save/clear/restore callbacks.
- Replace per-family diagnostic-retention booleans with one Cook-context policy
  controlling editor-only filtering.
- Formalize the existing captured-package representation as the immutable
  boundary between live-object capture and byte encoding.
- Add failure, determinism, nested-field, authored/cooked round-trip, and
  live-state preservation coverage.
- Update the lasting asset lifecycle, asset package, and serialization
  contracts when their implementation lands.

## Non-Goals

- Compiling reflected members out of DurinGame object layouts in this plan.
- Adding `DURIN_WITH_EDITORONLY_DATA`; this remains a separate layout and
  cross-variant reflection-schema decision.
- Implementing a fully concurrent Cooker, background live-object traversal, or
  concurrent editor mutation.
- Redesigning DDC keys, family payload schemas, DBLK layout, compression,
  manifest publication, redirector canonicalization, or runtime payload load.
- Converting every custom archive or non-package serializer to filter
  `EditorOnly`; each archive must opt in explicitly.
- Treating `EditorOnly` as `Transient`. Editor-only fields continue to persist
  in ordinary editable `.dasset` packages.
- Removing the general authored-package `PropertyFilter` until all non-Cook
  clients have been audited. Cook code must stop depending on it.

## Design Decisions and Invariants

### Property semantics

- The reflected spelling is `DPROPERTY(EditorOnly)` and the runtime flag is
  `EPropertyFlags::EditorOnly`; do not introduce an `AuthoredOnly` synonym.
- `EditorOnly` means "eligible for ordinary editable-package persistence but
  omitted when `Ar.IsFilterEditorOnly()` is true." It does not mean
  `DURIN_WITH_EDITOR`, `Edit`, `ReadOnly`, or `Transient`.
- Ordinary package save/load, duplication, editable copy, property snapshot,
  and transactions retain their existing behavior unless their archive
  explicitly enables editor-only filtering.
- Cooked package capture sets `Purpose=CookedPackage`, `bPersistent=true`,
  `bCooking=true`, `bFilterEditorOnly=true` by default, and carries the exact
  platform/profile target.
- The same central predicate filters top-level object properties and nested
  reflected struct properties. Arrays, maps, and fixed arrays inherit the
  decision from the property that owns the value; individual container elements
  do not invent independent policy.
- Custom struct serializers that mix editor-only and runtime fields must honor
  the archive state themselves or be converted to complete reflected-field
  traversal. The package writer must reject a claimed automatic filter that a
  custom serializer bypasses.
- `EditorOnly` remains reflection/save-policy metadata. It does not add a DAST
  field-record member or require a package-format bump; current in-memory field
  manifest comparison may continue to compare the complete live flags across
  discovery and capture passes.

### Per-save overrides

- Save overrides are owned values with lifetime covering every discovery,
  validation, capture, and encoding pass. They never borrow stack references or
  descriptor spans from `FCookContext::AddPackage` callbacks.
- Overrides are keyed by exact object identity and reflected property identity,
  not property-name strings.
- Registration validates that the target object belongs to the frozen package
  graph, the property belongs to the object's reflected schema, the replacement
  type matches exactly, and no conflicting override exists.
- A value override serializes the replacement through the same reflected
  property codec used for the live value. Object and soft references retain
  ordinary package identity and dependency semantics.
- An omission override is explicit per-save policy. `EditorOnly` remains the
  declaration-site mechanism for stable authored-versus-cooked ownership.
- Discovery and payload capture observe the same effective values and omitted
  fields. A mismatch fails before bytes are published.
- `CookedPayload` and `DBodySetup::CookedCollisionPayload` remain runtime
  package fields. They are not `EditorOnly`; Cook supplies their descriptor
  values through save overrides.

### Ownership, threading, and failure

- Core owns archive state and reflected property traversal; CoreDObject owns
  reflected value operations; AssetCore owns package save policy, override
  validation, immutable package capture, and encoding; Engine owns classification
  of family fields and creation of family Cook overrides.
- Live package graph discovery and value capture execute on the asset-owning
  thread under the existing no-concurrent-edit assumption. They perform no
  writes to the source objects.
- The captured package owns all bytes, strings, descriptors, dependency paths,
  and replacement values needed for encoding. Encoding and publication must not
  dereference the live package graph.
- Failure at override registration, discovery, capture, encoding, validation,
  or publication leaves live object values, package dirty state, revisions, and
  diagnostics unchanged.
- Cook output remains deterministic for equal live state, Cook target, payload
  bytes, and override values.
- The default production policy strips editor-only data. A diagnostic Cook may
  retain it only through one `FCookContext` policy; asset-family APIs do not
  carry independent retention booleans.

### Compatibility

- Existing ordinary authored packages remain loadable without resave.
- Cooked runtime packages keep their existing asset classes, property names,
  payload descriptor types, payload ids, schema versions, and bulk locations.
- Removing an editor-only field from Cook output is treated as intentional
  absence and loads the field's default value; it is not an incompatible schema
  error.
- Existing redirector resolution and Cook canonicalization remain after package
  capture and operate on the produced bytes exactly as before.

## Current Foundations and Gaps

### Foundations to reuse

- Core `FArchiveState` already contains Cook, editor-only filtering, target, and
  purpose facts.
- CoreDObject has central reflected object/struct traversal and property value
  snapshot/storage operations.
- AssetCore package capture already freezes the package object graph, performs
  discovery and payload passes, and builds an internal `FCapturedPackage` tree
  before DAST encoding.
- `FCookContext::AddPackage` already delays package-byte construction until
  DBLK descriptors are known.
- Engine Cook tests already validate payload descriptor round trips and the
  absence of selected source metadata in loaded cooked assets.

### Gaps to close

- `EPropertyFlags` and DurinHeaderTool do not recognize `EditorOnly`.
- CoreDObject skips only `Transient` and deprecated-on-save fields; it does not
  consume `IsFilterEditorOnly()` at either object or nested struct depth.
- AssetCore package capture always presents authored-package semantics and its
  callback filter applies only to top-level properties.
- `FAssetPackageSerializationOptions` has no domain, target, or owned save
  override data.
- Engine Cook callbacks maintain independent save, clear, filter, and restore
  lists. Nested material-slot provenance cannot be expressed by the current
  top-level filter.
- Current tests assert cooked output but do not systematically assert that the
  live package remains byte-for-byte and semantically unchanged during success
  and injected failure.
- Capture and encoding are adjacent internal steps rather than an enforced
  lifetime/thread boundary.

## Implementation Stages

### Stage 0: Define the implementation boundary

- [x] Inventory the live-mutation pattern across all known Cook asset families.
- [x] Confirm the existing Core, CoreDObject, AssetCore, and Engine ownership
  seams.
- [x] Select UE-compatible `EditorOnly` terminology and keep compile-time macros
  orthogonal to serialization policy.
- [x] Select recursive archive filtering for stable field ownership and owned
  per-save overrides for Cook-generated values.
- [x] Record compatibility, threading, failure, and diagnostic-policy
  invariants.

#### Acceptance Gate

- Passed: scope, selected design, affected families, and validation requirements
  are explicit; Stage 1 can begin without an unresolved architecture choice.

### Stage 1: Implement recursive EditorOnly archive semantics

- [x] Add `EPropertyFlags::EditorOnly` and update DurinHeaderTool parsing,
  generated registration, diagnostics, and parser/writer tests for
  `DPROPERTY(EditorOnly)`.
- [x] Introduce one CoreDObject property-selection helper that preserves current
  transient/deprecation behavior and additionally skips `EditorOnly` when the
  archive filters editor-only data.
- [x] Apply the helper to `SerializeDObjectProperties` and reflected struct
  fallback serialization at every container depth.
- [x] Add CoreObjectTests coverage proving ordinary archives include
  editor-only values while filtering archives omit top-level and nested values.
- [x] Extend AssetCore package serialization options with an explicit save
  domain and target state; retain authored semantics as the default.
- [x] Make cooked capture use `EArchivePurpose::CookedPackage`, persistent Cook
  state, target facts, and the selected editor-only policy in both discovery and
  payload passes.
- [x] Add AssetPackageTests coverage for authored round trip, cooked omission,
  nested struct omission, deterministic repeated capture, and missing-field
  defaulting on load.

#### Acceptance Gate

- `DPROPERTY(EditorOnly)` is reflected correctly, ordinary authored packages
  retain it, cooked package capture recursively excludes it, and no live value
  is changed to achieve either result.

### Stage 2: Add non-mutating per-save overrides

- [x] Add AssetCore `FObjectSaveOverride`-style public types for per-object
  omission and per-property omission/value replacement, using exact reflected
  identities and owned replacement storage.
- [x] Provide a type-safe registration helper that copies replacement values,
  retains reference roots where required, and reports duplicate, foreign-object,
  foreign-property, and type-mismatch failures.
- [x] Apply overrides during graph discovery and value capture without changing
  the source container pointer or property storage.
- [x] Ensure override values participate in dependency collection, default-delta
  decisions, deterministic map ordering, manifest equality, and package
  validation exactly like live values.
- [x] Define and test precedence: object omission, explicit property omission,
  `EditorOnly` filtering, value override, then ordinary live value.
- [x] Add failure-injection coverage proving invalid overrides and capture/encode
  failures leave the entire live package unchanged.
- [x] Add CorePropertyValueSnapshotTests or a narrower new contract target only
  if the owned reflected-value machinery requires new registration or fixture
  ownership.

#### Acceptance Gate

- AssetPackageTests can serialize a package with effective property values and
  omissions different from the live object, including an inner package object,
  while observers and post-call snapshots see the original object state on
  success and failure.

### Stage 3: Migrate Cook asset families

- [x] Mark Texture2D source import data, source hash/fingerprint, dimensions,
  channel count, and transparency provenance `EditorOnly`; replace its
  `CookedPayload` swap with a value override.
- [x] Mark TextureCube source import data `EditorOnly` and override its cooked
  descriptor without mutation.
- [x] Mark VolumeTexture retained source/source-import data `EditorOnly` and
  override its cooked descriptor without mutation.
- [x] Mark TerrainHeightmap source import data and source file/format diagnostics
  `EditorOnly` and override its cooked descriptor without mutation.
- [x] Mark StaticMesh source import data plus nested material-slot `SourceName`
  and `SourceMaterialIndex` fields `EditorOnly`; override render and BodySetup
  collision descriptors without mutation.
- [x] Mark SkeletalMesh derived-data key and nested material-slot source fields
  `EditorOnly`; override its cooked descriptor without mutation.
- [x] Mark AnimationClip derived-data key `EditorOnly` and override its cooked
  descriptor without mutation.
- [x] Override EnvironmentLighting's cooked descriptor without mutation.
- [x] Classify `DBodySetup::CollisionBuildRevision` and
  `CollisionBuildStatus` under the shared editor-only/transient rule selected by
  their authored persistence requirement; remove the Cook name filter.
- [x] Replace family-specific `bRetainDiagnosticSourceMetadata`,
  `bRetainDiagnosticSourceData`, and `bRetainDiagnosticEditorMetadata` parameters
  with the `FCookContext` editor-only-data policy.
- [x] Delete the migrated save/clear/restore blocks and Cook-specific
  `PropertyFilter` lambdas; add a source audit that rejects recurrence in the
  migrated families.
- [x] For every migrated family, capture all affected live properties, package
  dirty state, and relevant revisions before Cook and compare them after both a
  successful package build and an injected failure.

#### Acceptance Gate

- All eight families produce loadable equivalent cooked packages with exact
  required descriptors and stripped editor-only data, and none writes to a live
  asset or inner object during package serialization.

### Stage 4: Enforce the immutable capture/encode boundary

- [x] Separate live graph capture from DAST byte encoding around the existing
  captured-package representation without exposing AssetCore-private wire
  structures to Engine.
- [x] Make the captured representation own all effective override values,
  strings, reference identities, dependencies, and authored bulk transfers.
- [x] Add assertions or API constraints that live-object access ends when
  capture completes and that encoding consumes only the immutable captured
  representation.
- [x] Preserve the frozen-graph check and ensure object creation/removal during
  capture fails without publishing partial bytes.
- [x] Add a deterministic encode test that captures once, encodes repeatedly,
  and receives identical bytes without revisiting the live graph.
- [x] Document the thread contract: capture remains on the asset-owning thread;
  a future Cooker may schedule encode/publication off-thread only after capture.

#### Acceptance Gate

- Package encoding and validation can complete from an immutable captured value
  after the live traversal scope has ended, with deterministic output and no
  source-object dereference.

### Stage 5: Complete validation and publish lasting contracts

- [x] Run the smallest affected contract and feature targets throughout the
  implementation, then the bounded Cook integration targets in the validation
  matrix.
- [x] Run `fast-all`, followed by the repository-wide native-test gate because
  the change alters shared CoreDObject reflection and package serialization.
- [x] Confirm production Cook defaults to filtering editor-only data and that an
  explicit diagnostic policy retains it consistently across migrated families.
- [x] Verify existing authored packages load without resave and repeated Cook
  output remains deterministic.
- [x] Update Serialization, Asset Packages, and Asset Data Lifecycle with the
  implemented property, override, capture, and thread contracts; avoid leaving
  the completed plan as the only authority.
- [x] Remove or narrow obsolete Cook helper APIs and verify no migrated Cook path
  installs temporary descriptors or clears reflected source metadata.

#### Acceptance Gate

- All required tests and documentation validators pass, lasting contracts own
  the implemented behavior, and every Definition of Done item has evidence.

## Validation Matrix

| Concern | Required coverage | Evidence |
| --- | --- | --- |
| Header annotation and generated flags | DurinHeaderTool reflection-property parser/writer tests | Generated registration contains `EPropertyFlags::EditorOnly`; invalid spellings remain diagnostic |
| Recursive archive filtering | `CoreObjectTests` focused cases | Top-level, struct, array-of-struct, map-value struct, and fixed-array fields follow the same filter |
| Owned replacement values | `CorePropertyValueSnapshotTests` when changed, plus `AssetPackageTests` | Scalar, struct descriptor, object reference, and soft reference replacements are owned and type checked |
| Package-domain behavior | `AssetPackageTests` | Authored save includes EditorOnly; cooked save omits it and preserves defaults on load |
| Generic Cook integration | `AssetCookTests`, `AssetBulkContainerTests` | Target state, descriptor/bulk placement, manifest, and canonicalization remain valid |
| Texture families | `TextureTests`, `TextureCookIntegrationTests` | Texture2D, TextureCube, and VolumeTexture output/load parity and live-state preservation |
| StaticMesh and collision | `StaticMeshTests` | Nested slot provenance is stripped, both descriptors load, BodySetup/live mesh remain unchanged |
| SkeletalMesh and AnimationClip | `SkeletalAssetTests` | Derived keys/source slot metadata are stripped and descriptors/load behavior remain valid |
| Terrain | `TerrainHeightmapCookTests` | Source metadata is stripped, payload loads, live heightmap remains unchanged |
| Environment lighting | `EnvironmentLightingTests` | Descriptor and payload load without live descriptor mutation |
| Failure safety | Focused AssetPackageTests and family failure-injection cases | Every failure phase preserves reflected values, dirty state, revisions, and diagnostics |
| Determinism and immutable encode | AssetPackageTests repeated capture/encode cases | Equal inputs produce equal package bytes; repeated encode does not visit live objects |
| Broad shared-runtime regression | `fast-all`, then the repository-wide native-test gate | Required because `EPropertyFlags` and shared reflected serialization change |
| Documentation lifecycle | `doc validate --scope changed` and `doc plan validate --scope all` | Active plan and lasting contracts have valid metadata and links |

Follow [Agent Testing Workflow](../Agents/Testing.md) for selection and
[Agent Build and Run Workflow](../Agents/BuildAndRun.md) before configuring,
building, or running repository targets. GPU-backed texture integration is a
correctness gate here, not a performance qualification.

## Definition of Done

- No migrated Cook implementation saves and restores reflected asset fields or
  installs a generated descriptor on a live object for package serialization.
- Every stable editor-only field is declared once with `DPROPERTY(EditorOnly)`;
  Cook code contains no parallel string-name exclusion list for it.
- Cooked package capture recursively honors `IsFilterEditorOnly()` and exposes
  exact Cook target facts.
- Generated descriptor values use validated, owned per-save overrides.
- Ordinary authored save/load semantics and existing package compatibility are
  preserved.
- Success, injected failure, and observer tests prove live assets and package
  state remain unchanged.
- Package capture and encode have an enforced immutable boundary suitable for
  later off-thread encoding.
- Production and diagnostic editor-only-data policies are centralized in
  `FCookContext`.
- Required focused, integration, broad native, HeaderTool, and documentation
  validation passes are recorded in Current Status.
- Lasting runtime documentation describes the final behavior and ownership.

## Deferred Follow-ups

- Introduce `DURIN_WITH_EDITORONLY_DATA` only with an explicit cross-variant
  reflection, ABI, HeaderTool, AssetTool, and authored-package compatibility
  design.
- Schedule immutable package encoding and publication on workers after the
  Cooker has an owned task/lifetime model; this plan establishes but does not
  exercise that concurrency.
- Decide whether the general authored-package `PropertyFilter` should be
  deprecated after all non-Cook clients are inventoried.
- Consider required-at-Cook metadata for descriptor properties if additional
  asset families repeatedly omit mandatory overrides.
- Generalize diagnostic editor-only retention into finer categories only when a
  concrete shipping/debug package requirement cannot use the single context
  policy.

## Related Documentation

- [Asset Data Lifecycle and Storage](../Runtime/Assets/AssetDataLifecycle.md)
- [Asset Packages](../Runtime/Assets/AssetPackages.md)
- [Serialization](../Runtime/Core/Serialization.md)
- [Volume Textures](../Runtime/Assets/VolumeTextures.md)
- [Agent Testing Workflow](../Agents/Testing.md)
- [Agent Build and Run Workflow](../Agents/BuildAndRun.md)

## Related Code

- [`FArchiveState` and archive purposes](../../Engine/Source/Runtime/Core/Public/Serialization/Archive.h)
- [`EPropertyFlags`](../../Engine/Source/Runtime/CoreDObject/Public/DObject/ObjectMacros.h)
- [Reflected object and struct serialization](../../Engine/Source/Runtime/CoreDObject/Private/DObject/Archive.cpp)
- [Asset package serialization options](../../Engine/Source/Runtime/AssetCore/Public/Asset/PackageAuthoring.h)
- [DAST v4 package capture](../../Engine/Source/Runtime/AssetCore/Private/AssetPackageV4ArchiveAdapter.cpp)
- [`FCookContext`](../../Engine/Source/Runtime/AssetCore/Public/Asset/Cook.h)
- [Texture2D Cook](../../Engine/Source/Runtime/Engine/Private/Texture/Texture2D.cpp)
- [TextureCube Cook](../../Engine/Source/Runtime/Engine/Private/Texture/TextureCube.cpp)
- [VolumeTexture Cook](../../Engine/Source/Runtime/Engine/Private/Texture/VolumeTexture.cpp)
- [TerrainHeightmap Cook](../../Engine/Source/Runtime/Engine/Private/Terrain/TerrainHeightmap.cpp)
- [StaticMesh Cook](../../Engine/Source/Runtime/Engine/Private/StaticMesh/StaticMeshCook.cpp)
- [SkeletalMesh Cook](../../Engine/Source/Runtime/Engine/Private/SkeletalMesh/SkeletalMesh.cpp)
- [AnimationClip Cook](../../Engine/Source/Runtime/Engine/Private/Animation/AnimationClip.cpp)
- [EnvironmentLighting Cook](../../Engine/Source/Runtime/Engine/Private/EnvironmentLighting/EnvironmentLighting.cpp)
