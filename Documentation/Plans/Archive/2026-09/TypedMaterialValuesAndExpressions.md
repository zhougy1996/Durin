# Typed Material Values And Expressions Plan

Summary: Replace all-alternative material values and universal authored nodes with typed parameter storage, owned MaterialExpression objects, and detached compiler snapshots.

Last reviewed: 2026-09-15

Status: Archived
Completed: 2026-09-15

## Current Status

Completed on 2026-09-15. All six stages and required acceptance gates pass.
The final Win64 Debug runs use DMAT v7 and material Cook contributor version 5
from revision `a7a7f96d4`; this closeout changes documentation only.

| Final Windows coverage | Result | Receipt under `Build/.agent-state/logs/` |
| --- | --- | --- |
| Game workspace `all` | Passed | `20260915-100539-313351-31256-cmake.log` |
| Editor workspace `all` | Passed | `20260915-101033-952084-37048-cmake.log` |
| Fresh Sandbox Win64/Game Cook | 7 packages, including DefaultMaterial | `20260915-101121-635163-28536-DurinAssetTool.log` |
| Fresh RoadWeaver Win64/Game Cook | 4 packages, including DefaultMaterial | `20260915-101207-240879-19652-DurinAssetTool.log` |
| Sandbox Game startup/render/shutdown | 120 ticks; normal exit | `20260915-101152-452035-16744-DurinGame.log` |
| RoadWeaver Game startup/render/shutdown | 120 ticks; normal exit | `20260915-101247-484533-11496-DurinGame.log` |

Both Game runs used separately staged fresh Cook outputs on NVIDIA GeForce GTX
1060 6GB with Vulkan validation enabled. Neither logged errors. Sandbox retained
its known missing GrayboxPawn visual warning and discarded an incompatible
pipeline cache. These are correctness checks, not GPU timing qualification.
Cook outputs and JSON reports remain under `Build/TypedMaterialFinal/`; the
[qualification report](../../TypedMaterialValuesAndExpressions.qualification.json)
embeds both final Cook reports and records the exact commands and receipts.
The first Cook launch after building only DurinAssetTool failed before logging;
the Editor `all` build completed the runtime outputs and the fresh retry passed.

The final macOS native suites and renderer qualification listed below are reused
for unchanged source. Changed-document and all-plan validation pass. The owning
asset lifecycle contract now also records DMAT v7 and Cook recipe version 5.

## Final Integration Evidence Before Windows Closeout

Final runtime/editor contracts now describe owned concrete expressions, direct
Build snapshots, cloned transaction candidates, ownership marker 2, DAST v10 and
DMAT v7. Changed-document and all-plan validation pass. Final macOS arm64 checks:

- SceneImportTests: 10 passed (`20260915-030750-108234-88357-SceneImportTests.log`).
- AssetPackageReloadTests: 13 passed (`20260915-030809-791336-88420-AssetPackageReloadTests.log`).
- MaterialVulkanTests: qualification passed on Apple M4 outside the sandbox
  (`20260915-030909-266483-88538-ctest.log`). The earlier build-only invocation
  required qualification mode and is not execution evidence.
- SceneImportVulkanTests: qualification passed outside the sandbox
  (`20260915-030931-005919-88558-ctest.log`); full output retained at
  `Build/TypedMaterialFinal/SceneImportVulkan-20260915.log`.
- StaticMeshTests: 111 passed (`20260915-031034-233690-88665-StaticMeshTests.log`).
  Three ignored transaction-reset results were corrected; all 14 material cases
  passed afterward (`20260915-031112-222069-88741-StaticMeshTests.log`).
- Fresh Win64/Game Cook: Sandbox published 7 packages and RoadWeaver 4, including
  DefaultMaterial. Receipts: `20260915-030945-561667-88626-DurinAssetTool.log` and
  `20260915-031001-674367-88642-DurinAssetTool.log`; outputs reside under
  `Build/TypedMaterialFinal/{SandboxCook,RoadWeaverCook}`.

At this macOS checkpoint, final Win64 Game execution was unavailable. The final
Windows receipts above now close that gate. Earlier Stage 4 Game receipts predate
DMAT v7 and remain historical evidence only.

## Prior Implementation Receipts

These checkpoints describe intermediate states; current completion is recorded
above. Their outstanding-work statements are historical.

Stage 5 removes the unused universal graph validators and reflected Program/node/
function-call graph records. Cook payload schema advances from 6 to 7 to remove
its redundant authored Program version word; IR, generator, envelope and payload
version checks remain authoritative. Previously cooked outputs must be rebuilt;
no compatibility reader is introduced. Material Cook contributor version is 5 so
warm Cook caches invalidate the previous payload. All 234 material regression cases
pass (`20260915-030510-482916-87122-MaterialTests.log`; historical baseline capture
excluded). After contributor invalidation, nine Cook/render representation cases
pass (`20260915-030628-923393-87654-MaterialTests.log`), followed by workspace `all`
(`20260915-030633-677809-88131-cmake.log`). Searches across every workspace project's
source/test roots find no universal Program/node/function graph, old owner graph
API, expression Lower context or old authored graph schema reference. The Stage 5
removal gate is complete; final documentation and integration qualification remain.

All expression `Lower()` implementations, the lowering context and numeric
conversion helper are removed. Applicable-field package checks read concrete
parameter definitions; Lerp/default/swizzle checks now build detached IR. The
all-class input visitor check remains, while the obsolete function-to-legacy-call
conversion test is retired (production port behavior remains covered by direct
Build/function tests). All 18 expression cases pass
(`20260915-030143-525166-86110-MaterialTests.log`), and workspace `all` passes
(`20260915-030147-138727-86404-cmake.log`). No source/test root in the workspace
references expression `Lower()` or its context. Universal node/graph declarations
and now-unused compatibility validators still require removal.

The runtime owner graph readers/setters and instance/interface forwarding APIs
are removed, together with their Program-to-expression construction module and
expression-to-Program projections (736 lines). Engine, Sandbox and RoadWeaver
source/test searches contain no remaining references to those APIs. Workspace
`all` passed (`20260915-025823-048697-85555-cmake.log`), and the material regression
passed 235 cases (`20260915-025906-131053-85832-MaterialTests.log`; historical
baseline capture excluded). Expression `Lower()` methods and their direct test
consumers still retain the universal node types; those are the next removal boundary.

All function native fixtures now use concrete expressions, including dependency
invalidation, relocation/deletion, Cook fingerprints, nested Surface overrides
and package reference checks. Round trips compare reflected typed fields, owned
outputs, signatures and callee identity across fresh package lifetimes. The obsolete
compatibility-copy assertions and unused Program-producing test helpers are removed.
All 36 function cases pass (`20260915-025648-412087-85327-MaterialTests.log`),
followed by workspace `all` (`20260915-025703-931495-85409-cmake.log`). Searches
across Engine, Sandbox and RoadWeaver source/test roots find old graph accessors
only in the runtime compatibility implementation; its removal remains outstanding.

The macOS Stage 5 continuation migrates root-call atomic admission and instance
snapshot coverage, plus import-provenance round trips, to concrete expression
collections and outputs. Clang's authored-opcode switch warnings now have explicit
branches; transaction tests check reset results, proxy override coverage handles
Vector4, and typed override reflection uses the required dependent-template syntax.
The current material regression passes all 235 selected cases (only the historical
baseline capture excluded): `20260915-025123-088269-84721-MaterialTests.log`.
The test target compiles without warnings after those fixes
(`20260915-025058-019803-84607-cmake.log`). These receipts do not close the remaining
legacy API removal or final GPU/Cook/Game gates.

Stages 0 and 1 are complete. Stage 1 provides the non-reflected selected value
API, five typed instance arrays, concrete expression-owned defaults and counted
render-thread resource references. Cook emits five logical record arrays in
declaration order without serializing variant indices or native layouts.

Stage 2 implements all 46 expression classes, direct `Build()` and production
detached-IR compilation. Compiler/node-family checks and the recovered Stage 0
render comparison pass; all 28 reference PNGs are byte-identical.

Stage 3 authoring migration is implemented. Working copies, Apply, transaction history and clipboard
schema 7 own independent concrete expression snapshots. Interface/port/call edits,
input connections/defaults, deletion, constant extraction/inlining, parameter and
Surface commands, function previews, texture drops and function movement publish
through typed owner APIs. Graph equality checks preserve revisions during
presentation-only edits and Undo/Redo. Layout reads typed connections, including
call and Surface-override edges, and preserves node labels. Details edits parameter,
constant and UV fields and input promotion through typed snapshots, capturing
candidates only on submission. Catalog entries describe concrete expression classes;
creation validates the descriptor and populates only applicable typed defaults.
Constant type changes, inline constants/swizzles and the function editor node draft
now use concrete replacements with independent transaction copies. Function graph
publication checks dependency closure to reject recursive replacement targets.
The seven standard
functions, explicit PBR template and structural import recipes author typed graphs.
The separate clipboard reference carrier and old recipe-producing APIs are removed.

Inspection now reads typed expressions directly, including fixed/inline defaults,
call bindings and Surface attribute pins. Detached node views retain only the
selected display payload instead of universal nodes. The all-catalog check compares
every pin/default with the transitional reference and verifies no managed objects
are allocated; decoded-normal sampling output is reported as Float3.

Public document Capture/Commit now carry owned expression candidates; Commit
clones again so caller-held drafts cannot mutate history. Legacy editor
CreateNode/ReplaceNode/ReplaceProgram adapters and their request type are removed.
New material setup, parameter reachability, movement checks, diagnostics and texture
previews also read typed collections. MaterialEditor no longer reads or writes a
universal authored Program.

