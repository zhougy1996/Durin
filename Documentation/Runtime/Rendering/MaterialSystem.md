# Material System

Summary: Define material assets, parameters, render proxies, invalidation, passes, and fallback behavior.

Modules: Engine, Renderer, RenderCore

Last reviewed: 2026-08-25

Durin's material architecture keeps declaration ownership, instance resolution,
editor presentation, and renderer consumption at explicit boundaries.

`Materials/MaterialTypes.h` is the authored/reflected parameter and static
property surface. Renderer-facing layout, compatibility, immutable
representation, builder, pipeline identity, and fallback declarations live in
`Materials/MaterialRenderTypes.h`; `MaterialRenderProxy.h` includes that narrow
surface directly. Their implementations are separated into canonical authored
schema, layout compatibility, representation/builder, and diagnostics files.
This split retains the current v3 table and identity, the isolated v1 decoder,
the error material, and diagnostic counters.

## Parameter Domain

- `DMaterialInterface` is the common asset/component-facing contract. Persistent
  parameter identity is an `FGuid`; public human/API lookup uses
  case-insensitive `FName`.
- Runtime Engine owns `FMaterialParameterDefinition`, including identity, name,
  type, base value, display metadata, ordering, presentation, numeric range, and
  texture-usage hint. The Material Editor consumes this schema and does not own
  a parallel descriptor table.
- `DMaterial` stores one ordered reflected definition collection. Definition
  identity, type, order, and metadata are canonical; only the nested values are
  editable.
- `DMaterialInstance` references a parent material interface and stores one
  ordered collection of GUID/value overrides plus an optional all-or-nothing
  static-property override. Dynamic resolution walks the current
  instance, its parent instances, and the root material, and reports the object
  that supplied the value. Parent cycles are rejected.
- Parent changes preserve unmatched overrides as orphans for explicit editor
  removal. Orphans are never resolved into render data.
- `DMaterial` owns one reflected static-property set: blend mode, shading model,
  two-sided state, depth-write policy, and masked-opacity threshold. Instances
  inherit that complete set through the canonical parent chain unless their
  validated complete static override is active; scene import uses this boundary
  for glTF alpha and two-sided state.
- Resolved static properties form a versioned shader-map identity (blend mode,
  shading model, and mask threshold) nested in a pipeline identity (shader map,
  two-sided state, and depth-write policy). The renderer lazily caches shader
  maps by shader identity and PSOs by the complete effective pipeline identity,
  so a pipeline-only change does not rebuild the shader map.
- Opaque sections disable blending; Masked sections additionally discard the
  saturated OpacityMask constant/texture product only when it is strictly below
  the static threshold; Translucent sections use straight-alpha blending and
  deterministic per-view back-to-front center sorting. Automatic depth writes
  are enabled for Opaque/Masked and disabled for Translucent, while explicit
  depth policy overrides that default. One-sided materials cull back faces with
  mirrored-transform winding correction; two-sided materials disable culling.

The current shader contract is the canonical metallic/roughness PBR schema.
It declares BaseColor, tangent-space Normal, Metallic, Roughness, Ambient
Occlusion, Emissive, Opacity, and OpacityMask constants and Texture2D roles.
Every texture role also owns UV-channel, UV-scale, UV-offset, UV-rotation, and
packed sampler-state parameters. The sampler state preserves glTF minification,
magnification, mip filtering, and independent U/V addressing.
Their GUIDs are permanent because serialized overrides must survive renames.
Engine resolution compiles the declarations into the versioned v3 render
layout identified by `MaterialRenderLayoutV3Id`; the layout owns compact
uniform offsets and eight resource indices. User-authored parameter
declarations and compiled layouts remain deferred work.

`DMaterial` additionally persists one reflected material program defined by
`Materials/MaterialProgramTypes.h`. Version 1 is a bounded typed expression DAG
with stable node/parameter/link identities and one required record covering all
eight surface outputs. Constants, parameter and texture reads, UV resolution,
sampling, arithmetic, composition, explicit conversions, safe normal decode,
and RNM normal blending form the closed opcode domain. `DMaterialInstance`
stores no graph and resolves the root base program through its existing parent
chain, so dynamic GUID overrides remain independent of authored node order.

