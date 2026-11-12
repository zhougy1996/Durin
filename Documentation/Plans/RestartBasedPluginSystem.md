# Restart-Based Plugin System Plan

Summary: Build all eligible source plugins independently of project enablement; select plugins at startup and deploy only the enabled closure.

Last reviewed: 2026-11-13

Status: Active
Completed:

## Current Status

Stages 0, 1, 1.1, 2, 3, 3.1, 4, and 5 are complete for the selected Win64
source-build milestone. Builds enumerate all eligible plugins independently of
enablement; startup admits only the enabled subset from a version 2 inventory.
Projects freezes descriptor-derived content/shader roots and ordered config
inputs before consumption. Plugin assets use stable read-only virtual mounts;
ShaderBuild consumes early roots and existing source/dependency fingerprints;
project/application readers apply layered defaults without saving inherited data.
Compatible override-only edits still require restart without a build. Stage 5
implementation, automated qualification and user-reported manual acceptance
pass. Stage 6 (cooked plugin deployment) is complete for Win64 Debug: the existing
Cook transaction freezes Game selection/defaults, and Stage publishes one
complete source-independent Game. Stage 7's Win64 integration, source toggle,
final builds and documentation qualification pass. Native macOS build/staging/
execution and privileged link/junction execution remain unqualified; Stage 7 and
the overall plan remain open. Native staging currently rejects non-Win64 targets.

Stage 6 acceptance and Stage 7 Win64 qualification (2026-11-13):

- Tested base `4bfed8fbe` plus this task's final source/fixture changes. Exact
  SHA-256 inputs, workspace membership, preset identities and native inventories
  are recorded in ignored `Build/PluginStage6/final-inputs.json` and
  `workspace-qualified.json`. The disposable fixture lives under that directory;
  authored workspace/project declarations and saved Editor settings are restored.
- Final disabled-plugin `all` builds pass for `windows-msvc-x64`, Win64 Debug,
  `Win64-Debug-DurinEditor` and `Win64-Debug-DurinGame`. They cover all ordinary
  Engine/Sandbox/RoadWeaver consumers and the Runtime, dependent, content-only
  and Editor companion fixtures. Logs under `Build/.agent-state/logs/`:
  `20261113-033045-238235-33324-cmake.log` and
  `20261113-033106-847408-12008-cmake.log`. Earlier enabled-fixture final builds
  also pass. `disabled-build.py` and `disabled-build.json` retain reproduction.
- `qualify.py` passes 31 process invocations: 17 expected successes and 14
  expected rejections. No-plugin, content-only, dependent Runtime+Editor and
  checked-but-blocked selections Cook, Stage and run three ticks cleanly.
  Deployment inventories contain only selected Runtime modules and native
  dependencies; Editor binaries, optional unreferenced assets, authored Shader
  sources, developer Saved settings and Intermediate trees are absent.
  `processes.json`, per-command logs and the Cook/staged directories retain raw
  evidence. `before-no-build-hashes.json` and `no-build-hashes.json` prove both
  available build inventories remain unchanged across selection-only cases;
  final disabled builds also preserve those same inventory hashes.
- The enabled Game constructs a reflected plugin component, checks plugin asset
  values and the `/Game/` -> Consumer -> Runtime reference chain, explicitly
  loads a plugin-default AlwaysCook asset, and reads its native-only program
  from the project cooked Shader library. Optional content is not retained.
  The base uses Providers, Consumer uses EngineReady; traces verify reverse
  shutdown order. Cook retains the existing asset/reference/Shader pipelines.
- A relocated complete Game passes with source plugin directories,
  `Durin.dworkspace`, Engine Shader sources and Engine/project development caches
  physically unavailable, and PATH restricted to Windows system directories.
  It passes again with a Saved user override. `relocation-environment.json`
  records the exact deployment/unavailable paths; `relocated*.log` and traces
  retain runtime evidence. Every source input is restored after the run.
- Required references to a checked-but-blocked plugin, a NeverCook asset and
  an Editor companion type fail before publication; the previous committed Cook
  manifest is unchanged. Existing intentional source-only/editor-field stripping
  keeps its original policy. Incompatible build identity, selection drift,
  missing native dependency and corrupt Cook input fail Stage without replacing
  the prior runtime. Missing plugin DLL/explicit asset, corrupt Shader library,
  configuration, snapshot/manifest and mismatched runtime selection all reject
  staged startup with bounded diagnostics. No development fallback is accepted.
- Ordinary Sandbox Cook/Stage/Game startup passes with its actual native module
  and a temporarily empty default-Level setting (`ordinary.py`, `ordinary.json`).
  The real source Editor passes three ticks and clean shutdown. Runtime provider
  and EngineReady Consumer startup-failure injections both exit with their own
  cleanup and drained task pools (`hosts.py`, `hosts.json` and logs/traces).
  Four fresh Editor/Game startup-probe processes pass disabled then re-enabled
  admission without building or republishing (`source.py`, `source.json`).
- Native PluginResolutionTests pass 15, including detached runtime schema,
  selection, mount/config bindings and failed-admission output isolation;
  RenderShaderCookedLibraryTests pass 8. Final logs use prefixes
  `20261113-032244-017905-34468` and `20261113-032338-771802-30740` under
  `Build/.agent-state/logs/`. AssetCookTests and CookFunctionalTests pass through
  `@domain=asset-cook,kind=feature`; log
  `20261113-030337-318539-23348-ctest.log`. Their relevant Cook implementation
  inputs are unchanged by later admission diagnostics/fixture refinements.
- Python Cook/staging/plugin metadata/build regressions pass 134 with one
  privileged symbolic-link skip (`python-final.xml`). Stage tests cover ordinary
  and delay imports, transitive native closure, target/selection/build drift,
  source overlap, missing/corrupt inputs, publication rollback and preserving the
  backup even when rollback's destination is occupied. Windows scanner locks use
  a bounded rename retry; a permanent rollback failure reports its retained path.
- Affected-test selection expands to all because of temporary workspace fixture
  membership. Both all-build gates, owner Cook/Projects/Shader tests and real
  source/staged processes cover this change. Stage 5 UI/relaunch acceptance and
  unchanged metadata/mount/Shader mechanisms retain their earlier receipts;
  there is no new GUI-interaction claim. Lasting Cook/deployment/startup contracts
  and the Editor plugin workflow are published. Changed-document and all-plan
  validation pass. macOS native coverage requires an available owner/CI lane;
  no such lane is available from this Windows checkout, and no macOS staging or
  SDK-distribution support is claimed.

Stage 6 partial checkpoint (2026-11-12, Win64 Debug):

- At the user's request, stop expanding implementation and commit after bounded
  quick validation; unfinished work and end-to-end gates are explicitly deferred.
  Stage 6 remains In progress, with no acceptance gate closed by this checkpoint.
- Added detached Game selection/configuration capture and CMNF PluginSnapshot
  auxiliary publication in the existing Cook transaction. The asset tool enables
  capture and exposes a read-only stage-info preflight that validates CMNF file
  extents/digests before returning detached metadata. Added cooked runtime
  inventory admission in Projects, with relative-path containment, host identity,
  required-file hashes and frozen selection/module/configuration bindings.
- Added an initial DevTool stage command to select matching Game artifacts,
  inspect PE/native dependencies and prepare a whole runtime in a temporary
  output before replacement. This command has syntax/registration/path/import
  checks, not a successful full Cook-to-Stage qualification. Its runtime schema,
  publication failure handling and complete native dependency coverage still
  require dedicated positive/negative process tests before release acceptance.
- Game Providers move before the first rendering consumer so native plugin
  Shader declarations can be registered in time. A source-fixture Game reaches
  initialization, runs three ticks and shuts down cleanly with this ordering;
  that smoke does not establish cooked plugin Shader or relocated execution.
- Quick checks pass: PluginResolutionTests 14, AssetCookTests 38,
  AssetBulkContainerTests 11; Python Cook/plugin regressions 112 with 1
  privileged symbolic-link skip. Source logs use prefixes
  `20261112-205830-591092-39012`, `210156-530474-20600` and
  `210202-587623-14652` under `Build/.agent-state/logs/`.
  Python report: `Build/PluginStage6/python-quick.xml`.
- DurinAssetTool and Game Launch builds pass; final logs:
  `Build/.agent-state/logs/20261112-205833-252595-10932-cmake.log` and
  `20261112-205840-056665-3008-cmake.log`. Shared Cook request/manifest changes
  and the new sources make affected selection expand to all. Per the user's
  quick-check scope, use owner builds/regressions now; broad all-build coverage
  across projects is deferred and must not be inferred from these bounded runs.
- Raw quick checks, source Game smoke and legacy/unbound Cook rejection are in
  `Build/PluginStage6/quick-stage.json`, `quick-game.py`, `quick-game.json`,
  `quick-game.log` and `quick-stage-preflight.log`. Final input hashes and the
  tested base are retained in `Build/PluginStage6/quick-inputs.json`.
- Deferred: the complete no-plugin/content-only/dependent Cook-to-Stage matrix;
  an explicit-load/AlwaysCook fixture and cooked plugin reference checks;
  native plugin Shader registration/use from the cooked library; actual target
  configuration precedence in the staged Game; relocation with source/workspace
  and development caches unavailable; publication rollback, corrupt/missing
  artifacts and selection-drift negative cases; native macOS qualification.
  Existing automated tests do not substitute for these new-feature gates.

Stage 5 acceptance (2026-11-12, Win64 Debug):

- Accepted implementation revision:
  `8edd32cad50f1640726b88d16d2e50e581c290bc`. The user confirmed that manual
  acceptance passed in this conversation and requested closing Stage 5.
  This is user-reported acceptance, not an additional agent-run GUI test.
- Together with the automated and process evidence below, the confirmation
  closes the remaining UI persistence, pending-versus-active and cancelled
  restart/unsaved-work acceptance gates. All Stage 5 checks are complete for
  the selected Win64 source-build milestone. Earlier pending notes are
  superseded by this receipt.