Expanded PBR, standard-function and dual-layer Rust native fixtures now build
owned concrete expressions and publish through typed APIs. Their shared consumers
compile in the native test aggregate. A detached Program reference adapter remains
only for legacy compiler qualification and is scheduled for Stage 5 removal.
The construction-helper and ownership audit found no production legacy graph
setter consumers outside the runtime transition implementation. Stage 4 has rebuilt
DefaultMaterial and all seven standard functions through fresh typed recipes,
validated staged packages with fresh readers, and preserved package/leaf identities
in both project inventories. The unreferenced GraphAuthoringV5 fixtures are retired;
no mounted material instances require recreation. The exact before/after hashes
and retired files are recorded in [the rebuild manifest](../../TypedMaterialValuesAndExpressions.rebuild.json).

Cook now includes Engine service-owned roots such as DefaultMaterial. Fresh Sandbox
and RoadWeaver Cook runs published 7 and 4 packages respectively, including the
default material. Receipts are `Build/TypedMaterialValuesRebuild/Sandbox-cook.json`
and `RoadWeaver-cook.json`. MaterialVulkanTests passed with retained test work
(`20260914-233529-745454-29044-MaterialVulkanTests.log`); this is correctness evidence,
not an exclusive-lane timing result or a Stage 0 image comparison. Scene import
Vulkan also passed (`20260914-233644-506646-30268-SceneImportVulkanTests.log`).
The Game `all` build passed (`20260914-235310-987458-36972-cmake.log`), and both
projects initialized Vulkan, rendered 120 engine ticks and exited normally using
their separately staged Cook outputs: Sandbox `20260914-235441-202325-3480-DurinGame.log`,
RoadWeaver `20260914-235444-939189-35408-DurinGame.log`. Sandbox reports its unrelated
missing GrayboxPawn visual. The recovered pre-migration baseline uses revision `e6e7b272e` with only the
original measurement harness restored. All 28 retained Vulkan PNGs are byte-identical
and all 135 parameter/function-port identity records match. The measured default-only load regression was traced to forced serialization of
class-default outputs. Ordinary default elision removes that overhead while the
ownership version and expression collection stay mandatory. Custom-output reload,
Cook stripping and staged fresh-reader tests pass. All 11 recipes now reduce package
bytes, owning-thread allocation requests and save/load medians against the recovered
baseline. The shipped total is 270,227 to 151,338 bytes, with exports increasing from
8 to 224; object overhead is included in those totals.

[The qualification report](../../TypedMaterialValuesAndExpressions.qualification.json)
retains all samples, section bytes, object counts, identity records, image hashes,
Cook reports and exact receipts. Six Debug CRT samples per operation use sample zero
as warmup; medians use samples 1-5. These are allocation requests, not retained or
peak memory, and do not establish Release or GPU timing performance.

| Recipe | Save ms before / after | Load ms before / after |
| --- | --- | --- |
| Default constructor | 36.772 / 27.738 | 12.878 / 10.264 |
| Explicit template | 1,560.780 / 398.560 | 1,044.400 / 170.551 |
| Structural plain | 104.842 / 42.998 | 66.511 / 22.623 |
| Structural transformed/packed | 206.501 / 74.715 | 103.771 / 34.811 |

The live-owner legacy compiler snapshot overload is removed. Function, expression,
scene-import, graph-operation and instance-variant consumers now request detached
IR snapshots directly. Normalization/compiler fixtures construct selected IR
payloads, retain dependency-before-consumer ordering when permuting independent
nodes, and still check canonical identity, shared-DAG bounds, resource layouts,
source generation and compiled stages. Literal-default, shared-fetch and decoded-normal
parity fixtures now construct concrete expression objects. The remaining function compiler fixtures have also migrated, and the old compiler
input, normalization/compile/identity overloads, and universal-node function
expansion implementation are removed. Legacy owner graph readers/setters and
function snapshot adapters still prevent closing the Stage 5 removal gate.
Focused graph/normalization/compiler coverage passed 69 cases
(`20260915-011822-564155-15504-MaterialTests.log`), and function/expression coverage
passed 55 (`20260915-012007-740947-15376-MaterialTests.log`). SceneImportTests
passed 10 (`20260915-012054-144732-42724-SceneImportTests.log`), the instance
variant/lifecycle case passed (`20260915-012143-128991-1076-MaterialTests.log`),
and workspace `all` passed (`20260915-012202-526143-17760-cmake.log`).

Function inline-default, all-eight Surface-override, and pre-pruning expansion-bound
fixtures now construct typed expressions and call the direct Build path, including
their function bodies. The expansion-bound check still accepts twenty calls and
rejects the twenty-first before dead-node pruning; typed Build now reports the
required `Bounds` diagnostic category for expanded IR limits. Authored collection
limits remain unchanged. Focused migrated cases passed in
`20260915-012357-944396-32204-MaterialTests.log`,
`20260915-012448-468777-41792-MaterialTests.log`, and
`20260915-012632-709232-39472-MaterialTests.log`. The combined function/expression
regression passed 55 cases (`20260915-012651-859225-30168-MaterialTests.log`),
and workspace `all` passed (`20260915-012730-133884-31112-cmake.log`).

Independent-call/multiple-output identity tests, nested Surface extraction, nested
texture-default/resource binding with actual shader compilation, and nested source
diagnostics now compile through typed Build results. Invalid nested links and ports
are injected into the real expression graph, so their owning function and root call
path are checked on the production Build path. All 36 function cases passed before
removing the obsolete implementation (`20260915-013115-370059-29908-MaterialTests.log`).
Source and test searches across all workspace projects found no remaining old
compiler-input or compile/normalize entry-point references.
After removal, 67 function/expression/normalization/compiler cases passed
(`20260915-013332-218063-33464-MaterialTests.log`). The unused legacy node conversion
helpers are also removed, and normalization/identity helpers now accept the sole
IR input type directly. The final twelve compiler/normalization cases passed
(`20260915-013416-231714-25928-MaterialTests.log`), followed by workspace `all`
(`20260915-013427-193129-32384-cmake.log`).

Function preview, graph admission/paste and package-reload preparation now validate
typed dependency bodies directly and publish only owner/path/revision stamps.
The validator retains recursion, shared-subtree height, dependency count, signature
and local expression checks; failed validation leaves previous stamps unchanged.
Function insertion validates concrete call bindings after constructing their typed
inputs instead of creating a universal call snapshot. The legacy function-body snapshot and closure APIs are now removed after migrating
the remaining native consumers; the smaller call-binding record remains only in
compatibility graph validation. All 36
function cases passed (`20260915-013858-013400-12228-MaterialTests.log`), package
reload passed 13 (`20260915-013944-445259-30532-AssetPackageReloadTests.log`), and
workspace `all` passed (`20260915-013951-816397-24508-cmake.log`).

`BuildFunctionSnapshot`, function snapshot/closure records and their capture
implementation are removed. Package round trips now compare detached typed IR,
parameter declarations and source maps; reload invalidation obtains owner stamps
from the production typed compiler snapshot. Detached port bindings remain readable
after unloading their package. The legacy local Program validator is temporarily
co-located with other compatibility validators instead of retaining a snapshot module.
The owning runtime function contract now documents typed collections and direct IR
emission. All 36 function cases passed (`20260915-014354-959293-40596-MaterialTests.log`),
package reload passed 13 (`20260915-014450-772291-19172-AssetPackageReloadTests.log`),
and workspace `all` passed (`20260915-014500-753971-31232-cmake.log`).

Render-proxy, compiled render representation and compile-lifecycle native fixtures
no longer read or publish compatibility Programs or function graphs. They construct
concrete expressions for resource sampling, custom Cook layouts and pending parameter
replacement, and edit typed output defaults/signatures for scheduling and dependency
invalidation. Cook stripping checks the actual expression collection. Reflected
no-op edit checks now target `ExpressionCollection`; failed candidate admission uses
an invalid typed connection. The typed recipe fixture now supports the concrete
Float3 truncation expression required by those custom resource graphs. Ten render
representation/lifecycle cases passed (`20260915-015101-249289-20532-MaterialTests.log`),
and five focused proxy/Cook cases passed (`20260915-015129-529889-26416-MaterialTests.log`).
The initial broader run exposed the missing test-helper constructor before that
fix (`20260915-014752-449395-16020-MaterialTests.log`); it is not a passing receipt.
The native test aggregate subsequently built all affected targets, including GPU
qualification executables (`20260915-015143-406824-27912-cmake.log`; build-only).

Editing-session and parameter-panel tests no longer use compatibility Program or
function graph reads/writes. Apply/Discard, asynchronous rejection, function replacement,
transaction labels and source/draft isolation inspect typed outputs, expressions,
parameters and presentation. Ten session/lifecycle cases passed
(`20260915-015514-754557-3432-MaterialTests.log`) and eleven panel cases passed
(`20260915-015547-045462-29268-MaterialTests.log`). Scene import fixtures now inspect
concrete sample owners and output selectors, construct the packed six-sample function
network directly, and clone typed function bodies to verify preserved user edits and
incompatible interfaces. All ten import cases passed
(`20260915-015859-037382-18384-SceneImportTests.log`). The import GPU test's structural
assertions likewise inspect typed expressions; rendering inputs remain unchanged.
`SceneImportVulkanTests` built (`20260915-015931-395319-24280-cmake.log`); GPU execution
was not repeated for these assertion-only changes.

