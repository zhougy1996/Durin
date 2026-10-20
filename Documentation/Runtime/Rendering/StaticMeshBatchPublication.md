# Retained StaticMesh Batch Inputs

Summary: Ordinary StaticMesh publishes immutable geometry and material inputs while each view captures instance and LOD state independently.
Modules: Engine, Renderer

Last reviewed: 2026-10-20

## Publication boundary

`FStaticMeshSceneProxy::CaptureStaticMeshBatches_RenderThread` publishes a
`shared_ptr<const FStaticMeshBatchPublication>`. The renderer opts in only for
the exact ordinary `FStaticMeshSceneProxy` class without a render-instance
array. Derived providers, SplineMesh, explicit render instances and invalid
asset layouts continue through virtual dynamic collection.

The asset shares `FStaticMeshBatchGeometry`: LOD policy/readiness and strong
`FMeshGeometryRecord` owners. Each proxy retains per-LOD
`FStaticMeshBatchInputs`, containing geometry and resolved immutable material
snapshots in section order. `FMeshBatch::StaticInputs` references that owner;
its legacy material vector remains empty. Both paths pass through the same
collector admission and renderer capability/resource validation.

Transforms, world bounds, primitive/batch IDs, custom numeric data, selected
LOD and view/pass state are collected separately. Updating these values does
not rebuild stable material arrays. See [material numeric data](MaterialCustomData.md)
for the independent instance-data contract. The renderer may additionally retain the view-independent Section preparation
described below. Visibility results, final RHI bindings, PSOs and shadow-map
contents are not part of these input publications.

## Capture and update rules

`FStaticMeshBatchCaptureContext` belongs to one render preparation command.
The receiver and its shadow cascades share it. No scene, material, geometry or
readiness mutations may occur between uses. A new command must create a fresh
context. Material proxies resolve once per context; raw proxy keys never
escape into worker jobs. Workers capture only immutable publication owners.

| Change | Next capture behavior |
| --- | --- |
| No input change | Reuse the publication and per-LOD material arrays |
| Material publication on the same proxy, including inherited changes | Resolve current representation record IDs and rebuild affected proxy inputs |
| Material slot replacement | Invalidate proxy publication immediately |
| Missing material or out-of-range slot | Retain the error-material snapshot and track its representation ID |
| LOD screen size, readiness, count, layout or geometry record replacement | Replace asset geometry publication; dependent proxies rebuild on capture |
| Transform, bounds or custom numeric data | Capture fresh instance overlay; retain stable inputs |
| Explicit render-instance array installed | Invalidate publication and use dynamic collection |
| Resources released | Reset asset publication and release asset references through the existing retirement path |

Geometry checks compare the current asset fields even for native producers
that do not use an asset revision API. Section and binding changes must publish
a replacement immutable geometry record. Mutable edits to an already published
record are outside the geometry contract. LOD selection still runs separately
for each receiver/shadow view and preserves unavailable-LOD fallback.

## Stable Section preparation

For ordinary StaticMesh with the exact `FStaticMeshBatchBinding` local factory,
`FMeshDrawDescriptionCache` retains two immutable renderer-owned tables. One
stores binding compatibility and per-section Forward/Shadow/GBuffer capability
facts per published LOD and preparation pass. The other stores the Section draw
descriptions for that LOD and pass policy. A miss builds the selected table
lazily; warmed views select it without repeating per-section capability and
command-description lookups. Dynamic providers, render instances, derived
bindings and custom vertex factories retain the existing preparation path.

The table identity includes the immutable `FStaticMeshBatchInputs` owner and
pass policy: Full/Shadow preparation, raster mode, depth convention and mirrored
winding. Input owners already encode geometry/binding and every material
representation. LOD selection and readiness checks still run for each view;
tables contain no requested/selected LOD indices or visibility decisions.

| Dependency/change | Section preparation response |
| --- | --- |
| Material publication, slot replacement, error-material publication, geometry or LOD-policy/readiness replacement | New immutable input owner selects new tables; older captured tables remain immutable |
| Raster mode, depth convention or determinant sign | Select a distinct policy table |
| Transform within the same winding, bounds, custom data, view matrices or shadow bias | Reuse stable tables; rebuild current instance/view overlay |
| Missing factory registration or unavailable binding | Do not retain a negative capability result; retry normal preparation |
| Material binding requires renderer-selected fallback | Keep the result frame-local; do not persist the fallback-dependent Section table |
| Device resources/service released | `Reset` clears description, capability and Section tables through the existing renderer release path |

Registered factory implementations are immutable for module lifetime and their
keys cannot be replaced. A future replaceable factory or scene-dependent shader
permutation must add its revision to the table dependencies or invalidate the
tables; it cannot silently reuse this contract.