- This closeout changes documentation only; tested source, fixture and runtime
  inputs are unchanged, so existing passing receipts remain applicable.
  Changed-document and all-plan validation pass. Stage 6/7 and native macOS
  qualification remain open; the overall plan remains Active.

Stage 5 presentation follow-up (2026-11-12, Win64 Debug):

- Base `80a09e979` plus the window-layout inputs captured in
  `Build/PluginStage5/ui-layout-inputs.json`. Keep the name/checkbox interaction;
  add a full-width search/Refresh row, plugin/selection counts, wrapped names and
  warnings, distinct catalog/filter empty states and a fixed footer for status
  and right-aligned actions. Geometry follows UI scaling and ImGui spacing;
  warnings/errors/info use existing semantic theme colors. Long diagnostics stay
  in wrapped tooltips instead of pushing the action row out of view.
- MainFrame and the Editor launcher build pass. Final MainFrame log:
  `Build/.agent-state/logs/20261112-201330-000652-36100-cmake.log`; launcher log:
  `20261112-201035-680732-38540-cmake.log` in the same directory. The rebuilt
  Editor initializes, runs three ticks with the fixture project and exits cleanly
  without error/fatal/Vulkan validation diagnostics. Raw command, DLL hash and
  output: `Build/PluginStage5/ui-layout-smoke.py`, `ui-layout-smoke.json` and
  `ui-layout-smoke.log`. User history/layout files are restored afterward.
- This changes only presentation; reuse the 14-case model/Refresh/Cancel receipt
  below because its relevant inputs are unchanged. No new tests mirror layout
  constants or repeat unrelated native/Python/process coverage. Manual panel
  resizing, theme/scale visual checks and final interaction were handed to the
  user; the subsequent acceptance is recorded above. The runtime smoke does
  not claim visual panel inspection. Documentation and all-plan validation pass.

Stage 5 Refresh/Cancel follow-up (2026-11-12, Win64 Debug):

- Base `8c5b64f7b533bd406d8f20a7cd7375c3986a3ee1` plus the final follow-up
  inputs recorded in `Build/PluginStage5/refresh-inputs.json`. Refresh explicitly
  rescans plugins and reloads disk settings/build availability; its tooltip
  explains that unsaved choices are discarded. A failed refresh keeps the
  previous usable session. Cancel restores only the saved in-memory snapshot
  and does not scan, read or write descriptors/inventories.
- PluginResolutionTests pass 14. The new rescan regression adds a nested plugin,
  removes another, changes saved choices and removes/restores the inventory;
  refresh updates all states without writing the descriptor. The cached-edit
  regression now also verifies Cancel while descriptor/inventory directories
  are unavailable. Log:
  `Build/.agent-state/logs/20261112-200216-973700-2408-PluginResolutionTests.log`;
  raw XML: `Build/PluginStage5/refresh-native.xml`.
- The owning native target, actual MainFrame UI module and Game Projects build
  cover the changed method and its only production caller. Build logs use
  `20261112-200317-700350-36720-cmake.log` and
  `20261112-200221-780750-30116-cmake.log` under `Build/.agent-state/logs/`.
  The affected selector
  also lists unrelated tool/workflow targets by module labels; symbol inspection
  confirms they do not call the changed edit-session method. No public API/ABI,
  resolver, startup, relaunch or build-graph change requires repeating the earlier
  all-build/process/Python qualification. Subsequent user-reported interactive
  acceptance is recorded above; no computer-use interaction is claimed here.

Stage 5 validation (2026-11-12, Win64 Debug, windows-msvc-x64):

- Tested base `4ee8a859240c13a749108c30105f5dd732f2f38d` plus the final source
  and fixture changes. SHA-256 inputs, variants, qualified workspace and runtime
  settings are retained under ignored `Build/PluginStage5/final-inputs.json`.
  Temporary workspace membership and saved user UI/history settings were
  restored after qualification. Documentation-only edits preserve these receipts.