Package-reload tests now publish typed nested calls, restore cloned typed bodies
and compare reflected fields after failed recursive reload. All thirteen cases passed
(`20260915-020534-079580-33864-AssetPackageReloadTests.log`). Material Vulkan fixtures
now author typed default/aggregate surfaces, HDR values, finite operands producing
Inf/NaN, and roughness-clamp edits. The no-graph error fallback is excluded from
compilation by checking its concrete root, preserving the original fallback test.
The complete Vulkan material scene passed (`20260915-020601-550934-17316-MaterialVulkanTests.log`),
including pixel equivalence, edited output differences and restoration. The earlier
run (`20260915-020327-455590-10392-MaterialVulkanTests.log`) exposed that test-helper
root guard and is not acceptance evidence. Expression ownership tests now compare
concrete children, reflected properties, signatures and presentation directly;
compiler-input copy mutation remains detached from the live owner. All nineteen
expression cases passed (`20260915-020719-173848-11616-MaterialTests.log`).
The native aggregate is current (`20260915-021213-818392-25844-cmake.log`);
this successful incremental check follows the shared-fixture rebuild.

Graph-operation coverage now inspects typed collection counts and output links,
compares detached expression fields for save/duplicate/Undo, and constructs typed
parameter, aggregate, dense-layout and clipboard fixtures. Canvas defaults, UV
extraction and constant-width replacement inspect concrete fields. All 58 graph
operation and material-creation cases passed
(`20260915-021747-141442-31016-MaterialTests.log`). Function clipboard, canvas stress and parameter-conversion fixtures now also use
typed expressions. Complete field snapshots cover function signatures and child
state; clipboard lifetime checks release construction references before collection.
Catalog compatibility checks build real typed candidates across every offered pin
and source type, including dangling links and malformed swizzles. Graph-operation
tests no longer use legacy Program/node objects, graph getters/setters or Lower.
All 58 cases passed again (`20260915-022413-881606-30968-MaterialTests.log`);
the preceding compile failure was a missing test include, corrected before this run.

Per user direction, routine canvas screenshot generation was removed: two
screenshot-only scenarios and the software PNG rasterizer were deleted, while
interaction, texture-channel, parameter, Undo/Redo and draw-data assertions remain.
The remaining 56 graph-operation/material-creation cases passed
(`20260915-022845-594907-38156-MaterialTests.log`, 11.115 seconds).
Same-command diagnostic XML timing was 30.497 seconds before (58 cases)
and 11.115 seconds after (56 cases), recorded under
`Build/MaterialTestSpeed/{before,after}.xml`; these are local timing observations,
not hardware performance qualification. The existing Vulkan pixel-comparison
gates are unchanged.

Schema/editing tests now construct typed parameter owners and retyping candidates,
check concrete reflection and deterministic fields, reject malformed typed graphs,
and use typed compiler snapshots for publication checks. The removed authored
Program version/opcode adapters are no longer exercised here: package ownership
marker rejection remains covered by the existing package-schema tests, and retired
IR opcodes are rejected directly. Fixed input-array arity is covered by concrete
pin and IR validation. All 63 parameter/schema/normalization cases passed
(`20260915-023510-701964-26924-MaterialTests.log`), plus the publication case
(`20260915-023554-545064-18296-MaterialTests.log`). An initial assertion used the
legacy aggregate-exclusivity category; it now checks the typed diagnostic's Type
category and explicit conflict message. Only MaterialFunctionTests still uses
legacy graph reads/writes among native test sources.

Function fixtures now inspect concrete calls and output links for editor operations,
construct resource fan-out and nested texture/Surface bodies directly, and validate
typed function terminals and port bindings. Package round-trip capture releases
all construction references before unload. Focused editor/recipe checks passed four
cases (`20260915-023806-747455-39116-MaterialTests.log`), eight resource/editor/port/package
cases passed (`20260915-023926-664459-39068-MaterialTests.log`), and both dependency-stamp
and nested-texture cases passed (`20260915-024021-668999-20576-MaterialTests.log`).
Legacy root-commit, package spoofing and dependency invalidation fixtures remain.

Stages 2-4 are qualified. Stage 5 is in progress. Instance override lookup and
iteration now read the five typed arrays directly; the old override record, cached
projection and invalidation/reference-rewrite machinery are removed. Parameter panel,
asset-tool and test consumers use direct selected-value reads and enumeration.
Function-dependency notifications traverse typed calls, and authored override
reachability traverses the detached expression IR. Selected-function navigation and
asset-tool inventory read expression owners directly. The missed-notification test
now mutates the real call expression rather than a compatibility cache.
Presentation sanitization and position membership now use typed expression GUIDs;
layout edits no longer rebuild universal-node projections. The retained compatibility
getter clears removed labels so history reads cannot observe stale presentation.
Presentation/history verification passed seven cases
(`20260915-002009-667653-25804-MaterialTests.log`), complementing the 91 passing
material graph/function cases in `20260915-001259-416417-37232-MaterialTests.log`.
Workspace `all` passed (`20260915-002025-911715-39396-cmake.log`).
Parameter declaration derivation now reads concrete expression owners directly for
candidate admission, loaded graphs, reflected edits and Cook. Cook no longer
refreshes the universal-node projection to recover parameter metadata. GUID ordering,
identity/name validation and publication-on-success are preserved. The complete
MaterialTests target passed all 232 cases across 20 suites (excluding the separately
recorded timing capture), receipt `20260915-002303-209842-27544-MaterialTests.log`;
workspace `all` passed (`20260915-002704-820363-33768-cmake.log`). SceneImportTests
passed all 10 cases (`20260915-002721-600603-21984-SceneImportTests.log`).
Material functions no longer retain a universal Graph cache or a projection-reset
PostLoad override. The temporary compatibility getter returns a detached value;
mutating it cannot change the typed collection. Function/expression/graph coverage
passed 106 cases (`20260915-002944-143020-30048-MaterialTests.log`), package reload
passed 13 (`20260915-003135-937515-6088-AssetPackageReloadTests.log`), and workspace
`all` passed (`20260915-003143-768283-2776-cmake.log`).
Typed owner validation now uses expression Build semantics directly: material
load/edits validate through `ValidateSurface`, and function load/edits through
`ValidateFunction`. Local authoring checks retained call-port types without requiring
callee bodies; compilation still admits and expands the real dependency closure.
Private opaque validation values are discarded and cannot become compiler inputs.
Surface ports retain their eight attribute types, and same-type call outputs share
validation values. Function setters no longer project universal nodes for validation.
Parameter admission checks the shared 128-owner bound and node/parameter GUID
separation. Full MaterialTests passed 234 cases
(`20260915-003820-246441-35240-MaterialTests.log`); subsequent parameter-bound and
validation-value changes passed six focused cases including the maximum 256-call,
16-output-per-call fixture (`20260915-004252-809177-3704-MaterialTests.log`).
SceneImportTests passed 10 (`20260915-004311-633883-35604-SceneImportTests.log`),
AssetPackageReloadTests passed 13 (`20260915-004357-067531-27392-AssetPackageReloadTests.log`),
and workspace `all` passed (`20260915-004406-349425-41412-cmake.log`).
Material owners no longer retain Program or function-call projection storage, nor
a universal-node code checkpoint. Typed validation produces a transient 128-bit
edit fingerprint from graph operations, GUID connection selectors, call ports and
callee object handles. Parameter defaults and display metadata are excluded. Typed
setters no longer construct a Program to classify edits; parameter writes and load
no longer maintain compatibility caches. Temporary compatibility getters return
independent optional Program values and call vectors, and native consumers retain
explicit copies where needed instead of references into cached storage.
Full MaterialTests passed 237 cases across 20 suites
(`20260915-005402-039482-9648-MaterialTests.log`). `DurinNativeTests` built
(`20260915-010008-613308-26912-cmake.log`; unchanged retry after a discovery timeout).
The remaining old package test now constructs typed recipes and compares reflected
expression fields and child ownership, checks deterministic typed package fields,
rejects malformed saves without replacing the saved package, and rejects abandoned
children before publication. It and missing-ownership-marker coverage passed
(`20260915-010305-712310-20004-StaticMeshTests.log`), complementing the other ten
passing static-mesh material cases in `20260915-010015-955164-36004-StaticMeshTests.log`.
Both material and function serializers reset their ownership marker before loading
and reject values other than 2; instance override storage has its own required marker.
Current integration receipts: SceneImportTests 10/10
(`20260915-010347-313024-14700-SceneImportTests.log`), AssetPackageReloadTests 13/13
(`20260915-010433-753213-39404-AssetPackageReloadTests.log`), SceneImportVulkanTests
build-only (`20260915-010441-492386-26164-cmake.log`), and workspace `all`
(`20260915-010450-979041-36404-cmake.log`).
Legacy projection APIs, compiler interfaces and final validation remain. Instance coverage passed 53 material tests, 47 function/parameter-panel tests
and all 10 scene-import tests. Receipts: `20260915-000033-623122-39812-MaterialTests.log`,
`20260914-235949-277274-30432-MaterialTests.log`, and
`20260915-000100-848105-42344-SceneImportTests.log`. Workspace `all` passed
(`20260915-000152-252117-33840-cmake.log`). Typed dependency queries passed 99 material/function/panel cases and the
updated missed-notification case (`20260915-000607-503550-22004-MaterialTests.log`,
`20260915-000811-726081-11484-MaterialTests.log`); SceneImportTests passed all 10
(`20260915-000846-812424-39956-SceneImportTests.log`). Workspace `all` passed
(`20260915-000950-036857-29980-cmake.log`), and the read-only standard-function
inventory reports all eight typed owners with matching built-in dependencies. Foundation work does not count as completion of downstream stages.

Current Stage 3 validation receipts (Win64 Debug; unchanged earlier coverage is
reused where the later edits do not affect its inputs):

