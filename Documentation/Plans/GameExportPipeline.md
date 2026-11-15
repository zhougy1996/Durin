# Game Export Pipeline Plan

Summary: Establish explicit Game paths, delivery layout and deployment declarations through an engine-owned export service and worker, shared by Editor, DevTool and CI clients.

Last reviewed: 2026-11-15

Status: Active
Completed:

## Current Status

Stages 0-4 are complete for the bounded Win64 source-backend gates described below. Exports
use the stable Binaries/Config/Content tree, transactional reduced Cook snapshot,
external evidence and per-user storage. Native staging now selects completed
receipt products through declared linked/dynamic/module/file closure; directory
scans and import-driven discovery no longer authorize files.
See [implemented deployment](../Runtime/Application/GameDeployment.md) and
[versioned contracts](../../Engine/Source/Developer/GameExport/Contracts/README.md).
Stage 6 automated Win64 qualification is recorded below. The plan remains Active:
manual Editor interaction and the clean Windows prerequisite lane are still open.

Stage 6 architecture convergence decision (2026-11-15): CMake generates the
source backend Game product sets from resolved declared presets, including actual
binary directories and platform/configuration. DevTool Cook/Stage consume that
metadata; Export uses its published component copy. Native Export resolves one
plain target value and passes it to Stage without rereading backend/project target
selection. Component membership follows existing link/dynamic edges, explicit
asset-tool provider dependencies, project Game roots and Runtime plugin providers;
remove the all-shared-library inventory. Keep native Cook, standalone worker,
receipt/closure/admission checks, fresh-directory publication and owned children.
No Editor UI interaction; existing manual/clean-Windows gates remain open.

Convergence validation, tested `14ef459b5` plus these changes on the same Win64
MSVC developer host (raw evidence: `Build/GameExport/TargetConvergence/`):

- Four incremental `all` builds pass for Debug/Release Editor/Game across
  Engine/Sandbox/RoadWeaver. Final path canonicalization changes the existing
  CookCompatibility source fingerprint: refresh both host and Game builds before
  delivery; the first mismatched Cook correctly rejects without a success marker.
  `final-builds.json`, `bound-builds.json`, `bound-game-builds.json` retain commands
  and build-log references. All six final Game receipts exclude export/Editor modules.
- GameExportContractTests: 31 pass, including platform/preset disambiguation and
  retained failure/ownership/closure checks. Final log:
  `Build/.agent-state/logs/20261115-074137-734944-43792-GameExportContractTests.log`.
  Client/component/schema Python: 129 pass (`python.xml`); DHT build/plugin:
  44 pass, one directory-symlink case skipped for Windows privilege (`dht.xml`).
- Fresh Sandbox Debug/Release exports verify startup and archive. DevTool Cook
  selects the Release receipt from a Debug host; Stage consumes the same generated
  backend with an explicit Game preset and its Game runs 30 ticks. Direct,
  DevTool, prebuilt and native-client Release exports have identical Runtime.json
  and all 42 archive payload files (`delivery.py`, `delivery-receipt.json`).
- Tool inventory shrinks from 61 to 48 Debug files, removing 13 unrelated DLLs;
  final Release has 47 files. Actual link/dynamic/provider dependencies remain.
  Raw and published Game product sets match. `component-diff.json` compares tools
  only, excluding bundle-added receipt/Cook/products/catalog metadata.
- The dependent/content-only/no-plugin fixture Cook/Stage/Game lane passes with
  generated metadata, including Editor-typed content rejection and retained Cook
  (`fixture-receipt.json`). Its later-input difference is only the canonical path
  helper, covered by the final native suite and real Sandbox delivery lanes.
- Narrowed prebuilt direct/native-client export also passes with workspace,
  source/assets/shaders/plugins/Saved caches/Tools/.agents/.venv and fixture source
  unavailable on System32-only PATH. All paths/workspace bytes are restored
  (`offline.ps1`, `offline-receipt.json`). `final-receipt.json` records 69 schema
  documents, 255 protocol events and component/file hashes.
- Runtime/Editor/project runtime source is unchanged from `14ef459b5`; reuse its
  owner/platform/Cook/plugin suites and relocation/read-only/import qualification.
  Current builds and fresh exact-receipt deliveries cover changed build/tool inputs.
  Manual Editor UI and clean-Windows prerequisite gates remain open; plan stays Active.

Stage 6 Win64 qualification (2026-11-15), tested `e34c06f8e` plus this task's
receipt projection, worker-entry and existing plugin-fixture corrections:

- Preserve CookCompatibility in the selected receipt. Final selected/availability
  documents now both satisfy target-receipt-v1; the existing native export case
  checks that the projection retains the original compatibility value. This fixes
  a schema inconsistency without changing closure selection or weakening checks.
- Initialize the worker's existing Core path context from explicit EngineRoot
  before Cook-manifest/package-path decoding. The stronger isolation lane exposed
  a default-mount source-directory probe that merely hiding Engine/Source did not
  exercise. Installed direct/native-client export now also passes with authored
  Engine Content/Shaders and workspace/development paths unavailable.
- Migrate the existing plugin fixture's structured error to ToString and its
  native Shader library lookup to RuntimePathBinding("Cook"). Both Editor/Game
  variants compile. No production runtime API or new export abstraction is added.
- Final incremental all gates pass for Win64 Debug/Release Editor and Game,
  covering Engine/Sandbox/RoadWeaver. After the worker-entry-only fix, both Editor
  all gates pass again; Game inputs/CookCompatibility remain exactly unchanged,
  so the final Game all results and current Cook/deliveries are reused. Commands,
  exits and detailed build-log references are in
  `Build/GameExport/Stage6/final-builds.json`, `final-all-*.log`,
  `bound-builds.json` and `bound-all-*.log` in that directory.
- All six final Game availability receipts pass schema checks and exclude
  GameExport/MainFrame/DurinExportTool, Editor hosts and disposable fixture modules.
  Debug/Release ExportComponent inventories are hashed/validated and exclude the
  removed workspace fixture. The original three-project Durin.dworkspace is restored.
- Fresh owner coverage passes: five launch contract targets, AssetMetadataQueryTests,
  four Shader contract targets, RenderShaderCookIntegrationTests and
  RenderShaderBuilderTests (12 targets). Exact selections/log references:
  `Build/GameExport/Stage6/native-test-receipt.json`. All 30 GameExportContractTests
  pass after the receipt fix; log:
  `Build/.agent-state/logs/20261115-042216-672021-22840-GameExportContractTests.log`.
  Reuse unchanged PlatformProcessContractTests (10), CookFunctionalTests (45),
  PluginResolutionTests (19) and the 125 Python contract/schema/client cases from
  Stage 5. Their owning production source/tests/schema and toolchain are unchanged;
  no broad native-suite rerun substitutes for the real-process lanes.
- The registered temporary PluginFixture qualifies dependent Runtime+Editor,
  content-only and no-plugin selections through current Cook/Stage and 30-tick
  Game processes. Only selected Runtime DLLs ship; Editor-typed required content
  rejects Cook without replacing its committed manifest. The dependent process
  loads its typed project/plugin reference chain and native cooked Shader; its
  trace orders Providers before EngineReady and reverses shutdown. Original
  workspace bytes, qualified membership, source hashes, commands and results:
  `Build/GameExport/Stage6/fixture-receipt.json` and `fixture.py`.
- Final Sandbox Debug/Release exports retain PlayerPawnDefaults and GrayboxPawn,
  omit unreferenced NewLevel, and bind the exact completed target receipts without
  extra pawn roots. Source Editor (10 ticks), Browser (3) and source Game (30)
  start/exit automatically through DevTool; there is no Editor UI interaction.
  `Build/GameExport/Stage6/Final/sandbox-receipt.json` retains the commands/logs.
- Current direct, DevTool, prebuilt and native shared-client Release requests
  have identical Runtime.json and all 42 archive payload files. The newly bundled
  prebuilt component contains no native C++/headers in its catalog metadata;
  it remains Build=reuse/Cook=reuse, not offline Cook or full SDK qualification.
  Native cancellation returns 130 after child retirement and leaves no completion
  marker. Unchanged disconnect/concurrent-writer/failure coverage from Stage 5
  remains applicable. See `Final/clients-receipt.json` below.
- With workspace/source/Content/Shaders/plugins/Saved caches/Tools/.agents and
  the disposable fixture source moved out, SDK direct and native-client export
  and all three fixture Games pass on System32-only PATH. Sandbox Debug/Release
  run 120 ticks from unrelated cwd after Chinese/space-path relocation and rename,
  preserve user overrides, and run under a write-denying ACL. A write probe is
  denied; every installation file hash remains unchanged and user data is external.
  All moves and ACLs are restored. The checker uses the repo Python environment;
  the unchanged Stage 5 component lane additionally proves .venv-unavailable use.
- Delivery checks set DURIN_VULKAN_VALIDATION=off to exclude the optional installed
  Debug SDK layer. Actual loaded modules come only from the delivery and Windows
  directories; no compiler/Editor module loads. Release PE ordinary/delay imports
  and loaded modules contain no Debug CRT. This developer host is Windows 11
  10.0.26200, MSVC ABI 19.44.35228.0-8, RTX 3090, Vulkan 1.4.351; it is not a clean VM.
  Missing frozen config/snapshot/Shader/business DLL returns 2. Missing Core is
  rejected by the Windows loader before engine entry (0xC0000135).
- Final exact receipts, 62 schema-validated documents and 373 emitted events,
  inventory/hash/module/import records and reproduction scripts are under
  `Build/GameExport/Stage6/Final/`: `final-receipt.json`, `delivery-receipt.json`,
  `clients-receipt.json`, `offline-restored.json`, `sandbox.py`, `clients.py`,
  `offline.ps1`, `delivery.py` and `final_check.py`. The current SDK is InstalledFinal.
  Earlier interrupted qualification attempts remain diagnostic artifacts.