Fresh materials own the permanent canonical fixed-surface program. Packages
written before the `Program` field existed retain that constructor value during
load and write version 1 on their next save. A present empty, unknown-version,
or malformed program does not take the legacy transition: bounded validation
rejects invalid enums and GUIDs, count/string/byte/input/depth limits, dangling
links, cycles, non-finite constants, bad parameter references, input types, and
missing or incompatible surface outputs before residency. Diagnostics are
bounded and sort by stable category/node/location/message identity. Duplication
deep-copies program values while preserving program GUIDs; presentation names
round trip but do not affect rendering semantics.

The persisted program is authored state, not a render artifact. GameThread can
snapshot it, parameter declarations, code-affecting static properties, target,
compiler identity, and virtual dependency fingerprints into a detached value
request. Normalization removes dead and presentation-only state, canonicalizes
commutative inputs and numeric bytes, and produces versioned typed IR plus a
stable digest independent of authored node order, node GUIDs, and dynamic
parameter/resource values. Two-sided state and depth policy remain pipeline-
only and do not enter this program digest.

The synchronous compiler lowers that IR to bounded deterministic Slang using
stable IR-index symbols and exact floating-point bit expressions. RenderCore
accepts the generated root as owned memory, resolves only allowlisted virtual
imports, retains cache/artifact ownership, compiles forward, GBuffer, and
masked-shadow fragments, and reflects their exact 24/17/3 binding contracts.
The complete value-owned result includes identity, IR, source, dependencies,
three compiled stages, phase timings, and bounded diagnostics; any failure
retains no publishable partial stage set.

Base materials synchronously compile at construction, successful load, program
edits, and blend/shading/mask-threshold edits. Their immutable render data and
base proxy layer carry a shared accepted compiler result; its digest extends
`FMaterialShaderMapIdentity`. Instances inherit the exact parent handle.
Dynamic values/textures and pipeline-only two-sided/depth changes rebuild only
the existing v3 layer and preserve the compiled identity. A required compile
failure publishes no partial stages and resolves to ErrorMaterial with bounded
diagnostics. Identical canonical bases reuse the same accepted immutable result
within the process.

Production Renderer resource slots key generated shader maps, PSOs, diagnostics,
and deterministic draw ordering by the material-program digest plus the exact
pass and geometry-domain contract. On the rendering thread they combine the
shared fixed geometry vertex stage with the accepted generated `FragmentMain`,
`GeometryFragmentMain`, or `ShadowFragmentMain` artifact and create a complete
typed shader map transactionally. Opaque shadow retains the fixed material-
resource-free fragment. StaticMesh, SplineMesh, SkeletalMesh, Terrain, Material
Preview, and thumbnails therefore consume the same accepted surface program;
none reads the authored graph or IR.

Generated forward evaluation uses the same world normal frame, specular-AA,
directional/local/environment lighting, shadow, UV transform/rotation, Terrain
coverage, and exact-v3 binding helpers as the fixed characterization path.
GBuffer uses the same evaluator and publishes the established octahedral
normal, effective roughness, AO, opacity, and emissive encoding. Shader reload
rebuilds shader-dependent slots and device invalidation discards then lazily
recreates device-dependent RHI resources from the retained accepted compiler
result; neither operation reinterprets the authored program.

## Renderer Boundary

- `DStaticMesh` owns the ordered material-slot definitions. Each slot has a
  mesh-local user-facing `Name`, exact imported `SourceName`, original
  `SourceMaterialIndex`, and an optional default material. The vector index is
  the stable slot identity; sections store only that compact index.
- Reimport matches unique non-empty exact source names first and unique source
  indices second. Matched entries keep their existing index, user name, and
  default while source evidence is refreshed. Removed entries remain reserved,
  new entries append, and section construction consumes the explicit imported-
  to-stable map rather than searching historical source indices.