| Coverage | Result | Receipt under `Build/.agent-state/logs/` |
| --- | --- | --- |
| Graph operations, editing sessions, functions and parameter panel; public candidate/history isolation | 109 passed | `20260914-225559-925980-37208-MaterialTests.log` |
| Typed parameter reachability: resource-only sample use excludes unused UV dependencies | Passed | `20260914-225726-570075-29968-MaterialTests.log` |
| Typed connection-order and expression/Surface deletion | 15 passed | `20260914-212841-552314-16388-MaterialTests.log` |
| Structural scene import | 10 passed | `20260914-232514-152822-36092-SceneImportTests.log` |
| Layout/movement, including typed call/Surface dependencies and label retention | 7 passed | `20260914-220809-714795-35340-MaterialTests.log` |
| Details/canvas/parameter commands | 16 passed | `20260914-221504-317971-9164-MaterialTests.log` |
| Details idle-frame object-allocation guard | Passed | `20260914-221617-529814-14288-MaterialTests.log` |
| Catalog coverage, including all concrete shapes and invalid descriptors | 7 passed | `20260914-222444-138692-13232-MaterialTests.log` |
| Typed inspection parity across all catalog shapes; no managed-object allocation | Passed | `20260914-224508-641056-21508-MaterialTests.log` |
| Workspace `all` | Passed | `20260914-225755-705317-20436-cmake.log` |
| Typed expanded/standard/Rust fixtures, graph operations, functions, parameter panel and editing sessions | 161 passed; two shipped-DefaultMaterial cases deferred to Stage 4 | `20260914-232315-214527-31808-MaterialTests.log` |
| Fresh staged shipped-asset save/reload/recipe ownership | Passed | `20260914-232733-955712-26872-MaterialTests.log` |
| Shipped DefaultMaterial runtime proxy regressions | 2 passed after rebuild | `20260914-232831-631125-28668-MaterialTests.log` |
| Full material regression, baseline capture excluded | 231 passed; new rebuild test failed cached-path isolation, fixed and rerun below | `20260914-232843-441203-6024-MaterialTests.log` |
| Renderer tests followed by rebuild test with fresh asset lifetime | 54 passed | `20260914-233253-210619-38576-MaterialTests.log` |
| Native test aggregate, including shared fixture GPU consumers | Build passed after transient test-discovery timeouts | `20260914-232300-803591-27804-cmake.log` |

Final Stage 1 validation (Win64 Debug):

- All 213 selected MaterialTests passed:
  `Build/.agent-state/logs/20260914-195210-661482-23760-MaterialTests.log`.
  The seven shipped-asset cases and expensive baseline recapture remain Stage 4
  gates, not accepted exclusions from final plan validation.
- All 10 SceneImportTests passed:
  `Build/.agent-state/logs/20260914-195621-589888-28636-SceneImportTests.log`.
- Workspace `all` passed:
  `Build/.agent-state/logs/20260914-195021-296889-37480-cmake.log`.
- MaterialVulkanTests and SceneImportVulkanTests compiled against the new API:
  `Build/.agent-state/logs/20260914-195703-025569-9212-cmake.log` and
  `Build/.agent-state/logs/20260914-195743-466254-23568-cmake.log`.
  GPU execution remains pending the shipped-asset rebuild.
- Changed-document validation passed for both changed documents. The affected
  selector expands this shared Engine change to all tests; the bounded material
  and scene-import suites cover this increment, with final workspace-wide
  acceptance still required in Stage 5.

The missing historical measurements and images were reproduced in the isolated
`Build/B0` worktree at pre-migration revision `e6e7b272e`, with only the original
capture harness restored from `60d193819`. The report retains its hash and receipts;
the current implementation was not used to generate the old baseline.

## Earlier Foundation Receipts

The following checkpoints record earlier increments, not the current stage
selection or a new stop instruction.