- Publish [Exporting a Game](../Editor/Guides/ExportingGames.md) and update the
  runtime/asset/plugin cross-references. Changed-document/all-plan/diff validation
  passes. Manual save prompts/responsiveness/cancel interaction stays pending at
  the user's request. No independent clean Windows lane is configured; positive
  prerequisite installation and actual absent-prerequisite behavior there remain
  mandatory open gates. Keep the overall plan Active and unarchived.

Stage 5 convergence decision (2026-11-15): all Stage/export publication requires
fresh directories and final completion markers; remove low-level replacement,
backup and rollback. Keep the worker wire protocol at the external boundary and
call Stage directly with parsed request data and ordinary progress callbacks.
All preview entry points check configuration and show planned phases only; they
do not admit Cook/native products or claim a product fingerprint. Validate each
canonical tool component once per process and retain its immutable metadata;
changing a tool installation requires restarting its client/worker processes.
Source/prebuilt capabilities and execution-time receipt/closure/hash/admission,
input overlap, exclusive directory ownership and child retirement checks remain.
Editor UI interaction remains deferred at the user's request.

Stage 5 convergence validation against `a236411c7` plus these changes,
2026-11-15, Win64/MSVC/Vulkan:

- [x] Stage/export share exclusive fresh directory creation and final completion
  marking. Remove Stage candidate/backup/restore/writer-lock generations; retain
  direct external evidence, copy hashes and delivery admission checks.
- [x] One worker boundary validates requests/emits events. Internal Stage takes
  parsed request data and ordinary progress callbacks; no internal request/event
  serialization or second protocol handler remains.
- [x] Every preview entry point checks configuration and reports phases only,
  with null Output/PlanFingerprint. Update event schema to distinguish preview
  from published success; published success still requires a product fingerprint.
- [x] Cache successful component metadata/inventory validation by canonical path
  per process; failures are not cached. Preserve mandatory prebuilt component
  validation and source/prebuilt execution-time native/Cook checks.
- Affected-test analysis resolves GameExportContractTests. All 30 native cases
  pass on Win64-Debug-DurinEditor, including marker failure, retained cancelled
  partial files, existing-directory rejection, corrupt-Cook preview versus actual
  admission and component caching/failure retry. Log:
  `Build/.agent-state/logs/20261115-031917-220555-41956-GameExportContractTests.log`.
  ExportComponent and ExportWorkerClientFixture builds pass. No public API, Core
  process or Cook implementation changed; bounded coverage is sufficient for
  this convergence, without repeating Stage 6's workspace all-build gates.
- GameExport contract/schema/component and DevTool export/stage/Cook Python tests
  pass (125), including four preview/publication fingerprint-schema cases. Report:
  `Build/GameExport/Convergence/python.xml`.
- Real source Release auto Build/Cook/export with verification/archive, DevTool
  reuse export and low-level Stage pass. Existing Stage destinations return 2;
  all three preview entry points allocate nothing and report no product identity.
  Production code is unchanged after these runs; the subsequent event-schema
  update and four Python cases are validated against the recorded actual events.
- Prebuilt direct and native shared-client exports with verification/archive pass
  while Tools, .agents, .venv and Engine/Source are moved out of the workspace
  and PATH contains only System32. All paths are restored. No Editor UI was used.
  This preserves prebuilt reuse capability; it does not qualify offline Cook or
  a complete installed SDK. The native fixture exercises the Editor's shared
  process client, not its manual save/progress interaction gate.
- Six successful jobs share Runtime.json SHA256
  `9c52da6923866f095ca1a34ab6ff03088f05a403bf59af13d3f1a2001ad849cc`;
  all 42 ZIP payload files match. Each marker binds its own archive bytes.
  Client cancellation and stdin disconnect return 130 with child-retirement
  acknowledgement and no completion marker; concurrent writers return [0, 2].
  Actual runtime/marker/component documents, inventories and 303 emitted events
  pass schema/hash checks. Reproduction, logs and final receipt:
  `Build/GameExport/Convergence-qualify.py`,
  `Build/GameExport/Convergence/isolate.ps1`,
  `Build/GameExport/Convergence/check.py` and
  `Build/GameExport/Convergence/final-receipt.json`.
- Changed-document, all-plan and diff checks pass. Editor manual interaction
  remains deferred; Stage 6/clean-machine/other-platform gates remain open.

Stage 5 implementation (2026-11-15): Win64 source and installed prebuilt-reuse
export orchestration, completion markers, the shared process client and minimal
Editor action are implemented. All five implementation tasks and three automated
acceptance gates pass. The Editor interaction/save-prompt/responsiveness acceptance
remains open: window-access approval timed out and the user explicitly requested
no Editor UI operation. Native Editor-client API checks do not substitute for that
manual UI gate. Stage 6 and clean-machine/other-platform qualification remain open.

The export operation uses a fresh invocation/Delivery directory and final marker;
low-level Stage now shares the fresh-directory/final-marker rule. Source
builds use engine-generated CMake product metadata and the existing incremental
GameReceipt/Cook owners. Installed components declare compatible native/Cook reuse,
not compilation or new offline Cook. No cross-phase rollback or resume is added.

Stage 5 validation against `76bc52012` plus these changes, Windows/MSVC/Vulkan:

- Incremental `all` builds pass for Win64-Debug-DurinEditor and Win64 Debug/Release
  DurinGame across Engine, Sandbox and RoadWeaver. The ExportComponent and native
  ExportWorkerClientFixture targets build. Game receipts exclude GameExport,
  MainFrame and DurinExportTool from the runtime closure. Final relevant production,
  fixture and tool inputs remain unchanged after the recorded passing runs.
- Native tests pass: PlatformProcessContractTests 10, GameExportContractTests 27,
  CookFunctionalTests 45 and PluginResolutionTests 19 (101 total). Process coverage
  includes simultaneous large input/output, callback failure, cancellation and
  descendant retirement. Export tests cover preview, explicit receipt matching,
  fresh destinations, Build/Cook/Stage/verify/archive failures and marker-write
  failure, with no false completed export or cross-phase recovery.
  Reports: `Build/NativeTestResults/Win64-Debug-DurinEditor/`.
- GameExport schema/receipt/component and DevTool export/stage/Cook Python tests
  pass (121); report: `Build/GameExport/Stage5/python.xml`. Twelve actual emitted
  component/backend/runtime/completion documents and recorded protocol events pass
  offline JSON Schema validation (`schema-receipt.json` in the same directory).
- Direct engine CLI exports Sandbox Release with no target/preset/root knowledge,
  running incremental Build, Cook, Stage, 30-tick verification and archive. DevTool
  produces the same delivery manifest and archive entry contents. Reuse-only preview
  creates no output; an existing destination is rejected. PlayerPawnDefaults and
  GrayboxPawn are present, while unreferenced NewLevel is absent. Reproduction and
  raw commands/events: `Build/GameExport/Stage5/qualify.py` and `qualification/`.
- An installed component containing 148 declared files exports successfully with
  Tools, .agents, .venv and Engine/Source temporarily unavailable and PATH restricted
  to System32. Both direct worker and the exact process API used by Editor succeed,
  including startup verification/archive. The SDK contains catalog descriptors,
  not native C++/headers; all source directories are restored. Raw receipts/logs:
  `Build/GameExport/Stage5/isolation-paths.json`, `isolation-receipt.json`,
  `isolated-direct.log`, `isolated-editor.log`. The standalone client fixture invokes
  RunGameExportWorker, not Editor UI automation.
- All four direct/DevTool/isolated-client delivery manifests hash to
  `5688cd103a4aeb971a9ef382b77f3198150c2dcd87b560589824e25efde60a96`;
  42 archive payload files also match. Each marker binds its own archive bytes;
  ZIP timestamps can vary between invocations. `shared-content-receipt.json`
  records these comparisons. Real client cancellation and worker stdin disconnect
  return 130 only after owned children retire and leave no completion marker;
  `cancellation-receipt.json` and corresponding logs retain the evidence. Two
  concurrent workers targeting one invocation produce exactly one success and one
  existing-writer rejection (`concurrent-receipt.json`), with no competing marker.
- The Editor reaches Ready(project) with the export action compiled. Actual window
  operations were not performed after the user's explicit deferral. Source startup
  log: `Build/GameExport/Stage5/editor.log`; the test-launched Editor was stopped.
- Changed-document/all-plan and diff validation pass. No full SDK distribution,
  installed offline-Cook, macOS controlled-process support or clean-machine
  CRT/Vulkan redistribution qualification is inferred from these bounded gates.

Stage 5 scope decision (2026-11-15, documentation only): each invocation uses a
fresh export directory and publishes ExportComplete.json only after all requested
phases pass. Existing successful directories are never overwritten. Build/Cook
caches retain their existing ownership; no cross-phase transaction, rollback,
backup-directory recovery or interrupted-job resume is required. Validation and
owned-child retirement remain mandatory. The selected contracts and acceptance
matrix below supersede the earlier whole-pipeline replacement requirement.
Validation against `be5231053` plus this documentation-only change: changed-document,
all-plan and diff checks pass. No source, schema or fixture changes were made; native
builds and runtime tests are not required for this scope.
At that documentation-only commit, implementation and all Stage 5 checkboxes
remained open. Current low-level Stage
replacement and Cook transactions are unchanged by this documentation update.


Stage 4 refinement (2026-11-15): replace the Sandbox-specific
ModifyCook pawn root with one authored player-pawn defaults asset. The existing
Game settings projection gains optional Game.DefaultData, an exact top-level
primary-data-asset reference. Cook validates the configured asset and uses its
package as a required root; ordinary reflected references retain its mesh closure.
Sandbox PlayerPawn reads this same setting and typed defaults asset at BeginPlay.
The persisted Mesh reference is the sole mesh identity; no CDO scan, new reference
registry or custom AssetManager class is introduced. Existing target binding,
CookManifest ownership, retention and publication rules remain unchanged.