- `DStaticMeshComponent` persists a positional `OverrideMaterials` array.
  Resolution for each current index is non-null component override, mesh
  default, then the Engine-owned `/Engine/Materials/DefaultMaterial` proxy.
  Empty assignments remain null in serialized component and mesh state; the
  service binding is transient. Mesh assignment preserves the complete
  array: shared indices apply immediately, entries beyond a smaller mesh are
  dormant, and a later larger mesh reactivates them. Dormant entries bind no
  dependency and never reach the scene proxy; Clear All removes them too.
- Material-side code resolves the canonical built-in GUIDs into one immutable
  `FMaterialRenderRepresentation` carried by `FMaterialRenderData` alongside
  the static shader/pipeline identity. `FMaterialRenderRepresentationBuilder`
  is the only GUID-to-layout compilation seam. The renderer consumes the
  validated v3 binding contract and never performs GUID or `FName` lookup or
  reads reflected material objects.
- StaticMesh, SkeletalMesh, and Terrain use one Renderer-private material
  binding resolver. It accepts only the exact v3 field table through
  `TryGetMaterialRenderBinding`; an unsupported layout records the shared
  fallback reason, emits a renderer-specific `ShaderBinding` diagnostic, and
  selects the code-constructed ErrorMaterial before shader-map or pipeline
  selection. An incompatible terminal is a checked invariant and skips the
  affected production draw.
- Scene-proxy construction walks the current mesh slots in order and retains
  one stable `FMaterialRenderProxyRef` binding per slot. A mesh assignment or
  rebuilt mesh render layout replaces the proxy; dynamic parameter, static
  identity, and parent changes publish through the existing proxy in place.
- Dirty flags classify the publication that a material mutation produces:
  dynamic parameters and proxy-safe static properties publish a new immutable
  local layer, while parent changes publish a new parent-proxy identity and
  local version. The render thread resolves inherited state lazily.
- A material publication owns one pending wave per proxy. Repeated edits before
  the render command is consumed replace that pending wave, so only the newest
  immutable state is applied. The command stream preserves publication order
  before later rendering commands consume the proxy. If a material is edited
  while render-command admission is stopped, the retained wave is replayed when
  the rendering thread starts; the first preview or scene-proxy consumer does
  not observe an uninitialized snapshot.
- Component material assignment is a binding update, not material-content
  invalidation. A component-wide revision orders rapid changes across
  independent slots and rejects stale render commands.
- The static mesh shader implements Cook-Torrance GGX direct lighting and
  split-sum image-based environment lighting, plus an unlit viewport mode.
  Direct evaluation clamps perceptual roughness to `[0.045, 1]`; that positive
  lower bound makes the GGX distribution denominator finite without an
  independent epsilon floor. The distribution term uses the stable equivalent
  form `(1 - NoH^2) + NoH^2 * alpha^2`, preserving the supported smooth-surface
  peak. Standard-Lit surfaces apply renderer-owned specular antialiasing after
  the final world-space shading normal is built: bounded screen derivatives of
  that normal increase an effective perceptual roughness before both direct and
  split-sum environment evaluation. The authored roughness, material render
  representation, shader-map identity, and BRDF denominator remain unchanged.
  `FSceneViewModeSettings::bEnableSpecularAA` defaults on and exists only as a
  per-view development A/B seam; there is no material parameter or normal-map
  payload change.
  Missing role textures retain the selected material and use Renderer-owned
  white, black, or flat-normal resources. Missing environment resources retain
  the selected material and use the black environment set.

### Default and Error Surfaces

`FDefaultMaterialService` is initialized on the game thread from
`DEngine::Init()` after Engine Content mounts and render-command admission are
ready, before world or scene-proxy creation. It synchronously loads the exact
base `DMaterial` at `/Engine/Materials/DefaultMaterial`, roots that asset, and
retains one counted proxy for level, preview, and thumbnail consumers. Engine
destruction detaches world, scene, viewport, preview, and thumbnail consumers,
then shuts the service down before AssetCore and rendering shutdown. Loading is
never lazy and never occurs on the render thread.