Stage 0 is complete. The package/duplication and transaction ownership
primitives now have executable qualification for polymorphic editor collections,
independent Apply copies, detached deletion, GC retention, Undo/Redo, and Cook
descendant stripping. Stage 1 is in progress: instance persistence now uses five
typed reflected arrays, and the editor's property transactions target the matching
array. The former unified override record is a non-reflected read-only projection.
Cross-array duplicate IDs and unsupported instance schemas fail serialization;
orphan values and sampling policy remain retained. The transient variant value API
and expression/default ownership migration are still outstanding.
Stage 1/2 preparation now supplies 46 concrete reflected expression classes,
covering every supported opcode, with common parameter metadata, concrete defaults,
applicable pins, fixed coordinate defaults, and expression-owned call bindings.
All graph-side connections use `FMaterialExpressionInput` with expression GUID,
output index, and output GUID. Numeric defaults are separate fields on their
concrete expression (or call binding), remain present while connected, and survive
disconnection. Texture and Surface connections carry no numeric default storage.
Lowering explicitly creates `FMaterialProgramLink`; graph records do not embed it.
The unified-input round-trip and lowering tests passed:
`Build/.agent-state/logs/20260914-164446-220485-36068-MaterialTests.log`.
The corresponding workspace `all` build passed:
`Build/.agent-state/logs/20260914-164458-304142-28060-cmake.log`.
Per-expression lowering uses the existing program node only as a local intermediate.
The final entry point is expression `Build()` into typed IR. The current `Lower()`
and Program graph are temporary migration machinery and must be removed, rather
than retained as an adapter in the final expression-to-compiler path.
The source generator now validates detached IR directly instead of reconstructing
an authored Program graph. Direct checks cover topological indices, signatures,
depth, parameter bindings, finite constants/defaults, and swizzle bounds before
indexed generation. Five compiler tests passed, including malformed IR created
without any authored graph:
`Build/.agent-state/logs/20260914-165010-176048-31752-MaterialTests.log`.
The material regression passed all 206 cases (baseline recapture excluded):
`Build/.agent-state/logs/20260914-165031-730034-27456-MaterialTests.log`.
The final expanded-link bound and its rejection test passed the five-case compiler
suite in `Build/.agent-state/logs/20260914-165411-002037-36548-MaterialTests.log`.
The final workspace `all` build passed:
`Build/.agent-state/logs/20260914-165422-484367-39924-cmake.log`.
Direct expression construction now exists for all 43 non-function classes through
`FMaterialExpressionBuildContext` and virtual `Build()`. Numeric, coordinate,
sampling/multi-output, parameter, and Surface families emit IR without Program
nodes. The context owns traversal caches, cycle/depth/node/link checks, exact input
types, retained-default validation, and detached source/parameter records; failed
builds return diagnostics without partial IR. Six expression tests passed:
`Build/.agent-state/logs/20260914-170150-391127-31720-MaterialTests.log`.
After unique parameter-owner admission was added, the six tests passed again in
`Build/.agent-state/logs/20260914-170233-795443-34924-MaterialTests.log`; workspace
`all` passed in `Build/.agent-state/logs/20260914-170310-913201-38848-cmake.log`.
All 46 concrete classes now implement direct `Build()`, including function inputs,
outputs, and calls. A bounded owning-thread body provider supplies expression
collections and signatures; invocation-local caches bind GUID ports while sharing
detached IR storage. Numeric, texture, Surface, input-alias, and UV0 defaults are
preserved; texture defaults travel as a selected value alternative until sampling,
never as fabricated resource indices. Nested calls retain source paths and port
diagnostics. Required/mismatched bindings, missing/duplicate terminals, recursive
calls, cyclic defaults, invalid disconnected function expressions, and expanded
IR bounds reject the result without publishing partial IR or dependencies.
At this checkpoint, complete root-graph admission and material-owner/compiler
snapshot integration remained pending. The function owner exposed its expression
collection to the body provider; the production cutover is recorded below.
The final function increment passed all 14 expression/compiler cases and the
workspace `all` build:
`Build/.agent-state/logs/20260914-172718-182863-33704-MaterialTests.log` and
`Build/.agent-state/logs/20260914-172728-349041-37608-cmake.log`.
`FMaterialIRCompilerInput`, `NormalizeMaterialIR`, and `CompileMaterialIR` now
provide a detached IR-only route through normalization and backend compilation.
Validation is shared with source generation without constructing authored nodes;
parameter declarations are checked before pruning, while device layout limits
apply only to active bindings. Normalization prunes unreachable nodes, canonicalizes
commutative input ordering and selected immediate widths, excludes inactive Surface
root fields, and remaps source metadata. Both compiler entries share backend code.
The direct entry produced identical generated source, layout, stage entry points,
and shader hashes to the retained synthetic compiler baseline. An expression-built
Surface also compiled through this entry into all three required shader stages.
All 16 focused expression/compiler tests passed:
`Build/.agent-state/logs/20260914-173625-541443-37320-MaterialTests.log`.
All 213 material regression cases passed (baseline recapture excluded):
`Build/.agent-state/logs/20260914-173734-417984-31276-MaterialTests.log`.
MaterialVulkanTests and the workspace `all` build passed:
`Build/.agent-state/logs/20260914-174126-861409-40076-MaterialVulkanTests.log` and
`Build/.agent-state/logs/20260914-174218-807732-35704-cmake.log`.
`DMaterialFunction` now persists its signature and owned expression collection
under ownership schema 2. The graph-complete load hook checks unique children,
exact ownership, abandoned descendants, expression identities, and local graph
validity. Apply duplicates candidates before replacing the collection, detaches
retired children, and preserves invalid external call references for diagnostics
at compilation. Dynamic collections have no fixed CDO children and are explicitly
serialized; load/duplication constructors do not create extra initial children.
Labels reside in presentation and survive position edits. Native Program setters
temporarily construct expression candidates, and getters produce non-reflected
projections; neither is an asset reader or upgrade path, and both are removed with
the remaining Stage 3 consumers. Old persisted function schemas are rejected.
The standard-function Cook test now constructs fresh recipe fixtures; shipped
packages remain pending the Stage 4 rebuild. The initial 214-case regression
passed 208 cases and exposed six migration assumptions; after fixing these,
all six plus the expanded expression suite passed (17 cases):
`Build/.agent-state/logs/20260914-180711-305954-35904-MaterialTests.log`.
This includes independent Apply/save/load/duplication, labels, direct Build from
the loaded owner, failed-edit atomicity, ownership/schema rejection, nested
diagnostics, and Cook/load without authored function assets.
SceneImportTests passed all 10 cases and the workspace `all` build passed:
`Build/.agent-state/logs/20260914-180743-289632-31108-SceneImportTests.log` and
`Build/.agent-state/logs/20260914-180814-333622-32492-cmake.log`.
The final full material regression passed all 215 cases (baseline recapture
excluded): `Build/.agent-state/logs/20260914-180832-149010-30192-MaterialTests.log`.
The material owner now also persists an expression collection and concrete typed
terminal defaults under ownership schema 2. Program and the independent call
table are non-reflected projections. Material parameter setters write the owning
typed parameter expression; child property events refresh the derived schema,
classify shader changes, and notify the material owner for Undo/Redo and render
invalidation. Apply clones children before committing, and restored graphs reject
invalid ownership, orphan descendants, parameter declarations, and local links.
Function-call references are explicitly EditorOnly so Cook's dependency discovery
keeps them as build inputs rather than runtime package roots. Numeric ranges are
now restricted to scalar recipe parameters, matching their concrete owners.
The typed material owner save/load/duplication and failed-edit test passed:
`Build/.agent-state/logs/20260914-182108-148737-29444-MaterialTests.log`.
The first broad material run exposed stale reflection/fixture assumptions and
Cook reference policy; seven cases still require the shipped DefaultMaterial
rebuild. They remain outstanding Stage 4 gates, not accepted exclusions from
final validation. At this owner-storage checkpoint, production compilation still
consumed the temporary projection; the direct IR cutover is recorded below.
After updating the consumers and fixing the policy, all 209 material cases outside
the seven outstanding shipped-asset tests passed:
`Build/.agent-state/logs/20260914-182911-110013-40480-MaterialTests.log`.
The separate expensive baseline recapture was also excluded from this regression
run; it remains a final measurement gate.
Scene import now constructs explicit empty defaults for multiplier inputs so its
authored-recipe comparison matches the expression projection without mistaking a
fresh structural parent for an edited asset. All 10 SceneImportTests passed,
including parent reuse and reimport, and the workspace `all` build passed:
`Build/.agent-state/logs/20260914-183455-085127-37156-SceneImportTests.log` and
`Build/.agent-state/logs/20260914-183543-724487-40508-cmake.log`.
Production compilation now captures the material's owned expressions through
`Build()` directly into `FMaterialIRCompilerInput`. The compile queue, synchronous
fallback, normalization, and backend compilation consume detached IR. Function
owner handles, paths, and revisions are captured with the snapshot and checked at
completion and Cook publication without rebuilding the temporary Program graph.
Root admission validates all expressions, including dead nodes, unique parameter
metadata, retained terminal defaults, exact output types, and authored link bounds.
Failed snapshots preserve the caller's previous input and owner stamps.
Parity checks compare generated source and layout with the temporary compiler,
and identity after applying the same IR normalization to both results. Legacy
Program identities can differ because direct normalization excludes inactive
terminal defaults; the existing IR encoding and version remain unchanged.
Program projections, native graph setters, and editor adapters remain temporary
consumers pending later stages. This increment does not complete the plan; work
stops after its validation and commit at the user's request.
The final runtime code passed 210 of 211 selected material cases in
`Build/.agent-state/logs/20260914-185555-139538-9892-MaterialTests.log`.
The remaining case expected an inactive connected default to change identity;
its corrected coverage verifies identity reuse while connected and a changed
identity after disconnection. All 22 expression/compiler/publication cases then
passed in `Build/.agent-state/logs/20260914-185923-504586-31324-MaterialTests.log`.
The seven shipped-asset cases and expensive baseline recapture remain excluded
from this incremental run, with their final gates still open.
All 10 SceneImportTests and the workspace `all` build passed:
`Build/.agent-state/logs/20260914-185940-839991-25148-SceneImportTests.log` and
`Build/.agent-state/logs/20260914-190039-633122-20836-cmake.log`.
MaterialVulkanTests did not pass: loading the shipped schema-1 DefaultMaterial
failed before qualification could finish, and fixture teardown asserted after
the failed assertion. The retained failure is
`Build/.agent-state/logs/20260914-190022-999737-41368-MaterialVulkanTests.log`.
This GPU gate also remains pending the Stage 4 shipped-asset rebuild.
The IR immediate migration now replaces simultaneous literal/parameter/swizzle
members with `FMaterialIRPayload`, selecting no immediate, a bounded numeric
literal, a parameter GUID, or a bounded swizzle. Direct Build, normalization,
source generation, and tests use the selected payload. Canonical encoding rejects
opcode/payload disagreement before emitting bytes and retains the existing
explicit logical format; neither variant indices nor native layouts are encoded.
All 208 material regression cases passed (baseline recapture excluded):
`Build/.agent-state/logs/20260914-170724-973481-33104-MaterialTests.log`.
The retained compiler probe matches the pre-change receipt exactly: 10,156 canonical
bytes, 15,351 generated bytes, source hash `171b1d04339f5889f9cc577a58f72b52`,
identity `b551156d0f84f52bd667d942fdcf9cdf`, and 145,365 cooked bytes. MaterialVulkanTests
and all 10 SceneImportTests passed:
`Build/.agent-state/logs/20260914-171108-886248-29604-MaterialVulkanTests.log` and
`Build/.agent-state/logs/20260914-171216-096222-38600-SceneImportTests.log`.
The workspace `all` build passed:
`Build/.agent-state/logs/20260914-171317-875097-9060-cmake.log`.
At the initial expression-library checkpoint, neither owner had switched storage.
Both now persist concrete expression collections; the transient value API and
remaining native/editor consumers still need migration. No old-asset reader or
second persisted authored schema is introduced.
Four expression tests passed (including the complete class/opcode map, typed
Save/Load and duplication, retained defaults, and function-port lowering):
`Build/.agent-state/logs/20260914-161712-463235-32700-MaterialTests.log`.
The workspace `all` build passed:
`Build/.agent-state/logs/20260914-161905-075260-40812-cmake.log`.

Owner integration accounts for an observed loader ordering constraint:
`ApplyLinkerValues` invokes each object's Serialize in export order, so an owner's
Serialize cannot validate yet-unpopulated expression children. PostLoad runs after
all values, but its void API cannot reject publication. The new read-only
`DObject::ValidateLoadedObjectGraph` boundary now runs after package values are
restored, and after the entire selected batch for prepared reload graphs. Rejection
precedes PostLoad/final publication and preserves previous prepared output. The
same hook rejects invalid transient object graphs and duplicates after their values
are restored. Two qualification tests prove child-state visibility, cross-package
batch ordering, ordinary-load rollback, no PostLoad on failure, and output retention:
`Build/.agent-state/logs/20260914-162819-316245-40156-AssetPackageTests.log`.
The workspace `all` build passed:
`Build/.agent-state/logs/20260914-162850-788108-3916-cmake.log`.
Workspace `all` build and all 87 regular native test targets passed for this shared
lifecycle change (`20260914-162850-788108-3916-cmake.log` and
`20260914-163200-956456-5800-ctest.log` under `Build/.agent-state/logs/`).
The instance-storage increment passed 201 MaterialTests (the separate baseline
capture was excluded), all 10 SceneImportTests, MaterialVulkanTests, and the
workspace `all` build. Receipts:
`Build/.agent-state/logs/20260914-155649-432515-38264-MaterialTests.log`,
`Build/.agent-state/logs/20260914-160029-989191-33660-SceneImportTests.log`,
`Build/.agent-state/logs/20260914-160202-084851-30244-MaterialVulkanTests.log`, and
`Build/.agent-state/logs/20260914-160007-065658-39860-cmake.log`.
The preliminary ImportedSurface cleanup removes the obsolete shipped material and
its production initializer; scene imports continue to generate structural parents.
An explicit `asset material-template` entry creates PBRSurfaceMaterial_MR at an
unused destination for future instance-parent workflows. This optional recipe
remains in scope for typed-expression migration, but no template asset is shipped
or automatically initialized.
Stage 0 qualifies object ownership and captures behavioral baselines before the
shared API changes begin.

Stage 0 receipts (Win64 Debug, 2026-09-14):

- AssetPackageTests: 154 cases in 6 suites passed, including two new polymorphic
  collection/owned-child cases. Receipt:
  `Build/.agent-state/logs/20260914-152728-859524-32816-AssetPackageTests.log`.
- EditorOperationTests: 45 cases in 5 suites passed, including a new custom
  transaction that detaches a child, collects, undoes/redoes, and releases it
  when history is reset. Receipt:
  `Build/.agent-state/logs/20260914-153157-179283-40680-EditorOperationTests.log`.
- MaterialVulkanTests passed; the retained run is
  `Build/.agent-state/logs/20260914-153037-136204-9004-MaterialVulkanTests.log`.
  Its 28 reference PNGs are copied to `Build/TypedMaterialValuesBaseline/Images`.
  The preceding passing run cleaned its temporary images and is not the retained
  baseline. These runs establish correctness, not exclusive-lane timing.
- Fresh construct-free identity audits cover 18 Sandbox-mounted and 14
  RoadWeaver-mounted packages:
  `Build/TypedMaterialValues-Sandbox-identity.json` and
  `Build/TypedMaterialValues-RoadWeaver-identity.json`.
