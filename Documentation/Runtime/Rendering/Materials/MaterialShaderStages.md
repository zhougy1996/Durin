# Material Shader Stage Selection and Sharing

Summary: Material geometry and Pass usages select an atomic required stage set whose compatible binaries share independently through ShaderBuild.
Modules: Engine, RenderCore, Renderer

Last reviewed: 2026-11-16

## Declared usages

`FMaterialStaticProperties::GeometryUsage` and `PassUsage` are serialized base
material declarations, inherited by material instances. They participate in
shader identity and ordinary material edit invalidation. Defaults preserve all
existing editor uses. Instance property overrides do not replace these masks.
Set them through `DMaterial::SetStaticProperties`; invalid or empty masks are
rejected transactionally.

| Geometry bit | Generated vertex entry | Availability |
| --- | --- | --- |
| 1 | `VertexMain` | Local direct and indirect draws |
| 2 | `SplineVertexMain` | Spline direct draws |
| 4 | Reserved legacy usage | Normalized away; no generated stage |

| Pass bit | Generated fragment entry |
| --- | --- |
| 1 | `FragmentMain` |
| 2 | `GeometryFragmentMain` |
| 4 | `ShadowFragmentMain` |
| 8 | `HitProxyFragmentMain` |

`MaterialShaderEntries` owns stable Shader type/source-entry pairs, frequencies,
geometry/Pass eligibility and Editor selection. `SelectMaterialShaderEntries`
returns typed classifications; serialized selections use stable Shader type
names. No ordinal or six-bit stage mask is a persisted request identity.
`FMaterialCompilationOutput::Entries` stores full existing RenderCore Shader keys,
sorted by full key and paired with per-entry requirements. Keys include material
identity/layout, Shader type, entry, target, frequency, permutation, pass version
and output-layout identity. Vertex requests additionally carry a mesh key with
`LocalVertexFactory` or `SplineVertexFactory`. Their shared vertex transport uses
mesh pass key zero; renderer composition adds the actual pass key and registered
runtime Shader type to its own mesh-map identity. Registered factories explicitly
reusing built-in generated transport compose it with their own factory identity;
fixed vertex implementations exclude generated vertex contracts even when source
entry names collide. Opaque mask thresholds canonicalize before key comparison.
HitProxy live-time cache invalidation consumes the coverage and selected vertex
entry contexts rather than unrelated whole-module surface requirements.
Pixel products remain shared
across factories.
All declared passes and selection algorithms share the selected geometry entries. Editor compilation retains
declared HitProxy use; both production Game Cook preparation paths explicitly
exclude it through `FMaterialCompilerEnvironment::bIncludeEditorPasses`.
An editor-only material with no remaining Game use fails compilation rather
than publishing an empty program. The default has six editor stages and five
Game stages; a Local Forward-only material has two.

GPU grouping remains an optional renderer optimization. Local geometry uses its
same vertex stage with either direct or GPU-selected IDs; GeometryUsage no longer
selects a culling algorithm. New materials default to mask 3. Load/edit and
identity canonicalization strip legacy bit 4. Fixed custom vertex
factories retain their existing explicit opt-in contract; these masks describe
the built-in generated Local/Spline implementation.

## Independent stage products

Detached translation retains generated evaluation source for diagnostics.
`Material/SurfacePasses.slang` owns geometry and fragment entry bodies, GBuffer
packing and lighting execution; `Material/SurfacePassResources.slang` owns pass
resource declarations. The expression generator composes these captured modules
with typed attribute evaluation and material resource reads. Both files belong
to the immutable material compiler environment dependency closure.

After normalization and static selection, `BuildMaterialCompilationOutput`
analyzes demanded attribute roots per entry. Forward/GBuffer request applicable
surface attributes; Unlit GBuffer has no surface evaluation. Shadow and HitProxy
request only masked coverage. Vertex entries evaluate WPO and transport the union
of interpolators needed by selected pixel passes. Pixel analysis stops at explicit
VertexInterpolator boundaries. Each closure records active attribute/parameter IDs,
compact resource indices, collection IDs, interpolator semantics, WPO/mask flags
and material evaluation context. Logical scene inputs remain empty for Surface.
The whole-module layout and context are conservative; they do not replace these
per-entry requirements or the pass adapter's own camera/lighting bindings.

`MakeMaterialShaderStageRequests` projects each pixel entry's roots before
source generation. Shadow/HitProxy therefore do not gain bindings from unrelated
surface expressions. Shared layout/varying changes and pixel IR symbol numbering
can still invalidate a request. Vertex symbols use dense local
indices, preserving reuse across pixel edits. Vertex and pixel requests retain
the shared uniform layout and varying slot allocation; changing either can
invalidate otherwise reusable products. Selected coverage-only programs also
remove unrelated parameters from their whole-module layout.

Stage virtual paths derive from evaluation source content independently of the
whole-material identity. ShaderBuild additionally keys entry/frequency, effective
macros, target, compiler environment and immutable module dependencies. Vertex
requests use neutral pixel macros. The import closure remains conservative.

ShaderBuild's existing bounded memory cache (128 outputs / 256 MiB) and DDC own
stage retention. Immutable `FSharedByteBuffer` allocations share across accepted
programs. Whole-material semantic joining and its existing retention bounds
remain in place. The program compiling manager submits at most two selected stages
concurrently through a reentrancy-safe dispatch loop, tracks each request in a
plain compile record for cancellation/priority/draining, and never
blocks a material worker on ShaderBuild I/O. There are at most six selected
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
cold editor material now has six independent ShaderBuild compilations, where
the former request compiled all entries in one session. Local Forward-only
uses require two. Warm whole-program reuse submits no compiler work; pixel-only
constant edits reuse both compatible vertex products and require at most
four pixel compilations. These are work/artifact counts, not a cold-time or
frame-time speedup claim.

## Atomic admission and Cook

Typed entry selections and complete requests persist with translated and cooked
products. Admission checks the canonical full-key sequence, exact supported
selection, target, pass/output ABI, factory and layout identity. Binary stages
are matched by source entry and frequency rather than vector position. Missing,
extra, duplicate or mistyped entries reject the candidate. Reflection validates
the closed pass allowlist and requires material resource/collection bindings to
belong to that entry's analyzed closure. Optimization may remove conservative
requirements; it may not introduce unrelated bindings. No partial set publishes.
Cook reconstructs collection schemas before admission and checks selection
against saved material usage. Production Game excludes Editor HitProxy entries.

Renderer Shader Map admission verifies required generated geometry and fragment
entries before fixed-source fallback. This also applies to the fixed opaque
shadow fragment, whose declared material Pass remains Shadow. Excluded uses
fail admission without compiler work. Game continues to load accepted binaries
without MIR, generated source, ShaderBuild or authoring graphs.

Pass contract 14, DMAT 17, MIR 10 and detached translation schema 6 carry this
contract. Normalization/generator revisions 10/18 and compile adapter 15 invalidate
old producer results. Rebuild earlier compiled/Cook products; authored graph,
output, function and instance versions remain 6/4/3/2 and layout remains 4.

The design follows UE's separation of material shaders, mesh material shaders
and Vertex Factory permutations, and its sparse compilation selection described
in [Shader Development](https://dev.epicgames.com/documentation/en-us/unreal-engine/shader-development-in-unreal-engine)
and [FMeshMaterialShaderType](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Engine/FMeshMaterialShaderType).
Durin uses bounded authored usage declarations and typed sparse entry requests;
this does not implement UE's full permutation registry.