Refinement validation against `8ccadd2e3` plus these changes on Win64 MSVC/Vulkan:

- `build all` passes for Win64-Debug-DurinEditor, Win64-Debug-DurinGame and
  Win64-Release-DurinGame, covering the shared Game-settings structure and native
  reflection consumers in Engine, Sandbox and RoadWeaver. Existing Stage 4 target
  compatibility gates remain intact. Final production inputs are unchanged after
  these builds and qualification; later edits only update documentation/evidence.
- Native tests pass: CookFunctionalTests 45, AssetReferenceStoreTests 15,
  SandboxGameplayTests 13, GameExportContractTests 20 (93 total). New Cook coverage
  changes the defaults asset's hard reference and observes the selected package
  change on the next Cook without another root. Missing asset/object, wrong type,
  subobject/invalid paths and a NeverCook dependency preserve the committed output.
  Settings coverage verifies optional/scalar admission and preservation across
  default-Level updates. Existing role/input/movement and deployment checks pass.
  Reports: `Build/NativeTestResults/Win64-Debug-DurinEditor/`.
- GameExport contract/receipt and DevTool Cook Python tests pass (107), with report
  `Build/GameExport/PawnDefaults/python.xml`. Six actual emitted Debug/Release
  runtime, Cook and receipt documents pass offline JSON Schema validation.
- Real Debug-host Cook -> Debug/Release Game stage and 30-tick startup pass without
  an explicit root or missing-pawn diagnostic. Both include PlayerPawnDefaults,
  GrayboxPawn and ordinary dependencies; unreferenced NewLevel remains absent.
  Seven target incompatibilities, a NeverCook rule on the defaults asset, and
  missing/wrong-type/missing-object Game.DefaultData settings fail and retain every
  committed Cook file hash. Diagnostic Logs/DDC are excluded from that comparison;
  temporary settings are restored. Reproduction and per-process receipts/logs:
  `Build/GameExport/PawnDefaults/qualify.py` and `qualification/receipt.json` under
  that same evidence directory. The committed asset is saved through the normal
  native package writer; the temporary authoring fixture is not production code.
- Changed-document, all-plan and diff validation pass. Stage 5/6, installed-backend,
  macOS and clean-machine redistribution gates remain open.