- A fresh optional template was created only in the isolated
  `Build/TypedMaterialValuesBaseline/Baseline.dproject`. Production assets were
  not rebuilt. `Build/TypedMaterialValuesBaseline/inventory.json` records source
  consumer locations, package identities/references, package section sizes,
  SHA-256 fingerprints, and retained image fingerprints; its adjacent
  `inventory.py` reproduces the inventory against the current checkout.
- Changed-document validation passed (one document). The affected-test selector
  expands these native-test-only edits to the Engine project; the two changed
  test targets were run in full, plus the explicitly required material Vulkan
  baseline. No production API changed, so no workspace `all` build was required
  for this ownership-qualification increment.

The ownership tests qualify existing primitives, not the future MaterialGraphDocument
implementation. No generic serializer defect was demonstrated. Clearing a reflected
collection alone leaves a live Outer child in authored exports; deletion must
reparent that exact child to a transient history owner before saving. Undo restores
the owner and collection, and history enumerates the child through its collector.
Do not mark a history-retained child as garbage. Apply duplicates children into
the destination and retires the old destination graph; copying pointers is invalid.
Cook removes unreachable editor-only children including their descendants.

The rebuild baseline now includes 11 freshly constructed recipe packages and 135
parameter/function-port identity records in
`Build/TypedMaterialValuesBaseline/RebuildRecipes`. The capture test passed:
`Build/.agent-state/logs/20260914-154353-732087-14732-MaterialTests.log`.
`measurements.tsv` records six save/load samples per package, with sample zero
reserved for warmup. Allocation counts/bytes are owning-thread Debug CRT allocation
requests (including reallocations), not peak memory, live bytes, worker allocations,
or release-build performance. All snapshots are taken offline without shader work.
The 92-file consumer inventory now includes program/function families and inferred
API consumers across all three projects. No downstream stage is marked complete.

Baseline medians from samples 1–5:

| Recipe | Save ms | Load ms | Save allocation requests | Load allocation requests | Save requested bytes | Load requested bytes |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Default constructor | 66.272 | 20.688 | 67,090 | 19,147 | 3,255,948 | 1,005,877 |
| Explicit template | 3,032.630 | 1,495.500 | 3,227,215 | 1,119,410 | 181,817,900 | 58,601,947 |
| Structural plain | 159.661 | 84.523 | 173,807 | 77,454 | 8,516,817 | 3,808,872 |
| Structural transformed/packed | 391.980 | 192.599 | 410,128 | 162,740 | 22,344,720 | 8,460,080 |

Fresh structural packages are 5,211 and 9,100 bytes; their Names/Schemas/Values
sections are 3,138/365/776 and 3,270/369/4,529 bytes, respectively, with one object
export each. The default constructor package differs from the shipped default's
authored overrides; keep the shipped baseline separately. Function samples and
exact IDs are retained in the TSVs. Stage 4 compares the same recipes and sampling
instrumentation, and also rechecks the shipped package set and Vulkan images.
The unreferenced GraphAuthoringV5 fixture files will be removed in Stage 4 rather
than recreated as unsupported historical assets; current native tests construct
their authored and cooked fixtures from recipes.

Preliminary cleanup/creation validation (Win64 Debug):

- Workspace `all` build passed, including the new explicit template API/command:
  `Build/.agent-state/logs/20260914-151445-172628-39132-cmake.log`.
- Cleanup's affected selection passed all 87 regular native targets (including
  material/import Vulkan integration):
  `Build/.agent-state/logs/20260914-150759-964152-41780-ctest.log`.
- After adding the explicit template, MaterialTests passed 200 cases in 19 suites:
  `Build/.agent-state/logs/20260914-151551-987420-39428-MaterialTests.log`.
- Final SceneImportTests passed all 10 cases after fixture lifetime cleanup:
  `Build/.agent-state/logs/20260914-151934-635941-38272-SceneImportTests.log`.
- Asset command forwarding/grammar tests passed 33 cases. The new command was
  exercised against the isolated `Build/PBRSurfaceMaterialCommand/Test.dproject`:
  preview wrote no package, apply produced a compatible v10 package, and a
  repeated apply failed with unchanged file hash. Cook published one package:
  `Build/.agent-state/logs/20260914-151735-930766-19388-DurinAssetTool.log`.
- Post-removal identity audits cover 18 Sandbox-mounted and 14 RoadWeaver-mounted
  packages, with no reference to the retired template. Reports:
  `Build/ImportedSurfaceRemoval-Sandbox-after.json` and
  `Build/ImportedSurfaceRemoval-RoadWeaver-after.json`. A maintenance apply
  recreated neither the retired nor optional template.

The user explicitly requests rebuilding the small existing material set instead
of upgrading old assets. Recreate DefaultMaterial, required
standard material functions, and affected fixtures from current recipes. Do not
implement a legacy reader, conversion tool, neutral export/import bridge, or
dual-schema transition. Inventory dependencies to confirm the rebuild closure;
preserve externally referenced asset identities and built-in parameter IDs.

The inspected workspace contains Engine, Sandbox, and RoadWeaver. Current
`FMaterialParameterValue` reflects Scalar, Vector2, Vector3, Vector4, Texture,
SamplerState, and TextureFallback simultaneously; its enclosing declaration or
override supplies the discriminant. `FMaterialProgramNode` likewise reflects
every node payload, and both materials and functions persist arrays of it.
`FMaterialProgram` currently crosses authoring and compilation boundaries.
`DMaterial::ParameterSchema` is already a transient graph-derived projection;
this ownership decision must survive the refactor.

The obsolete 58,379-byte ImportedSurface template is removed before this refactor.
Use newly imported structural parents, explicit PBRSurfaceMaterial_MR templates,
and representative test graphs for size
comparisons. Per-expression object/export records introduce overhead that must
be measured alongside eliminated fields. Capture fresh baselines in Stage 0.

## Goal

An authored value contains exactly its selected value. An authored expression
owns only fields meaningful to its node family. Material instances store typed
overrides. Compilation consumes detached immutable data, and cooked execution
does not require expression objects. Stable parameter, node, and function-port
identities required by dependents and existing editing/rendering behavior survive
asset reconstruction.

## UE Reference And Selected Decisions

Epic's public API documents show three useful boundaries:

- [FMaterialParameterValue](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Engine/FMaterialParameterValue)
  has a type discriminator and union-based alternatives with typed accessors.
- [UMaterialInstance](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Engine/UMaterialInstance)
  stores separate scalar, vector, texture, and other parameter arrays.
  [FScalarParameterValue](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Engine/FScalarParameterValue)
  combines parameter identity with a scalar value.
- [FMaterialExpressionCollection](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Engine/FMaterialExpressionCollection)
  holds object references to expressions. Concrete classes such as
  [UMaterialExpressionAdd](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Engine/UMaterialExpressionAdd)
  declare their own inputs and retained defaults.

These are public API observations, not a claim that UE's private package or
compiler implementation is identical to the proposed Durin implementation.

### Parameter values and persisted records

Keep `FMaterialParameterValue` as a non-reflected C++ value API backed by
`std::variant<float, FVector2, FVector3, FVector4, FMaterialTextureValue>`.
`FMaterialTextureValue` groups a texture reference, sampler state, and fallback.
The alternative is the type authority: derive `GetType()` from it and expose
checked accessors and factories. Do not maintain a separately mutable tag or
silently convert a type mismatch. Equality compares only the active alternative.
Do not put an unsupported `std::variant` into `DPROPERTY`.

Persist instance overrides as five reflected typed arrays, with entries holding
`ParameterId` and their concrete value. Texture entries hold the whole texture
value including sampling policy. Reject duplicate IDs across arrays at the
validated mutation/load boundary. Preserve existing orphan overrides and their
type; reject an incompatible active assignment without silently dropping it.
An override explicitly equal to its parent remains an override.

Split common parameter identity/presentation metadata from its typed default.
Typed parameter expressions own this metadata and their concrete default.
Scalar range and texture usage belong to the applicable expression family.
The unified definition/override views become derived transient C++ projections,
with documented invalidation on owner revision changes. Migrate direct-field and
`span` consumers rather than introducing a second writable table. Retain stable
parameter GUIDs and existing built-in IDs.

Transient variants and derived caches need explicit reference collection when
they can outlive their reflected owner. Transactions, clipboard state, pending
edits, and render publication must retain their texture resources through the
existing ownership mechanisms. Cook writes explicit typed logical records;
neither variant indices nor native C++ memory layout become a wire contract.

### Authored MaterialExpression objects

Introduce `DMaterialExpression : DObject`. Materials and functions own their
expression objects through Outer and a reflected EditorOnly collection of
`TObjectPtr<DMaterialExpression>`. Expressions are package-internal objects,
not independently managed asset files. The base carries stable node identity
and the common expression interface, not Parameter, UVSettings, or a universal
Inputs/defaults payload. Presentation stays in the existing separate GUID-keyed
editor data and remains excluded from compilation.

Use concrete reflected types for constants, typed parameters, arithmetic,
sampling, swizzle, coordinate operations, surface operations, and function
terminals/calls. Factor shared implementation through bounded family bases where
the fields actually match. Stage 0 maps every existing opcode to a concrete
type; no supported opcode may fall back to the universal persisted node.

Expression inputs retain stable source-node/output identities. Keep the current
GUID link scheme instead of requiring pointer links between expressions. Each
expression declares its applicable inputs and retained disconnected defaults.
Connections continue to override defaults without erasing them. UV settings
belong only to sampling/coordinate expressions; parameter metadata belongs only
to parameter owners. A function-call expression owns its callee reference and
port bindings, eliminating the independent authored call table after migration.
Function signature metadata remains function-owned.

