# Material Shader Stage Selection and Sharing

Summary: Material geometry and Pass usages select an atomic required stage set whose compatible binaries share independently through ShaderBuild.
Modules: Engine, RenderCore, Renderer

Last reviewed: 2026-10-20

## Declared usages

`FMaterialStaticProperties::GeometryUsage` and `PassUsage` are serialized base
material declarations, inherited by material instances. They participate in
shader identity and ordinary material edit invalidation. Defaults preserve all
existing editor uses. Instance property overrides do not replace these masks.
Set them through `DMaterial::SetStaticProperties`; invalid or empty masks are
rejected transactionally.

| Geometry bit | Generated vertex entry | Availability |
| --- | --- | --- |
| 1 | `VertexMain` | Local direct draws |
| 2 | `SplineVertexMain` | Spline direct draws |
| 4 | `GPUCullingVertexMain` | Optional Local GBuffer grouping; requires bit 1 |

| Pass bit | Generated fragment entry |
| --- | --- |
| 1 | `FragmentMain` |
| 2 | `GeometryFragmentMain` |
| 4 | `ShadowFragmentMain` |
| 8 | `HitProxyFragmentMain` |

`MaterialShaderEntries` is the canonical descriptor table for entry names,
frequencies, persisted stage bits, geometry/Pass eligibility and editor-only
selection. Compile options, identity encoding, source requests and stage
validation consume that contract; Renderer uses named entry bits rather than
positional constants. The existing seven-entry order and Cook schema are unchanged.
`SelectMaterialShaderStages` computes their union in canonical entry order.
GPU-culling contributes a vertex only when GBuffer is selected. All declared
passes share the selected direct geometry entries. Editor compilation retains
declared HitProxy use; both production Game Cook preparation paths explicitly
exclude it through `FMaterialCompilerEnvironment::bIncludeEditorPasses`.
An editor-only material with no remaining Game use fails compilation rather
than publishing an empty program. The default has seven editor stages and six
Game stages; a Local Forward-only material has two.

GPU grouping remains an optional renderer optimization. A material excluding
its GPU vertex skips GPU pipeline preparation and renders through its declared
Local direct route, including views requesting grouping. Fixed custom vertex
factories retain their existing explicit opt-in contract; these masks describe
the built-in generated Local/Spline implementation.

## Independent stage products

Detached translation retains the complete generated source for diagnostics.
`MakeMaterialShaderStageRequests` generates frequency-specific closures and
submits one entry per ShaderBuild request. Vertex source excludes pixel
expression evaluation; fragment source excludes WPO evaluation and generated
geometry entry functions. Vertex temporary symbols use dense stage-local
indices so changes in pixel IR numbering do not rename vertex expressions.

Stage virtual paths derive from generated source content, independently of
whole-material identity and usage masks. The existing ShaderBuild output key
also includes entry/frequency, effective macros, target, compiler environment
and verified immutable import dependencies. Vertex requests use neutral pixel
property macros. Equivalent vertex source therefore shares across compatible
pixel edits, shading-property variants, passes and usage declarations.

The shared uniform layout and varying interface remain in both closures.
Adding/removing active parameters or changing interpolation/custom-data channels
invalidates incompatible vertex products. WPO changes invalidate vertex source.
The import closure is conservative, retaining all material environment imports;
changes in those dependencies still invalidate affected requests. Fragment
requests currently share one pixel source closure across passes, so an emissive
edit can rebuild a shadow request even when optimized shadow code is unchanged.

ShaderBuild's existing bounded memory cache (128 outputs / 256 MiB) and DDC own
stage retention. Immutable `FSharedByteBuffer` allocations share across accepted
programs. Whole-material semantic joining and its existing retention bounds
remain in place. The program compiling manager submits at most two selected stages
concurrently through a reentrancy-safe dispatch loop, tracks each request in a
plain compile record for cancellation/priority/draining, and never
blocks a material worker on ShaderBuild I/O. There are at most seven selected
requests per material. Completed stage metadata and backing capacities charge
the existing aggregate 8 MiB material result cap before retention. Each submitted
request receives the remaining aggregate output allowance; retained outputs plus
the two live reservations total at most 16 MiB. This preserves the active-output
allowance and separate 8 MiB assembly overlap. Results occupy canonical request
slots, so arrival order cannot reorder the program. Failure stops new submissions
and drains accepted requests before assembly; the first failing canonical slot
selects the Shader diagnostic. Cancellation also drains accepted requests.
Inline callbacks update slots without recursively submitting more work.
No new unbounded stage cache is introduced.

The finer granularity trades extra cold session submissions for reuse: a full
cold editor material now has seven independent ShaderBuild compilations, where
the former request compiled seven entries in one session. Local Forward-only
uses require two. Warm whole-program reuse submits no compiler work; pixel-only
constant edits reuse all three compatible vertex products and require at most
four pixel compilations. These are work/artifact counts, not a cold-time or
frame-time speedup claim.

## Atomic admission and Cook

`RequiredStages` persists with translated and cooked products. Validation requires
exact count, canonical entry order, frequency, target and the existing strict
resource reflection allowlist for that set. Missing, extra, duplicate, reordered
or mistyped stages reject the complete candidate. No partially compiled set is
published. Cook validation also checks the set against the serialized usage
selection; low-level codecs can represent either the declared editor or Game
set, while production Game preparation always selects the latter.

Renderer Shader Map admission verifies required generated geometry and fragment
entries before fixed-source fallback. This also applies to the fixed opaque
shadow fragment, whose declared material Pass remains Shadow. Excluded uses
fail admission without compiler work. Game continues to load accepted binaries
without MIR, generated source, ShaderBuild or authoring graphs.

Pass contract 11, Cook payload schema 15 and detached translation schema 2 carry
this contract. Generator revision 14 and compile adapter 12 invalidate old
producer results. Older cooked payloads must be rebuilt.

The design follows UE's separation of material shaders, mesh material shaders
and Vertex Factory permutations, and its sparse compilation selection described
in [Shader Development](https://dev.epicgames.com/documentation/en-us/unreal-engine/shader-development-in-unreal-engine)
and [FMeshMaterialShaderType](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Engine/FMeshMaterialShaderType).
Durin currently uses bounded usage masks and content-based stage requests rather
than UE's full shader-type/Vertex Factory permutation registry.