The authored default is opaque, lit, one-sided, automatic-depth-write neutral
gray with BaseColor `(0.5, 0.5, 0.5)`, Normal `(0, 0, 1)`, Metallic `0`,
Roughness `0.5`, AmbientOcclusion `1`, zero Emissive, unit Opacity and
OpacityMask, and no textures. It is valid authored data and is not classified
as an error.

Invalid representation construction, material compilation, structural missing
proxies, Renderer layout rejection, and unavailable default content converge
on `GetErrorMaterialRenderData()`. This asset-independent exact-v3 terminal is
opaque, unlit, two-sided, depth-writing magenta with no texture, package, DDC,
Cook, AssetCore, or RHI dependency. Whole-material diagnostics use the distinct
reasons `UnassignedDefault`, `DefaultAssetUnavailable`, `MaterialDataInvalid`,
`UnsupportedLayout`, and `MissingProxy`; normal default selection increments a
counter without per-component logging, while default-asset failure logs once
per service lifecycle. Texture and environment recovery do not increment these
whole-material counters.

## Versioned Render Representation

`FMaterialRenderRepresentation` is an Engine-owned immutable snapshot with
three parts: a `FMaterialRenderLayoutIdentity`, a validated uniform byte
payload, and counted RHI texture-reference resources. The current v3 layout is
416-byte, 16-byte-aligned data with 48 uniform fields and eight resource
fields; its exact field table is identified by `MaterialRenderLayoutV3Id`.
The constant, UV channel/scale/offset, and texture fields occupy the first 352
bytes; eight rotations and eight packed per-role sampler states occupy the v3
suffix. Material assets persist authored parameters rather than this transient
render representation, and every production builder starts from the canonical
v3 seed. No asset load or cook path deserializes a prior render layout, so the
v2 factory, validator, decoder, binding type, and renderer upgrade branches do
not form a content compatibility boundary and have been removed. The isolated
v1 factory and decoder remain for their separate compact-layout contract, but
production renderers accept only v3.

Construction validates the version and identity, field counts, compact-index
contiguity, types, sizes, alignment, non-overlapping ranges, finite values,
zero padding, and resource counts before publication. Invalid construction
returns the complete deterministic ErrorMaterial representation and a diagnostic;
it never publishes a partially filled payload. The representation retains no
reflected object or raw texture pointer.

`FMaterialRenderProxy` resolves parent and local layers into a copied builder,
publishes only a complete representation, and keeps the existing cache,
coalescing, stale-update, startup-replay, and counted-resource contracts.

## Renderer Surface Execution

`FSceneRenderer` owns one Renderer-private surface-material resource service.
StaticMesh, SkeletalMesh, and Terrain keep their own geometry preparation,
vertex programs, pipelines, batching, counters, and diagnostics, but consume
one canonical fragment contract. The contract builds the 256-byte aligned PBR
surface uniform and resolves the eight ordered roles with fallbacks `White`,
`FlatNormal`, `White`, `White`, `White`, `Black`, `White`, and `White`.
Missing or unready referenced textures select the role fallback without
retaining a raw RHI pointer beyond the current command submission.

The service owns generation-aware sampler slots keyed by the complete
`FMaterialSamplerState`; identical states across all three geometry families
therefore share one device-generation sampler. Device invalidation and
renderer shutdown release this owner once, while shader maps, pipelines,
geometry, Terrain topology, and height resources remain family-owned. A failed
sampler or incomplete resolved packet rejects the smallest owning draw or
Terrain batch, preserving feature-local attempt/result accounting.

Resolution is pass-aware. Opaque shadow draws resolve no material uniform,
texture, sampler, environment, or receiver-shadow resource. Masked shadow
draws resolve the uniform and only OpacityMask role 7. Forward and GBuffer
draws resolve all eight roles; only forward adds lighting, environment, and
directional-shadow inputs. Irradiance, prefilter, BRDF LUT, and environment
sampler are accepted only as a complete set and otherwise fall back together.
Directional-shadow texture and sampler each retain their deterministic array
and material-sampler fallback.