Class/type replacement is a validated graph command. Preserve compatible links
and identity where existing semantics allow; reject incompatible changes or
explicitly disconnect them through the existing command policy. Undo retains
the replaced state. Do not persist every old class's inactive fields as hidden
history. Rebuilt recipes establish their intended defaults directly. Preserve
meaningful connected-input defaults and instance override semantics in the new
model; historical inactive payload preservation is not an asset requirement.

### Compiler and editor boundaries

Build the authored expression graph on the owning thread directly into an immutable
typed IR snapshot through expression `Build()` methods. Resolve connections,
applicable defaults, and function calls at this boundary; normalization and code
generation consume detached IR. `FMaterialProgramNode` may temporarily serve as
a migration target, but the final path contains no Program graph adapter or
reverse reconstruction of authored nodes. Compiler nodes use bounded typed
payloads and omit editor metadata. Worker snapshots contain no live expression pointers or object-load
operations; capture resource/dependency identities through the established
compiler snapshot contract.

Preserve atomic candidate validation, preview working copies, Apply/Discard,
revision invalidation, and stale compilation rejection. Copying a collection
of object pointers is not a deep graph copy. Working-copy creation and Apply
must duplicate/reparent owned expressions correctly without sharing mutable
children with the source. Deleted expressions must not remain serializable
merely because their Outer still points into the asset. History retains deleted
state through the transaction system rather than abandoned package children.

Keep DAST v10, its default-baseline rules, and its whole-container replacement
semantics. Do not add generic array patches or a new global package format for
this material schema change. Qualify expression CDO/default-relative saving,
owned-object export discovery, duplication, and EditorOnly graph stripping
before relying on them. Fix a demonstrated generic ownership defect at its
owner with focused tests; do not invent a material-specific package serializer.

## Field and opcode inventory

The following destinations are the implementation map, not declarations of
already implemented classes. All expression names below use the
`DMaterialExpression` prefix. Numeric family bases may share code and applicable
fields; no family base contains parameter, sampling, function, and surface payloads
together. The enum has no supported opcode at numeric values 3 and 30.

| Existing opcode | Concrete expression suffix(es) | Authored fields beyond node identity |
| --- | --- | --- |
| Constant | ScalarConstant, Vector2Constant, Vector3Constant, Vector4Constant | Concrete numeric value; class supplies width |
| Parameter | ScalarParameter, Vector2Parameter, Vector3Parameter, Vector4Parameter | Common parameter metadata and concrete default; scalar-only range |
| TextureParameter | TextureParameter | Parameter metadata, texture value including sampler/fallback, usage |
| TextureSampleParameter2D | TextureSampleParameter2D | Texture parameter fields, UV link and retained UV settings |
| TextureSample2D | TextureSample2D | Texture and UV links, retained UV settings |
| Add, Subtract, Multiply, Divide, Minimum, Maximum | Add, Subtract, Multiply, Divide, Minimum, Maximum | Two numeric inputs and retained defaults, numeric result width |
| Negate, OneMinus, Absolute, Saturate, Normalize, Sine, Cosine | Negate, OneMinus, Absolute, Saturate, Normalize, Sine, Cosine | One numeric input/default and applicable width; Normalize excludes scalar |
| Clamp | Clamp | Value, minimum, maximum inputs/defaults of the same numeric width |
| Lerp | Lerp | Two numeric inputs/defaults and scalar alpha input/default |
| MakeFloat2, MakeFloat3, MakeFloat4 | MakeVector2, MakeVector3, MakeVector4 | Exactly two, three, or four scalar inputs/defaults |
| Swizzle | Swizzle | Numeric source/default and selected component sequence; output width is sequence length |
| Splat2, Splat3, Splat4 | Splat2, Splat3, Splat4 | Scalar input/default; class supplies result width |
| TruncateToFloat, TruncateToFloat2, TruncateToFloat3 | TruncateToScalar, TruncateToVector2, TruncateToVector3 | Numeric source/default; class supplies result width |
| DecodeNormalRG | DecodeNormalRG | Float2 input/default; Float3 result |
| BlendNormalsRNM | BlendNormalsRNM | Two Float3 inputs/defaults |
| UVChannel | UVChannel | Scalar channel input/default; Float2 result |
| TextureCoordinates | TextureCoordinates | Channel, scale, offset, rotation inputs and retained defaults |
| MakeSurface | MakeSurface | Eight attribute inputs with applicable scalar/Float3 defaults |
| GetSurfaceAttributes | GetSurfaceAttributes | Surface input and selected output attribute mask |
| SetSurfaceAttributes | SetSurfaceAttributes | Base Surface input and attribute-keyed override links |
| FunctionInput | FunctionInput | Stable function port ID; signature remains function-owned |
| FunctionOutput | FunctionOutput | Stable function port ID and source link |
| FunctionCall | FunctionCall | Callee reference, GUID-keyed input/default and output bindings |

| Old field or record | Destination or discard classification |
| --- | --- |
| ParameterValue.ScalarValue / Vector2Value / VectorValue / Vector4Value | `float` / `FVector2` / `FVector3` / `FVector4` active alternative; matching reflected override array |
| ParameterValue.TextureValue / SamplerState / TextureFallback | One `FMaterialTextureValue` alternative and reflected texture override record |
| ParameterDefinition.Id, Name, DisplayName, GroupName, SortOrder, Presentation | Common parameter metadata owned by the parameter expression |
| ParameterDefinition.Type | Derived from the concrete parameter class/default; no independently writable tag |
| ParameterDefinition.Value | Concrete default on the owning typed expression; transient unified definition projection |
| ParameterDefinition.bHasRange, MinimumValue, MaximumValue | Scalar parameter expression only |
| ParameterDefinition.TextureUsage | Texture-owning parameter expression only |
| ParameterOverride.ParameterId / Type / Value | ParameterId plus concrete value in five arrays; transient unified override projection |
| ProgramNode.Id | Expression base node GUID; preserve GUID links and function-output IDs |
| ProgramNode.Opcode / ResultType | Concrete class plus bounded numeric-family width where applicable |
| ProgramNode.Inputs / InputDefaults | Shared `FMaterialExpressionInput` connection members and separate applicable defaults on the selected family; connected defaults remain retained |
| ProgramNode.Literal | Typed constant value; discard on non-constant families |
| ProgramNode.Parameter | Parameter-owner fields only; discard inactive parameter history on other families |
| ProgramNode.SwizzleLength / SwizzleX/Y/Z/W | Swizzle component sequence only; discard inactive components |
| ProgramNode.DisplayName | GUID-keyed presentation; excluded from detached compiler nodes |
| ProgramNode.FunctionPortId | Function terminal expression only |
| ProgramNode.SurfaceAttributeMask | GetSurfaceAttributes selection only |
| ProgramNode.SurfaceAttributes | SetSurfaceAttributes bindings only |
| ProgramNode.UVSettings | Sampling/coordinate families only |
| Program.Outputs | Material-owned terminal links and typed attribute defaults |
| Program.SchemaVersion / FunctionGraph.SchemaVersion | Required owner schema gate, independent of DAST v10 |
| FunctionGraph.Signature | Function-owned port definitions, stable port GUIDs, names and ordering |
| FunctionGraph.Calls / Material.FunctionCalls | Fold each call's Function, Inputs and Outputs into its expression; NodeId becomes expression Id |
| FunctionDefault.None / Numeric / Texture / Surface / Input / UV0 | Preserve the existing bounded signature-default record and validate selected semantics: absent, numeric, sampler/fallback, attribute constants, input GUID, or UV0 builtin |
| FunctionInputBinding.ExpectedType / Default | Retain port compatibility check and connected numeric default; never silently coerce |
| Graph presentation and import provenance | Separate editor-only owner data; no compiler/code identity contribution |

Value API decisions already fixed by the plan are `GetType()`, checked
`GetScalar()` / `GetVector2()` / `GetVector()` / `GetVector4()` / `GetTexture()`,
and the existing `MakeScalar` / `MakeVector2` / `MakeVector` / `MakeVector4` /
`MakeTexture` factories. Texture collection must visit and update the active
reference through `FReferenceCollector`, including reference-rewriting collectors.
Projection pointers/spans expire on owner revision changes. Snapshot lowering
replaces live texture/callee references with captured resource/dependency identities
before worker dispatch; moving a variant into a worker is not sufficient.

Concrete persistence and boundary decisions for implementation:

- Numeric operation inputs use a GUID link and numeric component vector of
  length zero (absent) or one through four; no other lengths are admitted.
  Each concrete operation declares its exact applicable pins, and its signature
  validates the selected input width before lowering. This includes fixed-width
  operations so a retained numeric input has one consistent absence/value
  representation without a separately mutable type tag. There is no
  parameter/texture/surface payload in a numeric input record. Sampling UV
  settings instead use fixed scalar channel/rotation and Vector2 scale/offset
  records with presence flags. This refines the initial fixed-pin representation;
  it is not a size or allocation improvement claim.
- Keep the existing bounded `FMaterialFunctionDefault` signature record for
  this migration. Its None/Numeric/Texture/Surface/Input/UV0 alternatives do not
  include resource object references. Validate its selected semantics using
  the function-owned port type; preserve InputId aliases, UV0, required flags,
  and connected numeric defaults. Broad signature-default compaction is outside
  the stated scope. Call expressions own only callee and typed port bindings;
  they do not contain parameter, arithmetic, or coordinate fields.
