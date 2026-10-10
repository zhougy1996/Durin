# Sky Lighting

Summary: Scene-owned Sky Lights consume ordinary HDR cubes or capture a bounded procedural sky and generate transient diffuse/specular lighting on the GPU.

Modules: Engine, Renderer, RenderCore, VulkanRHI, MetalRHI, TextureCompressor, DurinEd, Launch

Last reviewed: 2026-10-05

## Authoring and source selection

Add a Sky Light actor independently of a Sky Box or Procedural Sky actor.
Specified Cube accepts a linear RGBA32F ordinary TextureCube, at most 512 pixels
per face. Import a panorama with HDR output using the normal cube importer;
reimport, retained source, ordinary mips, and Cook follow [Cube Textures](CubeTextures.md).
Captured Sky requires an enabled Procedural Sky actor in the same World.
It captures only that analytic infinite sky: no meshes, clouds, fog, atmosphere,
sun disc, or Sky Box textures. Directional lights still supply direct sunlight.

Each Scene selects eligible components by descending Priority, ascending
persistent GUID, then object path. Priority is bounded to -1000..1000.
Missing inputs contribute black. Intensity is bounded to 0..16 and applied
while sampling. Specified Cube sampling uses inverse component rotation;
Captured Sky ignores Sky Light rotation. Translation and scale have no effect.
The procedural actor's world rotation transforms its up and sun directions.

Automatic Refresh defaults to one second, bounded to 0.25..60 seconds. It
regenerates only after the procedural snapshot changes. Recapture requests a
refresh even with automatic updates disabled. Details exposes eligibility,
generation age, GPU update time, and pending/failure state. Editing intensity
or specified-cube rotation does not require filtering again.

## Radiance and filter conventions

Visible sky and capture use `ProceduralSky.slang`. Coordinates are +X forward,
+Y right, +Z up; cube layers are +X, -X, +Y, -Y, +Z, -Z.
For normalized direction D, h=dot(D,Up). Above the horizon the base is
lerp(Horizon,Zenith,pow(h,p)); below it is lerp(Horizon,Ground,-h).
The broad halo adds HaloColor * pow(max(dot(D,ToSun),0),q) *
smoothstep(-0.05,0.05,h). Multiply by Tint and exp2(exposure), then clamp linear
radiance to 0..16384. Display mapping happens only at presentation.

Default Zenith is (0.18,0.28,0.50), Horizon (0.30,0.32,0.35), Ground
(0.025,0.020,0.018), Halo (0.5,0.39,0.275), normalized ToSun (-0.4,0.5,0.75),
Up +Z, Tint white, p=1, q=16, exposure=0. Colors/tint are bounded to 0..16,
p to 0.25..8, q to 1..64, exposure to -8..8.

Capture uses a 128-face RGBA16F cube with eight ordinary mips, integrating
16 solid-angle-weighted samples per texel. Diffuse is a 16-face RGBA16F cube
with 256 cosine-distributed samples: stored irradiance E equals pi*C for
constant input C; consumers divide by pi. Specular is a 128-face RGBA16F cube
with eight mips and 128 GGX samples, alpha=roughness squared, source LOD from
sample solid angle, roughness=mip/7; mip zero copies radiance. These are
lighting mips, distinct from ordinary TextureCube mips.

The shared 128-square RGBA16F split-sum BRDF LUT uses 1024 samples and is
generated once per renderer resource generation. It does not depend on the
source cube. Forward and deferred consumers share the same environment
equations, complete generation, intensity, rotation, and maximum-mip controls.

## Lifetime and recovery

World Tick coalesces requests even without a viewport. Launch drains them
after RHI frame begin, before viewport submission, so dynamic upload allocation
always has a live frame producer. Immutable native snapshots
retain counted texture references and diagnostic mailboxes, never components
or managed asset pointers. Ownership/source epochs are distinct from animated
provider revisions. An admitted job is allowed to complete while animation
coalesces into the latest pending state; animation cannot starve publication.

Each Scene admits at most one GPU job. A weak generation inventory counts
exported images still owned by active, candidate, or external consumers;
retired active sets stay strongly owned through an ordered GPU retirement
marker. Admission reserves a conservative 4 MiB for the fixed output envelope
and waits if lighting images would exceed 16 MiB or retained sources plus
lighting would exceed 64 MiB. RDG pool counters exclude exports and are not
used as this budget. Backpressure is a distinct Details diagnostic. An ordered
GPU completion marker gates the next job. GPU timing queries are optional
diagnostics on backends that support timestamps. RDG declares every face/mip
access and extracts the complete irradiance/prefilter set together. Each filter
pass batches all faces of one mip and binds exact writable 2D face views. Each
complete job dispatches an asynchronous GPU submission before another scene is
admitted, bounding unsubmitted Metal command-buffer use. Publication means all commands were recorded
on the ordered RHI timeline, not that CPU code observed GPU completion.
Normal runtime updates perform no synchronous CPU readback.
World-driven scenes admit once at frame start; later views in that frame
reuse the result. View-only preview scenes can admit through their render path.

An authority change immediately discards the old active set. A same-authority
creation failure retains the last complete set and retries at the bounded
refresh interval. Scene removal releases scene ownership; submitted work owns
its resources through execution. Renderer device invalidation clears all
registered scene generations, including inactive scenes, and the shared LUT.
See [Renderer Resource Recovery](RendererResourceRecovery.md).

The supported path requires linear filtering of RGBA32F/RGBA16F,
RGBA16F storage images, 8x8 compute groups, and 512-face cubes. Vulkan and
Metal expose this through `bSupportsSkyLighting`; Metal additionally probes
`supports32BitFloatFiltering` on the admitted device. GPU timestamps are not
required for filtering, publication, refresh, or retirement. Exhausting the
optional timing pool also leaves lighting updates enabled. On Metal without
timestamps, the GPU update-time diagnostic remains unmeasured (zero), while
completed-update counts use actual GPU completion. An unsupported device
contributes neutral black with a diagnostic.

## Studio content

`/Engine/Renderer/DefaultStudioCube.DefaultStudioCube` is an ordinary HDR cube.
Checked-in levels, newly created levels, asset previews, and thumbnails use
normal Sky Light component references. Renderer startup does not load a fixed
environment asset. `StudioCubeGenerate <Engine/Content>` reproducibly builds
the retained analytic panorama and ordinary cube using TextureCompressor. The sole
argument selects the output content root; generation writes the Studio cube
package there. Assign it through the normal Sky Light authoring workflow.
The one-time level migration command has been removed after content migration
completed. No legacy environment asset loader or bake program remains.

## Qualification

`DevTool test MetalSkyLightingTests --mode qualification --report` validates
scene-owned captured and specified HDR sources on Metal and Vulkan in inline
and threaded execution, including all six faces and eight filtering mips,
nonuniform cube orientation, manual refresh, and retirement. Both backends
report GPU timing diagnostics when timestamp sampling is available.

`DevTool test SkyBoxVulkanIntegrationTests --timeout 600` exercises real GPU
capture/filter readback alongside HDR cube face/mip and display sampling.
The former Launch runtime smoke and its per-frame timing hooks have been removed.

Cooked Game establishes the cooked asset domain before asset loading and maps
normal virtual content roots to read-only directories beneath the executable's
Cook root. It loads shader libraries and material bytecode without ShaderBuild,
TextureCompressor, or importer modules. Material compiler identity is retained as
provenance; cooked compatibility checks versions, target, stage/layout contracts,
and byte hashes. Explicit Cook roots must include runtime-selected defaults
and native gameplay assets in addition to the level's serialized dependencies.