## Dependency And Invalidation Model

- Material dependencies are forward-only. `DMaterialInstance::Parent` is the
  canonical relationship, and dependency tests walk that chain iteratively with
  a cycle guard. A material depends on itself; a base material has no other
  material dependency.
- Loaded direct-child and transitive-dependent queries share one stable
  `GDObjectArray` snapshot helper on the game thread and return sorted,
  generation-safe object handles. They do not load assets, retain dependents, or
  expose the live object array during callbacks. The helper reports the query
  operation, snapshot count, scanned work, and result count for diagnostics.
- Materials and static meshes own no reverse component collections, and
  instances and components keep no registered-value mirrors for Parent, mesh,
  or material assignments. Loading, reflected edits, transactions,
  duplication, destruction, and garbage collection therefore have no
  registration-reconciliation step.
- Ordinary material mutation does not construct or flush a global material
  update context. It publishes directly to the stable material proxy, performs
  no `GDObjectArray` snapshot, and performs no component enumeration.
  Parent and descendant proxies observe inherited changes during their next
  render-thread resolution without a child enumeration.
- The removed global material update context had no production callers after
  proxy publication. Explicit structural work now uses the generic primitive
  and scene lifecycle APIs owned by the initiating subsystem; material queries
  remain loaded-runtime queries only. `FlushRenderingCommands()` remains the
  explicit visibility boundary for tests, import, preview, and save workflows.
- Editor hierarchy queries that must include unloaded assets belong to the asset
  registry and package systems, not the runtime material object relationship.
- Static-mesh render-data changes use a separate on-demand loaded-component
  scan and select components whose current mesh assignment equals the changed
  mesh. Rebuilding render state then resolves current slot definitions,
  defaults, and positional overrides directly from canonical storage.
- Proxy diagnostics expose publication, coalescing, resolution-cache hit/miss,
  stale-publication, and binding-update counts. Loaded relationship queries
  expose their own operation, snapshot, scan, and result diagnostics. Both are
  diagnostics, not a persistent dependency index.

## Compatibility Boundary

Legacy scalar/vector/texture maps have no compatibility properties or implicit
migration. The package reader rejects their fields during schema preflight,
before constructing or publishing any package object. Material assets carry no
persistent parameter-schema version or upgrade chain; a package written under
an older parameter schema fails loading rather than being migrated.

Static-mesh components persist only the positional `OverrideMaterials`
collection, and StaticMesh slots persist no GUID or slot-schema version. The
former GUID-keyed override records and slot fields have no loader alias,
upgrade branch, or migration path. Authored packages using those schemas are
incompatible; repository content was recreated directly under the current
schema.

## Environment Lighting

The studio image-based-lighting baseline is a hidden Engine asset at
`/Engine/Renderer/DefaultStudioEnvironment`, shared by level, preview, and
thumbnail rendering. It is neither a user material parameter nor scene
SkyBox state. The checked-in authoring payload contains deterministic
irradiance, GGX-prefiltered radiance, and a split-sum BRDF LUT generated only
by the independent `EnvironmentLightingBake` offline tool.

`DEnvironmentLighting` validates the versioned, checksummed authoring payload
and owns its Cook operation. Cook publishes the payload through the generic
package `.dbulk` companion without consulting DDC. Runtime loading accepts
only that cooked descriptor/payload pair. Renderer creates the RHI resources
on the render thread; an absent, invalid, or unavailable set resolves as one
black environment set, preserving direct lighting and Emissive. This internal
asset follows Engine content and asset-cook ownership rather than `DevTool`
build orchestration.

## Static Mesh Vertex Contract

