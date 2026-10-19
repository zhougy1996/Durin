# Retained StaticMesh Batch Inputs

Summary: Ordinary StaticMesh publishes immutable geometry and material inputs while each view captures instance and LOD state independently.
Modules: Engine, Renderer

Last reviewed: 2026-10-19

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
for the independent instance-data contract. This retains batch inputs, not
prepared draw commands, visibility results, PSOs or shadow-map contents.

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

## Lifetime

The asset and proxy retain their latest publications. Collection and worker
snapshots hold shared immutable owners of geometry, material representations,
bindings and their RHI inputs. Replacing slots, rebuilding geometry, releasing
an asset or removing its scene proxy cannot mutate an older captured frame.
No asset or scene-proxy pointer is retained in a published batch. Old owners
retire when the last captured frame releases them, using existing RHI lifetime
rules; publication does not introduce a device wait. Capture contexts must be
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