Stage 4 (2026-11-14): Sandbox selects a native AssetManager policy through the
existing project settings and ModifyCook hook; it declares the pawn's explicit-load
package without another root/configuration registry. Tool-hosted Cook resolves an
explicit completed Game receipt (configuration may differ from the host), copies its
identity into FCookRequest, and retains host identity only as external provenance.
A configuration-independent build-generated fingerprint covers runtime/provider
sources and DHT/schema inputs. Host and target must match this conservative contract,
platform/architecture, engine version and compiler ABI before discovery. The target
receipt digest and Game build identity bind both transactional Cook snapshots.
No Game DLL is loaded into the Cook host; provider lifetime/publication stay unchanged.
The conservative source contract does not promise arbitrary mixed-binary ABI
compatibility or replace provider/DDC versioning; see [Cook ownership](../Runtime/Assets/Build/Cooking.md#coordinator-and-input-ownership).

Stage 4 validation on Windows/MSVC/Vulkan, tested against `1f7ed41c2` plus these
changes (final runtime inputs unchanged after qualification):

- `build all` passes for Win64-Debug-DurinEditor, Win64-Debug-DurinGame and
  Win64-Release-DurinGame. This covers shared Cook/Projects consumers across Engine,
  Sandbox and RoadWeaver; Release Game is the explicit cross-configuration gate.
- Native tests pass: CookFunctionalTests 44, GameExportContractTests 20,
  PluginResolutionTests 19, PrimaryAssetTests 29, AssetCookTests 38 and
  TargetPlatformTests 2 (152 total). Existing retained references, bundles,
  AlwaysCook, NeverCook, EditorOnly and blocked-plugin checks remain selected.
  Receipt mutation fixtures refresh Cook bindings to keep their original semantic
  checks reachable; a separate exact-receipt drift test requires recooking and
  preserves the previous deployment. Reports: `Build/NativeTestResults/Win64-Debug-DurinEditor/`.
- GameExport contract/receipt and DevTool Cook Python tests pass (107).
  Report: `Build/GameExport/Stage4/python.xml`. Six actual Debug/Release runtime,
  Cook and availability-receipt documents also pass offline JSON Schema validation.
- `cook --preset Win64-Debug-DurinEditor --project Sandbox/Sandbox.dproject
  --configuration Debug|Release --output-dir Build/Exports/Stage4/Sandbox<configuration>Cook`
  uses no explicit root. Both include GrayboxPawn and its dependency closure;
  unreferenced `/Game/Levels/NewLevel` remains absent. Host provenance remains
  Debug/DurinEditor; target snapshots and exact receipt digests bind the selected
  Debug/Release Game. Both `stage` outputs run 30 ticks from an unrelated working
  directory with isolated user-data roots and no missing-pawn diagnostic.
- Seven incompatible target mutations (compatibility, ABI, project, configuration,
  architecture, variant and platform) fail before publication. A valid project
  NeverCook rule on the required default Level fails with the expected NeverCook
  diagnostic. Every committed output file hash remains unchanged (Logs/DDC are
  writable diagnostics/cache, excluded from the committed-generation comparison).
  Temporary project settings are restored. Reproduction and per-process evidence:
  `Build/GameExport/Stage4/qualify.py`, `qualification/receipt.json` and logs beneath
  `Build/GameExport/Stage4/`.
- With Water enabled through a temporary startup override, Debug-host -> Release
  Cook/stage and a 30-tick Game run also pass. GrayboxPawn is retained without an
  explicit root; no Water content directory is retained while native Water and its
  Shader library load normally. The startup override is restored. Reproduction:
  `Build/GameExport/Stage4/water.py`; process receipts/logs: `qualification/water.json`
  and `Water-*.log` under the same Stage4 evidence directory.
- Documentation validation and diff checks pass. Installed/backend orchestration,
  macOS staging, clean-machine CRT/Vulkan redistribution and remaining final
  qualification stay open; local Release startup does not establish those gates.

Stage 3 architecture convergence (2026-11-14): the completed target receipt is the
single export native-product authority. It now retains catalog BuildInputs plus
plugin Binary/Phase bindings, replacing export's secondary PluginBuild admission.
Source Editor/plugin admission retains its existing inventory. Availability is
parsed once into concrete values; receipt validation, pure closure selection,
selected-file/PE validation and publication have separate responsibilities. No
backend interfaces, factories or general graph framework are introduced.

- [x] Unify export plugin compatibility and native products in the target receipt.
- [x] Decode availability once; retain selected document and original evidence bytes.
- [x] Keep graph selection free of I/O and fix delivery paths/categories before publication.

Convergence validation on 2026-11-14 against `c01aee23d` plus these changes:

- `test affected --explain` selects GameExportContractTests; the final affected
  selection passes all 19 cases. Existing publication/cancellation/PE/target/hash
  checks remain covered. Added cases preserve selected plugin descriptor/phase/main
  binary checks without a legacy inventory, and reject native data overwriting
  generated configuration or Cook destinations before writing the candidate.
- DurinExportTool builds in Win64-Debug-DurinEditor. The Win64-Debug-DurinGame all
  build passes and regenerates enriched receipts for Engine, Sandbox and RoadWeaver.
  No shared runtime API changes; Editor all/native-wide tests are not repeated for
  this private service/backend refactor. Native XML and command logs remain under
  `Build/NativeTestResults/Win64-Debug-DurinEditor/` and `Build/.agent-state/logs/`.
- 92 focused Python/schema/receipt/client cases pass; receipt:
  `Build/GameExport/Convergence/python.xml`. Receipt publication retains logical
  build inputs and explicit plugin binding; the client no longer sends PluginInventory.
- Real Sandbox and Water Cook/stage/run checks pass using enriched receipts. During
  Water Stage the entire legacy Game `PluginBuild/Sandbox/Debug` directory (inventory
  and copied DLLs) is temporarily renamed and restored in finally. Original compiled
  plugin products remain in their declared source locations. Water is staged from
  those receipt products and loads in a fresh 30-tick Game process. Inventory and
  availability receipt bytes/timestamps are unchanged by selection-only operations.
  Missing/changed native artifacts and stale native inputs preserve the previous
  public manifest. Emitted selected receipts and runtime manifests pass schemas;
  selected receipt bytes remain canonical and their digest is bound by Runtime.json.
  Reproduction and raw evidence: `Build/GameExport/Convergence/closure.py` and
  `Build/GameExport/Convergence/closure/`.
- The final destination-plan implementation runs from a renamed Chinese path with
  spaces and from a write-denied installation, preserving installation file hashes.
  Missing config/snapshot/Shader inputs still exit 2. Reproduction and receipts:
  `Build/GameExport/Convergence/qualify.py` and
  `Build/GameExport/Convergence/qualification/`. The later destination-collision
  guard changes only invalid plans; passing package bytes/bindings are unchanged.

Changed-document validation (four documents), all-plan validation (11 active,
2 completed, 457 archived) and `git diff --check` pass. Later edits are documentation
only and do not change native artifacts, receipt inputs or runtime bindings.
Stages 4-6, Release/clean-machine and installed/prebuilt qualification remain open.

Stage 3 refinements (2026-11-14): DynamicDependencies declares All/Win64/MacOS
module edges separately from linker dependencies. Engine declares Renderer and RHI
declares the platform backend; ordinary Engine roots no longer manually enumerate
those products. Aggregate GameReceipt targets build the dynamic closure, including
valid dynamic back-links through Engine, without cyclic per-library build edges.
RuntimeFiles and package CMake properties declare data/licenses and redistribution
provenance. Sandbox's existing BaseModules root participates in the same receipt
closure as engine and plugin modules.

The source backend uses TargetReceipt, ArtifactsRoot and
DependenciesRoot, without GameRuntime or output-name fallback. Completed availability
receipts retain build-all plugin semantics; the worker projects selected external
TargetReceipt.json and binds its canonical bytes through NativeReceiptSha256.
AvailabilityReceipt.json preserves the original. Inputs bind recorded source/build
files, generated Build.inputs and CMakeCache configuration; startup-only overrides
are excluded. Dependency roots explicitly admit the checkout's shared External
junction. Host-only SDK/import inputs may use absolute paths in external evidence;
Host/Workspace cannot authorize deployed products. Output overlap checks cover the explicit dependency root as well as source and
artifact roots. This is a source backend, not an installed/prebuilt implementation.

The implementation reuses CMake's concrete direct-link resolver. Core caches native
library paths and hashes during bootstrap; explicit module loads check the cache.
This keeps file-category admission separate from generic Files and avoids repeated
manifest parsing. Native receipt projection and PE validation stay in GameExport;
DevTool supplies metadata and dispatches requests.

Stage 3 validation on 2026-11-14, tested against `231ef59ff` plus these changes:

- Win64-Debug-DurinEditor and Win64-Debug-DurinGame incremental all builds pass,
  covering Engine, Sandbox and RoadWeaver after shared DHT/CMake graph and Core
  context changes. Bounded final worker/test builds cover the subsequent private
  receipt serialization changes. Shared context consumers were searched across all
  Durin.dworkspace source/test roots. Native execution uses the established
  non-sandbox MSVC lane. Broad builds cover the changed graph/header boundary;
  unrelated native runtime tests are not part of this stage's behavior selection.
- GameExportContractTests (18), PluginResolutionTests (19), and the five-target
  launch contract selector pass. Cases cover dynamic modules and non-DLL data,
  unrelated/stale DLL exclusion, category-specific imports, missing products,
  inactive/Editor edges, differing-byte loader-name collisions, wrong ABI and PE
  machine, cached verified native paths, cancellation and publication rollback.
  The shared native/Python plugin corpus includes a dynamic edge with a valid
  linked back-reference. XML/logs remain under
  `Build/NativeTestResults/Win64-Debug-DurinEditor/` and `Build/.agent-state/logs/`.
- Python/schema/DHT/client tests: 255 pass, 2 skip because Windows symlink creation
  lacks privilege. Generated CMake path evaluation runs with the prepared CMake
  directory on command-local PATH. Receipt publication tests prove deterministic
  bytes, explicit product membership and removal of a success marker when a
  required product is missing. Receipt: `Build/GameExport/Stage3/python.xml`.
- The real completed Sandbox availability receipt has 28 module records and 21
  products. Independent Cook plus receipt-driven Stage succeeds at
  `Build/Exports/Stage3/SandboxCook` and `SandboxGame`. Explicit pawn Cook root is
  retained until Stage 4. Base selection keeps 27 modules/20 products; enabling
  Water keeps 28/21 and loads Water Providers in a bounded 30-tick Game run.
  Availability receipt and PluginBuild bytes and timestamps remain unchanged
  throughout selection-only Cook/stage operations. Tracy's declared license is
  deployed; the captured MSVC CRT minimum is 14.44.35207.0.
- Planting an unrelated DurinGame-prefixed DLL in the runtime directory never
  deploys it. Removing or changing Sandbox's required DLL fails before publication;
  a changed recorded native input is explicitly diagnosed as stale. All failures
  preserve the previous public Runtime.json hash. Emitted availability/selected
  receipts and Runtime.json pass schemas; selected receipt bytes are canonical
  and their digest matches NativeReceiptSha256 and Cook SelectionFingerprint.
  Reproduction, processes and receipts: `Build/GameExport/Stage3/closure.py` and
  `Build/GameExport/Stage3/closure/`. Direct source metadata example:
  `Build/GameExport/Stage3/source-backend.json`.
- The final receipt-driven package runs from an unrelated cwd after relocation to
  a Chinese path containing spaces, after rename, and with an inherited write-deny
  ACL. Three 30-tick runs load the Level, mesh and Shader library; installation
  hashes remain unchanged. Missing config/snapshot/Shader inputs exit 2. Source
  Editor (10 ticks), Browser (3), and explicit source Game (30) exit normally.
  Reproduction/logs: `Build/GameExport/Stage3/qualify.py`,
  `Build/GameExport/Stage3/qualification/`, `Build/GameExport/Stage3/hosts.py` and
  `Build/GameExport/Stage3/hosts/`. Closure checks were repeated after the final
  publisher product-root guard. The reused relocation/storage/host receipts have
  identical native bytes and runtime bindings; only the external receipt digest
  changes with the publisher's recorded source hash.
- Changed-document validation (five documents), all-plan validation (11 active,
  2 completed, 457 archived) and `git diff --check` pass. Documentation-only edits
  do not change the validated runtime or native inputs.

Release/clean-machine prerequisites, installed/prebuilt execution, cross-configuration
Cook/provider compatibility and macOS export remain open. Historical Stage 1/2
migration receipt/layout bindings below are superseded by Stage 3.

Stage 2 refinements (2026-11-14): both full ExportEvidence.json and minimal
RuntimeCook.json use the existing PluginSnapshot auxiliary kind and Cook transaction,
without another asset catalog. Delivery projects out only the evidence record from
CookManifest; original runtime snapshot bytes and every asset record are retained.
The reduced schema uses current EngineVersion/target/ABI and minimal selection;
completed native build identity and provider compatibility remain Stages 3/4.
NativeReceiptSha256 still binds external NativeInputs.txt migration evidence.
Full source capture, original CMNF, backend metadata and source hashes are stored
at `<output>.evidence/<generation>/`, outside delivery. Evidence publication may
leave an orphan generation on failure but preserves the previous runnable output.

Stable identity is authored in `Configs/Project.yaml` as `Export.ApplicationId`,
with `org.durin.<ProjectName>` as the absent-setting default. Export does not mutate
source settings on first publication. Sandbox explicitly persists org.durin.Sandbox;
project renames retain that setting. LocalAppData is the only staged storage policy;
the old ExecutableSaved policy requires restaging. Empty user overrides, explicit
legacy-user migration and isolated verification roots avoid inherited-default seeding.

Stage 2 validation on 2026-11-14, tested against `eda9acf67` plus these changes:

- Incremental `all` builds pass for Win64-Debug-DurinEditor and
  Win64-Debug-DurinGame, covering Engine, Sandbox and RoadWeaver after the shared
  Core API/link and common executable UTF-8 manifest changes. Native execution uses
  the already established non-sandbox MSVC lane. Source/test consumer symbol searches
  cover all Durin.dworkspace projects. The affected selector expands to all for the
  shared build/Core changes; bounded behavior targets below plus both all builds
  cover bootstrap/storage/Cook/Projects and export without unrelated runtime tests.
- Launch contract selector (five targets), GameExportContractTests (14),
  PluginResolutionTests (19), LaunchStorageTests (7) and CookFunctionalTests (44)
  pass. Coverage includes stable bindings, invalid IDs/legacy policy, containment,
  immutable default layering, migration/override retention, missing output preservation,
  mount/config/identity mismatch, publication rollback, and atomic rollback of full
  evidence plus runtime snapshot. XML and logs:
  `Build/NativeTestResults/Win64-Debug-DurinEditor/` and `Build/.agent-state/logs/`.
- 132 Python/schema/client/bootstrap cases pass; receipt:
  `Build/GameExport/Stage2/python.xml`. Actual emitted Runtime.json and RuntimeCook.json
  also pass their schemas (`Build/GameExport/Stage2/check_emitted.py`).
- Independent DurinAssetTool Cook and DevTool stage succeed with
  `Build/Exports/Stage2/SandboxCook` and `Build/Exports/Stage2/SandboxGame`.
  Cook explicitly retains `/Game/Models/GrayboxPawn`, allowed until Stage 4.
  The named executable resolves its default ../../Runtime.json from an unrelated cwd
  after copying outside the checkout into a Chinese path containing spaces and then
  renaming the installation. Default Level, pawn mesh and cooked Shader data load;
  three bounded 30-tick runs exit normally with no missing pawn visual diagnostic.
- Read-only qualification denies file writes and directory additions with a scoped
  inherited Windows ACL; a write probe is denied, the Game still runs, and every
  installation file hash remains unchanged. The test restores the ACL. Saved config,
  logs and Cache are confined to an isolated external Durin/org.durin.Sandbox root;
  no root or binary-adjacent Saved appears. Missing frozen Project config, runtime
  snapshot and Shader library each fail with exit 2. Reproduction, process/ACL logs
  and receipts: `Build/GameExport/Stage2/qualify.py` and
  `Build/GameExport/Stage2/qualification/`.
- Explicit migration copies legacy user configuration once and preserves a newer
  destination on the next run. A missing committed runtime snapshot makes stage
  fail while preserving the prior Runtime.json hash. Receipts and reproduction:
  `Build/GameExport/Stage2/extra.py` and `Build/GameExport/Stage2/extra/`.
- Source Editor (10 ticks), Browser (3 ticks) and explicit source Game (30 ticks,
  independent Cook root) exit normally through DevTool; reproduction and receipts:
  `Build/GameExport/Stage2/hosts.py` and `Build/GameExport/Stage2/hosts/`.
  Final binaries were restaged and relocation/read-only/migration/source-host checks
  repeated after the final Projects admission checks. Later documentation edits
  do not change the validated runtime inputs.
- Changed-document validation (four documents), all-plan validation (11 active,
  2 completed, 457 archived plans) and `git diff --check` pass.

These receipts qualify Win64 Debug layout/storage, not Release redistribution,
clean-machine prerequisites, installed backends, cross-configuration Cook or macOS
staging. Those gates remain open. Stage 1 evidence below records the historical
migration layout/storage, superseded by Stage 2.

Stage 1 migration refinements (2026-11-13): dependency ordering requires the
source-stage backend to consume existing Game products/plugin inventories before
Stage 3 completed-build receipts exist. Source-stage requests therefore do not
require the future Receipt field; unsupported prebuilt requests still require it
and fail capability handling. NativeReceiptSha256 binds NativeInputs.txt migration
evidence, and Cook.SelectionFingerprint binds the full committed PluginCook.json
snapshot until the later receipt/snapshot changes. UserData.Policy explicitly
records ExecutableSaved in this migration; LocalAppData remains the Stage 2
selection and is rejected rather than silently ignored. There is no user-facing
legacy-layout/storage switch. Source Game gains an optional explicit --cook-root
for a committed Cook generation independent of its binary directory. These are
bounded migration bindings, not completed later-stage contracts.

Stage 1 validation on 2026-11-13, tested against `ba0e2f652` plus these changes:

- Win64-Debug-DurinEditor and Win64-Debug-DurinGame incremental all builds pass,
  covering Engine, Sandbox and RoadWeaver and the tools-only build boundary.
  The owning service target is rebuilt for final request/rollback checks. MSVC
  C1902 occurred inside the sandbox; successful native builds/tests ran outside it.
- The launch contract selector (`@domain=launch,kind=contract`, five targets),
  PluginResolutionTests (16 cases) and GameExportContractTests (11 cases) pass.
  Coverage includes bootstrap containment/hash failures, explicit launch modes,
  detached plugin admission, ordinary/delay PE imports, native dependency drift,
  writer ownership, cancellation during copying, actual publication rollback and
  the registered tools module entrypoint.
  XML/log receipts remain under `Build/NativeTestResults/Win64-Debug-DurinEditor/`
  and `Build/.agent-state/logs/`.
- 132 Python/schema/client/bootstrap cases pass; receipt:
  `Build/GameExport/stage1-python.xml`. Source build metadata and toolchain are
  unchanged for reused passing coverage; later document edits do not change inputs.
- Direct worker preview/stage and DevTool stage produce byte-identical Runtime.json
  files. Preview is non-publishing. Correlated cancel, disconnect and wrong-ID
  frames return 130/130/2, acknowledge cleanup where applicable and preserve the
  prior public manifest. Reproduction and raw receipts:
  `Build/GameExport/stage1_protocol.py` and `Build/GameExport/stage1-protocol/`.
- Sandbox Cook uses the existing independent DurinAssetTool and the temporary
  `/Game/Models/GrayboxPawn` root allowed until Stage 4. A real Game with no
  sentinel Engine/Content and no .dproject runs 30 ticks from an unrelated cwd
  after copying outside the checkout to a path containing spaces. Missing/corrupt
  manifests fail with exit 2 both beneath and outside a source checkout.
  Reproduction and logs: `Build/GameExport/stage1_smoke.py` and
  `Build/GameExport/stage1-smoke/`; Cook/staged inputs:
  `Build/Exports/Stage1/`.
- Source Editor (10 ticks), Browser (3 ticks) and explicit source Game (30 ticks,
  independent --cook-root) start and exit normally through DevTool. The Cook host
  and export worker run headlessly without a live Editor. Final host reproduction
  and receipts: `Build/GameExport/stage1_hosts.py` and
  `Build/GameExport/stage1-hosts/`. The emitted Runtime.json also passes its v2
  schema; the Game Ninja graph contains neither GameExport nor DurinExportTool
  targets.

These are Stage 1 migration receipts. Release, read-only/per-user installation,
installed-style execution, clean-machine prerequisites, full client orchestration
and native macOS staging are not closed by this evidence. Historical fixtures
remain in [portable goldens](../../Engine/Tests/Fixtures/GameExport).

Stage 0 validation on 2026-11-13: 77 portable pytest/jsonschema contract cases pass
on Windows with the checkout's prepared Python environment, against
`392e9d0ba43b22ba12bb698b28a7579df38f4223` plus these Stage 0 changes;
changed-document and all-plan lifecycle checks pass. No native build is required
for schema/fixture changes. Schema tests are not evidence for native bootstrap,
process cleanup, installed execution or clean-machine gates. Raw test receipt:
`Build/GameExport/stage0-tests.xml`.

Baseline revision: `871de3b06486954716dba968b25dad4163b904ad`.
Sandbox Win64 Debug and Release exports already Cook, Stage and run 120 ticks
both in their original folders and in copies outside the checkout. Release
verification also inspects PE imports and loaded modules for Debug CRT/development
binary dependencies. These are migration-smoke receipts, not clean-machine or
new-layout acceptance. Raw reports/reproduction scripts remain under ignored
`Build/Exports/SandboxVerification/` and `Build/Exports/SandboxReleaseVerification/`.

Baseline gaps (recorded before implementation; current progress is owned by the stage checks):

| Gap | Baseline implementation/evidence | Required outcome |
| --- | --- | --- |
| Engine-root discovery needs an empty Content directory | [Paths.cpp](../../Engine/Source/Runtime/Core/Private/Misc/Paths.cpp), [stage.py](../../Tools/DurinDevTool/durin_dev_tool/stage.py) | Explicit deployment roots; no source-directory probing in a staged Game |
| Cook root equals executable directory | [EngineLoop.cpp](../../Engine/Source/Runtime/Launch/Private/EngineLoop.cpp) | Content and binaries can move independently within one declared layout |
| Build output shape leaks into delivery | Stage emits `Engine/Binaries/Win64/<Configuration>/Runtime/DurinGame/...` | A stable project-facing delivery tree |
| Deployment selection is partly inferred from existing DLLs | Stage globs engine DLLs and supplements plugin inventories with PE imports | Complete target receipts, including dynamically loaded modules and runtime files |
| Native explicit loads require operator knowledge | Sandbox PlayerPawn loads `GrayboxPawn` by string; exports needed an extra `--root` | Persisted project Cook rules or tracked asset references |
| Target configuration follows the Cook host | [PluginCook.cpp](../../Engine/Source/Runtime/Projects/Private/PluginCook.cpp) captures host configuration | Separate tool-host identity, Game build identity and Cook target policy |
| User output is beside the executable | [RuntimeStorage.cpp](../../Engine/Source/Runtime/Launch/Private/RuntimeStorage.cpp) | Writable per-user data independent of installation location |
| Product staging logic is implemented in a source-development wrapper | `durin_dev_tool/stage.py` owns selection/copy/publication policy; current export scripts select targets manually | Engine-owned service/worker with shared request/result contracts and thin clients |

## Goal And Scope

A developer selects a project, Win64 target, Debug/Release configuration and
destination through Editor, the engine export tool, DevTool or CI. One
engine-owned export service produces a complete, relocatable Game with
explicit paths, declared native/file dependencies and reproducible asset
retention. Export reports the selected inputs and results without exposing
internal target selection as a routine user responsibility.

Win64 Debug and Release source-engine builds are the first acceptance scope.
Source Editor, Browser, source Game and headless tools remain working. Generic
metadata/path contracts must preserve supported platform semantics; native macOS
Game staging remains an explicitly separate outstanding gate.

Retain immutable plugin admission, logical asset identities, existing Cook/DDC
and Shader products, including existing domain-local transactions. This plan does not introduce
a second asset registry, cooker, module loader or application startup state machine.
Loose cooked files remain supported. Pak/IoStore equivalents, physical chunks,
DLC, installers, code signing, Shipping qualification, full SDK distribution and
a full packaging-profile UI are deferred. A minimal Editor export action with
save/progress/cancel/result handling is included as a client. Tool-component
packaging and isolated installed-style execution are included; producing the
complete installed Editor/SDK product remains with its owning plan.

## UE Reference And Selected Adaptation

| UE reference | Adopt in Durin | Boundary |
| --- | --- | --- |
| [BuildCookRun and build operations](https://dev.epicgames.com/documentation/en-us/unreal-engine/build-operations-cooking-packaging-deploying-and-running-projects-in-unreal-engine) | Engine-owned export orchestration behind an independent tool, called by Editor, CLI and CI | Engine ownership does not require running heavy work inside the Editor process; low-level commands keep their no-implicit-build/Cook contract |
| [RuntimeDependencies and staged file kinds](https://dev.epicgames.com/documentation/unreal-engine/integrating-third-party-libraries-into-unreal-engine) | Declare runtime modules/files and emit target build receipts | PE import inspection supplements declarations; it cannot discover every dynamic load |
| [BuildGraph receipt tasks](https://dev.epicgames.com/documentation/unreal-engine/buildgraph-script-tasks-reference-for-unreal-engine) | Select actual build products and runtime dependencies from completed-build records | Reuse DHT/CMake ownership; no independent directory-scanning build catalog |
| [Primary Asset Cook rules](https://dev.epicgames.com/documentation/en-us/unreal-engine/cooking-content-and-creating-chunks-in-unreal-engine) | Use current AssetManager, AlwaysCook and tracked references | No guessing arbitrary C++ strings and no copy-all-content workaround |
| [Packaging settings](https://dev.epicgames.com/documentation/en-us/unreal-engine/project-section-of-the-unreal-engine-project-settings) | Make runtime prerequisites and delivery settings explicit | Do not copy UE container formats or promise an installer in this milestone |

These are architectural adaptations from public documentation, not claims of
matching UE's private implementation or every platform's output layout.

## Selected Contracts

### Engine service, worker and client boundary

Introduce an engine tools/developer service module, working name `GameExport`,
and an engine program `DurinExportTool`. Stage 0 freezes their registered names,
dependencies and versioned request/progress/cancellation/result contracts.
They belong to the engine tool distribution and are excluded from the shipped
DurinGame dependency closure. Export ownership does not move into the gameplay
Engine module or the running Game process.

```text
Editor action / DevTool client / direct CLI or CI
  -> versioned export request
  -> independent DurinExportTool process
  -> GameExport service
     -> build backend + completed-build receipts
     -> existing DurinAssetTool/Cook services
     -> staging, inspection, verification and archive services
```

- GameExport owns request normalization, target/closure/layout/Cook policy,
  prerequisite checks, reuse decisions, orchestration and completion marking.
  DurinExportTool hosts it headlessly and owns the job and child-process lifetime.
  The service/worker do not depend on Editor presentation modules or a live
  Editor session. Tests may exercise service APIs directly; production clients
  use the worker. Heavy Cook work retains its existing independent host.
- Editor owns input/settings presentation, its normal save/unsaved-work boundary,
  process dispatch and asynchronous progress/cancel/result presentation. Build,
  Cook and Stage do not run on its GameThread or consume unsaved Editor objects.
- DevTool provides source-development convenience, optional toolchain preparation
  and tool discovery/dispatch. It forwards explicit environment/backend inputs
  without independently selecting targets, computing closures or staging files.
  CI can invoke the engine tool directly without using DevTool.
- Reuse existing build, asset, Shader, filesystem, process and transaction owners.
  Extract product policy from `stage.py` into the engine service; DevTool's stage/
  export commands become adapters. There must not be an old Python staging path
  and a new native staging path with separate rules after migration.
- The worker invokes engine-provided build/Cook backends directly; it never calls
  DevTool recursively. The source-build backend may require CMake, DHT, a compiler
  and source metadata. An installed-style backend uses shipped engine metadata,
  compatible products and receipts; unsupported build capability fails explicitly.
  Packaging prebuilt products does not require source build capability.
- A tool component includes the worker, service dependencies, provider/resource
  and schema metadata needed for its declared capabilities. An installed-style
  Editor resolves this component from engine metadata, not `DevTool.bat`, a repo
  Python environment, `.agents/DevTool.user.json` or an engine source workspace.
  Project-authored assets/descriptors and declared prerequisites remain valid inputs.
- Use one owned worker per invocation with a bounded, versioned process protocol;
  define exit/error mapping, progress/log separation, cancellation and child cleanup.
  Client disconnect or Editor shutdown triggers orderly cancellation/cleanup;
  detached background jobs are deferred. Direct CLI uses existing console
  cancellation handling. Reuse existing process facilities rather than
  introducing an export daemon or another task framework.

### One startup path context

Core owns low-level immutable path values; Launch selects/initializes the host
mode before any EngineDir/RootDir/config/storage consumer caches a value.
Projects continues to own project/plugin identity and admission. Extend existing
FPaths/mount/configuration services instead of adding parallel deployed variants.

- Source Editor/tools obtain their source context through existing host setup.
  Source Game becomes an explicit host launch mode exposed by DevTool; migrate
  its launcher, probes and documentation rather than inferring source mode from
  nearby files.
- A standalone Game defaults to staged mode. Its launcher passes the deployment
  manifest explicitly; direct executable launch uses one documented location
  relative to the executable. An invalid/missing staged manifest is a bounded
  failure, including when a source checkout exists higher in the directory tree.
- Relative paths resolve against the manifest's canonical deployment root,
  never the current working directory. Reject escapes, unsupported schema/target,
  conflicting roots and aliases escaping the deployment before publishing paths.
- RootDir, EngineDir, engine/project content, binary, config and third-party
  accessors receive explicit mode-specific bindings. Audit callers that append
  source-only suffixes. Cooked mounts use the declared Cook root, not LaunchDir.
- Staged project admission consumes a minimal detached project view from the
  runtime manifest; no authored `.dproject` or workspace lookup is necessary.
  Source descriptor management and Editor history retain their existing owners.

### Stable delivery layout

The selected Win64 layout is project-facing and independent of CMake's build
directory hierarchy. The output folder can be renamed without changing identity:

```text
Sandbox/
  RunGame.bat
  Runtime.json
  Binaries/Win64/
    Sandbox.exe
    DurinGame-*.dll
    <declared native dependencies>
  Config/
    Project.yaml
    DurinGame.yaml
    Defaults/DurinGame.yaml
  Content/
    CookManifest.bin
    <versioned runtime Cook snapshot>
    Engine/...
    Game/...
    Plugins/<Id>/...
    Shaders/ShaderLibrary.dslb
```

The executable and imported native DLLs initially share the binary directory,
respecting Windows loader behavior before application code can run. The root
batch launcher locates the executable/manifest; a native bootstrap is optional
future presentation work. Source build outputs retain their existing layout.
There is no empty Engine/Content, Project/Content or ThirdParty directory used
only to satisfy discovery. Real declared non-asset files may create directories.
Preserve `/Engine/`, `/Game/` and `/Plugins/<Id>/` serialized identities.

Evolve `PluginRuntime.json` into deployment-root `Runtime.json` schema v2, including
generic target/project/path/native fields and the existing plugin section. Core
reads only its generic bootstrap subset; Projects validates its project/plugin
subset. Both consume one document, not separately maintained inventories.
CookManifest remains the sole asset catalog/checksum authority. Reduce the Cook
snapshot to required runtime selection/configuration bindings; keep full authored
descriptors, workspace inputs and build provenance in export evidence. Hashes
bind the runtime manifest to the committed Cook/native inputs.

Existing exported folders retain their original binaries/behavior and are not
rewritten. New output moves to the new schema atomically. Unsupported old Cook
snapshots receive an explicit recook/restage diagnostic; there is no permanent
legacy-layout switch or silent source fallback. Stage 1 may map the new schema
onto current physical paths during migration; Stage 2 changes those path values,
using the same runtime mechanism rather than a second layout implementation.

### Declared deployment inputs

DHT/module metadata and CMake target rules declare linked inputs, dynamically
loaded Runtime modules, and required runtime files. Dynamic module dependencies
cause build/receipt membership without inventing a linker dependency. Platform,
host and configuration eligibility remain part of the existing module graph.

A completed-build receipt records target/project/engine identity, platform,
architecture, configuration, runtime variant, ABI, exact build products, required
dynamic modules and runtime files. Records include ownership, source artifact,
deployment-relative destination, file kind and content identity. Emit receipts
only after their inputs successfully build; configuration alone is not a receipt.
Ordinary Engine/project modules and selected plugins use the same deployment
closure, while the available plugin build inventory may remain a superset.
Selection-only overrides are deployment inputs, not native build-availability
inputs; they must not force configure/build or inventory republication when
matching artifacts already exist. Source builds still compile all eligible
discovered plugins under the existing build policy.

Stage consumes receipts plus frozen Cook selection. It never authorizes files by
globbing a binary directory. PE ordinary/delay import checks detect missing or
incompatible native inputs; dynamic libraries and non-DLL data still need explicit
declarations. Reject differing DLLs with the same loader identity, path escapes,
stale receipts, wrong target/ABI and dependencies on inactive/Editor modules.

Distinguish OS-provided libraries from redistributable runtime dependencies.
Presence in the build machine's System32 is not sufficient evidence of an OS
guarantee. Stage 0 selects an explicit Win64 system-library policy and VC++
runtime delivery/prerequisite policy with provenance/license requirements.

### Cook, configuration and writable data

Game configuration/target build identity are explicit Cook/export inputs, separate
from the tool-host configuration and provenance. A compatible Debug Cook host
must be able to target Release Game without importing Game DLLs into the host.
Validate host provider/schema compatibility against the selected target inputs;
retain the standalone tool's existing provider/work lifetime and offline saved-
input contract. Never switch providers or mutate active plugin admission mid-run.

Use existing target platform policy, AssetManager settings, primary IDs, bundles
and required references. Persist Sandbox's native explicit-load requirement in
project policy or a tracked asset configuration. Normal export must include
GrayboxPawn without a caller remembering an extra command-line root, and must
exclude unrelated optional content. Ad hoc `--root` remains a developer override.
Preserve NeverCook, EditorOnly/source-only and blocked-plugin semantics.

Effective Engine/plugin/project defaults remain immutable deployment inputs.
Saved user overrides, logs and caches use a stable application identity and an
explicit per-user writable policy (Win64 LocalAppData), independent of package
location/configuration. Preserve existing overrides during migration; never seed
user overrides with merged inherited project/plugin values. Portable writable
storage, if exposed, must be explicit. Installation write failure cannot cause
fallback into source or an arbitrary current directory.

### Unified export service and execution request

The proposed direct engine-tool entrypoint, not yet implemented, is:

```powershell
DurinExportTool.exe export --project Sandbox/Sandbox.dproject --platform Win64 --configuration Release --output Build/Exports/SandboxRelease --verify
```

Editor submits the equivalent request to a worker. DevTool retains a source-
development convenience command which forwards to that same tool:

```powershell
.\DevTool.bat export --project Sandbox/Sandbox.dproject --platform Win64 --configuration Release --output Build/Exports/SandboxRelease --verify
```

The service normalizes one request and freezes project, target, effective plugin
selection, Cook policy, layout, prerequisites and output identity. It selects
the exact tool and Game build targets from declarations. Clients may collect
settings/defaults, but authoritative validation and plan construction happen in
the service. Users need not list Renderer/VulkanRHI or choose a Cook-host preset
merely to request a Game configuration. Equivalent client requests must resolve
the same export plan and content/selection identity; transient job/log paths may differ.

The worker executes the service's Build -> Cook -> Stage -> optional verification/
archive plan using existing services and child tools. Individual build/cook/stage
commands remain available as clients of their owning services. Source build
environment preparation by DevTool remains an optional client step, not a
runtime dependency of the worker. Reuse compatible build receipts before
requesting incremental construction of missing/stale declared inputs. Skip-build/
skip-Cook modes require compatible receipts and committed outputs; they cannot
weaken freshness/identity
checks. Every preview entry point checks configuration and reports planned phases
without product admission, building, publishing or a product fingerprint.

### Export completion and output ownership

Stage 5 uses a fresh per-invocation directory, for example
`Exports/Sandbox/<invocation-id>/`. The service validates and exclusively creates it;
an existing directory is rejected, even if incomplete. No existing successful
export is replaced, merged into or automatically deleted. Retry uses another new
directory; build/Cook reuse still requires the ordinary receipts and input checks.
Job paths and IDs do not enter normalized content identities. Preview reports no
product fingerprint.

The invocation directory contains `Delivery/` with the existing Runtime.json and
Binaries/Config/Content layout, optional archive artifacts beside Delivery, and
`ExportComplete.json`. Work files, logs and isolated verification user data stay
outside Delivery. An archive contains only the delivery payload, so it does not
contain or recursively hash the completion marker.

The worker writes the versioned completion marker atomically and last, after all
requested Build/Cook/Stage/verification/archive work, required validation and child
retirement succeed. It binds the delivery Runtime.json hash and any requested
archive hashes. A directory without a valid matching marker is incomplete and must
not be presented as a successful export. The marker is job completion metadata;
it neither replaces receipt/CMNF/Runtime.json checks nor expands runtime file authority.
Successful results name the invocation directory. Preview creates no directory,
lock or marker. Marker/protocol schemas and client fixtures change together when
Stage 5 is implemented; this decision does not change the current wire implementation.

Build failure does not roll back build outputs or caches. Cook keeps its existing
internal transaction/cache rules; orchestration adds no enclosing transaction.
Stage writes only the new delivery directory. Failure, cancellation, disconnect or
worker death before completion leaves the invocation incomplete and earlier exports
untouched. Incomplete files may remain for diagnosis or explicit cleanup; automatic
cleanup, backup restoration, cross-phase undo and resume are not acceptance gates.
Low-level Stage follows the same fresh-directory/final-marker rule, with its
payload directly in the requested directory. A completion-marker write failure is publication
failure; an incomplete directory is never reported as success. Once the marker is
published, late cancellation or failure to deliver the result does not undo it.

Inputs must be saved before dispatch and remain stable until exit; concurrent edits
are unsupported. No input snapshot, editing lock or durable orchestration journal is
required. Keep target/ABI/schema, receipt freshness, declared closure, hashes,
output/input overlap and per-directory writer checks. Editor save cancellation or
failure dispatches no job. After dispatch, cancel, client disconnect or Editor
shutdown stops and reaps owned children before acknowledging cancellation; it must
not be implemented as untracked termination. Direct CLI follows console cancellation;
detached jobs remain outside scope. After marking completion, report success rather
than cancellation. Keep marker publication a short non-cancellable final step.

## Ownership And Related Plans

| Responsibility | Owners |
| --- | --- |
| Explicit bootstrap paths, DLL and low-level filesystem mechanisms | Core, Launch |
| Detached project/plugin admission and configuration bindings | Projects, Launch |
| Module/runtime file declarations and completed-build receipts | DurinHeaderTool, CMake and engine build backends |
| Target policy, retained assets and runtime Shader products | Engine, TargetPlatform, ShaderBuild, RenderCore |
| Export policy, planning, staging, archive, inspection and verification | Engine GameExport service; existing domain services retain their algorithms |
| Independent job execution and child-tool/process lifetime | Engine program DurinExportTool; DurinAssetTool remains the Cook host |
| Editor save/input/progress/cancel/result client | MainFrame/DurinEd, using the worker protocol |
| Source convenience command, toolchain preparation and dispatch | DurinDevTool client |
| Direct automation client | CI, invoking DurinExportTool |
| Export-tool component metadata and dependencies | Engine tool build/distribution rules; SDK packaging consumes the component |
| Default-level/native explicit-load fixture | Sandbox |

[Restart-Based Plugin System](RestartBasedPluginSystem.md) remains authoritative
for selection, phase order and immutable plugin lifetime; its native macOS gate
is not closed by this plan. This plan evolves the already implemented Win64
deployment envelope. [Runtime Startup And Scene Activation](RuntimeStartupAndSceneActivation.md)
owns readiness, scene operations and shutdown; inject configuration through its
existing bootstrap boundary without creating another startup state machine.
[DurinEditor SDK Packaging](DurinEditorSdkPackaging.md) owns Editor SDK products,
installed development interfaces and their layout. Share useful receipt and
redistribution metadata, without changing the SDK's selected layout or adding
SDK delivery to Game export. This plan supplies the export-tool component recipe
and tests an isolated installed-style bundle; SDK packaging owns inclusion in
its complete products. Neither the installed-style tool nor its Editor client
may delegate product export work back into source DevTool scripts.

## Implementation Stages

### Stage 0: Freeze migration contracts and baseline fixtures

Status: Complete. Contracts and portable fixtures validated; native consumers
are the next stage. System DLL/API-set classification is frozen in
`Engine/Source/Developer/GameExport/Contracts/win64-system-v1.json`; Debug requires
explicit developer/test CRT provision and does not redistribute Debug CRT.

- [x] Record current export behavior, concrete gaps, UE references and ownership.
- [x] Select the root/layout/receipt/Cook/orchestration direction and scope above.
- [x] Freeze versioned manifest/receipt fields, FPaths accessor bindings, source
  launch migration, application identity and Win64 prerequisite policy; also
  freeze the engine service/worker boundary, tool-component capabilities and
  versioned client request/progress/cancel/result protocol.
- [x] Add portable schema/golden fixtures for no-plugin, content-only, dependent
  Runtime+Editor, dynamic native modules and native non-asset data. Preserve the
  Sandbox Debug/Release receipts as historical baseline inputs.

Acceptance: schema fixtures encode valid layouts and target identities plus
invalid/missing manifest, containment and incompatible-version cases. Protocol
fixtures cover equivalent Editor/DevTool/direct-tool requests, version mismatch,
unsupported backend capability and cancellation acknowledgement. Every subsequent
stage has one selected contract to implement; no product behavior is claimed from
this planning document alone.

### Stage 1: Establish explicit startup paths and engine export host

Dependencies: Stage 0. Owners: Core, Launch, Projects, GameExport/DurinExportTool;
DevTool run/stage are clients.

Status: Complete for the Stage 1 Win64 Debug acceptance gates; evidence and
migration-only field bindings are recorded in Current Status.

- [x] Establish the minimum tools-only GameExport service and headless worker
  with direct preview/stage invocation, explicit engine/backend inputs and process
  protocol. Extract current staging policy from DevTool into this owner before
  extending it; retain existing domain algorithms and independent Cook lifetime.
- [x] Initialize immutable path context before cached FPaths/config/logger/storage
  reads. Publish paths only after bootstrap validation; retain early stderr errors.
- [x] Migrate staged admission and the engine Stage emitter to Runtime.json v2,
  initially permitting current physical path values. Preserve working end-to-end
  exports between commits; adapt existing plugin/native admission rather than duplicating it.
- [x] Implement explicit source Game launch policy in Launch, forward it through
  the DevTool run client, and migrate probes/consumers across every Durin.dworkspace
  project. Preserve Editor/Browser/tools.
- [x] Route cooked mounts, Shader library, project configuration and native lookup
  through declared roots; prevent staged-mode source/cwd fallback.

Acceptance: a deployed fixture without the sentinel Engine/Content starts from
an unrelated cwd. Missing/corrupt manifests fail both outside and beneath a
checkout without adopting its Engine. Source Editor, source Game, Browser and
headless tool startup retain their intended path context and clean shutdown.
The engine worker can preview/stage the fixture directly, and DevTool delegates
the same request without requiring a recursive DevTool invocation from the worker.

### Stage 2: Publish the stable delivery layout and writable-data policy

Dependencies: Stage 1. Owners: GameExport engine Stage service, Core, Launch, Projects.

- [x] Have the engine service emit the selected Binaries/Config/Content tree and
  named Game executable; remove build-configuration nesting and discovery-only directories.
- [x] Bind project/config/native/Cook paths relative to deployment root. Omit
  authored descriptors/workspace inputs unnecessary for runtime admission.
- [x] Resolve stable per-user Saved/config/log/cache roots before first write;
  keep frozen defaults read-only and preserve user overrides across relocation.
- [x] Keep full provenance in external export evidence; version and bind the
  minimal runtime snapshot through the existing Cook transaction.

Acceptance: Sandbox starts with default Level, mesh and cooked Shader data after
folder rename/relocation to a path containing spaces/non-ASCII characters. A
read-only installation runs using isolated writable user data without creating
Saved beside the executable. Required-file/mount/config failures retain usable
previous outputs; no empty directory is needed for path discovery.
Layout qualification may use the existing explicit pawn Cook root until Stage 4
persists that requirement as project policy.

### Stage 3: Stage the declared native and runtime-file closure

Dependencies: Stages 0-2. Owners: DHT/CMake and engine build backends,
GameExport, Projects. DevTool only dispatches source-development requests.

- [x] Extend existing module/target metadata for dynamic Runtime modules and
  runtime files; migrate Renderer, RHI selection, Sandbox and plugin fixtures.
- [x] Generate target receipts after successful builds, binding all ownership,
  compatibility, source artifact, destination and content identities.
- [x] Replace Stage DLL globs/manual target knowledge in the engine service with
  receipt-based selection for ordinary modules and the enabled plugin closure.
  Preserve build-all versus startup/deployment-subset semantics.
- [x] Apply declared file categories and system/prerequisite policy. Retain PE
  checks and reject undeclared imports/loads, collisions and stale/missing artifacts.

Acceptance: native dynamic-module and non-DLL fixtures work without manual copy/
build steps. Planting an unrelated or stale DLL in build output does not deploy
it. Missing required runtime files, inactive/Editor edges, same-name differing
DLLs and wrong target/ABI fail before publication. Available plugin artifacts
remain a superset and selection changes do not republish build inventories.

### Stage 4: Make Cook roots and target configuration declarative

Dependencies: Stage 3. Owners: Engine, Projects, DurinAssetTool, GameExport, Sandbox.

- [x] Pass the service's immutable target Game identity/configuration independently
  of host identity; validate provider/schema compatibility and bind committed Cook inputs.
- [x] Declare GrayboxPawn through existing project AssetManager/asset references;
  remove the requirement for export scripts to add a special Sandbox root.
- [x] Preserve merged defaults, retained references, explicit-load/AlwaysCook,
  bundle, blocked-plugin, NeverCook and EditorOnly/source-only behavior.
- [x] Keep asset checksums in CookManifest and required native Shader programs in
  the existing project library. Do not retain all enabled plugin content.

Acceptance: default Sandbox Cook includes the pawn mesh and its dependencies,
while an optional unreferenced fixture remains absent. Debug host -> Release Game
Cook succeeds with compatible providers. Incompatible hosts/targets and required
references conflicting with policy fail without changing committed Cook output.

### Stage 5: Complete engine export orchestration and shared clients

Dependencies: Stages 1-4. Owners: GameExport, DurinExportTool; MainFrame/DurinEd,
DevTool and CI integrations are clients. Reuse existing native/domain services.

- [x] Complete service-side request validation/planning and worker execution of
  Build/Cook/Stage/optional verification/archive, with receipt-based reuse and
  preview/report semantics. All target, closure and publication decisions have
  one owner; clients do not replicate the pipeline or invoke it in Editor memory.
  Use fresh per-invocation directories and a final completion marker as defined
  above; fixed-directory overwrite and cross-phase rollback are outside scope.
- [x] Implement engine-provided source-build and prebuilt/installed-style backend
  capability handling. Produce the worker component recipe, dependencies and
  lookup metadata; resolve it independently of DevTool profiles or repo layout.
  Missing compilation capability with stale/missing products is an actionable
  failure, not permission to use incompatible products or auto-install an SDK.
- [x] Add the minimal asynchronous Editor client: normal save/unsaved-work checks,
  request dispatch, progress, cancel and result handling. Adapt DevTool export/
  stage commands to dispatch the same worker protocol; support direct CLI/CI.
  Keep existing low-level commands and source-environment preparation available.
- [x] Complete worker/child ownership, cancellation acknowledgement, disconnect/
  shutdown policy and bounded error/exit mapping. Validate the isolated delivery
  and requested archive before writing the final success marker. Failure leaves
  an incomplete invocation, without undoing prior phases or replacing earlier
  exports. Reuse domain-local checks/transactions and the stable-input convention;
  keep logs and isolated verification data outside the delivery payload.
- [x] Provide common reproduction metadata and actionable phase/target/file
  diagnostics. Validate protocol versions and reject unsupported capabilities
  consistently across clients; keep source descriptors unchanged.

Acceptance:

- [x] Equivalent Editor, DevTool and direct CLI/CI requests resolve the same
  normalized target/closure/layout plan and output content identities. The direct
  engine command exports Sandbox without manual target/root/preset knowledge.
  Preview is non-mutating from every client; job IDs/log paths may differ.
  Each real invocation uses a fresh directory; successful completion requires a
  valid marker binding the checked delivery and requested archives.
- [ ] A minimal Editor export action runs heavy work in an observed child worker
  and remains responsive. Cancelled unsaved-work prompts or failed saves dispatch
  no job. Acknowledged job cancellation retires the owned child tree and leaves
  the Editor usable and earlier successful export directories untouched. The
  cancelled invocation has no completion marker; deleting its partial files is
  optional and is not required before acknowledging retired children.
  Manual UI interaction is deferred at the user's request; native process-client
  success/cancellation and source startup evidence are recorded above.
- [x] An isolated installed-style engine/tool bundle exports a fixture from
  compatible prebuilt Game/provider receipts and project inputs with source
  DevTool scripts, `.agents`, repo Python environment and engine source workspace
  unavailable. Both direct worker and Editor-client paths use the bundle's tool
  metadata. This proves the component contract, not completion of SDK packaging.
- [x] Invalid reuse, worker/client disconnect and injected failures in each
  requested phase leave no completed invocation and earlier successful exports
  untouched; any created partial directory remains unmarked. Reject existing destination directories and simultaneous
  writers; marker-write failure must not report success. Missing tool components,
  protocol mismatch, unavailable build backends and failed child tools produce
  the same structured failure categories for all clients; partial directories may
  remain but must never be reported as completed exports. Do not require cache
  rollback, fixed-directory replacement/recovery or interrupted-job resume.

### Stage 6: Qualify delivery and publish implemented contracts

Dependencies: Stages 1-5. Owners: all above, platform/CI qualification owner.

- [ ] Qualify the matrix below and record exact source/fixture/toolchain inputs,
  target receipts, selections, prerequisites, manifests, commands and outcomes.
- [x] At final acceptance, run incremental `all` builds for Win64 Debug/Release
  Editor and Game: shared Core path bindings and generated module/build-receipt
  membership affect all workspace projects. Also build the worker/component and
  verify export modules do not enter Game's dependency closure. Reuse unchanged
  passing receipts.
- [x] Run owner-specific path/configuration/admission, metadata, Cook and Shader
  tests, service/protocol/backend contract tests and bounded real-process/client
  cases. Follow [build](../Agents/BuildAndRun.md)
  and [test selection](../Agents/Testing.md); do not substitute every native test
  or repeated builds for the actual deployment/clean-machine gates.
- [x] Converge generated Game target information and single-pass Export/Stage
  target resolution; qualify the component's declared dependency closure without
  inventorying every shared-library target. See TargetConvergence evidence above.
- [x] Publish lasting contracts in Workspace/Development, Runtime/Application and
  owning asset domains, plus an export guide covering direct engine tool, Editor
  and DevTool clients and the worker component contract. Update plugin-plan cross
  references only for replaced contracts; preserve prior evidence and unqualified platforms.
- [x] Validate changed documents and all-plan lifecycle; complete/archive only
  when every mandatory gate has evidence.

Mandatory validation matrix:

| Case | Required evidence | Stage 6 status and evidence |
| --- | --- | --- |
| Win64 Debug and Release Sandbox | Default Level, native pawn mesh and cooked shaders; normal frame progress and exit; configuration/CRT identities match | Passed: final exact-receipt exports and relocated 120-tick runs; Final/sandbox-receipt.json and delivery-receipt.json. |
| No-plugin, content-only, dependent Runtime+Editor | Only selected Game/native/content closure; phase order; no Editor binaries/compiler/source requirements | Passed: current fixture-receipt.json; offline phase/asset/Shader traces in Final/delivery-receipt.json. |
| Explicit dynamic library and non-asset data | Receipt-selected files work; undeclared/missing files reject publication | Passed: live dependent plugin DLLs plus GameExportContractTests declared dynamic/data and missing/undeclared rejection cases. |
| Shared clients | Editor, DevTool and direct CLI/CI use one service/worker protocol and produce equivalent normalized plans/content identities; no client-side closure/staging policy | Automated pass: Final/clients-receipt.json; identical manifests/archive payloads. UI interaction is tracked separately. |
| Editor job boundary | Save failure/prompt cancellation starts no job; worker PID and child ownership observed; responsive progress/cancel/result handling; Editor shutdown/disconnect policy and failed-job isolation | Open: manual UI deferred by user. Shared native-client child ownership/cancel and automatic source startup pass. |
| Installed-style export tool | Shipped worker/component metadata, prebuilt products and authored project inputs suffice with source DevTool/config/Python workspace unavailable; direct and Editor clients use the bundle; unsupported build capability fails clearly | Passed for prebuilt/committed-Cook reuse: InstalledFinal, Final/offline-sdk-*.log and current inventory/schema checks; full SDK/offline Cook excluded. |
| Source hosts | Editor/Browser/source Game/tool startup and shutdown work through one existing lifecycle | Passed: current bounded automatic Editor/Browser/source Game and headless Cook/worker processes; no UI interaction. |
| Relocated/read-only install | Unrelated cwd; renamed Unicode/space-containing path; per-user output; no writes to immutable deployment files  Passed in Debug/Release: Unicode rename, ACL-denied write probe, immutable hashes and external retained user data. |
| Source/development inputs unavailable | Workspace, authored plugins/assets/shaders and development caches unavailable; restricted PATH; native modules load from the deployment/system locations allowed by policy | Passed for listed paths on System32-only PATH with optional Vulkan validation off; Final/offline-paths.json and offline-restored.json. |
| Clean Windows environment | No VS/compiler/SDK/source tree; positive run with declared prerequisites, clear prerequisite failure without them; inspect actual loaded modules and Release imports for Debug CRT | Open: no independent clean Windows lane configured. Current developer-host imports/module checks do not close this gate. |
| Failure and drift | Missing/corrupt native/config/Cook/Shader/snapshot/tool-component files, wrong schema/protocol/ABI/selection, stale receipt, worker/child/backend and write/verification/archive/marker failures; earlier successful directories unchanged, incomplete invocations unmarked, owned children retired; no cross-phase rollback | Automated pass: final missing-file cases plus current 30 native export cases and reused Stage 5 process/protocol/failure/drift evidence. |

Raw reports, traces and reproduction scripts belong under ignored Build outputs
or CI artifacts. A 120-tick migration smoke does not close clean-machine coverage.
If the clean-machine lane is unavailable, record it as an open gate. Native
macOS staging, Shipping, installers, signing and container-format work remain
explicitly outside this milestone; do not infer their support from Win64 success.

## Execution And Handoff

Implement Stage 0 -> 1 -> 2 -> 3 -> 4 -> 5 -> 6. Preserve source/build ownership;
no concurrent writers in one checkout. When changing shared APIs or descriptors,
search and migrate consumers across every project in Durin.dworkspace, including
tests and source launch paths. Validate owners/affected consumers before final
matrix coverage. Avoid unrelated startup/readiness or SDK-layout redesign.
Engine ownership applies from the initial extraction onward: no staging/export
policy may land only in a DevTool wrapper or Editor action while awaiting a later
port. Clients remain request/process adapters, and exported Games only consume
runtime deployment data rather than shipping the export service/tool.

Implementation commits update this plan's evidence-backed checks and use exact
`Plan: Documentation/Plans/GameExportPipeline.md` and the executed stage title as
`Stage` trailers. This initial document records decisions and baseline evidence;
it does not authorize marking runtime/schema/fixture implementation gates passed.