- Static meshes retain up to four UV channels. Missing channels are filled with `(0, 0)` while the LOD records how many source channels were present.
- Tangents are stored as `xyz` plus a handedness sign in `w`; the bitangent is reconstructed as `cross(N, T) * sign`.
- Missing normals and tangents are generated deterministically. Missing vertex colors use linear white, so they do not change the material base color.
- Each imported node-mesh instance becomes a section. Sections keep contiguous index ranges and reference a stable source material slot; source material assets are not created automatically.

The render-resource ownership, physical stream layout, and vertex-factory
boundary are defined in [Static Mesh Rendering](StaticMeshRendering.md).

## Static Mesh Derived Data and Cooking

StaticMesh source provenance is an optional complete `FSourcePath` in any
allowed mount content directory and records the exact source hash, Assimp
importer version, and import axes. Source organization is independent of the
StaticMesh package path. Existing mounted sources are referenced without a
copy; external files require an explicit writable destination. Reimport reads
only the persisted source, while changing one reference, replacing shared
bytes, repair, and relocation are separate editor operations. Legacy
package-relative source fields are rejected. The
canonical DDC key also includes builder version 4, render-payload schema 5, and target
platform. A valid warm DDC object can load from persisted identity while source
and Assimp are unavailable.

StaticMesh render schema 5 is a little-endian, checksummed chunk layout for bounds, a
bounded material-slot count, per-LOD screen-size policy and geometry metadata,
sections, vertex streams, and index buffers.
Readers bound all counts and ranges, reject invalid numeric data and indices,
skip only optional unknown chunks, and publish render data only after complete
validation. Schema 3 and older payloads are rejected rather than converted.
Cook uses stable payload ID
`6d9f79b5-7b68-4d91-a42c-2a6063fcab16`, strips source/import metadata, and
loads the render payload only through the cooked package descriptor and DBLK
companion.

Material Preview acquires shared `/Engine/Models/Sphere` and
`/Engine/Models/Box` StaticMesh assets through the canonical
editor retention service. Multiple documents coalesce by virtual asset identity;
preview creation performs no transient OBJ import, and retained handles provide
the GC lifetime edge.

Content Browser Material and MaterialInstance thumbnails acquire the same
retained sphere and bind fully resolved `FMaterialRenderData`; they do not own a
second mesh import or a live viewport. Their cache key contains the material
package plus a deterministic, sorted, cycle-guarded closure of parent-material
and referenced-texture package fingerprints. A parent, local override, or
referenced texture change therefore regenerates the dependent preview, while a
warm PNG hit performs no asset load or render. Provider, scheduling, recovery,
and UI behavior belongs to the editor
[Asset Thumbnails](../../Editor/Architecture/AssetThumbnails.md) contract.

## Design Rules

- Components and assets use `DMaterialInterface`; renderer code consumes only render data and shader maps.
- Mesh slot vector index is the persistent component-assignment identity.
  Display names are renameable conveniences; imported names and source indices
  are reconciliation evidence; section indices consume the stable table.
- Parameter GUID is authoritative persistent identity; `FName` is lookup and
  display-facing identity, never serialized override identity.
- A valid definition set contains no invalid or duplicate GUIDs, `None` names,
  duplicate names, or deviations from the canonical built-in schema.
- Instances override parameters without duplicating declarations or the
  parent's shader program.
- Static properties are authored on base materials and inherited atomically by
  default. An instance may persist one validated all-or-nothing static-property
  override; imported materials use this to retain blend mode, mask threshold,
  and two-sided state without creating importer-specific base materials.
- Static properties belong in shader-map/permutation keys; dynamic parameters
  belong in uniform/resource bindings. A dynamic-only update reuses the cached
  identity, while a static update causes the next draw to resolve or create the
  matching shader-map and pipeline entry.
- Material object mutation crosses to the rendering thread through explicit
  update commands. Replacing the material assigned to a component still
  rebuilds its scene proxy because the set of render snapshots changes.

Long-term sequencing and current editor/rendering limitations are tracked in
the [Material System Roadmap](../../Roadmaps/MaterialSystem.md); executable work
uses the bounded plans linked from that roadmap.
