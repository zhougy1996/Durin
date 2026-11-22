# Material Shader Stage Selection and Sharing

Summary: Material geometry and Pass usages select an atomic required stage set whose compatible binaries share independently through ShaderBuild.
Modules: Engine, RenderCore, Renderer

Last reviewed: 2026-11-23

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
| 16 | `SingleLayerWaterFragmentMain`, `SingleLayerWaterDepthFragmentMain` |

Single Layer Water color usage selects the water entry and retains the Forward
entry for diagnostics. Its GBuffer usage canonicalizes away. The water entry
evaluates Opacity, masked coverage when present, and the dedicated water
output roots. The ordinary Forward view does not demand those optical roots.

Registered `FMaterialShaderType` / `FMeshMaterialShaderType` descriptions own
Shader type/source-entry pairs, frequency, domain, geometry/pass eligibility,
blend applicability, permutation ID and editor selection. The type constructor
supplies its actual registered name, source entry and frequency; no separate
material entry table or alias registry participates. `SelectMaterialShaderEntries`
returns actual Shader type names in deterministic order. Semantic entry roles
remain only for attribute/resource analysis.

`FShaderPermutationParameters` carries target, permutation ID and optional VF.
`FMaterialShaderPermutationParameters` adds detached domain, static material
properties, quality, feature level and editor policy. Type and VF policies see
these captured facts; workers do not query authored objects. Their environment
callbacks contribute effective macros to both request identity and compilation.
Callbacks preserve the registered target, entry and frequency.

`FMaterialCompilationOutput::Entries` stores full RenderCore Shader keys sorted
by full key. Compiled products occupy the corresponding canonical slots; typed
runtime lookup resolves the exact contract before selecting its code slot.
Different types/VFs may use the same entry spelling without substituting each
other's product. Keys include material identity/layout, actual Shader type, entry,
target, frequency, permutation, pass version and output layout. Vertex requests
also carry the registered VF mesh key. Renderer composition adds its mesh-pass
key without rewriting the compiled type or VF identity. Local and Spline use
`FStaticMeshVertexShader` and `FSplineMeshVertexShader` across all passes; GBuffer
has no duplicate vertex types. Opaque and masked shadow fragments use their
respective registered types with the canonical `ShadowFragmentMain` entry.
PostProcess uses the registered `FPostProcessMaterialShader`. Fixed custom
vertex implementations exclude generated vertex contracts even when source
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
factories keep independent fixed Shader contracts; the built-in geometry bits
remain valid before Renderer registration. Registered VF descriptions may
contribute additional usage bits.

## Independent stage products

Detached translation retains generated evaluation source for diagnostics.
`Material/SurfacePasses.slang` owns fragment entry bodies, GBuffer packing and
lighting execution; `Material/SurfacePassResources.slang` owns pass resource
declarations. RenderCore's `FVertexFactoryShaderDescription` owns geometry module
imports, material entry includes and generic policy/environment callbacks. Local
and Spline entry bodies live under `VertexFactory/`; their shared geometry
adapter calls typed material evaluation. Spline's macro belongs to its VF.
The expression generator consumes the selected VF description without choosing
Local/Spline implementations itself. Module/include names are relative to the
Engine shader mount. Registered roots outside the base compiler environment
closure receive separate fingerprints and join its verified immutable source
capture; missing or changed roots reject before compilation. The import closure
remains conservative and reads no uncaptured filesystem fallback. Registered
VF sources need not reside in the built-in VertexFactory directory. The Engine
mount prefix bounds imports; the verified capture determines the available files.

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
requests use neutral pixel macros plus the selected type/VF environment.

ShaderBuild's existing bounded memory cache (128 outputs / 256 MiB) and DDC own
stage retention. Immutable `FSharedByteBuffer` allocations share across accepted
programs. Whole-material semantic joining and its existing retention bounds
remain in place. The program compiling manager submits at most two selected stages
concurrently through a reentrancy-safe dispatch loop, tracks each request in a
plain compile record for cancellation/priority/draining, and never
blocks a material worker on ShaderBuild I/O. The built-in default selects six requests; registered extensions may add
requests within the existing bounded job/output limits. Completed stage metadata and backing capacities charge
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

## Logical bindings and runtime projection

Each parameter owner shares its declaration list between Slang and registered
C++ parameter metadata: View, Primitive, Surface, GPUScene, Spline, HitProxy and
SceneTextures. Members describe names, shader types, resource/runtime kinds,
required/optional policy and Draw/Pass lifetime scope. They contain no descriptor
numbers or ordinal pass masks. Material resources and collections use
`FShaderParametersMetadataBuilder`; their qualified identities contain parameter
or collection GUIDs. Registered Shader and VF metadata compose with that dynamic
schema before compilation. Extensions add owner metadata and source declarations
without changing a global resource enum or material selection switch.