- Per user direction, management uses plugin names and independent checkboxes.
  A checked request with an unchecked prerequisite remains checked and displays
  a dependency warning; save preserves the request, while startup keeps that
  consumer and its transitive consumers inactive. Other checkboxes are unchanged.
  This supersedes earlier rejection/cascade semantics. Native and Python
  resolution share this rule and optional `InactivePlugins` manifest diagnostics;
  builds still enumerate all eligible plugins. The lasting contract is in
  [plugin management](../Workspace/Plugins.md#editor-plugin-management).
- Checkbox latency was caused by synchronous full-workspace resolution and
  per-row admission on each edit. Edits now use cached metadata and only the
  in-memory dependency graph. Full disk checks remain at open/refresh, save and
  restart. The regression moves descriptor and inventory directories away after
  opening, performs 4,000 successful edits and confirms restart still performs
  fresh disk checks. Its recorded Debug fixture sample is 212,985 microseconds
  total (about 0.053 ms/edit), not a general frame-time guarantee. Raw test
  properties are in `Build/PluginStage5/native-plugin.xml`.
- Both all-target builds pass, covering the shared Projects API/request changes,
  native/Python selection integration, ordinary workspace consumers, final
  MainFrame presentation and both plugin fixture variants. Logs:
  `Build/.agent-state/logs/20261112-194141-945299-26780-cmake.log` and
  `20261112-194213-124710-32068-cmake.log`.
- Native PluginResolutionTests pass 13, including unchecked dependencies,
  independent checked defaults, draft isolation, unavailable files during edits,
  compatibility/build diagnostics, save failure and restart drift checks. Shared
  golden cases cover direct/transitive blocked requests and an unaffected plugin.
  Log: `Build/.agent-state/logs/20261112-194122-427008-28808-PluginResolutionTests.log`.
  Python plugin/build/shared-descriptor coverage passes 90 with 1 privileged
  symbolic-link skip (`Build/PluginStage5/python-plugin-final.xml`). An additional
  descriptor-wide schema scan hits an existing generated negative-test descriptor
  containing `broken` under Engine/Binaries; that broad scan is not used as
  plugin acceptance evidence.
- Existing document-exit protections pass 3 focused EditorShellTests cases;
  all 4 LaunchProcessBoundaryTests pass, including explicit-project deferred
  relaunch consumption and occupied-project admission. Logs use prefixes
  `191346-468308-30504` and `191350-862387-24296` under
  `Build/.agent-state/logs/`; the document/relaunch implementation is unchanged
  by later checkbox/selection refinements.
- Editor and Game each pass 13 process invocations: ownership-parent/child,
  readonly, stale, disable/observe-disabled, enable/observe-enabled,
  save-blocked/observe-blocked, re-enable/observe-enabled, missing-build and
  observe-disabled again. Each save retains the current frozen modules and mount
  revision. Blocked requests remain checked on reopening and load no dependent
  native modules in the next process. Failed writes preserve drafts/file bytes;
  a missing build permits a valid save but blocks restart readiness. Build graph
  and inventory hashes remain unchanged across toggles. Reproduction/raw evidence:
  `Build/PluginStage5/qualify.py`, `processes.json`, per-mode logs and
  `<variant>-no-build-hashes.json`.
- Four actual Editor/Game hosts pass three ticks and clean shutdown, testing
  enabled and blocked-request configurations. Enabled modules start/stop in
  dependency order; blocked native plugins never start, startup logs their reason,
  and the hosts remain usable without rebuilding. No error/fatal/Vulkan validation
  diagnostics were accepted. Evidence: `Build/PluginStage5/hosts-final.py`,
  `hosts-final.json` and matching host logs/traces. Affected selection expands
  to all ordinary tests for fixture/new-source/build inputs; both all-builds,
  owner-specific regressions and real hosts bound this feature. No new GPU
  algorithm or native test infrastructure requires broader execution.
- The user exercised the simplified checkbox window and reported dependency
  affordance and edit latency issues; both are corrected and covered above.
  Final visual interaction, Save/Cancel and dirty-document restart/cancel were
  handed to the user and subsequently accepted as recorded above. No additional
  agent-run UI interaction is claimed. Documentation and all-plan validation pass.

Stage 4 acceptance (2026-11-12, Win64 Debug, windows-msvc-x64):

- Tested base `6f56507e2265fddde226e76d3bf85c2f9a5c6c96` plus final source and
  fixture changes. SHA-256 inputs, preset identities, qualified workspace and
  generated config/inventory inputs are retained in ignored
  `Build/PluginStage4/final-inputs.json` and `workspace-qualified.json`.
  Temporary workspace membership and saved user application configs were
  restored. Later documentation edits do not change relevant executable inputs.
- Both runtime variants passed all-target builds, covering the new Core config
  exports, ShaderBuild-to-Projects dependency, ordinary workspace consumers,
  DurinAssetTool and plugin fixtures. Final logs:
  `Build/.agent-state/logs/20261112-182251-342843-34076-cmake.log` and
  `Build/.agent-state/logs/20261112-182253-650337-32420-cmake.log`.
- Seven independent resource processes passed: fixture creation, direct mounted
  admission/loading, catalog scan/loading, physical plugin relocation, disabled
  plugin-class loading, mount collision, and descriptor path escape. The virtual
  package identity and early Shader source fingerprint survive relocation without
  a build or inventory republication. Plugin saves return ReadOnlyMode; mount
  dependency checks allow Game-to-plugin and required plugin-to-plugin edges,
  and reject plugin-to-Game or undeclared sibling edges. Disabled defaults and
  roots disappear; failed plugin-class loads preserve source bytes and publish
  no resident package. Results, traces, scripts and logs are under
  `Build/PluginStage4/resources.json`, `resources.py`, and matching log files.
- Four actual Editor/Game host processes passed three ticks and clean shutdown.
  Provider-start assertions verify that dependent plugin defaults override base
  plugin defaults, project settings override plugin values, and saved user values
  override project settings. A no-project Browser and real DurinAssetTool check
  also passed; the tool reports the plugin and project-owned plugin-class assets
  as compatible. No host error/fatal/Vulkan validation diagnostics were accepted.
  Reproduction/evidence: `Build/PluginStage4/hosts.py`, `hosts.json`,
  `tool-browser.json` and `asset-tool-check.log`. These 13 process cases qualify
  source startup/resource integration, not plugin-bearing cooked deployment.
- Native regressions passed: PluginResolutionTests 9, RenderShaderBuilderTests
  37, AssetPackageTests 240, PrimaryAssetTests 29, project game settings 5,
  YAML documents 8, and CoreFileSystemTests 57 with 2 link-privilege skips.
  The ShaderBuilder test compiles the repository fixture through a plugin virtual
  root, advances the existing reload generation after source edits, verifies
  changed output, and verifies derived files stay outside its source directory.
  Actual resource processes separately verify descriptor-driven early roots.
  Existing shader dependency/manifest/cache cases remain passing; neither cache
  identity nor source-reload policy was replaced.
- Native logs use the `20261112-181344` (plugins), `180508` (ShaderBuilder),
  `182231` (packages), `182249` (primary assets), `181331` (project settings),
  `181342` (YAML) and `181536` (filesystem) prefixes under
  `Build/.agent-state/logs/`. Source relevant to the reused focused results is
  unchanged; final package/settings tests and process cases cover the later
  diagnostic/exception-handling refinements. Python plugin/build/shared-descriptor
  regression passed 88 with 1 symbolic-link privilege skip;
  `Build/PluginStage4/python.xml`. Affected selection expands to all due to the
  new public header, module declaration and fixture inputs. Both all-builds,
  owner-specific regressions and real processes bound this change; no new GPU
  algorithm or native test infrastructure was introduced.
- Lasting resource/configuration contracts and fixture instructions are updated.
  Maps merge recursively; later scalars/sequences replace. Bootstrap logging
  retains its existing early saved-config policy; subsequent module consumers
  receive the final layered document. Ordinary saves still edit raw project
  configuration. Native macOS and privileged symlink/junction execution remain
  unqualified; cooked plugin assets/shaders/config staging remains Stage 6 work.
  Document and all-plan validation pass.

Stage 3.1 acceptance (2026-11-12, Win64 Debug, windows-msvc-x64):

- Tested base `925481217d138c4e0408d33ddf49d88d35def2b7` plus the final
  source/fixture changes. SHA-256 inputs, target variants and the reproduction
  script are retained under ignored `Build/PluginStage3_1/final-inputs.json` and
  `qualify.py`. Later documentation edits do not change tested source inputs.
  Temporary fixture membership in Durin.dworkspace was restored.
- Both `build all` gates passed with all fixture overrides disabled:
  Win64-Debug-DurinEditor and Win64-Debug-DurinGame. All eligible fixture modules
  and cross-DLL reflected consumers built; Game excludes the Editor companion.
  Logs: `Build/.agent-state/logs/20261112-173308-333435-39444-cmake.log` and
  `Build/.agent-state/logs/20261112-173848-237025-6104-cmake.log`. Ordinary
  Engine/Sandbox/RoadWeaver targets were included. Target/output ownership and
  platform/configuration layout are unchanged; Stage 2 Release isolation
  evidence remains applicable alongside current Python variant/configuration
  coverage. An Editor fixture DLL snapshot taken during the Game build remained
  stable through its completion/publication, recorded in
  `editor-isolation-hashes.json`.
- 28 independent process cases per variant passed (56 total), including six
  actual Editor/Game hosts completing three ticks and clean shutdown. Enable,
  persisted disable, and re-enable all work without configure/build or inventory
  publication. `*-no-build-hashes.json` records unchanged build-graph and inventory
  hashes across the cycles. Browser starts no plugins; edits preserve active
  modules and mount revision; reflected providers retain phase/dependency order.
- Both variants accept extra unselected entries with missing files and start
  a Runtime-only host while the built, disabled consumer DLL is unavailable.
  They reject all six incompatible identity fields, legacy inventory version,
  stale selected descriptor inputs, wrong owners, duplicate entries, path escape,
  missing selected modules/files/dependencies/inventory, disabled native imports,
  and conflicting startup overrides. Expected failure probes confirm admission
  failure before startup; provider-failure probes confirm bounded cleanup.
  Logs, traces and case results are `DurinEditor-probes.json`,
  `DurinGame-probes.json`, and corresponding files under `Build/PluginStage3_1`.
- Full DHT Python regression passed 523 with 2 symbolic-link privilege skips;
  DevTool workspace/build-configuration regression passed 55. Reports:
  `Build/PluginStage3_1/python.xml` and `devtool.xml`. Final native
  PluginResolutionTests passed 8/8 after restoring ordinary workspace membership;
  log: `Build/.agent-state/logs/20261112-174242-323892-32844-PluginResolutionTests.log`.
  Affected selection expands to all because build/inventory infrastructure
  changed. Both all-builds, the full DHT suite, DevTool regressions, and selected
  native/real-process coverage bound this change; Core loading/lifetime and
  startup insertion points are unchanged, so Stage 3 worker-drain/later-provider
  and process-boundary evidence remains applicable. GPU behavior is unchanged.
- Stage 2 disable-removes-target/empty-inventory checks and Stage 3
  override-causes-stale-inventory/rebuild checks are historical and replaced by
  the new target-stability and subset-admission evidence above. Other earlier
  acceptance evidence is retained. Ordinary module descriptor checking remains
  conservative across the workspace; unused plugin files and inactive plugin
  metadata do not invalidate the selected subset. Native macOS and privileged
  symlink/junction execution remain unqualified. Lasting build/startup contracts
  and fixture instructions are updated; document and all-plan validation pass.

Stage 3 admission checkpoint (2026-11-12, not accepted):
- Added read-only native inventory admission and a focused regression covering
  success, stale selection, incompatible identity, missing file, path escape,
  wrong owner and unselected module; failure preserves the previous output.
- Changed-document and all-plan validation passed. Outside-sandbox
  `test PluginResolutionTests` passed 8/8 on Win64-Debug-DurinEditor with the
  final admission source and fixture changes on base `caede8f7f6a5ab92aa29c3d73e933b07fde90ca6`. Build log:
  `Build/.agent-state/logs/20261112-152152-509746-14984-cmake.log`;
  native log:
  `Build/.agent-state/logs/20261112-152207-054854-14984-PluginResolutionTests.log`.
  The earlier sandbox C1902 failure was resolved by the authorized external run.
  This validates the standalone admission API; host integration and process
  qualification remain open. No Stage 3 acceptance checkbox is closed.

Stage 3 acceptance (2026-11-12, Win64 Debug, windows-msvc-x64):

- Tested base `2e508c939207c993e165839443290cd8d5bd570f` plus the final
  Stage 3 source/fixture changes. Input SHA-256 hashes are retained under ignored
  `Build/PluginStage3/final-inputs.json`; later documentation edits do not alter
  these inputs. Temporary fixture workspace membership was restored.
- Both runtime variants passed `build all` with the fixture and again with
  ordinary Engine/Sandbox/RoadWeaver membership. Final ordinary build logs:
  `Build/.agent-state/logs/20261112-163825-838185-31252-cmake.log` and
  `Build/.agent-state/logs/20261112-163912-963944-3016-cmake.log`.
  This covers the shared Core layout and all direct project API consumers.
- 46 independent fixture processes passed: 13 admission/startup probes per
  variant, four persisted-edit/rebuild processes per variant, five complete
  Editor/Game host runs, six later-provider/stale/missing-file host failures,
  and one existing no-plugin staged Game run. Normal hosts exit after three
  ticks with code 0; expected failures exit 1 and record bounded teardown.
  Browser starts no plugins. Runtime reflection is available at its phase.
  Editor starts Runtime, dependent EngineReady, then EditorReady modules; Game
  omits the Editor module. Shutdown reverses completed startup order.
- Probes reject all six incompatible native identity fields, stale selection,
  missing inventory/DLL/runtime dependency, unknown explicit registration,
  early name-based loading and runtime unload. Provider failure cleans its
  partial state once; later-provider failure also stops the earlier provider.
  Saving disabled overrides leaves current module membership and mount revision
  unchanged; the next process rejects stale artifacts, then observes no native
  plugins after rebuild despite stale DLL files. Re-enable/build restores startup.
- Raw case results, traces and logs are in `Build/PluginStage3/`:
  `DurinEditor-probes.json`, `DurinGame-probes.json`, both `*-cycle.json`,
  both `*-hosts.json`, and `host-extra.json`. The reproduction scripts are
  `qualify.py`, `cycle.py`, `hosts.py`, and `extra.py` there. The tracked
  fixture/probe lives under `Engine/Tests/Fixtures/Plugins/PluginFixture`.
- Final source regression: PluginResolutionTests 8/8, CoreConcurrencyTests
  `FModule*` 11/11 (including owned worker drain and failure retention), and
  LaunchProcessBoundaryTests 4/4. Final native logs use the
  `20261112-162755`, `20261112-162805`, and `20261112-162810` prefixes in
  `Build/.agent-state/logs/`. Broader affected selection is bounded by both
  all-builds, lifecycle regressions, and real host/DLL process coverage; GPU
  rendering behavior and the native test framework are unchanged.
- A supplemental three-case LaunchStartupCharacterizationTests selection built,
  but did not execute: its corrected characterization-mode invocation was
  blocked before creation by disconnected automatic approval. It contributes
  no passing evidence. Corresponding Browser/ordinary-host behavior is covered
  by the direct host cases above; no whole-suite claim is made.
- Lasting startup and source/staged boundaries are documented in
  [Plugins](../Workspace/Plugins.md#frozen-native-startup) and
  [Runtime lifecycle](../Runtime/Application/RuntimeLifecycle.md).
  Missing/stale artifacts never trigger an implicit build. The existing Browser
  relaunch/unsaved-work owner is retained. Plugin-bearing staged deployment
  requires Stage 6; no-plugin staged startup remains source-independent.
  Native macOS execution remains unqualified.

The 2026-11-12 convergence keeps these stages unchanged: typed descriptor fields
are the persistent authority, the manager owns both editable and active state,
and public bool operations report failures directly. Discovery treats a plugin
descriptor directory as a traversal boundary, matching UE; descriptors below
that boundary are ignored rather than audited. Module files are consumed only
for selected, host/platform-eligible plugins. Selection exposes typed values
alongside its canonical artifact projection.

Convergence acceptance (2026-11-12, Win64 Debug, windows-msvc-x64):

- Tested base `55ee89820` plus the convergence source/fixture changes. Final
  source hashes are retained in ignored
  `Build/PluginStage1_1/convergence-inputs.json`; documentation-only edits do not
  change these inputs. No stage was added and Stages 2-7 remain open.
- `build all` passed for Win64-Debug-DurinEditor and Win64-Debug-DurinGame after
  the public interface changes. Final build logs are
  `Build/.agent-state/logs/20261112-121153-586846-28620-cmake.log` and
  `Build/.agent-state/logs/20261112-121255-360278-39460-cmake.log`.
- PluginResolutionTests passed 6/6, including 49 shared plugin cases and the
  typed edit/save, unchanged-override, active-state isolation, and typed-result
  projection checks. Python descriptor/plugin/workspace/configuration/document
  regressions passed 213 tests with 2 symbolic-link privilege skips.
- CoreFileSystemTests passed 57 with 2 symbolic-link privilege skips.
  LaunchProcessBoundaryTests passed 4/4. The selected bounded-exit,
  missing-explicit-project, automated-history, and empty-default-Level startup
  characterization cases passed 4/4. Reports use the `convergence-` prefix under
  `Build/PluginStage1_1`; process-boundary coverage precedes only internal
  manager/consumed-path refinements and remains applicable to its unchanged
  relaunch and lease behavior.
- The Game smoke refreshed staged binaries from the final Game build, reused
  the existing complete Cook resources and EmptyGame fixture, and exited after
  3 ticks with exit code 0 and no error/fatal/validation diagnostics. Log:
  `Build/PluginStage1_1/convergence-game.log`.
- Document and all-plan validation passed. Native macOS and privileged symbolic
  link/junction execution remain unqualified; plugin native loading is still
  outside this acceptance.

Stage 1 acceptance (2026-11-11, Win64 Debug, windows-msvc-x64):

- Tested base `170364896642edb210e0fe3ce60ed06a2e010dee` plus the Stage 1
  working-tree changes. Final relevant inputs are recorded with SHA-256 hashes
  under ignored `Build/PluginStage1/inputs.json`. Subsequent status/documentation
  edits do not change the validated source, fixtures, schemas, or environment.
- PluginResolutionTests passed 2/2 native tests, including all 46 shared
  canonical-selection/error fixtures. Editor and Game are resolver query inputs
  within this Win64 Editor native executable; this is not Game-process loading
  qualification. Raw native output: `Build/PluginStage1/native.log`.
- Python plugin/descriptor/workspace/scaffolding/configuration regression:
  156 passed, 1 skipped. Includes both readers, build/selection schemas, existing
  Engine/Sandbox/RoadWeaver roots, relocated/reordered inputs, fingerprint
  invalidation, and a Runtime plugin consuming the real Engine descriptor graph
  in Editor/Game selections. Report: `Build/PluginStage1/python.xml`.
- CoreFileSystemTests: 57 passed, 2 skipped, including active-project override
  capture and invalid-override rejection. Raw output:
  `Build/PluginStage1/core-filesystem.log`. All three skipped tests require
  symbolic-link creation privileges unavailable on this host; lexical path
  escape and ordinary ownership validation passed. Native macOS execution and
  privileged link/junction execution remain unqualified.
- DurinLauncher, DurinAssetTool, Sandbox, and RoadWeaverEditor built successfully
  with Win64-Debug-DurinEditor. Their relevant shared project API remained
  unchanged during the final resolver-only refinements. Build logs are retained
  under `Build/.agent-state/logs/20261111-161439-532335-36040-cmake.log`,
  `20261111-161628-858099-46412-cmake.log`,
  `20261111-161753-947340-43172-cmake.log`, and
  `20261111-161827-170599-26960-cmake.log` in that directory.
- Affected-test analysis expands to all because Engine's project descriptor and
  native-test registration changed. Bounded source-plugin and Core tests plus
  direct shared-API consumers cover Stage 1; no generated plugin target
  selection/startup is wired yet. The explicit all-build gate stays in Stage 7.
- Lasting Stage 1 rules, phase insertion points, and schema ownership are in
  [Plugin metadata and selection](../Workspace/Plugins.md). Documentation and
  all-plan validators pass; final verification receipts are retained under
  `Build/PluginStage1/documentation.log`.

Existing foundations include `FModuleManager`, `.dmodule` generation, DHT,
extension content mounts, initialization-time shader mounts, modular features,
and World subsystem registration. They are reusable mechanisms, not evidence
that plugin discovery or enablement already works.

## Goal

A project can enable a plugin containing Runtime and Editor modules, content,
shaders, and configuration. Source builds compile every discovered workspace
plugin eligible for the target platform and host, regardless of project
enablement. Startup selects a validated subset of those available artifacts.
Enablement edits affect the next process; the current process keeps its original
plugin set. A cooked Game runs with only its declared plugin closure.

## Selected Scope And Decisions

- Support repository/source builds first, on the engine's supported platforms.
  First executable qualification is Win64; qualify platform-independent metadata
  everywhere covered by existing CI. macOS native execution remains an explicit
  outstanding gate until measured there.
- Discover `Engine/Plugins` and `<ActiveProject>/Plugins` recursively by
  `.dplugin`; stop descending at each plugin root. Reject case-insensitive
  plugin/module identity collisions; ignore descriptors beneath a plugin root. Do not provide
  implicit project-over-engine replacement or arbitrary search paths initially.
- A plugin is an optional distribution/enablement container. A module remains
  the compilation, reflection, and native loading unit. Existing project modules
  continue to work without becoming plugins.
- Introduce a small runtime `Projects` module depending on `Core`, with no
  Engine, Renderer, or editor dependencies. It owns plugin descriptors,
  project/plugin descriptors, selection, and manifest admission. Keep generic
  loading in Core's `FModuleManager`; Core must not depend back on Projects.
  Stage 1 retained ordinary-project management in Core as an interim boundary.
  Stage 1.1 moves descriptor/management policy to Projects while preserving
  Core's low-level path context and mount/module mechanisms.
- `.dplugin` owns schema version, stable ID, display/version metadata, contained
  module paths and host/platform eligibility, required plugin dependencies,
  optional content/shader/config roots, and module startup phases. `.dmodule`
  remains authoritative for link dependencies and reflection headers. Version
  metadata does not promise cross-version native compatibility.
- `.dproject` owns explicit plugin enable/disable overrides. Unmentioned plugins
  are disabled unless required by the enabled closure. Explicit disable prevents
  dependency activation: requested consumers stay inactive with a diagnostic
  while retaining their true overrides, as refined in Stage 5. Do not add
  optional dependency/version solving in this scope.
- Project enablement controls startup and deployment membership, not compilation.
  Build all eligible plugins discovered under Engine and workspace-project plugin
  roots through the existing module pipeline; preserve platform/host filtering
  and incremental compilation. Game builds exclude Editor modules. Keep build
  availability, frozen startup selection, and cooked deployment closure distinct.
- Use versioned schemas, common fixtures, and canonical resolved manifests to
  keep Python build-time and C++ startup implementations equivalent. Do not
  require embedding Python in Game or sharing implementation across languages.
- Freeze a process's plugin identity/selection at project admission, before
  plugin-dependent content or shader consumption. Project Browser can remain
  usable without a selected project. Switching to a different plugin set
  requires restart; do not retrofit project-switch hot loading.
- Native plugins target the exact engine build identity, platform, architecture,
  configuration, runtime variant, and toolchain/ABI profile. Validate before
  loading plugin libraries. Source plugins may require configure/build before
  restart when artifacts are missing or incompatible, or source/build inputs
  change. Changing only enablement requires restart, without configure/build or
  inventory republication when compatible artifacts already exist.
- Content-only plugins are supported. Plugin content uses stable virtual roots
  such as `/Plugins/Water/`; physical relocation must not change asset identity.
- Initial phases are provider registration before project content, engine-ready,
  and editor-ready. Shader root declaration is descriptor-driven and may need
  to occur earlier than provider callbacks. Phase names and exact hook mapping
  are frozen in Stage 1 against the actual startup sequence.
- The active set is immutable. No live enable/disable, DLL unloading, hot reload,
  reflected-type replacement, object migration, or live mount changes are added.
  Normal process-exit cleanup and partial-startup cleanup are still required.
- Binary-only third-party SDK distribution, marketplace/install/update services,
  sandboxing, general Renderer extension APIs, and Water implementation are out
  of scope. Water motivates the integration fixture but is not a prerequisite.

## Ownership And Related Work

| Area | Primary owner | Integration owners |
| --- | --- | --- |
| Discovery, descriptors, selection, manifest admission | New `Projects` runtime module | Core project/path primitives; DevTool and DHT Python descriptor readers |
| Ordinary-project descriptors, current project, status queries, edit-session ownership | Projects after Stage 1.1 | Core mechanisms; Launch lifecycle; editor history/restart policy |
| Module graph, reflection, generated targets | DurinHeaderTool and CMake | DurinDevTool; `.dproject`/`.dmodule` schemas |
| Process/project startup and exit | Launch, Engine, DurinEd | Projects; Core ModuleManager |
| Content mounts, catalogs, packages | Core, AssetRegistry, Engine | Projects supplies immutable mount definitions |
| Shader roots, source builds, cooked programs | ShaderBuild, RenderCore, Engine | Projects supplies roots and selection identity |
| Plugin configuration | Core configuration mechanisms | Projects defines ordered plugin inputs; Engine applies settings |
| Enablement UI | DurinEd, MainFrame | Projects; existing project-edit ownership and transactions |
| Cook and deploy closure | Engine, DurinDevTool | DHT/CMake binary inventory; ShaderBuild |

Coordinate startup hooks with the active
[Runtime Startup And Scene Activation plan](RuntimeStartupAndSceneActivation.md).
Its existing startup/readiness ownership must not be replaced by a second
plugin-specific startup state machine. The implemented
[Runtime lifecycle](../Runtime/Application/RuntimeLifecycle.md) remains the
starting contract until that plan changes it.

The [DurinEditor SDK Packaging plan](DurinEditorSdkPackaging.md) owns installed
SDK layout, imported engine targets, and external C++ consumer distribution.
This plan must expose reusable identity/inventory metadata without making the
source-plugin milestone depend on completing that SDK plan.

The [Game Export Pipeline plan](GameExportPipeline.md) owns the follow-up migration
of Win64 startup roots, delivery layout, generic target receipts, project Cook
policy and unified export entrypoint. It preserves this plan's immutable plugin
selection/lifetime and historical deployment receipts. Native macOS staging and
execution qualification remain open here; the new export plan does not close them.

Follow [Workspace and projects](../Workspace/WorkspaceProjects.md),
[Build system](../Development/Build/BuildSystem.md),
[Build and run](../Agents/BuildAndRun.md), and
[Testing](../Agents/Testing.md) when selecting implementation coverage.

## Implementation Stages

### Stage 0: Define scope and stage acceptance

Dependencies: None.

Outcome: A reviewable implementation sequence with explicit restart semantics.

- [x] Select source/content plugin scope and deferred capabilities.
- [x] Assign descriptor, module, startup, asset, shader, and UI ownership.
- [x] Define dependencies and observable acceptance gates for Stages 1-7.

Acceptance: This plan states the selected behavior and existing-plan boundaries;
completion here does not claim an implemented plugin subsystem.

### Stage 1: Implement descriptors and deterministic plugin resolution

Dependencies: Stage 0.

Modules/tools: New Projects; Core project descriptor integration;
DurinHeaderTool schemas/readers; DurinDevTool descriptor/workspace services.

Outcome: A project resolves to one validated, explainable plugin selection.

- [x] Register Projects in Engine's project/module graph without introducing
  Core-to-Projects or Projects-to-Engine dependencies.
- [x] Add `.dplugin` schema and project overrides; preserve existing descriptors
  with no Plugins field. Define absent versus explicitly empty values.
- [x] Implement discovery, containment checks, duplicate detection, host/platform
  filtering, required dependency closure, cycle checks, and stable ordering.
- [x] Validate cross-plugin `.dmodule` references against declared plugin
  dependencies. Reject dependencies on disabled/ineligible plugin modules and
  Runtime-to-Editor links, including transitive violations.
- [x] Freeze phase ordering and its mapping to startup hooks, including a rule
  rejecting an earlier-phase consumer of a later-phase provider.
- [x] Define canonical selection/build manifests with schema version, logical
  owners, reasons for activation, inputs/fingerprints, and binary identity.
  Separate machine-local resolved paths from portable identity.

Acceptance gate:

- [x] Shared fixtures produce equivalent canonical selections in Python and C++:
  empty project, content-only plugin, required closure, Editor/Game selection,
  explicit-disable conflict, cycle, duplicate ID/module, missing descriptor,
  path escape, invalid phase edge, and unknown schema version.
- [x] Existing Engine, Sandbox, and RoadWeaver project descriptors retain their
  existing effective module roots when no plugins are enabled.
- [x] Errors identify the descriptor and complete failing dependency chain.

### Stage 1.1: Unify project management and descriptor ownership

Dependencies: Stage 1.

Status: Completed on 2026-11-12.

Modules/tools: Projects, Core, Launch, Engine, DurinEd, MainFrame; all direct
project API consumers, including DurinAssetTool and project/test targets in
Durin.dworkspace; DHT and DevTool descriptor readers and plan-context tooling.

Outcome: Projects manages ordinary projects and plugins through one descriptor
model and an explicit current-project service, without introducing live project
switching or changing existing startup/readiness ownership.

#### UE Alignment And Selected Boundaries

Use [UE Projects](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Projects)
and [IProjectManager](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Projects/IProjectManager)
as naming and responsibility references. Match principal concepts and observable
semantics, including bool results, status output parameters, and explicit failure
reason outputs where UE uses them. Retain Durin's `.dproject`/`.dmodule` formats
and native string/value vocabulary; internal helpers may use structured errors.
There is no requirement to retain `std::expected` or Durin error enums on these
principal public APIs. This alignment does not promise UE source/binary compatibility.

| Surface | Selected Stage 1.1 behavior |
| --- | --- |
| FProjectDescriptor | Typed project metadata, shared read/load/validation, module roots, plugin references, and mount declarations; independent of current-project mutation |
| IProjectManager::Get | Single service entry for ordinary-project management; Projects owns its implementation |
| LoadProjectFile / GetCurrentProject | Load and validate a descriptor, then publish the current descriptor; separate descriptor data from derived paths/session state |
| QueryStatusForProject / QueryStatusForCurrentProject | Read-only project status using the same descriptor validation; do not load modules, change current project, or equate valid metadata with compatible binaries |
| LoadModulesForProject | Route existing ordinary-project module loading through the manager at the existing host startup point; Core still executes native loading |
| SaveCurrentProjectToDisk / IsCurrentProjectDirty | Explicit descriptor editing/save contract, preserving unaffected fields and using atomic publication; failed save retains the dirty state |
| SetPluginEnabled / RemovePluginReference | Edit requested project configuration only; removing an override differs from writing false; no change to the process's admitted active plugin set |

The descriptor model may share module eligibility/loading-phase vocabulary with
plugin metadata, following UE's FModuleDescriptor concept. Do not import every
UE host type, phase, settings API, or IPluginManager method without a Durin need.
Plugin module phase execution and native compatibility admission remain Stage 3;
dependency-aware plugin UI and restart handling remain Stage 5.

- Projects owns project descriptors, status queries, current-project context,
  descriptor edit state, and project edit-session ownership. Keep descriptor
  inspection usable without initializing Engine, Renderer, editor UI, or Python.
- Core owns JSON/file/process primitives, generic FModuleManager, FPaths queries,
  immutable mount registry, and containment/dependency validation. Projects
  supplies validated path context and mount definitions; Core must not interpret
  `.dproject` fields or acquire a dependency on Projects.
- Projects may own recent-project record/persistence values. DurinEd/MainFrame
  owns interactive history ordering, auto-restore choice, and unsaved-work policy;
  Launch owns startup argument selection and deferred process relaunch. Game
  and command-line tools must not accidentally inherit interactive editor policy.
- Build tools retain a Python representation of the same contract. Share schemas
  and fixtures with C++; do not embed Python in the runtime or require C++ tools
  merely to inspect project metadata.

#### Implementation Tasks

- [x] Add the typed project descriptor and UE-aligned project manager interface,
  separating persistent descriptor state from derived context and session policy.
- [x] Move applicable APIs/data from Core's Project/ProjectError/ProjectHistory
  facilities to their selected owners. Migrate all source and test consumers in
  every workspace project; add explicit `.dmodule` dependencies where required.
  Do not leave a permanent Core facade depending back on Projects.
- [x] Make current-project admission, status/history inspection, and plugin
  resolution reuse the same project reader. Remove duplicate `.dproject` field
  interpretation from Core path/mount code and the native plugin resolver.
- [x] Preserve one validated project/mount snapshot: failed inspection/admission
  cannot publish partial current-project, path, or mount state. Retain the
  existing immutable mount lifetime and restart-required project switching.
- [x] Define descriptor edit/save semantics, unknown/schema-field handling,
  project ownership checks, and failure recovery. Keep requested descriptor
  edits distinct from the active process selection and preserve unrelated data.
- [x] Keep project module load order, reflected-provider readiness, initial
  content scan, and shutdown/relaunch placement equivalent to existing behavior;
  coordinate with RuntimeStartupAndSceneActivation rather than adding a new loop.
- [x] Align Python/C++ absent-versus-empty behavior: omitted BaseModules uses
  ModuleDirs; explicit empty BaseModules means no base roots; omitted/empty
  Plugins means no explicit overrides. Preserve existing checked-in project roots.
- [x] Extend plan-context stage discovery to recognize dotted numbers so Stage
  1.1 can be selected independently and its exact provenance is preserved.
- [x] Publish lasting project-management ownership/API rules in Workspace and
  relevant lifecycle documents; revise the interim boundary recorded by Stage 1.

#### Acceptance Gate

- Initial tooling checkpoint: focused Python regression covers dotted-stage
  identifiers (including distinct 1.1/1.10), completed-parent task boundaries,
  JSON/Markdown context, and omitted/empty BaseModules in DHT/DevTool. Raw report:
  `Build/PluginStage1_1/python.xml`: 172 passed, 2 skipped because directory
  symbolic-link creation is unavailable. Tested base `3563d2bbd` plus this
  checkpoint's working-tree changes. The repository's plan-context command independently
  selects Stage 1.1; captured JSON is `Build/PluginStage1_1/context.json`.
  Document and all-plan validation passed. This is not native project-manager acceptance;
  the shared native descriptor fixtures, ownership migration, process smokes,
  and Editor/Game all-build gates below remain open.

- Second checkpoint (2026-11-12): PluginResolutionTests passed 3/3, including
  the shared 46 plugin fixtures and new descriptor/status-query/atomic-save/
  failed-save-dirty-state coverage. CoreFileSystemTests passed 57 with 2
  symbolic-link privilege skips. Raw logs:
  `Build/.agent-state/logs/20261112-103551-552573-20308-PluginResolutionTests.log`
  and `Build/.agent-state/logs/20261112-103242-938457-33672-CoreFileSystemTests.log`.
  DurinLauncher also built successfully after the DLL export/header migration;
  final build output: `Build/.agent-state/logs/20261112-103552-940354-28780-cmake.log`.
  This checkpoint does not close the transactional admission, shared schema,
  lifecycle-policy, or Editor/Game all-build acceptance gates.

#### Final Acceptance (2026-11-12)

- Tested base `cfd97de477086d2cd22462596cc90cee1e5900bc` plus this stage's final
  working-tree inputs. Source/fixture hashes and configuration/artifact identities
  are retained under ignored `Build/PluginStage1_1/final-inputs.json`.
  Final documentation/status edits do not alter the relevant validated inputs.
- Win64 Debug Editor and Game `all` builds passed, including the standalone
  asset-tool consumer. Final logs in `Build/.agent-state/logs/`:
  `20261112-110554-437143-31484-cmake.log` and
  `20261112-110616-063599-17068-cmake.log`.
- Python regression: 210 passed, 2 directory-symlink privilege skips;
  `Build/PluginStage1_1/final-python.xml`. Native project/manager tests: 5/5,
  including 18 common project fixtures and all 46 unchanged plugin fixtures;
  `Build/PluginStage1_1/native-projects.xml`. Core file/project/mount regression:
  57 passed, 2 symlink privilege skips; `Build/PluginStage1_1/native-core.xml`.
- Launch process boundaries: 4/4, including exclusive project ownership and
  consume-once deferred relaunch arguments; `Build/PluginStage1_1/native-launch.xml`.
  Selected characterization: Browser bounded exit, missing explicit project,
  automated history preservation, and empty-default-level startup all passed
  (4/4); `Build/PluginStage1_1/native-startup.xml`. Unrelated command-handler
  characterization was not expanded into this selection.
- Sandbox and RoadWeaver Editor each completed 3 ticks and clean shutdown, with
  no error/fatal/Vulkan validation diagnostics. Logs under the same log directory:
  `20261112-110620-118661-28068-DurinEditor.log` and
  `20261112-110624-584498-34824-DurinEditor.log`.
- Game completed 3 ticks and clean shutdown from an isolated staged runtime with
  valid cooked default material, ShaderLibrary and application configuration;
  `Build/PluginStage1_1/staged-game.log`. Cook retained one package and published
  a validated manifest-last generation under `Build/PluginStage1_1/CookedGame`.
  An initial uncooked Game smoke returned zero but logged missing deployment
  resources and a Vulkan acquisition diagnostic; it was not accepted as a pass.
  Repeating with complete staged inputs produced no error/fatal/validation
  diagnostics. No RHI behavior or user asset was changed to mask that failure.
- Manager queries preserve actual mount-storage addresses as well as revision,
  path/context and ownership. Failed loads retain the active snapshot; failed
  atomic saves retain pending dirty state. Principal APIs now match UE bool and
  output-parameter conventions, with native strings and private structured errors.
- Core/Projects dependency guards pass; Projects remains Core-only. Native
  descriptor tests run without Engine/editor/Python. Dotted-stage selection and
  existing project roots remain covered. Affected-test analysis expands to all
  due to declaration changes; bounded behavior tests plus both explicit all-build
  gates cover this migration. GPU performance qualification is not claimed.
- Native execution is qualified on this Win64 host only. macOS execution and
  privileged symlink/junction paths remain unqualified; these do not imply a
  completed cross-platform Stage 7 gate. Document and all-plan validation pass;
  receipts are in `Build/PluginStage1_1/final-documentation.log`.

- [x] Shared fixtures establish C++/Python agreement on ordinary project fields,
  malformed/duplicate fields, omitted/empty values, plugin references, mounts,
  and Editor/Game root selection. Stage 1 plugin canonical fixtures remain valid
  unless an intentional, documented correction requires updating expectations.
- [x] Status queries leave current context, paths, mounts, module state, and
  history unchanged. Descriptor edits/atomic save preserve unrelated fields;
  failed writes preserve disk contents and pending edit state. Explicit false
  and removed plugin overrides remain observably different after reload.
- [x] Project Browser, explicit project opening, history/auto-restore, edit-session
  exclusion, and deferred relaunch retain their existing contracts. Game/tool
  project admission does not apply interactive history policy. Failed admission
  and normal exit leave no partial context or leaked ownership.
- [x] Engine, Sandbox, and RoadWeaver retain expected project roots and startup
  behavior; non-project Browser startup remains supported. Run focused project,
  plugin, mount, startup/history/relaunch regression coverage and the corresponding
  bounded process smokes, with evidence and platform skips recorded explicitly.
- [x] Dependency checks prove Core has no Projects/Engine/editor dependency and
  Projects remains Core-only. Ordinary project inspection works in a minimal
  native consumer. Dotted-stage plan-context discovery has focused regression
  coverage and selects Stage 1.1 independently of completed Stage 1.
- [x] Because public project types move across DLL boundaries and many owners
  gain direct dependencies, run `all` builds for Editor and Game at this stage's
  final acceptance. This gate covers the migration's ABI/build-graph risk; reuse
  unchanged passing results in Stage 7 rather than rerunning by commit identity.
- [x] Plan state and lasting documentation match the implemented boundary;
  document and all-plan validation pass. Only then make Stage 2 executable.

### Stage 2: Integrate plugin modules into build and reflection

Historical acceptance: this stage implemented selection-coupled targets and
inventories. Preserve its completed checks and receipts. Stage 3.1 replaces
disable-removes-target behavior and selection-bound inventory publication; the
checks below do not establish the revised contract.

Dependencies: Stages 1 and 1.1.

Modules/tools: DurinHeaderTool; CMake workspace/project/target generation;
DurinDevTool configure/build services; CoreDObject integration fixtures.

Outcome: Enabled plugin modules compile and expose reflected types through the
existing toolchain, without hand-editing Engine's ModuleDirs for each plugin.

- [x] Feed resolved plugin modules to CMake and DHT using the selected workspace
  membership; preserve module ownership independently from project ownership.
- [x] Generate export headers, reflection inputs, targets, include paths, and
  dependencies through existing module mechanisms.
- [x] Namespace generated state and outputs by plugin ownership and relevant
  target/configuration inputs. Reconfigure when discovery or enablement changes.
- [x] Produce a build manifest containing exact binary paths, build identity,
  selection fingerprint, and required native runtime dependencies.
- [x] Create a small repository-owned PluginFixture and a consumer fixture:
  Runtime module with a reflected component/asset, Editor companion, dependent
  plugin, and content-only plugin. Keep tests/fixtures under their owning test
  roots or ignored Build outputs; do not depend on Water development.

Acceptance gate:

- [x] Enabled fixture builds in DurinEditor and DurinGame; Editor companion is
  absent from the Game graph. Its reflected headers and API exports work across
  the fixture/consumer DLL boundary.
- [x] Disable the fixture, reconfigure, and prove its targets are absent and
  stale DLLs do not re-enter the selection. Re-enable and verify regeneration.
- [x] Configuration/variant outputs do not overwrite one another; no-plugin
  project builds remain unchanged in effective membership.

Stage 2 acceptance (2026-11-12, windows-msvc-x64):

- Tested base `e122ede14` plus this stage's final source/fixture inputs. SHA-256
  hashes and preset identities are retained under ignored
  `Build/PluginStage2/final-inputs.json`; final documentation edits do not affect
  those inputs. The repository fixture lives under
  `Engine/Tests/Fixtures/Plugins/PluginFixture` and was copied into ignored Build
  storage for temporary workspace registration. Durin.dworkspace was restored.
- The fixture inventory and cross-DLL consumer built and ran in
  Win64-Debug-DurinEditor, Win64-Debug-DurinGame, and
  Win64-Release-DurinEditor. Runtime and dependent plugin asset classes share
  reflected inheritance across DLLs. The Editor companion is absent from the
  Game graph. Content-only selection requires no native binary. Probes exited 0;
  manifests satisfy the shared inventory schema and every declared file exists.
- Disabling both overrides automatically reconfigured the Editor graph, removed
  the plugin targets, and published an empty binary list despite existing DLLs.
  Re-enabling regenerated targets and the consumer ran successfully. Final
  receipts and probe logs are in `Build/PluginStage2/acceptance-final.json` and
  the same directory. The first isolation check used a pre-reenable Debug hash
  baseline and detected a normal re-link; after establishing a post-reenable
  baseline, a separate Release build left every Debug DLL hash unchanged.
- Ordinary workspace `build all` passed for both Win64 Debug variants. The
  fixture targets and inventory targets disappear when temporary membership is
  removed; ordinary Engine/Sandbox/RoadWeaver targets remain. Final logs:
  `Build/.agent-state/logs/20261112-142153-937431-33440-cmake.log` and
  `Build/.agent-state/logs/20261112-142358-326193-39336-cmake.log`. Release fixture
  build log: `Build/.agent-state/logs/20261112-140140-758751-6840-cmake.log`.
- Full DHT Python tests plus DevTool workspace/build-config/document regressions:
  628 passed, 2 symbolic-link privilege skips. Native PluginResolutionTests:
  7/7 passed, including foreign-workspace root isolation; CoreObjectTests:
  181/181 passed. Reports: `Build/PluginStage2/python.xml`, `native.xml`, and
  `reflection.xml`. Affected selection expands to all because CMake infrastructure
  and unregistered fixture inputs changed. Both all-builds, direct native
  consumers, real DLL fixture execution, and the full DHT suite cover this stage;
  no native test execution framework, GPU behavior, or host startup changed.
- Principal plugin modules use Shared linkage with generated API headers;
  configuration-independent DHT data retains platform/variant/plugin ownership,
  while native outputs also separate configuration. Implemented rules are in
  [Plugin source build integration](../Workspace/Plugins.md#source-build-integration).
  Document and all-plan validation passed. Native macOS remains unqualified;
  runtime admission/loading and complete cooked deployment remain later stages.

### Stage 3: Admit and start the frozen plugin set

Historical acceptance: this stage required an exact selection-bound inventory.
Stage 3.1 replaces that equality/stale-selection rule with selected-subset
admission. Preserve startup phases, frozen membership, loading restrictions,
lifetime/cleanup behavior, and all original evidence below.

Dependencies: Stage 2.

Modules: Projects, Launch, Engine, DurinEd; narrowly scoped Core ModuleManager
and CoreDObject integration changes only where existing hooks are insufficient.

Outcome: Editor and Game load exactly the admitted plugin module set before
their consumers run, and report actionable startup failures.

- [x] Resolve project plugin inputs early enough to prepare immutable roots,
  validate build compatibility, and freeze selection before consumption.
- [x] Check manifests against current descriptors and binary identities before
  native loading. Missing or stale source-plugin artifacts report configure/
  build requirements; never build implicitly inside startup.
- [x] Register exact module paths and execute dependency-aware phase order.
  Ensure plugin reflection/providers are ready before catalog/package loading
  and before World subsystem membership is captured.
- [x] Enforce admitted module membership at loading boundaries so a disabled
  plugin cannot be loaded by guessing its name or by stale output discovery.
- [x] Keep Project Browser admission independent of project plugins. Define the
  restart path for opening a project with a different set, including plugins
  whose shaders require roots unavailable after renderer initialization.
- [x] Integrate partial-failure and normal-exit cleanup with existing startup/
  exit owners; stop callbacks and drain work before their providers disappear.
  Retain native code through the lifetime of plugin objects/reflection metadata.

Acceptance gate:

- [x] Process fixtures verify phase/dependency ordering, reflected-type
  availability, and frozen selection in both Editor and Game.
- [x] Missing DLL, incompatible identity, stale selection, provider-start failure,
  and attempted disabled-module loading fail at the correct boundary with an
  error result and clean teardown. No fallback silently omits required plugins.
- [x] A no-project Browser starts without project plugin initialization; opening
  a different-set project follows the documented restart path.
- [x] Editing overrides during a process does not change its modules or mounts;
  a new process observes the new set after any required build.

### Stage 3.1: Separate plugin builds from startup enablement

Dependencies: Stages 2 and 3.

Status: Completed on 2026-11-12.

Modules/tools: Projects, DHT, CMake, DurinDevTool, build-inventory publication,
Launch integration, and existing plugin fixtures/tests; retain Core loading policy.

Outcome: All eligible workspace plugins are built independently of enablement;
a new process admits only its enabled closure from available compatible artifacts.
Changing only project plugin overrides needs restart, not configure/build.

Selected adjustment (2026-11-12): Enablement is process policy, not build-target
membership. The Stage 2/3 coupling unnecessarily invalidates usable binaries when
only overrides change. Keep those stages complete as historical milestones and
qualify the replacement here before implementing Stage 4. Existing Workspace and
Runtime documents describe current code until this stage updates them.

- [x] Separate build discovery/module enumeration from startup resolution. Build
  all plugins under Engine and declared workspace-project plugin roots eligible
  for the platform/runtime variant, regardless of project overrides. Preserve
  descriptor boundaries, ownership, containment, collision/dependency validation,
  Shared linkage, generated APIs/reflection, output isolation, and ordinary roots.
  Validate eligible build modules even when disabled; do not synthesize an
  all-enabled startup selection or let override conflicts prune build targets.
- [x] Generate targets and native dependency closure for that build set. Make
  normal all-target builds publish its available artifacts without requiring a
  per-project enabled closure. Override-only edits must neither change plugin
  target membership nor require configure/build or inventory republication
  before startup; unrelated project build-field changes retain their behavior.
- [x] Revise/version build inventory production, schemas, Python/C++ readers,
  fixtures, and diagnostics together. Separate startup selection fingerprints
  from build artifact identity and relevant per-plugin/module build inputs.
  Preserve exact engine/platform/architecture/configuration/runtime/ABI checks;
  distinguish override-only changes from metadata changes requiring a build.
  Do not merely remove fingerprint validation or accept legacy inventories
  under the new semantics without an explicit migration/rejection rule.
- [x] Admit the enabled module subset from an inventory that may contain extra
  disabled plugins. Validate selected owners, relevant build metadata, exact
  binary paths, containment and required runtime files before publishing any
  output. Missing unused-plugin files must not block an otherwise valid selected
  closure. Never infer availability from scanning stale DLLs.
- [x] Pass only admitted selected modules to Core; extra inventory entries grant
  no loading permission. Validate native dependency closure so disabled plugin
  code cannot load indirectly through a selected module's runtime dependencies.
  Retain phase order, Browser isolation, frozen selection, process-lifetime code
  retention, partial-failure cleanup, and no implicit build during startup.
- [x] Update lasting build/startup contracts and fixture instructions after the
  implementation. Preserve original Stage 2/3 receipts as historical evidence;
  explicitly identify replaced checks and evidence reused for unchanged behavior.

Acceptance gate:

- [x] With all plugin overrides disabled, all-target builds still produce every
  eligible fixture module and inventory entry. Override toggles leave targets
  intact; Game excludes Editor modules and configuration outputs remain isolated.
- [x] Build once, then enable a previously disabled fixture and its dependency;
  launch a fresh Editor and Game without configure/build or inventory publication.
  Reflected providers load in order. Disable again and restart without rebuilding;
  disabled modules cannot load by name or through native dependency paths.
- [x] Extra unselected inventory entries, including missing unused files, do not
  prevent selected-subset startup. Missing selected files/dependencies, incorrect
  owners, duplicate entries, path escape, incompatible identities, and stale
  relevant build metadata fail with bounded diagnostics and clean teardown.
- [x] Editing overrides leaves the current process's modules/mounts unchanged;
  a fresh process observes the edits. Conflicting startup dependencies still
  fail even though all applicable modules were built. Browser remains independent.
- [x] Shared metadata/inventory regressions and Editor/Game process fixtures cover
  the new contract. Run all builds for both runtime variants at acceptance:
  changing generated target membership and inventory ownership affects workspace
  build/reflection graphs. Reuse unchanged phase/lifetime coverage with rationale;
  retain explicit macOS and privileged-link qualification gaps.
- [x] Document and all-plan validation pass. Record final inputs, tested variants,
  results and replaced acceptance rules; only then make Stage 4 executable.

### Stage 4: Integrate content, shaders, and plugin configuration

Dependencies: Stage 3.1; Stage 1 supplies early root declarations.

Status: Completed on 2026-11-12.

Modules: Projects, Core mount/configuration mechanisms, AssetRegistry, Engine,
RenderCore, ShaderBuild; ContentBrowser only for mount presentation.

Outcome: Enabled plugin data works in authored projects without mutable mounts
or late shader-root registration.

- [x] Merge enabled plugin mount declarations into the existing immutable mount
  publication transaction; validate collisions, outgoing dependencies, read-only
  policy, and canonical physical containment.
- [x] Include eligible plugin assets in catalog discovery and direct package
  loading. Preserve logical identities through physical relocation.
- [x] Feed descriptor-derived shader roots before ShaderBuild's initialization
  freeze. Include plugin source/dependency fingerprints in shader build/cache
  inputs, and keep derived outputs outside read-only plugin installations.
- [x] Define deterministic configuration precedence: engine defaults, enabled
  plugin defaults in resolved dependency order, project overrides, then existing
  user/command-line overrides where supported. Plugin defaults must not write
  into project files or replace unrelated existing precedence rules.
- [x] Diagnose references to disabled plugin types/assets before destructive
  editing/saving can discard unresolved data. Do not silently substitute or
  resave stripped representations.

Acceptance gate:

- [x] Fixture asset resolves via its plugin virtual path, survives plugin-root
  relocation, and enforces read-only and cross-mount dependency policies.
- [x] Fixture shader compiles through a plugin root and invalidates after source
  changes. Late-root registration is unnecessary; derived caches remain writable.
- [x] Plugin defaults apply only when enabled; project overrides win
  deterministically. Disabled referenced content/types yield actionable errors
  and cannot be silently lost through ordinary save flows.

### Stage 5: Add restart-required plugin management

Dependencies: Stage 4.

Status: Completed on 2026-11-12.

Modules: Projects, DurinEd, MainFrame; reuse existing project-edit ownership,
property/UI infrastructure, and project admission services.

Outcome: Users can review and change the next-start plugin selection safely.

- [x] Show discovered plugins as names with next-start checkboxes. Keep running
  versus pending differences and compatibility/build diagnostics in concise
  status text or tooltips rather than a multi-column state table.
- [x] Keep checkbox requests independent and editable even when dependencies
  are unchecked. Warn that a checked plugin with an explicitly disabled required
  dependency stays inactive; persist the request and apply the same rule at
  startup in native/Python resolution. Do not change other checkboxes. This
  supersedes the earlier reject-on-click behavior per user direction (2026-11-12).
  Keep structural descriptor/graph errors as errors. Cache metadata and build
  diagnostics when opening/refreshing: checkbox clicks must only evaluate the
  in-memory graph, with full disk validation at save/restart boundaries.
- [x] Persist validated project overrides atomically through the existing edit
  ownership contract. Display restart-required state for override-only changes
  with compatible artifacts; request a build only for missing/incompatible or
  stale build inputs. Never build or republish inventories merely to toggle a
  plugin, and do not mutate the current frozen selection.
- [x] Reuse normal unsaved-work handling before restart; preserve explicit
  project arguments and never discard edits through automatic relaunch.

Acceptance gate:

- [x] Enabling/disabling via UI survives restart and produces the same resolved
  set as descriptor-driven startup without rebuilding already-compatible plugins.
  Cancelled edits leave project files intact.
- [x] Pending state is distinct from active state. Failed writes, missing build
  artifacts, dependency conflicts, and cancelled restart retain a usable editor.
- [x] Read-only/project-ownership failures cannot partially replace descriptors;
  no UI action performs live module or mount changes.

### Stage 6: Cook and deploy the plugin closure

Dependencies: Stage 4; Stage 5 is not required for command-line cooking.

Status: Completed on 2026-11-13 for Win64 Debug; native macOS remains a Stage 7 gate.

Modules/tools: Engine cook/package services; DurinDevTool cook/staging;
ShaderBuild, RenderCore cooked libraries; DHT/CMake output inventories; Projects.

Outcome: One complete staged Game runs from its frozen plugin/runtime inventory
and cooked outputs, independently of source plugin directories and development
caches. Plugin resources enter the existing project Cook pipeline; plugins do
not own a second cooker, asset registry or publication system.

Design boundary:

- Follow UE's separation of [Build, Cook and Stage](https://dev.epicgames.com/documentation/unreal-engine/build-operations-cooking-packaging-deploying-and-running-projects-in-unreal-engine?lang=en-US),
  [primary-asset Cook policy](https://dev.epicgames.com/documentation/en-us/unreal-engine/cooking-content-and-creating-chunks-in-unreal-engine)
  and [cooked shader libraries](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/RenderCore/FShaderCodeLibrary),
  while retaining Durin's existing package formats, APIs and virtual roots.
- Preserve authored Content/Shaders/Configs ownership and stable
  `/Plugins/<Id>/...` asset identities. Cooking changes physical storage, not
  serialized identities or declared mount-reference permissions.
- Ship a whole Game in this stage. Reuse current output/container formats and
  one project Shader library; separate plugin archives, physical chunks, DLC,
  hot installation/update and per-plugin Shader libraries are out of scope.
  Water-specific content and rendering remain separate work.

Implementation tasks:

Game startup integration refinement (2026-11-12): native Shader declarations are
registered when provider libraries load, but Mona may consume the cooked Shader
library before StartGame. Run the existing Game Providers phase after object
and asset-domain initialization and before first rendering consumption; retain
its idempotent phase handling and the later EngineReady phase. Qualify ordinary
project startup alongside plugin startup rather than adding a new state machine.

- [x] Resolve and freeze the effective Game plugin closure for the exact target
  platform/configuration before Cook discovery. A checked request blocked by an
  explicitly disabled dependency stays in the authored project but is absent
  from this closure. Keep cook-host provider ownership/lifetime under the
  existing tool contract; stage Game artifacts from the matching Game build
  inventory, never Editor binaries. The available build inventory may remain a
  superset and must not be rebuilt or republished merely to choose this subset.
- [x] Feed selected plugin mounts, types/providers and merged project defaults
  into the existing [Cook coordinator](../Runtime/Assets/Build/Cooking.md).
  Collect assets from existing project/default-Level roots, retained references
  and AssetManager rules. Enabling a plugin does not retain every asset in its
  Content directory. Declare otherwise unreachable explicit-load assets through
  existing Primary Asset, AlwaysCook and bundle policy, using plugin defaults
  where appropriate; do not introduce a parallel plugin Cook-root system.
- [x] Preserve existing reference, EditorOnly and NeverCook validation across
  project/plugin mounts. A required reference to an inactive plugin's class or
  asset fails Cook with its owning plugin and dependency reason; do not silently
  remove the field/reference, activate the plugin or copy its whole directory.
- [x] Add selected plugin Shader inputs and required runtime programs to the
  existing Shader Cook selection. Include programs needed by native plugin code
  even when retained material references alone cannot discover them. Publish
  compiled programs in the existing project `Shaders/ShaderLibrary.dslb`, under
  the ordinary Cook transaction and target compatibility checks. Game must use
  the cooked library without reading authored Slang sources or rebuilding from
  a development shader cache.
- [x] Produce target runtime configuration from Engine defaults, selected plugin
  defaults in dependency order, then project overrides, preserving Stage 4 merge
  semantics. Stage only the required Project/Game configuration and preserve the
  existing runtime user-override policy. Exclude Editor configuration, developer
  Saved settings, source trees and Intermediate files; never write inherited
  defaults back into authored project/plugin configuration.
- [x] Publish a versioned, compact cooked-plugin runtime inventory containing
  the effective plugin/dependency identities, target/build compatibility,
  Runtime module paths/phases and native dependencies, mount bindings and
  required runtime configuration. Use deployment-relative paths and bind it to
  the Cook outputs. Keep asset entries/checksums and the Shader library in the
  existing `CookManifest.bin`; do not duplicate a second asset catalog/list.
  Staged startup reads these inventories without scanning source `.dplugin`
  files or requiring Durin.dworkspace, DHT or a compiler.
- [x] Stage only the selected Runtime/native dependency closure, reachable cooked
  content, cooked Shader library and runtime configuration. Reuse existing
  transactional publication: failure must not expose a runnable mixture of old
  and new outputs. Validate selected build inputs/identity before staging and
  reject mismatched runtime inventories, Cook identities or selection changes
  without recooking/restaging. Do not build or Cook implicitly during staging
  or Game startup, and do not fall back to source/development outputs. Cook
  inputs remain stable under the existing offline Cook contract; this stage
  does not add concurrent-edit watching or hot reload.

Acceptance gate:

- [x] Cook/stage no-plugin, content-only and dependent Runtime+Editor fixtures
  using existing tooling. Inspect inventories and physical outputs: only the
  effective Game closure is deployed; inactive/Editor binaries and data,
  unreferenced optional assets and developer settings are absent. Required
  native dependencies are present and plugin modules start in dependency/phase
  order. Selection changes reuse compatible builds without a build/inventory
  republication; existing no-plugin deployment remains working.
- [x] Relocate the staged fixture Game and make source plugin roots, workspace
  metadata, authored Shader sources and development caches unavailable. Verify
  plugin component/type behavior, project-to-plugin and declared plugin-to-plugin
  asset references, an explicit-load/AlwaysCook asset, configuration precedence
  and use of the cooked plugin Shader program. Logical asset paths remain
  `/Plugins/<Id>/...`; no source scan, compile or fallback path is exercised.
- [x] A checked-but-blocked plugin request is omitted from deployment while
  unrelated valid plugins remain usable. A project root or required reference
  to that plugin fails Cook before publication with the owning plugin/reason.
  Required references conflicting with NeverCook or EditorOnly policy also
  fail; unreferenced optional assets are not retained merely by plugin enablement.
- [x] Missing native dependency, missing explicit-load asset, missing/corrupt
  cooked Shader library or required configuration, stale/incompatible build,
  modified selection, or mismatched runtime/Cook inventory produces a bounded
  diagnostic failure. Failed Cook/stage attempts retain the prior committed
  output and never silently load a development copy.
- [x] Record exact target/profile, tested source and fixture inputs, selected
  closure, clean relocation environment and negative-case results. Store raw
  manifests, logs and reproduction scripts under ignored Build outputs or CI
  artifacts. Reuse unchanged receipts under the testing workflow; record any
  unqualified platform explicitly and leave final cross-platform integration
  coverage to Stage 7.

### Stage 7: Qualify integration and publish implemented contracts

Dependencies: Stages 1-6, including Stage 3.1 and the management UI.

Status: In progress; Win64 qualification completed on 2026-11-13, native macOS pending.

Modules/tools: All owners above; Engine, Sandbox, and RoadWeaver consumers;
documentation and applicable CI jobs.

Outcome: Restart-based plugins are a supported source-engine workflow.

- [x] Run the final build/launch/cook matrix for no-plugin, content-only,
  Runtime+Editor, and dependent-plugin fixtures. Include normal exit and
  representative partial-startup failures with outstanding owned work.
- [x] Run an `all` build for each supported runtime variant at final acceptance:
  plugin discovery changes generated target membership and reflection inputs
  across every project in Durin.dworkspace, so bounded module builds alone
  cannot qualify this build-graph change. Reuse earlier passing results when
  relevant inputs/environment are unchanged; do not repeat per documentation edit.
- [ ] Qualify Win64 native startup and relocated cooked Game execution. Run
  available macOS build/native acceptance through its owner or CI; record any
  unavailable platform gate explicitly rather than claiming full coverage.
- [x] Add focused regression coverage to existing metadata, module/startup,
  package, shader, and cook suites; avoid redundant implementation-mirroring tests.
- [x] Publish lasting descriptor/build contracts under Workspace/Development,
  startup contracts under Runtime/Application, asset/shader rules in their
  owning Runtime domains, and the restart workflow in Editor/Guides.
- [x] Record tested revision, selections, platform/configuration, fixture inputs,
  results, and outstanding gates here. Store raw logs/reports under ignored
  Build outputs or CI artifacts. Complete/archive only through plan lifecycle.

Acceptance gate:

- [x] All earlier gates have evidence covering final relevant inputs; no
  unsupported platform or SDK-distribution claim is implied by Win64 success.
- [x] Existing projects build and start without requiring plugin configuration.
- [x] A developer can create a source plugin with a reflected component, shader,
  content, and editor companion, build it while disabled, enable it and restart
  without another build, and deploy only the enabled Game closure without editing
  Engine's per-plugin module table.
- [x] Documentation validation and all-plan lifecycle validation pass.

## Execution Order And Handoff

Implement 1 -> 1.1 -> 2 -> 3 -> 3.1 -> 4, then UI (5) and cook (6), then
qualification (7). Stages 3.1 and 4 completed on 2026-11-12 with the acceptance
above. Stage 5 completed on 2026-11-12 after user-reported manual acceptance;
Stage 6 is complete for Win64 Debug; Stage 7's remaining gate is native macOS
staging support and build/execution qualification through an available owner or CI lane.
Preserve earlier receipts as
historical evidence and apply the replacement rules recorded by Stage 3.1.
Stage 1.1 completed on 2026-11-12 with the final acceptance above. Earlier
checkpoint receipts remain historical; use final-input evidence for continuation.
Stages 5 and 6 are logically independent after Stage 4; this is not authorization
for concurrent writers in one checkout. Follow repository worktree ownership.

For each implementation commit, update this plan's status and only the checks
supported by evidence. Use exact Plan/Stage trailers from this file and its
executed stage title. Do not mark a stage complete based on a merged API alone
when its process, artifact, or negative-case acceptance remains unverified.