- `FMaterialParameterValue::AddReferencedObjects(FReferenceCollector&)` visits
  only the active texture alternative and writes the collector's replacement
  back. Both const and mutable checked accessors return the selected concrete
  value by reference; mutation never changes the alternative implicitly.
  `DMaterialInstance::SetParameterOverride(const FGuid&,
  const FMaterialParameterValue&) -> bool` derives its type from the value.
- The expression base has only node identity and virtual lowering behavior.
  `LowerMaterialExpressions(span<const TObjectPtr<DMaterialExpression>>,
  FMaterialProgram&, vector<FMaterialFunctionCall>&)` runs on the owning thread
  and returns `FMaterialProgramValidationResult`. Its intermediate is local to
  authoring/snapshot capture. Existing `SnapshotMaterialCompilerInput` and
  `BuildFunctionSnapshot` remain the admission boundaries that capture resource
  and callee identities before worker dispatch. The final worker node payloads
  are bounded C++ variants and contain no editor metadata or live object pointers.
- Graph replacement validates candidates before publishing owned children.
  Working documents and Apply duplicate owned hierarchies; deletion/replacement
  detaches the exact retired children into transaction ownership. A projection
  cache may be rebuilt on revision change but cannot be independently edited or
  reflected as an alternate authored graph.

The package rebuild closure contains DefaultMaterial and all seven shipped
standard functions. The only serialized material-to-material inbound edges in the
two mounted inventories are StandardPBR to SampleNormal, and StandardPBR_ORM to
SampleNormal and SampleORM. Neither mounted inventory contains a MaterialInstance;
engine runtime service lookups still require the DefaultMaterial identity.
The six files under `Engine/Tests/Data/Materials/GraphAuthoringV5` remain a separate
fixture disposition item: current test source contains no `GraphAuthoringV5`
consumer. Do not mistake their retired ImportedSurface fixture for a shipped asset
or revive its production initializer.

Fresh package byte baselines (one export per package; section bytes are measured
from the DAST directory, not estimated from reflected record counts):

| Package | Total | Names | Schemas | Values |
| --- | ---: | ---: | ---: | ---: |
| DefaultMaterial | 3,386 | 1,596 | 168 | 874 |
| DecodeImportedNormalRG | 12,712 | 2,940 | 371 | 8,462 |
| ImportedSurfaceValues | 51,236 | 2,936 | 371 | 46,990 |
| SampleNormal | 19,054 | 2,900 | 371 | 14,844 |
| SampleORM | 15,398 | 2,888 | 371 | 11,200 |
| StandardPBR | 77,540 | 3,202 | 419 | 72,938 |
| StandardPBR_ORM | 68,738 | 3,318 | 422 | 64,012 |
| UVTransform | 22,163 | 2,896 | 371 | 17,957 |
| Fresh optional PBRSurfaceMaterial_MR | 56,075 | 4,363 | 415 | 50,362 |

These are pre-expression values. No size or performance improvement is claimed.

## Implementation Stages

### Stage 0: Qualify ownership and capture rebuild baselines

- [x] Remove the unused ImportedSurface asset/initializer and retain an explicit,
  non-overwriting PBRSurfaceMaterial_MR creation entry for optional instance use.

- [x] Inventory every existing opcode, parameter type, and direct-field consumer
  across source and test roots of all projects in `Durin.dworkspace`; record the
  old-field to expression-field mapping, including function defaults and calls.
- [x] Confirm the DefaultMaterial and standard-function dependency
  closure. Capture package identities, inbound references, dependent instance
  parameter IDs/overrides, rendered reference images, and package section sizes.
  Include affected authored/cooked fixtures. Internal node IDs and graph layout
  may be regenerated; preserve identities referenced by dependent assets.
- [x] Qualify a small polymorphic owned-expression graph through Save/Load,
  DuplicateObject, edit-copy/Apply, deletion, Undo/Redo reference retention, and
  Cook stripping. Verify no deleted or editor-only descendant leaks into exports.
- [x] Finalize the concrete expression class list and value/reference-collection
  API signatures; record any demonstrated infrastructure prerequisite here.

Completion: every supported field has a destination or an explicit discard
classification, the rebuild closure is known, and object ownership/copy/filter
behavior has executable evidence. No production rebuild precedes this gate.

### Stage 1: Introduce typed parameter values and instance storage

Depends on Stage 0.

- [x] Implement the transient typed value API and reflected typed override records.
- [x] Split parameter metadata/default ownership and migrate resolution, instance
  editing, render layers/proxies, resource collection, and Cook metadata consumers.
- [x] Remove independent mutable type/value pairs from new mutation APIs; migrate
  editor controls and all workspace consumers of the old direct fields.
- [x] Verify every value alternative, texture sampler/fallback, mismatch rejection,
  duplicate IDs, orphan behavior, inherited values, explicit equal overrides,
  reference retention, and save/load of typed records.

Completion: no new persisted parameter record contains inactive alternatives;
the unified runtime API has one type authority. Tests construct the new schema
directly; no compatibility storage or asset conversion structures are introduced.

### Stage 2: Add typed expressions and detached lowering

Depends on Stage 1 and the ownership gate in Stage 0.

- [x] Implement the mapped expression types and shared material/function collection.
- [x] Make parameter nodes the sole authored definition owners; move callee and
  port bindings into call expressions, and preserve function interface rules.
- [x] Implement graph validation and deterministic detached snapshot lowering.
  Preserve cycle/depth/count limits, source diagnostics, multi-output GUIDs,
  disconnected defaults, and function dependency invalidation.
- [x] Migrate compilation, code identity, derived schema, and Cook consumers.
  Exclude presentation and irrelevant payloads from shader identity; bump only
  affected material, compiler/cache, and cooked-payload schemas.
- [x] Verify node-family semantics and rendered/compiler parity against Stage 0.

Completion: materials and functions compile from typed expressions without a
second writable authored program or call table; workers do not inspect objects.

### Stage 3: Migrate authoring and import workflows

Depends on Stage 2.

- [x] Migrate MaterialGraphDocument commands, Details, pin/catalog code, preview,
  graph replacement, clipboard, transaction reference collectors, and Apply.
- [x] Update standard function recipes, the explicit PBRSurfaceMaterial_MR template, scene import,
  asset creation, and all workspace/test graph construction helpers.
- [x] Verify copy/paste GUID remapping, parameter ownership, function ports,
  delete/restore, class replacement, Undo/Redo, save/reopen, Apply/Discard, and
  reference lifetime after closing editors and running collection.

Completion: supported graph operations run against expressions end to end;
working and source assets never share mutable expression children.

### Stage 4: Rebuild material assets and qualify behavior

Depends on Stage 3.

- [x] Recreate DefaultMaterial and required standard functions
  directly with the new expression APIs and updated recipes. No old-schema load
  is needed to build them; do not create an upgrade or conversion path.
- [x] Recreate affected instances and fixtures if the dependency inventory finds
  them. Preserve referenced package/object identities, built-in parameter IDs,
  and required function-port IDs. Generate fresh internal node IDs/layout where
  appropriate. Validate staged packages before atomic replacement and keep a
  precise changed-asset manifest; leave unrelated assets alone.
- [x] Rebuild affected derived data and cooked output for Sandbox and RoadWeaver.
  Verify graph-stripped runtime defaults, overrides, dependency residency, and
  Game startup/rendering without expression authoring data.
- [x] Compare fresh structural import parents and the retained material set: package section
  bytes, node/object/export counts, save/load allocation/time, and render output.
  Explain object overhead separately from payload reduction. Do not claim size
  or performance wins from record counts alone; investigate regressions before
  accepting this gate.

Completion: fresh readers load all rebuilt assets; semantic and
render evidence passes, and size/performance results are recorded explicitly.
No external-asset compatibility is required by the current user instruction.

### Stage 5: Remove transition code and publish final contracts

Depends on Stage 4.

- [x] Remove old reflected universal values/nodes, authored program/call storage,
  legacy overloads, and redundant canonicalization paths.
  Search all three projects for remaining production consumers and old schemas.
- [x] Reject unsupported material schemas before publishing a partially loaded
  graph; never interpret a missing new collection as a valid empty old asset.
- [x] Update the owning runtime material documentation and
  [Material Graph Operations](../../../Editor/Architecture/MaterialGraphOperations.md)
  to describe implemented ownership, snapshot, transaction, and Cook contracts.
- [x] Complete an `all` build of the workspace, affected native suites, renderer
  qualification, both projects' Cook/Game checks, and documentation validation.
- [x] Record exact receipts and close the plan only after all gates pass.

Completion: one authored model and one typed value contract remain; no permanent
legacy material reader or alternate full-field save path remains.

## Validation And Scope Boundaries

Follow [build guidance](../../../Agents/BuildAndRun.md) and
[test guidance](../../../Agents/Testing.md) when implementing. Relevant suites include
material schema/instances, graph operations, function expansion, compile lifecycle,
render proxies, scene import, object/package serialization, clipboard and
transactions, plus Vulkan material/import coverage. Recompute exact registered
targets from the changed ownership rather than copying historical test counts.

Use [Serialization](../../../Runtime/Core/Serialization.md),
[Asset Packages](../../../Runtime/Assets/AssetPackages.md), and
[Transaction Records](../../../Editor/Architecture/TransactionRecords.md) as current
infrastructure contracts. This plan replaces the value-node authoring boundary
in Material Graph Operations as implementation lands, while preserving the
graph-owned parameter decision in
[Material Graph Owned Parameters](MaterialGraphOwnedParameters.md).

A universal reflected variant facility, arbitrary polymorphic containers,
generic array delta serialization, shader optimization, and unrelated material
features are outside this plan. Adjacent `FMaterialFunctionDefault` compaction
is limited to changes required for typed expression/signature correctness; a
broader redesign needs measured justification and an explicit plan update.