ShaderBuild reflects compiler allocation, resource dimensions and top-level
constant member offsets/sizes. ShaderFormat owns the named graphics root-layout
exception: default-space graphics descriptors occupy set 0 for vertex products
and set 1 for fragment products, keeping independently optimized stages disjoint.
It rewrites SPIR-V and reflection together, rejects collisions and preserves
explicit nonzero spaces. Resources within each stage retain compiler allocation.
Uniform payload packing remains version 5; admission checks its expected members
against compiler constant layout.

After reflection validation, each immutable entry publishes canonical logical
`FMaterialShaderBinding` names, qualified identities, owner/provider references,
runtime kind and lifetime scope together with the compiler's actual set, binding,
resource kind, dimension, array size and constant-buffer size. Translation carries no accepted
bindings. Final assembly and Cook admission require the exact optimized binding
sequence to match reflection, with no unknown, duplicate, mistyped, wrong-array or
out-of-scope resource. Material/collection reads must belong to the analyzed entry
closure. Conservative analysis can exceed optimized reads; mandatory HitProxy and
PostProcess alpha/input resources must remain present. Semantic matching uses names
and types. Binding schema version 3
travels with compilation output and incompatible versions reject before publication.
Each entry retains a semantic schema identity and a product-bound allocation
identity covering Shader/VF/target/permutation, code, locations and constant layout.
The material compiler environment hashes registered Shader/VF parameter-owner
schemas so metadata edits invalidate translated products. Generated stage requests
also include their exact composed schema identity. Static runtime preparation
applies the same registered-type compilation environment as the Cook producer;
parameter metadata does not introduce separate per-type backend macro variants.

Typed runtime maps verify accepted references against their code's reflection
before creating resources. Renderer uses the accepted physical locations;
Surface providers resolve compiled C++ member offsets using generic typed binding;
uniform-kind policy comes from owner metadata.
PostProcess consumes the accepted description directly for material and scene
bindings. Mesh pass/material lifetime filtering follows provider semantics rather
than descriptor set numbers; instance-selection replacement retains its accepted
location instead of assuming set 0/binding 31. RDG continues to own declared scene reads, texture views and resource
lifetimes; physical binding metadata does not manufacture graph dependencies.
Failure rejects the complete candidate and preserves existing published content.

## Atomic admission and Cook

Typed entry selections and complete requests persist with translated and cooked
products. Admission checks the canonical full-key sequence, exact supported
selection, target, pass/output ABI, factory and layout identity. Binary stages
are paired with contracts in canonical full-key order and checked for entry and
frequency agreement. Missing,
extra, duplicate or mistyped entries reject the candidate. Reflection validates
the closed pass allowlist and requires material resource/collection bindings to
belong to that entry's analyzed closure. Optimization may remove conservative
requirements; it may not introduce unrelated bindings. No partial set publishes.
Cook reconstructs collection schemas before admission and checks selection
against saved material usage. Production Game excludes Editor HitProxy entries.

Renderer Shader Map admission verifies required generated geometry and fragment
entries before fixed-source fallback. This includes the registered opaque
shadow type, whose declared material pass remains Shadow. Excluded uses
fail admission without compiler work. Game continues to load accepted binaries
without MIR, generated source, ShaderBuild or authoring graphs.

Cooked preparation validates the saved schema and product-bound allocation
identities using registered runtime Shader/VF metadata before publication.
Compatible dynamic instances share the immutable accepted program and allocation
records; numeric updates replace parameter payloads without compiling another
program. Optimized stages may retain no bindings when their entire parameter
closure is unused; required-resource validation follows the accepted per-entry
contract rather than a blanket nonempty-binding rule.

Pass contract 28, DMAT 29 and detached translation schema 18 carry this
contract. Normalization/generator revisions 19/30, compile adapter 26 and the
ShaderBuild compile algorithm revision 7 invalidate earlier producer artifacts.
Generic parameter schemas/allocation envelopes use version 2, DSHD uses version 7
and shared ShaderBuild output uses version 8. These codecs retain resource shape
and constant layouts through DDC and Cook.
Recook incompatible derived products; authored graph schemas remain unchanged.
The [material system](MaterialSystem.md) owns the remaining compatibility versions.

The design follows UE's separation of material shaders, mesh material shaders
and Vertex Factory permutations, and its sparse compilation selection described
in [Shader Development](https://dev.epicgames.com/documentation/en-us/unreal-engine/shader-development-in-unreal-engine)
and [FMeshMaterialShaderType](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Engine/FMeshMaterialShaderType).
Durin uses registered Shader/VF descriptions, bounded authored usage declarations
and sparse typed requests. Full UE permutation domains remain outside this contract.