Current material-uniform indices are resolved once per table per preparation
command and shared among that command's views. They never enter persistent
tables. Batch admission still validates current instance data and transforms.
View-dependent sorting, primitive IDs, final resource bindings and per-cascade
bias remain downstream. No scene/proxy pointer escapes into a table or job.

This follows the retained batch/pass-description separation and dependency
invalidation principles in [Epic's mesh drawing pipeline documentation](https://dev.epicgames.com/documentation/en-us/unreal-engine/mesh-drawing-pipeline-in-unreal-engine).
Durin builds selected tables lazily and retains strong resource owners; this is
not an implementation of UE's complete AddToScene command cache or shadow pass.

## Command-local Section resources

`FStaticMeshSectionResourceContext` belongs to one invocation of
`ResolveSceneRenderResources_RenderThread`. Receiver and shadow cascade resource
preparation borrow it serially on the render thread. Each prepared-pipeline retry
creates a fresh context; a standalone directional-shadow call creates one for its
cascade loop. Hybrid/GBuffer feature preparation retains its existing path.

Only captured ordinary, non-instanced StaticMesh inputs with the exact local
`FStaticMeshBatchBinding` opt in. The current collected binding must match the
description's geometry owner, with a declaration and nonempty streams. Dynamic
providers, derived bindings and explicit instances retain direct preparation.

Entries key the immutable draw description and an explicit resource policy
(`SamplersOnly`, `Forward` or `Shadow`). Before lookup, the current primitive
geometry owner and collected binding must match the description's immutable
geometry record; declaration, streams, local domain and exact binding type are
still checked. The description already owns that record, binding and declaration,
so neither extra pointer keys nor a second binding owner are needed. Entries
retain the description, material publication and resolved pipeline. These
identities cover material/program, geometry/LOD Section, factory layout, winding,
raster, depth and topology. Replacement publication selects a new entry; mutable
edits to published records remain outside the publication contract.

Renderer identity, the complete Shader/Device/Manual generation and renderer
release epoch invalidate the context before reuse. The surface-material resource
revision additionally covers sampler creation/replacement, failed creation,
bounded eviction and independent release. Only successful preparation enters
the context; pending or failed work follows the existing retry path.

Hits copy only the resolved material binding and pipeline. Live texture and
sampler currentness, collection schemas/payloads, environment and shadow pass
refresh, instance and view Uniforms, final bindings, visibility and shadow bias
remain in their existing paths. Each draw still records resource readiness and
success/failure. Context owners survive underlying cache-slot replacement and
retire when the invocation ends; they are never shared with worker jobs or kept
across commands, frames or device teardown.

## Command-local instance validation

`FStaticMeshPreparationCache` owns an `FMeshBatchValidationCache` shared by
collector admission and transform preparation for the receiver and cascades.
It is used serially on the render thread and discarded with the preparation
command. Worker snapshots copy the resolved transform; they never access the
mutable cache. Standalone collectors can omit the cache and retain direct checks.

Entries use a command-local hash table keyed by primitive and batch IDs; key
equality resolves hash collisions. Entries are never consumed in table order.
Node storage preserves references to resolved transforms when the table grows.
Every use compares the current matrix, bounds and custom numeric data against
the cached values; changed values are checked again, including transitions into
and out of invalid states. Only the custom-data float arrays use byte comparisons,
without comparing structure padding. Bounds and matrices use component value
equality. Inversion,
normal-matrix preparation, determinant and numeric-validity results can therefore
be reused across views without trusting a provider-supplied validation flag.
Admission requires a finite invertible double matrix; rendering separately
requires a finite linear determinant and representable normal matrix.

Purpose masks, publication/geometry/binding coherence, nonempty elements, IDs,
LOD ranges, current factory/layout keys, duplicate batches and dynamic element
validation still run for each submission. LOD selection, readiness, visibility,
material/resource resolution, sorting and shadow bias remain view/submission
dependent. This cache does not relax publication capture/update lifetime rules
or retain instance facts across commands or frames.

## Lifetime

The asset and proxy retain their latest publications. Collection and worker
snapshots hold shared immutable owners of geometry, material representations,
bindings and their RHI inputs. Replacing slots, rebuilding geometry, releasing
an asset or removing its scene proxy cannot mutate an older captured frame.
No asset or scene-proxy pointer is retained in a published batch. Persistent
renderer tables also retain these owners until eviction or service reset. Each table cache trims unused entries above 512 while retaining the
current submission working set. Owners retire after the last frame/cache
reference releases them, using existing RHI lifetime rules; publication does not
introduce a device wait. Capture contexts must be
destroyed before render-thread/device teardown and must never be reused after
scene updates or proxy removal.

## Validation observation

The 2026-10-19 Win64-Debug-DurinEditor diagnostic comparison used 128 ordinary
StaticMesh proxies, three available LODs, one shared material and four view
collections. Legacy and retained input collection alternated in one process,
with 30 warm-up pairs and 120 measured pairs in each of three rounds. Median
input-preparation times were 8.5443/7.7314 ms, 8.7017/7.7601 ms and
9.9645/8.6021 ms (legacy/retained), a 9.5-13.7% reduction in this phase.
This excludes subsequent draw preparation, resource creation and GPU work.

The shadow qualification's 97 images and metadata matched the previous path
byte-for-byte. Three end-to-end before/after runs showed substantial host/GPU
variation, so they establish no whole-frame performance improvement. Measurements
used Debug/Tracy, Vulkan validation and a GTX 1060 6GB outside an exclusive quiet
GPU lane; all timing is diagnostic. Local reports, raw paired samples and shadow
samples are under ignored `Build/StaticBatchPublication/`, against baseline
`15019f9bbf933d1bd753ab79f20f878bbd42c4e8`. Temporary comparison tests and CSV
instrumentation were removed after validation.


### Section preparation observation

The follow-up against baseline `bb6223be3` split collection, publication/LOD
facts, material resolution, draw-description lookup, worker assembly and
finalization. Across three 128-caster runs, collection was the largest measured
range (16.74 ms median of run medians, including receiver and cascade views),
followed by resolution (4.62 ms); sorting/finalization was about 0.64 ms. Nested
ranges and concurrent worker times must not be summed into frame wall time.
Additional admission/capability measurements confirmed that instance validation
is a substantial remaining cost; this optimization preserves that validation.

After removing the detailed timers, an in-process comparison alternated the
previous preparation path and stable Section tables for 128 ordinary meshes,
three available LODs, ForceLOD0, one shared Opaque material, a receiver and three
shadow preparations. Each of three rounds used 30 warm-up pairs and 120 measured
pairs, alternating execution order. The paired logical-preparation medians were:

| Round | Previous path | Stable Sections |
| --- | --- | --- |
| 1 | 19.0785 ms | 17.7444 ms |
| 2 | 19.1710 ms | 17.8968 ms |
| 3 | 19.2800 ms | 18.3179 ms |

This is a 5.0-7.0% reduction for the measured CPU logical preparation, excluding
resource preparation, submission and GPU work. It is Debug/Tracy diagnostic
evidence, not a Release/FPS or exclusive-lane performance gate. Separate full
shadow qualification covers five-box and 128-caster fixtures; small-scene
results do not establish a material improvement. Raw stage CSVs, paired CSVs,
reports and capture hashes are in ignored `Build/StableSections/`. Temporary
profiling, comparison controls and test cases were removed after validation.

### Admission and transform reuse observation

The follow-up against baseline `e6ee124a0` shares command-local instance checks
between collection and transform preparation. A temporary reference comparison
covered 1,024 input transitions, including mirrored/singular/nonfinite matrices,
normal-matrix overflow, invalid/restored bounds and custom data, LOD/binding
changes, duplicate batches and changing dynamic geometry. Cached and uncached
admission outcomes agreed for receiver and shadow collection; resolved valid
transforms and full prepared outputs also agreed. One unchanged batch built its
inverse once across four collections and preparation, without bypassing dynamic
element validation.

The same 128-mesh, three-LOD, ForceLOD0, shared-Opaque-material fixture alternated
uncached and shared admission within one process. Each of three rounds used
30 warm-up pairs and 120 measured pairs, with alternating order:

| Round | Uncached admission | Shared admission |
| --- | --- | --- |
| 1 | 17.9206 ms | 15.7859 ms |
| 2 | 17.8020 ms | 15.7328 ms |
| 3 | 17.8675 ms | 15.7567 ms |

This is an 11.6-11.9% reduction in median CPU logical preparation for a receiver
and three shadow views. It excludes resource preparation, submission and GPU
work. These Win64-Debug-DurinEditor/Tracy timings are diagnostic, not a Release
or frame-rate claim. Raw paired samples and reference/regression reports are
under ignored `Build/AdmissionReuse/`. Temporary comparison tests and controls
were removed after validation.

Validation passed the incremental `all` build, 149 cases across
`StaticMeshRenderPreparationVulkanTests`, `RendererSceneContractTests`,
`SplineTests` and `RendererResourceReloadVulkanTests`, and four cases in
`DirectionalShadowBaselineVulkanTests --mode qualification`. All 97 baseline
shadow images and their metrics file remained byte-identical to `e6ee124a0`.
