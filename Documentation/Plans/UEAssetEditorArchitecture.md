# UE Asset Editor Architecture Plan

Summary: Replace workspace-bound registration with UE-aligned asset definitions, editor instances with optional reflected owners, reusable toolkits, independent hosts, and metadata-driven actions.

Last reviewed: 2026-11-13

Status: Completed
Completed: 2026-11-13

## Current Status

Completed on 2026-11-13 under the user's final scope decision. Stages 0-9 code
implementation, automated qualification, shared-consumer builds and lasting
contracts are accepted as this plan's completed scope. The previously proposed
lifecycle-responsibility consolidation is removed from this plan; no replacement
plan is created and no implementation of that consolidation is claimed.

Real interface interaction (native dirty/pending close and host-transfer gestures,
interactive panel/layout restart and project switching) is explicitly excluded
from this completion scope at the user's direction. Those operations remain
skipped and unqualified, never passed. The two Windows symlink-privilege test
skips remain recorded limitations. Successful StaticMesh package reload and live
direct-Toolkit host transfer were not added; their documented limitations remain.
This completes the selected architecture migration and read-only direct pilot,
not unrestricted UE parity or all-platform/manual acceptance.

Final code is committed as `83d3b201f`; the Stage 9 final-input receipts below
record 585 selected passes, two skips and the passing incremental all Editor
consumer build. This closure changes documentation only. Source/test hashes are
rechecked against `stage9-inputs.json`, so applicable runtime receipts are reused.
Changed-document and all-plan lifecycle validation cover the completion metadata
and scope updates. The plan remains at this path pending normal batch archival.

Earlier checkpoint wording below records historical acceptance status; the final
scope decision above supersedes any previously open native-interface gates.


Stage 9 implementation and automated validation are complete. The default and
StaticMesh definitions share a private default-definition execution algorithm;
new-instance focus belongs to subsystem admission and reuse focuses once. Direct
mutable Edit is rejected before host/document work; immutable inspectors retain
Edit/View and mutable-feature Toolkits may use View. No new lifecycle state or
whole-window dirty-close continuation mechanism was introduced.

Final inputs are working changes based on `1c070b0f8`, Windows
`windows-msvc-x64`, `Win64-Debug-DurinEditor`. Receipts under
`Build/UEAssetEditorArchitecture/` are:

| Selection | Outcome | Receipt |
| --- | --- | --- |
| EditorOperationTests | 358 passed | `stage9-framework.xml` |
| AssetEditorFamilyTests | 10 passed | `stage9-families.xml` |
| EditorHostToolTests | 39 passed | `stage9-host.xml` |
| ContentBrowserWorkflowTests | 173 passed, 2 Windows symlink-privilege skips | `stage9-browser.xml` |
| StaticMeshTests / FStaticMeshEditorTests.* | 5 passed | `stage9-mesh.xml` |

These are 585 selected passes and two skips. The incremental all Editor consumer
build passed (`20261113-145731-240979-41224-cmake.log` under
`Build/.agent-state/logs/`), covering Engine, Sandbox and RoadWeaver. No Game
dependency or build graph changed. `stage9-inputs.json` records final source/test
hashes and receipt hashes/counts. The framework/host/browser/mesh receipts remain
applicable: the last production refinement only declares that the direct Mesh
definition has no reflected editor class; the refreshed family receipt verifies
mixed direct/generic View groups reject in both input orders instead of falling
back to a generic reflected editor. There were no subsequent production changes.

New cases verify one Focused event for new/reused reflected and direct opens,
mutable Edit rejection without host creation/document callbacks/publication,
and fixed View mutation guards. Existing grouped/role/mode, provider-retention,
load-failure and real relocation/restore coverage passes. Changed-document and
all-plan lifecycle validation pass. Native interface interaction remains skipped
and unqualified; at that checkpoint the plan remained Active pending the final
scope decision recorded above.
The follow-up interface decision preserves AssetDefinitionRegistry for discovery
and lookup and Definition::OpenAssets for opening. The three-callback public helper
was withdrawn before acceptance; selection policy remains a private default-definition implementation
and the direct inspector supplies only a Toolkit identity/construction factory.
Registry object-lifetime representation is unchanged; this aligns responsibilities,
not UE UObject ABI or every method name.
Lifecycle-responsibility consolidation is outside the completed scope. Native
interface interaction remains skipped and unqualified.


Stage 8 implementation and automated pilot qualification are complete. StaticMesh
Inspector is a direct Toolkit/IAssetEditorInstance and allocates no reflected
DStaticMeshAssetEditor. Texture, Material and Level retain reflected ownership.
The final Toolkit snapshot derives from these two actual ownership collections;
no secondary ToolkitManager registration/index or self-returning compatibility
facade remains. Active/focus context and lifecycle InstanceId work with both forms.
The shared admission helper preserves required-mode normalization and explicit-host
rejection. StaticMesh definition lookup preserves inherited definition matching.

Based on Stage 7 commit `44e873576`, Windows `windows-msvc-x64`,
`Win64-Debug-DurinEditor`, final selected receipts under
`Build/UEAssetEditorArchitecture/` are:

| Selection | Outcome | Receipt |
| --- | --- | --- |
| EditorOperationTests | 356 passed | `stage8-framework.xml` |
| AssetEditorFamilyTests | 8 passed | `stage8-families.xml` |
| EditorHostToolTests | 39 passed | `stage8-host.xml` |
| StaticMeshTests / FStaticMeshEditorTests.* | 5 passed | `stage8-mesh-editor.xml` |
| EditorShellTests | 63 passed | `stage8-shell.xml` |
| ContentBrowserWorkflowTests | 173 passed, 2 Windows symlink-privilege skips | `stage8-browser.xml` |
| AssetPackageReloadTests | 21 passed | `stage8-reload.xml` |

These are 665 unique selected passes and two skips. The earlier 201-case full
StaticMesh suite also passed; its five editor cases were refreshed after the
final mechanical test API migration and are counted only once above. The final
incremental all build passed (`20261113-122121-590672-7472-cmake.log` under
`Build/.agent-state/logs/`), covering Engine, Sandbox and RoadWeaver consumers.
The final build includes the direct activation-time correction; the refreshed
framework receipt verifies active-window ordering and the final family receipt
covers grouped/reversed selection reuse and object convenience opening. No later
production inputs changed. No Game dependency or build graph changed.
The earlier shell/browser/reload receipts remain applicable: subsequent private
admission extraction keeps reflected policy identical; final direct required-mode
and activation-time behavior is covered by the refreshed framework and actual
Mesh family receipts. Host behavior and the five inspector-case expectations are
unchanged by that private activation-time fix.

New cases cover direct initialization failure with no publication, required-mode
normalization and explicit conflicts, provider withdrawal/GC, stable value-event
identity and host close, and grouped/role/mode restore. The real Mesh case covers
object opening, independent Edit/View inspection, immutability, resource relocation,
GC, layout-profile stability, grouped reuse and restore without a reflected owner.
Engine explicitly rejects StaticMesh package reload as Unsupported; the case
verifies that rejection retains the original inspector/associations. This is not
a successful reload claim and no Engine reload capability was added.
`stage8-inputs.json` records final source/test and receipt hashes plus removed-symbol
searches; `stage7-inputs.json` records the preceding cleanup commit's source blobs
and receipts. Changed-document and all-plan validation pass. Native interface
interaction remained skipped and unqualified at this checkpoint; completion
uses the final scope decision above.

Stage 7 implementation is complete. Based on `893c4e126`, Windows
`windows-msvc-x64` / `Win64-Debug-DurinEditor`: EditorOperationTests 352 passed
(`stage7-framework.xml`), EditorHostToolTests 39 passed (`stage7-host.xml`),
AssetEditorFamilyTests 7 passed (`stage7-families.xml`), and
ContentBrowserWorkflowTests 173 passed with two existing Windows symlink-privilege
skips (`stage7-browser.xml`). Reports are under `Build/UEAssetEditorArchitecture/`.
The all Editor consumer build passed (`20261113-115659-638988-24824-cmake.log`
under `Build/.agent-state/logs/`). The subsequent test edit only removes a duplicate
assertion and unused lease; production inputs are unchanged. Stage 8 qualification is recorded above.

The 2026-11-13 follow-up review authorizes Stages 7-8 below: remove redundant
state first, then pilot a direct StaticMesh Toolkit instance. This supersedes the
mandatory reflected-session implementation choice, not the definition/opening,
read-only, lifetime or host contracts. Stage 6 receipts below remain historical
baseline evidence. Real interface interaction remains skipped and unqualified.


Stages 0-5 implementation and Stage 6 work packages 6A-6E are implemented.
The 2026-11-13 Stage 6 closure consolidates execution/presentation plumbing,
qualifies definition withdrawal independently of session close and pending work,
and migrates the remaining shared API consumers. The remaining required gates
are real interface interaction: native dirty/pending close and host transfer,
panel/layout restart and interactive project switching. They remain explicitly
skipped at the user's direction and unqualified. This historical checkpoint did
not complete the plan; the later explicit scope decision above governs completion.

The single final toolkit draw boundary retains callback/frame leases and selects
the owning editor context; session drawing delegates without nested dispatch.
Optional panel factories replace constructor flags. Browser invocation and drawing
share `FExtensionHost`; `FScopedHostPresentation` centralizes weak ownership,
foreign-host rejection and dialog stop/expiration. Texture keeps its asynchronous
cancel/drain protocol, including draining an expired owner when another host draws.
No physical DLL reload support or new provider unload permission was introduced.

Source/test searches cover Engine, Sandbox and RoadWeaver, the complete
`Durin.dworkspace` project list. Removed registration/route/instance interfaces,
`GroupAssets`, constructor panel flags and the old invocation host fields have no
remaining consumers. RoadWeaver's independent creation extension now uses the same
host context/helper; Sandbox has no specialized definition/toolkit consumer to
migrate. Reflected fixture definitions and session classes exercise extension
without central dispatch edits, including generic Edit/View and grouped sessions.
Lasting contracts are published in
[Asset definitions and editor sessions](../Editor/Architecture/Framework/AssetEditors.md).

Final-input evidence, Windows `windows-msvc-x64`, `Win64-Debug-DurinEditor`,
working changes based on `b2ea9f72d`: incremental `all` passed, covering the Engine,
Sandbox and RoadWeaver Editor consumer closure. The closure build is
`20261113-113223-964488-39304-cmake.log`; the final incremental gate after the
private Texture expiration/drain refinement is
`20261113-113649-216110-46436-cmake.log`, both under `Build/.agent-state/logs/`.
No Game dependency or build graph changed, so no unrelated Game build was added.

| Final selection | Outcome | Receipt under `Build/UEAssetEditorArchitecture/` |
| --- | --- | --- |
| EditorOperationTests | 352 passed | `stage6-final-framework.xml` |
| ContentBrowserWorkflowTests | 173 passed, 2 Windows symlink-privilege skips | `stage6-final-browser.xml` |
| EditorHostToolTests | 39 passed | `stage6-final-host.xml` |
| AssetEditorFamilyTests | 7 passed | `stage6-final-families.xml` |
| EditorShellTests | 63 passed | `stage6-final-shell.xml` |
| CoreConcurrencyTests / FModuleManagerLifecycleTests.* | 7 passed | `stage6-final-modules.xml` |
| EditorAssetWorkflowTests | 44 passed | `stage6-final-workflows.xml` |
| AssetPackageReloadTests | 21 passed | `stage6-final-reload.xml` |
| TextureImportWorkflowTests | 26 passed | `stage6-final-texture-import.xml` |

These are 732 passes and two skips. `stage6-final-inputs.json` records the base
revision, final source/test hashes, report hashes/counts and removed-symbol audit.
The browser, host, family, shell and module receipts remain applicable: the only
later production refinement changes private Texture import expiration/draining,
covered by the final import/workflow receipts; the framework rerun covers the
later test-fixture correction. No subsequent production input changed.

Automated parity evidence maps definitions/discovery, grouped/secondary sessions,
View/default editing, commands/extensions, events, MRU/restore and local accounting
to framework/browser/shell receipts. The host receipt includes successive activated
Level editor lifetimes, retained old toolkit handles and configured-Level policy;
family/workflow/reload receipts cover concrete controllers and package invariants.
Definition withdrawal while a dirty session remains alive, refused cancellation
retaining a withdrawn provider, terminal release and self-close during drawing
are explicitly covered. Module lifecycle preflight proves rejected unload does
not call Shutdown; unsupported provider DLL reload stays disabled. This closes
automated parity only: native host gestures, actual docking/restart interaction
and project switching still have no pass and remain assigned to the open gate.

The initial sandbox build hit MSVC C1902; the same build succeeded outside the
sandbox. The first framework run also exposed a stale test prerequisite:
`AutomationWaitsForDocumentCompletionAndPrimaryScenePresentation` entered
EditorReady without Providers/EngineReady. Its fixture now executes those real
project phases and reports startup diagnostics; the complete 352-case rerun
passes. Failed attempts remain in local logs and are not acceptance receipts.

Changed-document validation and all-plan lifecycle validation pass. Historical
checkpoints below describe their own inputs and do not add to the final count.

Stage 6E is implemented under the user's minimal-persistence and no-compatibility
decisions, superseding the earlier migration/window-restoration recommendation.
Runtime window IDs isolate live roots and panels and are never added to session
snapshots. Named/versioned toolkit profiles provide editor-level defaults scoped
by method, mode and role. Resource rename and group membership do not change the
profile. Dock templates reuse ImGui's configuration; YAML stores panel visibility
and Material preferences/viewports. No per-window geometry snapshot, new global
viewport store, alias registry, migration key, old-file probe or compatibility
fallback exists. Old path/group layouts are deliberately abandoned. Level retains
its existing stable shell surface and scene ownership.

Stage 6E final-input evidence, Windows `windows-msvc-x64`,
`Win64-Debug-DurinEditor`, working inputs based on `febfd4f3c`: incremental `all`
passed (`20261106-182245-269578-34400-cmake.log` under `Build/.agent-state/logs/`).
The earlier full closure rebuild is `20261106-181458-130953-35300-cmake.log`;
the final incremental gate covers the subsequent private Material merge fix.
No Game dependency changed. Final focused native receipts under
`Build/UEAssetEditorArchitecture/` are:

| Selection | Outcome | Receipt |
| --- | --- | --- |
| EditorOperationTests / FAssetEditorSessionTests.* | 34 passed | `stage6e-sessions.xml` |
| AssetEditorFamilyTests / MaterialRelocationRekeysWorkingSessionAndRetainsSave | 1 passed | `stage6e-material-relocation.xml` |
| MaterialEditorInteractionTests / FMaterialLayoutSettingsTests.* | 1 passed | `stage6e-material-settings.xml` |
| EditorHostToolTests / FLevelAssetEditorTests.* | 1 passed | `stage6e-level.xml` |

These are 37 focused passes. The session and Level receipts remain applicable:
later production changes only refine private Material preference merging, covered
by the refreshed settings and relocation cases. `stage6e-inputs.json` records final
source/test hashes and report summaries. Earlier broad/intermediate receipts are
not included in this final count. Actual interface operations remain skipped.

Only two new cases were added: headless named-template/window isolation and shared
settings merge/relocation. The existing real Material relocation case now checks
profile stability and reopen. A redundant asset-order/layout-key comparison case
was removed; tests of hash/string construction were not added. The headless case
exposed automatic Dock restore being bypassed by NoSavedSettings; first-use
explicit template assignment fixes it and runtime window names stay out of the
serialized settings. Shared preference fields and resource records merge without
stale-window overwrite. The module's existing relocation notification updates
stored profile viewports while windows are closed. Old layouts are neither read
nor migrated. Presentation-boundary consolidation and provider-retirement
qualification are covered by the 2026-11-13 closure above.

Stage 6D checkpoint evidence on Windows `windows-msvc-x64`,
`Win64-Debug-DurinEditor`, working inputs based on `22170984f`: `all` passed
(log `20261106-172801-371977-31080-cmake.log` under `Build/.agent-state/logs/`).
The workspace Engine, Sandbox and RoadWeaver consumer closure is covered; no
Game closure dependency changed. Final native receipts under
`Build/UEAssetEditorArchitecture/` record:

| Selection | Outcome | Receipt |
| --- | --- | --- |
| EditorOperationTests | 320 passed | `stage6d-framework.xml` |
| ContentBrowserWorkflowTests | 140 passed, 2 existing Windows symlink-privilege skips | `stage6d-browser.xml` |
| EditorHostToolTests | 38 passed | `stage6d-host.xml` |
| AssetEditorFamilyTests | 7 passed | `stage6d-families.xml` |
| EditorAssetWorkflowTests | 44 passed | `stage6d-workflows.xml` |
| TextureThumbnailTests | 14 passed | `stage6d-texture-thumbnails.xml` |
| EditorShellTests | 65 passed | `stage6d-shell.xml` |
| Independent browser fixture cases | 3 passed in isolated processes | `stage6d-browser-isolated.xml` |

The seven suites cover 628 unique passes and two skips; the three isolated cases
repeat part of that coverage. `stage6d-inputs.json` records final source/test hashes
and report summaries. Production inputs have not changed after the all build;
subsequent test refinements are included in refreshed owning receipts. No actual
application interface operation was performed. Earlier broader Stage 6 receipts
remain historical evidence, not additional final-input test counts.

Support responses follow UE's method/support/optional-required-mode shape.
Stateless feature policies are shared by definitions and sessions, while toolkits
consume the admitted context without duplicate support interfaces. Explicit hosts
are validated before loading and determine mode when compatible; absent hosts
allow required-mode normalization. Direct admission and host transfer combine
session policy and applicable definition constraints. Restore uses an explicit
flag instead of interpreting diagnostic Origin text. Native cases verify rejected
hosts never create toolkits, incompatible requirements reject an entire group,
explicit host mode overrides an ordinary preference, and direct Level restore is
rejected. Browser fixtures now use real definition registration and independent
extras; metadata/category queries and extras withdrawal remain load-free.
The old type-presentation registry and fixed category enum have no source/test
consumers in any workspace project. Persisted filter shortcuts resolve definition
category roots rather than introducing a second metadata taxonomy.

The layout and automated presentation/provider checks listed at this historical
checkpoint were subsequently qualified by 6E and the 2026-11-13 closure. The
explicitly skipped interface gates remain open.

Stage 6 checkpoint evidence on Windows `windows-msvc-x64`,
`Win64-Debug-DurinEditor`, working inputs based on `4e1731ad8`: the incremental
`all` build passed, including Sandbox and RoadWeaver/RoadWeaverEditor consumers
(log `20261106-162159-160850-42848-cmake.log` under `Build/.agent-state/logs/`).
Final native receipts under `Build/UEAssetEditorArchitecture/` passed:

| Selection | Outcome | Receipt |
| --- | --- | --- |
| EditorOperationTests | 317 passed | `stage6-framework.xml` |
| EditorShellTests | 65 passed | `stage6-shell.xml` |
| EditorHostToolTests | 38 passed | `stage6-host.xml` |
| AssetEditorFamilyTests | 7 passed | `stage6-families.xml` |
| AssetPackageReloadTests | 19 passed | `stage6-reload.xml` |
| ContentBrowserWorkflowTests | 138 passed, 2 existing Windows symlink-privilege skips | `stage6-browser.xml` |
| MaterialThumbnailTests | 8 passed | `stage6-material-thumbnails.xml` |
| StaticMeshThumbnailTests | 10 passed | `stage6-mesh-thumbnails.xml` |
| TextureThumbnailTests | 14 passed | `stage6-texture-thumbnails.xml` |
| StaticMeshTests | 201 passed | `stage6-mesh.xml` |

`stage6-inputs.json` records the base revision, final source/test hashes and report
summaries. The all-build inputs remain applicable; subsequent changes are test
coverage and documentation. No native application window was launched for this
checkpoint. The two browser skips remain unqualified, not passes.

Eight tests specifying deleted registration/descriptor contracts and three
legacy instance/route fixtures were removed; document close, deferred replacement,
GC roots, relocation and atomic reload coverage was migrated to attached session
controllers. Added cases cover one ordered unique batch per definition, conflicting
metadata, recursive batch rejection, pending View completion, one grouped session,
compatible mixed definitions, incompatible/missing group rejection, configured
session admission, shared-host detachment and foreign-owner rejection. A regression
also verifies that refreshing another session cannot finish a pending document
request; only its own document completion/cancellation path reports that outcome.

Level host configuration now belongs to the per-editor Level subsystem and is
injected into a reflected session before initialization. It survives closing a
surface without indexing or retaining retired sessions; the module host/session
map and toolkit-construction callback were deleted. Source/test searches across
all workspace projects found no removed toolkit registration/route interfaces or
`GroupAssets` replay. At that checkpoint, fixture-only browser type-presentation
registration remained; 6D removes it as recorded above. The entire plan is still
active because remaining layout, lifecycle and interface gates are open.

Stage 5 checkpoint evidence on Windows Debug Editor, working inputs based on
`1a6ec3372`: 315/315 framework/request/session cases (`stage5-framework.xml`),
38/38 MainFrame/Level host cases (`stage5-mainframe.xml`), 7/7 production-family
cases (`stage5-families.xml`), and 138 browser cases passed with the same two
Windows symlink-privilege skips (`stage5-browser.xml`). The final public-header
and host-readiness inputs passed the refreshed framework and MainFrame receipts;
unchanged browser and family receipts remain applicable. Tests cover grouped View
restore, role/mode preservation, missing providers, project rejection, bounded
terminal history without evicting pending requests, deferred association/event
ordering, cancellation retaining the current document, Level close/reopen and
successive editor lifetimes. Native reports and source hashes remain under
`Build/UEAssetEditorArchitecture/`.

Normal-window Sandbox startup reached Ready and exited cleanly. Seeded texture
Edit/Standalone and View/WorldCentric sessions survived independent process
restarts with stable roles and focus order, while a seeded Level restore item
was rejected and the configured GrayboxStage15 remained the startup scene.
RoadWeaver reached Ready with its configured Level and exited cleanly after
its browser extension consumer was migrated and rebuilt. An initial RoadWeaver
run exposed a stale consumer ABI in `FExtensionDescriptor`; the exact crash and
symbolized stack are retained in local run logs, and the passing rerun is recorded
rather than treating that failure as a pass. Run receipts include
`20261106-151100-536365-27556-DurinEditor.log`,
`20261106-152127-398882-25644-DurinEditor.log`, and
`20261106-151841-420323-29568-DurinEditor.log` under `Build/.agent-state/logs/`.
The final host readiness change also waits for engine Ready, so startup-Level
presentation cannot overwrite restored focus.

Interactive project-switch validation was stopped and explicitly skipped at the
user's request. No pass is claimed for that action or native dirty-close gestures;
remaining integrated acceptance stays open. The implementation checkboxes below
are not a declaration of full Stage 5 or plan acceptance. The editor test process
was stopped and the temporarily seeded Sandbox session snapshot was restored.

Stage 5 execution decisions: lifecycle notifications use copied value payloads
and scoped subscriptions owned by each editor subsystem; subscriptions never
retain sessions. Requested events precede dispatch and committed association
events follow publication. Local accounting counts committed outcomes only.
Persistence uses an explicit versioned project-scoped snapshot with stable
session roles, asset groups and focus order. Restore uses `OpenAssets` and
reports each unavailable item, with Level excluded from ordinary restore so
the configured startup scene remains authoritative. Level keeps its specialized
scene controller and readiness callbacks while moving to an owned session;
no parallel scene-loading state machine is introduced.

The existing engine contract permits one activated `DEditorEngine` per process
([editor subsystem lifetime](../Editor/Architecture/Framework/EditorSubsystems.md)).
The two-editor isolation gate therefore uses successive activated lifetimes,
including retained old toolkit/subscription handles, rather than bypassing engine
activation to manufacture simultaneous engines. Multiple asset sessions and both
presentation hosts coexist within the active editor. Engine activation semantics
are outside this asset-editor migration and remain authoritative.

Execution sequencing refinement: Stage 4 implementation may use the tested Stage 3
API checkpoint while the native-window/restart qualification remains open. This
lets the application qualification cover the migrated production families together.
It does not waive or mark Stage 3 acceptance complete; all original native, restart
and metadata-duplication gates must pass before final acceptance.

Stage 4 ownership refinement: bind document history to its requesting editor and
use a nested scoped transaction-editor context during asset callbacks. Existing
graph/property commands can then resolve the correct transactor without changing
the process `GEditor`. A scope restores its previous editor after reentrant calls;
an expired bound editor must never fall back to another host.

Stage 4 also makes live document IDs process-unique for native preview-resource
identity while keeping persistent keys canonical and role-based. Per-session
material settings inherit the previous store on first use and write isolated
version-2 stores. Material/Function graph editors explicitly reject View, including
direct session creation; Collection, VolumeTexture and generic property editing
support View, and Mesh inspection stays immutable. TextureCube metadata and its
generic View inspector are definition-backed.

Stage 4 focused validation, Windows Debug Editor inputs based on `f18997e5b`:
310/310 framework/transaction cases (`stage4-framework.xml`), 7/7 real-family
cases (`stage4-families.xml`), 70/70 property cases, 66/66 material interaction
cases, 13/13 material persistence cases, and 5/5 mesh session cases passed.
Independent module-startup thumbnail suites passed 14/14 Texture, 8/8 Material
and 10/10 Mesh; the real texture render consumer passed 3/3. Browser coverage
passed 138, skipped two unavailable Windows symlink-privilege cases, and failed
none (`stage4-browser.xml`). MainFrame passed 37/37, workspace/model regression
21/21 and package reload 19/19. Reports are under
`Build/UEAssetEditorArchitecture/`. The refreshed framework/family results cover
final retired-toolkit command/draw guards, failed host attachment, deterministic
primary selection and grouped close continuation/cancellation. Earlier owning
results remain applicable to unchanged property/graph/thumbnail behavior; final
integration still has to qualify the combined architecture.

Collection discard exposed an existing Engine unsupported-class rejection.
Exact-class layout validation, native World-state reference replacement and
post-commit refresh now pass with preserved overrides and injected pre-commit
failure rollback. Material working-copy roots participate in the same atomic
reload transaction. Volume discard also exercises real source-data loading.

Stage 3 implementation checkpoint (not stage acceptance): native viewport close
authorization, shell host factories, transactional host transfer, stable roles,
grouped roots, scoped panel/command/tool attachments and definition-backed nested
browser filters are implemented. Based on `b58df8573`, Windows Debug Editor
receipts under `Build/UEAssetEditorArchitecture/` record 53/53 framework cases
(`stage3-framework.xml`), 14/14 metadata cases (`stage3-metadata.xml`), 37/37
MainFrame cases (`stage3-mainframe.xml`), and 25/25 viewport/workspace cases
(`stage3-host-presentation.xml`). The 53 cases include actual ImGui frame drawing
of independent grouped/single roots and panels. The wider browser run passed
135 cases, skipped two unavailable Windows symlink-privilege cases, and failed
none (`stage3-browser.xml`). Native dirty-close/transfer interactions and full
panel-layout restart qualification remain open. Family metadata duplication was
removed through Stage 5. The refreshed
37-case MainFrame receipt covers the final active-layout reset menu wiring.

Stage 3 selected host adaptation: standalone uses Mona/ImGui's existing platform
viewports with forced independent native windows and owner close authorization,
not a second renderer. World-centric attaches to the Level editor's owning
MainFrame viewport/dock host, preserving Durin's major-tab layout. A per-editor
host factory supplies those presentation objects. Additional sessions use stable
named roles (the default secondary role is `Secondary`); identical type/method/
mode/role/asset sets are rejected. Group layout keys hash sorted canonical paths,
never live session counters. Host transfer preserves the session/controller and
rejects pending operations, failed attachment, or destination identity collisions.

Stage 2 ownership refinement: use a session-owned `shared_ptr` toolkit with
bounded callback/frame leases, matching the lifetime needed when a close occurs
inside its own drawing or command callback. The toolkit retains its document
controller and provider definition while borrowed by a frame; back references
to the reflected session are weak. This replaces the originally selected
`unique_ptr` spelling without adding a second persistent session owner.

Stage 2 evidence on Windows Debug Editor working inputs based on `f1343f438`:
43/43 definition/session/subsystem cases (`stage2-sessions.xml`), 10/10 actual
texture-session/thumbnail cases (`stage2-texture.xml`), 20/20 workspace regressions
(`stage2-workspaces.xml`), and 37/37 host regressions (`stage2-host.xml`), all under
`Build/UEAssetEditorArchitecture/`. The real texture cases cover Edit/View reuse,
atomic discard reload and external association replacement, and relocation of an
isolated duplicate followed by save. MaterialEditor, StaticMeshEditor, LevelEditor
and MainFrame consumer targets built after the shared interface migration.
The final 43-case and 10-case receipts include bind-once and retired-toolkit
command guards and shared-build cancellation policy. Source input SHA-256 hashes
are retained in `Build/UEAssetEditorArchitecture/stage2-inputs.json`; later
documentation edits do not change those native inputs. The earlier passing
workspace/host results still apply: subsequent changes affect only session-bound
toolkit guards and Texture2D close policy, covered by the refreshed suites.
Native standalone windows, browser/category migration and
the final `all` acceptance build remain later gates, not Stage 2 evidence.

The implemented foundations are documented in
[asset definitions and editor sessions](../Editor/Architecture/Framework/AssetEditors.md).

Stage 1 uses an uncached ancestor walk with a membership revision rather than
maintaining a class-pointer lookup cache. This deliberately removes invalidation
and stale-class lifetime complexity; lookup visits only the requested class's
ancestor chain. The requirement is immediate correct lookup after definition
changes, not the existence of a cache. The default definition foundation supplies
metadata now; Stage 4 still owns the functional generic property toolkit.

Stage 1 Windows Debug Editor working inputs based on `1de6a1384` passed
32/32 definition/subsystem cases in `EditorOperationTests` (selection
`FAssetDefinitionTests.*:FEditorSubsystemTests.*`), with the receipt at
`Build/UEAssetEditorArchitecture/stage1-dispatch.xml`. Compilation first encountered
sandbox-only MSVC C1902 and passed outside the sandbox. Workspace regression
passed 20/20 (`stage1-workspaces.xml`), host regression passed 37/37
(`stage1-host.xml`), and all seven new definition cases passed in independent
processes (`stage1-definitions-isolated.xml`) in the same evidence directory.
The modified production `MainFrame` target built successfully. Later edits to
those consumer receipts' inputs only added a private provider-identity key and
its inline-completion regression; no workspace/host behavior changed. The final
MainFrame build and 32-case suite include those exact inputs. No native-window,
session migration, generic editor, or full-build acceptance is claimed here.

Stage 0 baseline: `3be0cb25f`, Windows Debug Editor
(`Win64-Debug-DurinEditor`), direct non-window tests. `EditorShellTests`
`FEditorWorkspaceManagerTests.*` passed 20/20 and `EditorOperationTests`
`FEditorSubsystemTests.AssetEditor*` passed 6/6. Reports are
`Build/UEAssetEditorArchitecture/baseline-workspaces.xml` and
`Build/UEAssetEditorArchitecture/baseline-subsystem.xml`. Source layout fixtures
are under `Build/UEAssetEditorArchitecture/baseline-layouts/`; the source/test
consumer inventory is `Build/UEAssetEditorArchitecture/baseline-consumers.txt`.
These are baseline receipts, not new-architecture or interactive smoke evidence.

Review baseline: `a47e8f39a927137318dc5cc8f29c0bed699e48ae`.
The current implementation eagerly registers shared multi-document workspaces,
routes exact asset-class strings through `FEditorDocumentManager`, and creates
one `IAssetEditorInstance` adapter per committed asset document. MainFrame wires
concrete editor modules through `IAssetEditorModule::RegisterToolkits`.

The implemented contracts remain authoritative until their replacement stages
land: [workspace framework](../Editor/Architecture/Framework/WorkspaceFramework.md#asset-editor-toolkit-registration)
and [asset-editor subsystem](../Editor/Architecture/Framework/EditorSubsystems.md#asset-editors).
This plan intentionally supersedes their fixed-composition and exact-class-only
design decisions as implementation progresses; it does not describe them as
already changed.

## Goal

Fully align Durin's asset-definition, opening, session, toolkit, host, and
extension architecture with the modern UE model. Completion requires production
consumers to use the new model and removal of the old workspace registration
path. Shorter registration helpers and permanent compatibility adapters do not
satisfy the goal.

Alignment means equivalent responsibilities and lifecycle behavior, adapted to
Durin's reflected `D` objects, standard containers, Mona/ImGui presentation,
transaction services, package reload, and asynchronous operations. It does not
require copying Slate, UE containers, every specialized editor, or unrelated
runtime systems. Standalone and world-centric hosting, Edit/View opening,
default editing, lifecycle events, recent assets, and session restoration are
included. Local usage accounting was withdrawn in Stage 7; external telemetry collection is
not required. Editor-mode integration must have a real toolkit extension seam;
porting UE's individual modeling or gameplay modes is outside this plan.

## UE Reference and Parity Policy

Reference baseline: the UE 5.8 public API and migration documentation reviewed
on 2026-11-06:

- [UAssetDefinition](https://dev.epicgames.com/documentation/unreal-engine/API/Editor/AssetDefinition/UAssetDefinition?lang=en-US):
  type behavior, metadata queries, opening, and separate menu extensions.
- [UAssetDefinitionRegistry](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Editor/AssetDefinition/UAssetDefinitionRegistry):
  reflected definition discovery and class lookup.
- [UAssetEditor](https://dev.epicgames.com/documentation/unreal-engine/API/Editor/UnrealEd/UAssetEditor?lang=en-US)
  and [FBaseAssetToolkit](https://dev.epicgames.com/documentation/unreal-engine/API/Editor/UnrealEd/FBaseAssetToolkit?lang=en-US):
  editor objects, toolkit creation, and reusable UI foundations.
- [FAssetEditorToolkit](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Editor/UnrealEd/FAssetEditorToolkit),
  [FToolkitManager](https://dev.epicgames.com/documentation/unreal-engine/API/Editor/EditorFramework/FToolkitManager),
  and [UAssetEditorSubsystem](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Editor/UnrealEd/UAssetEditorSubsystem):
  toolkit hosting, live-instance management, and asset/editor associations.

These are public API references, not proof that all UE editors use the
`UAssetEditor`/`FBaseAssetToolkit` pair. Durin initially selected that pair as its standard
implementation path; Stages 7-8 supersede its mandatory use with a direct Toolkit pilot. Stage 0 must record exact API/version observations and
any intentional adaptation before coding; uncertainty about undocumented UE
internals must not be presented as a verified UE contract.

## Final Architecture

Stages 7-8 amend the original target: DAssetEditor/FBaseAssetToolkit is an optional
implementation shape, not a requirement for every asset editor. StaticMesh uses a
direct Toolkit instance through the same definition and opening protocol. The
implemented owning-domain contract and Stage 8 ownership decision govern this
pilot where earlier session-only diagrams or migration rows differ. The pilot
opens/restores both host modes; live direct-Toolkit transfer remains outside this
bounded pilot and native transfer interaction is still unqualified.

### Definitions and Discovery

`DurinEd` owns reflected `DAssetDefinition`, `DAssetDefinitionDefault`, and a
definition registry. A definition describes a type, not an open editor: supported
class identity, display name, color, icons, category paths, metadata filters,
operation support, and activation/open behavior. Definitions hold no active
document, preview, selection, transaction, or host state.

The registry has process-level membership tied to reflected type/module
lifetime. Eligible definition CDOs register automatically once reflection is
ready, including definitions from project/plugin modules loaded later. Dynamic
definitions use an explicit scoped registration path. Both paths share duplicate
checks, generation identity, invalidation, and retirement behavior. One effective
definition is allowed per exact asset class; nearest supported ancestor fallback
is deterministic, exact matches win, and registering a derived definition
invalidates affected lookup caches. Lookup must not load an asset package.

Definitions use stable class paths/metadata rather than forcing asset loading
or keeping unloaded provider class pointers alive. Discovery may load configured
provider modules through existing module mechanisms, but never creates all
toolkits at startup and never scans arbitrary DLLs. Module availability and
class discovery follow declared project/plugin composition.

### Open Dispatch and Sessions

Each `DEditorEngine` owns its `DAssetEditorSubsystem` and its sessions. Definition
queries are shared; open state, recent/restore state, operations, and hosts are
scoped to the requesting editor. An explicit context identifies the editor and
optional toolkit host; definition callbacks do not silently select `GEditor`.

The sole public open pipeline is:

```text
AssetEditorSubsystem -> definition lookup -> activation/open policy
  -> reuse/focus or create DAssetEditor session
  -> create FBaseAssetToolkit -> attach IToolkitHost
  -> publish committed asset/editor associations and opened events
```

`DAssetEditor` is the reflected session owner and the single externally indexed
`IAssetEditorInstance` for that session. The subsystem strongly owns active
session objects. Each session owns its toolkit's C++ lifetime; toolkit back
references do not create an ownership cycle. An internal toolkit manager tracks
live toolkits and host relationships without becoming a second session owner.
Direct toolkit implementations, if needed during migration, use one explicit
instance-registration mode and must never double-register the wrapper/toolkit.

A session edits one or more assets and can contain multiple internal documents.
Type ID, session ID, asset identity, document ID, panel ID, and persistent layout
key are separate concepts. Default opening focuses a suitable existing primary
session; secondary editors, grouped editing, or additional sessions require an
explicit definition/open policy. One class does not imply one live instance.

Open arguments carry asset metadata/path selections, Edit/View method, host/mode,
request origin, and an explicit loading service. Results distinguish unhandled,
rejected/failed, completed, cancelled, and accepted pending work. Pending results
include a request identity and an observable terminal outcome. Batch opening
reports per-item outcomes and aggregate status; it does not equate acceptance
with completion or discard successes when another item fails.

### Toolkit, Documents, Hosts, and Commands

`FAssetEditorToolkit` supplies common initialization, edited-object association,
focus/close integration, command routing, menus/toolbars, panel spawners, and
layout restore/reset. `FBaseAssetToolkit` adds opt-in viewport/details defaults;
graph-only or read-only editors need not construct unused panels or previews.
Editor-mode/tool extensions attach to an existing toolkit through a scoped seam,
without adding a second asset-routing mechanism.

`IToolkitHost` owns presentation attachment for standalone and world-centric
modes. Standalone means a host independent of the Level workspace, with real
native-window lifecycle where requested; it is not just a different dock ID.
World-centric hosting attaches to an eligible Level host. MainFrame provides
shell services and composition, not concrete asset-editor routing. Host
destruction requests normal close; pending or rejected close must not destroy
the owning session. Non-window test hosts support the same semantic lifecycle.

`FEditorDocumentManager` becomes a session-owned internal-document controller.
It retains document identity, activation, pending opens, dirty/close decisions,
and reload participation, but no type definitions or process-wide toolkit table.
Root host tabs represent sessions; document tabs/panels belong to their toolkit.
The Level workspace remains a specialized host and scene controller, not a
mandatory singleton-policy variant in every asset-editor descriptor.

Application commands resolve through focused toolkit/document command context,
then host/global context. Feature commands and context menus use scoped extension
registrations. Closing a session removes its callbacks before its state retires.
Layout persistence uses stable editor/panel identities and canonical asset keys
where needed, never transient session counters. Preserve existing settings and
layout keys or provide an explicit tested, versioned migration.

### Browser, Default Editing, and Lifecycle

ContentBrowser consumes definition metadata for presentation, categories, and
type filters. Creation, import/reimport, thumbnail rendering, detail providers,
and menu commands remain independent service extensions. This plan reuses the
existing metadata-based browser extension system and does not replace it with
an all-purpose definition callback. Querying menus, applicability, tooltips,
icons, or filters may not load selected asset packages; execution may load them.

`DAssetDefinitionDefault` supplies a generic property editor for supported types
without a specialized editor. Edit/View support is explicit; View blocks all
mutating commands, property edits, imports initiated by that view, and save paths.
It does not make an incompatible package eligible for fallback editing.

Lifecycle events distinguish requested, committed opened, focused, asset removed,
close requested, and closed outcomes. Reentrancy uses snapshots and lifetime
checks. MRU and persisted session restore use canonical asset identities,
supported method, and stable editor identity. Restore occurs after project and
providers are ready, handles missing assets/providers per item, and cannot race
or replace the authoritative configured startup-Level operation. Stage 7 removes
unused local usage accounting while retaining lifecycle subscriptions and MRU.

### Safety and Retirement

Session ownership retains edited objects through callbacks, asynchronous opens,
GC, and atomic reload. Reload and rename/relocation update edited-object roots,
document references, associations, persisted restore entries, and layout identity
according to an explicit preservation policy. Failed reload leaves the old
session/world usable; indexing alone never loads packages.

Removing a definition stops new dispatch and invalidates lookup caches; it does
not forcibly erase dirty sessions. Pending requests hold valid provider lifetime
or cancel and drain before provider retirement. Physical module unload requires
no live sessions, CDO/type instances, callbacks, extension copies, pending work,
or provider-owned custom changes. Preserve existing module preflight and operation
gates; automatic discovery does not imply support for dynamic DLL reload.
Providers remain non-reloadable unless the full reflected-type teardown protocol
is separately available and qualified.

Save/Discard/Cancel behavior, failure retention, transaction preview settlement,
async compilation cancellation/drain, package compatibility rejection, PIE edit
restrictions, and default-Level readiness remain mandatory. A generic editor or
new host must not bypass existing guards.

## Interface Migration Map

Names below are selected architectural targets; Stage 0 freezes exact signatures
and ownership before implementation. Shared APIs remain in the existing owning
`Durin` namespace unless a documented module extraction is necessary.

| Current interface or constraint | Final replacement or adjustment |
| --- | --- |
| `FAssetEditorRegistration` with class strings, Toolkit ID and `EDocumentPolicy` | Definition lookup plus `DAssetDefinition::OpenAssets(FAssetOpenArgs)`; remove old route and policy structures after migration |
| Parallel `Toolkits`/`AssetEditors` registration batch | Independent automatic definition registration, live-session creation, and host/panel registration; no mandatory cross-table batch |
| `IAssetEditorToolkit` as one eagerly created multi-document service per type | Session-scoped `FAssetEditorToolkit`/`FBaseAssetToolkit` implementations with common lifecycle behavior |
| `FAssetEditorToolkitId` used as type and generated document dock identity | Separate stable editor type, live session, document/panel, and persistent layout identities |
| `FAssetEditorToolkitDescriptor` mixes host metadata and singleton policy | Toolkit identity/layout initialization plus host-owned workspace/tab descriptors; remove duplicate singleton key/label/closable fields |
| `FEditorDocumentManager::RegisterBatch`, `OpenAsset`, `CanOpenAsset`, `FindToolkit`, and toolkit enumeration | Definition registry and subsystem dispatch for types; toolkit manager for live UI; session-local document operations |
| `FWorkspaceAssetEditorInstance` per-document adapters | Session object implements `IAssetEditorInstance`; one association entry per actual session/asset pair |
| `IAssetEditorInstance` focus/close-only subset | Add edited-object removal and host/tab access as required; retain close reason and expose pending close via structured operation state |
| `DAssetEditorSubsystem` bool/path overloads | Keep convenient wrappers, add metadata/open-args dispatch and per-item structured results; wrappers document accepted versus completed semantics |
| `IAssetEditorModule::RegisterToolkits` / `UnregisterToolkits` | Ordinary module lifecycle, reflected definitions, and separately scoped service extensions; remove mandatory interface |
| `FAssetTypePresentation` duplicates class display metadata and fixed categories | Definition-backed type presentation and category paths; retain separate rich details/command extensions where appropriate |
| MainFrame's fixed Material/Texture/StaticMesh registration loop | Declared module loading plus generic discovery and hosts; no concrete feature-editor list in dispatch code |
| Global menu drawing across all registered workspaces | Scoped menu extensions and active command contexts independent of eagerly created toolkit objects |

Relax exact-class-only matching, one-instance-per-type, one-document-per-instance,
mandatory global root keys, mandatory batch coupling, and destructive definition
unregistration. Retain exact-definition uniqueness, deterministic resolution,
atomic registration visibility, game-thread mutation, explicit provider lifetime,
and no silent loss of edits. Removing a uniqueness constraint at the instance
level must not remove it at the definition level.

## Dependencies and Boundaries

- [Runtime Startup And Scene Activation](RuntimeStartupAndSceneActivation.md)
  owns the authoritative project/Level load and readiness state machine. Stage 5
  integrates with its current contract; do not introduce another readiness owner
  or claim its outstanding gates as passed here.
- [UE-Aligned Override Loading](Archive/2026-10/UEAlignedOverrideLoading.md) owns loading
  semantics; editor opening consumes those services without changing package
  serialization or class override policy.
- [DurinEditor SDK Packaging](DurinEditorSdkPackaging.md) owns external SDK
  packaging. New exported APIs and dependencies must be communicated there;
  this plan qualifies a project-defined editor without claiming SDK delivery.
- [Async asset operations](../Editor/Architecture/Assets/AsyncAssetOperations.md)
  and existing transaction/reload contracts remain the operation substrate.
- Inspect all source and test roots declared by `Durin.dworkspace` (Engine,
  Sandbox, RoadWeaver) for each changed shared symbol and migrate all consumers.
  Do not add a module dependency from DurinEd to concrete feature editors or
  from runtime modules to editor-only definitions.

## Frozen Execution Contracts

The original stage contracts below are historical baseline decisions. Stages 7-8
supersede mandatory reflected ownership, the unused Toolkit index and usage
accounting; unchanged loading, close, GC and provider safety contracts remain.

### Consumer Inventory and Baseline Fixtures

The affected production framework is owned by DurinEd; MainFrame owns shell
composition, ContentBrowser owns extension presentation, and LevelEditor owns
scene/workspace hosting. MaterialEditor provides Material, MaterialInstance,
MaterialFunction and MaterialParameterCollection; TextureEditor provides
Texture2D and VolumeTexture; StaticMeshEditor provides the read-only mesh
inspector. TextureCube currently has thumbnail/browser support without a toolkit
route: preserve that behavior until the generic editor explicitly supports it.
Creation/import/detail/thumbnail contributions remain with those modules.

Direct shared-symbol searches of Sandbox and RoadWeaver Source found no existing
toolkit/route/type-presentation consumers. Their declared modules and startup
Levels are nevertheless integration consumers, and Stage 6 must repeat the search.
Tests using the old framework include EditorWorkspaceTests, EditorSubsystemTests,
AssetPackageReloadTests, texture/material/mesh thumbnail and editor tests,
EditorTextureSmokeTests, and material-function persistence tests. Their configured
target owners, rather than source filenames, select validation.

Baseline layout fixtures copy the four current workspace layout headers and
retain their original stable IDs and versions. Stage 3 compares those IDs and
tests the new persistence mapping; copying headers does not prove an interactive
layout round trip. That smoke gate remains assigned to Stages 3 and 6.

### API and Ownership Decisions

All new shared APIs live in DurinEd, in namespace `Durin`. No new module is needed.

- `DAssetDefinition` derives from `DObject`, is abstract, and exposes const
  `GetAssetClassPath() -> std::string`, `GetAssetDisplayName() -> std::string`,
  category/icon/color metadata, `SupportsOpenMethod(EAssetOpenMethod) -> bool`,
  and `OpenAssets(const FAssetOpenArgs&) -> FAssetOpenResult`.
- `FAssetDefinitionRegistry::Get()` owns process membership. Definition CDOs
  enter after full construction in `PostInitProperties`; `BeginDestroy` removes
  their exact generation. An explicit scoped token strongly retains dynamic
  definitions; registry indexes are non-owning object keys. Exact class paths
  are unique. Registry lookups return borrowed definitions valid only until the
  next lifecycle mutation; dispatch retains the chosen definition during calls.
- `FAssetOpenArgs` contains explicit requesting subsystem/editor context,
  canonical path/class metadata selections, Edit/View method, host/mode and
  origin. Loading is an explicit method/service used during execution only.
  `FAssetOpenResult` owns per-item status, diagnostic and request identity.
- `DAssetEditorSubsystem` owns request state and session strong references.
  `OpenAssets(const FAssetOpenArgs&)` is the structured entrypoint; legacy bool
  wrappers mean accepted (completed or pending), never proof of opened state.
  `GetOpenRequest` and `CancelOpenRequest` expose pending request outcomes.
- A definition rejection is final: it cannot fall through into the legacy
  adapter or generic editor. The Stage 1 adapter is used only when no definition
  is available and an old exact route exists; it does not publish fake CDOs.
  It is deleted in Stage 6. Unsupported View never uses the Edit-only adapter.
- `DAssetEditor::Initialize(const FAssetEditorInitArgs&)` is transactional.
  The subsystem owns the reflected object; it owns a shared toolkit with bounded
  callback/frame leases and
  session-local document manager. `CreateToolkit()` is its overridable factory.
  Toolkit/host back references are weak. `IToolkitHost` owns presentation only;
  destroying presentation must first resolve the session close result.
- `FAssetEditorToolkit::InitAssetEditor` installs identity, edited objects,
  command context and host attachment. `RegisterTabSpawners` /
  `UnregisterTabSpawners` own scoped panel factories; `FBaseAssetToolkit`
  optionally constructs default viewport/details panels.
- `FEditorDocumentManager` remains responsible for its document state machine
  during transition and moves under the session in Stage 2. The shell's one
  confirmation coordinator serializes pending close prompts across sessions;
  each session owns the operation and continues to exist while confirmation is
  pending. No destructor performs an implicit Discard.
- Runtime scene/Level operations and `DEditorEngine` retain startup readiness.
  The Level definition delegates to that operation; MainFrame only presents it.

```text
process reflection -> definition CDO lifetime -> registry (non-owning keys)
DEditorEngine -> DAssetEditorSubsystem -> strong DAssetEditor sessions
  -> shared toolkit -> session document controller / strong edited assets
  -> weak host attachment -> standalone or Level presentation
```

Open state is `Requested -> Pending -> Completed | Failed | Cancelled`, with
immediate `Unhandled | Rejected | Completed | Failed` outcomes permitted.
Request IDs are editor-local monotonic values; late completions must check both
request identity and provider/object generation. Close state is
`Requested -> PendingConfirmation/Draining -> Closed | Rejected | Cancelled`.
Save failure retains the pending close and all edits. A grouped session publishes
only committed asset membership, and partial batch failure never erases successful
items. Request state is retained until explicit release/session teardown, with a
bounded terminal-history policy added alongside MRU in Stage 5.

Events occur in this order: requested before dispatch; opened only after
initialization/asset association commits; focused after successful activation;
close requested before policy; asset removed after association removal; closed
after final association removal and before session retirement. Pending/rejected
work emits no opened/closed event. Snapshot dispatch rechecks subscriber and
session membership after callbacks; shutdown stops new requests before draining.

### UE Parity Matrix

The UE 5.8 public links in the reference section identify the baseline API.
The cases below are Durin's selected executable contract, not claims about
undocumented UE internal ordering.

| Reference API / capability | Durin equivalent and adaptation | Owner stage | Acceptance case |
| --- | --- | --- | --- |
| AssetDefinition / definition registry | Reflected `DAssetDefinition`, CDO registration and scoped dynamic definitions | 1 | Late class discovery, duplicate rejection, ancestor lookup and removal invalidate caches without package loads |
| AssetDefinition OpenAssets | Explicit host context and structured results, with Durin pending request identities | 1 | Mixed batch, recursion, failure and observable pending cancellation |
| AssetEditor / grouped editing | One reflected owner implements the externally indexed instance; standard-container ownership | 2 | Multiple sessions of one type, grouped assets and secondary instance fixture |
| AssetEditorSubsystem primary lookup | Per-editor associations, exact object generations | 2 | Reopen focuses the correct primary; two hosts cannot share mutable session state |
| AssetEditorToolkit / BaseAssetToolkit | Mona/ImGui panels, shared lifecycle and opt-in defaults | 2-3 | Shared close/command behavior; graph-only fixture allocates no default viewport |
| Toolkit host/mode | Real standalone and Level world-centric hosts | 3 | Native host close veto, independent same-type docking and persistence round trip |
| Read-only open method | Explicit View capability with guarded mutation paths | 2, 4 | Properties, commands and save cannot mutate View sessions |
| AssetDefinitionDefault | Generic reflected property session | 4 | Project-defined type opens without Engine dispatch edits; incompatibility remains rejected |
| Tool menu extensions / categories | Existing scoped browser extensions, definition-backed category paths | 3-4 | Metadata queries load zero packages; action execution loads selected assets only |
| Toolkit mode/tool attachment | Scoped extension interface with no second asset route | 3 | Attach/remove a test tool and release callbacks with the session |
| AssetEditorSubsystem notifications | Explicit committed lifecycle subscriptions | 5 | Pending failure emits no opened event; reentrant close safely removes later instances |
| Recent and restore support | Canonical project-scoped MRU/session settings | 5 | Missing provider/item isolation, method restoration and configured-Level precedence |
| Editor usage data | Withdrawn in Stage 7: no production consumer; keep lifecycle events and MRU | 7 | No counters/timing API or extra provider/session retention |
| Module/provider retirement | Existing mapped-module and reflected teardown restrictions | 6 | Removing definition leaves dirty session alive; unsafe physical unload is refused atomically |

No parity item is unassigned. Host/command fixtures cover the cross-cutting
ownership contract; feature regressions cover texture/material/mesh operations.

## Post-Stage 5 UE Simplification Review

Reviewed implementation: `e2d022b22`, 2026-11-06. This is a code/design review,
with the interactive qualification exclusions recorded above. Stage 6 now adopts this review. Recommendations below identify changes to the current
migration design rather than claiming the checkpoint is already fully UE-aligned.

### Reference Boundary

UE 5.8 public APIs confirm these distinct roles:

- [UAssetDefinition](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Editor/AssetDefinition/UAssetDefinition)
  separates metadata/open behavior from menu extensions, and exposes
  `GetAssetOpenSupport`.
- [FAssetOpenSupport](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Editor/AssetDefinition/FAssetOpenSupport)
  carries support for the requested method and an optional required toolkit mode.
  [FAssetArgs](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Editor/AssetDefinition/FAssetArgs)
  holds one metadata selection; [FAssetOpenArgs](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Editor/AssetDefinition/FAssetOpenArgs)
  adds method/host and exposes a mode getter.
- [UAssetEditor](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Editor/UnrealEd/UAssetEditor)
  provides editor-object lifetime and toolkit creation.
  [FBaseAssetToolkit](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Editor/UnrealEd/FBaseAssetToolkit)
  supplies widgets, default layout, tab spawners, details and viewport factories.
  [FAssetEditorToolkit](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Editor/UnrealEd/FAssetEditorToolkit)
  also implements `IAssetEditorInstance`; the object/toolkit pair is not the only
  publicly supported UE implementation path.
- [ILevelEditor](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Editor/LevelEditor/ILevelEditor)
  implements `IToolkitHost` and exposes hosted toolkits. It is a specialized host,
  not evidence that Level must be wrapped as an ordinary `UAssetEditor`.
- [FToolkitManager](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Editor/EditorFramework/FToolkitManager)
  manages live toolkits, while
  [UAssetEditorSubsystem](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Editor/UnrealEd/UAssetEditorSubsystem)
  exposes asset/editor associations, lifecycle notifications and restore services.

These references verify public interfaces. Recommendations about Durin ownership
and simplification are inferences from those interfaces and this checkout, not
claims about uninspected UE private implementations.

### Findings And Recommended Order

1. **P1: remove the compatibility architecture before adding more abstractions.**
   `FEditorDocumentManager` still contains toolkit/asset-route tables,
   registration batches, `FWorkspaceAssetEditorInstance`, and `bSessionMode`.
   MainFrame still builds descriptor-driven layout/menu paths and draws a legacy
   toolkit collection before drawing owned sessions. Production Level no longer
   needs those registrations. This is the largest removable source of conceptual
   branching: the same class is both an old workspace registry and a new session
   document controller. Remove the old mode and migrate its meaningful tests;
   retain one session-local document state machine. Remove the unused
   `IAssetEditorModule` header and the remaining shell/dispatch adapters together.
   Evidence: `EditorDocumentManager.cpp` registry state and `RegisterBatch`,
   `MainFrameModule.cpp` descriptor menus/drawing, and `AssetEditorModule.h`.

2. **P1: give host membership one authority and make world-centric hosting shared.**
   A session stores its host, `FToolkitManager::FEntry` stores another host link,
   and transfer updates both. The main-frame factory creates a separate
   `FMainFrameToolkitHost` even for world-centric sessions attached to the same
   main frame. Level adds `FLevelEditorModule::FIntegrationState::Hosts`, settings,
   callbacks and a weak session registration on top of subsystem session ownership.
   This creates bookkeeping and initialization-order constraints around one active
   editor. Keep the live toolkit-manager mechanism, but derive its host lookup
   from the authoritative session/toolkit relationship instead of double-writing
   it. Use one Level/main-frame world-centric host; create standalone hosts per
   independent window. Reassess the thin `DLevelAssetEditor` wrapper: retain it
   only if needed for Durin's asset association/lifetime contract, and move its
   configuration into the session/host rather than a parallel module registry.
   Let the Level definition delegate to the existing scene operation. Do not
   duplicate or replace that controller. Evidence: `ToolkitManager.h::FEntry`,
   `AssetEditorSessions.cpp::TransferAssetEditor`, the MainFrame host factory,
   and `LevelEditorModule.cpp::FIntegrationState`.

3. **P1: replace grouped-open replay with one real batch/session operation.**
   `OpenAssets` rewrites `Selected.Assets` to a single target. Restore therefore
   carries the full group separately in `GroupAssets`. The default definition
   then reloads/revalidates the group and scans sessions for every target.
   An N-asset group takes N group traversals; caching may avoid repeated disk
   loads, but it does not remove the duplicated execution and result reasoning.
   Use one authoritative selection and dispatch a compatible batch once, with
   one group initialization and per-item outcomes. Keep grouping policy explicit
   in the definition/session operation, including specialized mixed-type groups.
   Do not add a second registry or public builder layer to accomplish this.
   Evidence: `AssetOpenTypes.h::FAssetOpenArgs`,
   `AssetEditorOpen.cpp::OpenAssets`, `AssetDefinition.cpp::OpenAssets`, and
   `AssetEditorLifecycle.cpp::RestoreSessions`.

4. **P1: separate persistent layout identity from mutable resource identity.**
   The session derives `LayoutKey` once from canonical asset paths. Relocation
   remaps document/feature resource keys, while Material settings and common panel
   stores continue using the old session layout key. Restore serializes the new
   asset paths and derives a new key on restart. Code inspection therefore
   indicates an opened-and-renamed asset can use one settings namespace now and
   another after restart. Choose one explicit layout-identity policy: a stable
   editor/layout/role identity, or an asset/group identity whose aliases are
   migrated on authoritative rename. Apply it consistently to dock IDs, panel
   visibility, Material preferences and restore. Preserve existing layouts through
   an explicit migration rather than silently changing identifiers. Add a rename,
   close, restore and panel/settings round-trip regression. Evidence:
   `AssetEditor.cpp::Initialize`, `MMaterialEditor::OnResourceIdRemapped`,
   `MMaterialEditor::OnSessionBound`, `ToolkitUI.cpp::RestorePanelLayout`, and
   `AssetEditorLifecycle.cpp::CaptureSessions`.

5. **P2: normalize opening support and context before loading or constructing UI.**
   Method support is repeated in definitions and toolkit subclasses; host-mode
   requirements are checked later, and Level has its own mode adjustment.
   `FAssetOpenArgs` and the session separately store mode in addition to the host's
   mode. Introduce the UE-style support query, including required mode, and resolve
   one effective opening context before execution. Reuse the same feature policy
   for direct session validation, preserving the tested anti-bypass guard. Keep
   `Origin` diagnostic-only; the literal `Origin == "Restore"` should not be a
   control protocol. Pending requests also need to retain the effective method:
   `FOpenRequest` currently lacks it, so terminal events default to Edit even for
   a future View-capable pending definition. This is a concrete context-loss gap,
   not a reason to add another lifecycle manager. Evidence: `AssetDefinition.h`,
   `AssetEditorToolkit.h`, `MLevelEditor.h`, `LevelAssetEditor.cpp`, and
   `AssetEditorOpen.cpp::BeginOpenRequest/CompleteOpenRequest`.

6. **P2: consolidate repeated execution/presentation plumbing.**
   `DAssetEditor::DrawToolkitFrame` and the final toolkit draw wrapper each retain
   objects, defer GC and dispatch the same owning-editor callback. Keep one public
   execution boundary; internal calls should use its implementation path instead
   of nesting wrappers. Browser host lifetime matching/reset code is copied in
   Texture, Mesh, glTF and RoadWeaver modules. `FExtensionInvocation` and
   `FExtensionHost` carry overlapping ownership context. Standardize one host
   context and scoped presentation helper, retaining weak ownership, admission
   checks and cancellation/draining. Finally, default panels currently require
   both constructor booleans and optional factories; factory presence can express
   optional panels directly. Graph-only toolkits should need no opt-out protocol.
   These are bounded simplifications after ownership and batching are settled.

### Target Responsibility Map

| Component | Single responsibility after simplification |
| --- | --- |
| Asset definition | Metadata, open support and type-specific operation/grouping policy |
| Asset-editor subsystem | Dispatch, owned session lifetime, association indexes, observable requests and local history/restore |
| Asset-editor object | Ordinary session lifetime and its authoritative edited-document ownership |
| Session document controller | Internal document activation, pending candidate and dirty-close state; no type routes or toolkit registry |
| Toolkit/base toolkit | Widgets, scoped commands/panels and feature editing behavior |
| Toolkit manager | Live-toolkit discovery/indexing; no second authoritative host assignment |
| Toolkit host | Presentation, docking, native focus and close integration; one shared world-centric Level host |
| Level controller | Existing scene operation, readiness, PIE and project transitions |

A normal asset open should read as:

```text
Browser / Recent / Restore
  -> AssetEditorSubsystem
  -> AssetDefinition (support + execution)
  -> AssetEditor session -> Toolkit + session documents
  -> standalone window OR shared Level host
```

A Level open delegates from its definition to the authoritative scene controller
and existing Level host. Any thin session association bridge should stay visible
as that adaptation, not become a competing Level owner.

Keep metadata-only discovery, scoped registration, pending outcomes, dirty-close
vetoes, read-only enforcement, provider retention and atomic reload participants.
They protect demonstrated behavior. The simplification target is duplicate
ownership, input selections and compatibility modes. Do not remove lifetime
safety merely to reduce the number of classes or lines.

The accepted continuation order is: remove legacy mode; normalize support and
batch dispatch; consolidate host/Level ownership; unify identity/presentation
plumbing; then complete the existing integration gates. Skipped interface validation remains outstanding; Stage 6 implementation now
follows this order.

## Implementation Stages

### Stage 0: Freeze parity, ownership, and migration contracts

Dependencies: none. Outcome: an executable API and parity specification with
baseline evidence; no architectural questions are deferred into final acceptance.

- [x] Record the reviewed current architecture, target responsibilities, interface
  migration map, and selected stage order in this plan.
- [x] Inventory production/editor/test consumers across all workspace projects;
  map each editor family, route, menu, thumbnail, host, and startup dependency.
- [x] Freeze API signatures, module ownership, session ownership graph, reflected
  CDO admission/retirement, open/close operation states, and event ordering.
- [x] Record a UE parity matrix: referenced API/version, Durin equivalent,
  deliberate platform adaptation, stage owner, and observable acceptance case.
  Cover grouped sessions, primary/secondary editors, View, hosts, default editor,
  reflection discovery, extensions, MRU/restore, and local lifecycle accounting.
- [x] Capture baseline behavior/layout fixtures and select registered test
  targets with the host DevTool; record current failures separately.

Acceptance: reviewable ownership/state diagrams and parity matrix exist; no
unknown destruction order, close confirmation owner, startup integration owner,
or unassigned parity requirement remains. Baseline receipts identify revision,
configuration, project, selections, and fixture paths under ignored `Build/`.

### Stage 1: Introduce asset definitions and metadata-based dispatch

Dependencies: Stage 0. Outcome: one definition-based open entrypoint works while
legacy workspaces remain behind a temporary, explicitly bounded adapter.

- [x] Implement reflected definition/default classes, registry, class-path
  lookup, exact/ancestor precedence, membership revisions, and duplicate diagnostics.
- [x] Implement automatic CDO registration for initial and later module loads,
  scoped dynamic definitions, and safe registry/editor teardown in either order.
- [x] Add open arguments, Edit/View capability queries, structured per-item
  results and observable pending requests to the subsystem.
- [x] Route object/path/browser opening through definition dispatch. The legacy
  adapter translates at that boundary only; definitions do not enter old batches.

Acceptance: tests prove automatic discovery from a project/module without editing
MainFrame; duplicate exact definitions are rejected without partial mutation;
derived definitions replace ancestor fallback deterministically; cache invalidation
works after removal/replacement; metadata queries load zero asset packages.
Mixed batches, recursive opens, unavailable providers and pending cancellation
produce stable per-item results. Existing supported opens continue through the
single pipeline. Adapter removal is assigned to Stage 6.

### Stage 2: Establish sessions, toolkit foundations, and a texture pilot

Dependencies: Stage 1. Outcome: Texture2D has a real owned session and toolkit;
the document-to-editor adapter is no longer its instance model.

- [x] Implement `DAssetEditor`, toolkit manager, common toolkit base classes,
  and a non-window host; establish scoped command/panel extension seams.
- [x] Separate session/document/panel/layout identities and move the pilot's
  document controller into its session.
- [x] Implement initialization rollback, strong edited-object ownership,
  single association publication, pending close and one confirmation coordinator.
- [x] Migrate Texture2D using the existing loading, build, preview, dirty/save,
  discard, transaction, and reimport services; add functional Edit/View behavior.

Acceptance: opening A/B creates independent sessions; reopening A focuses the
intended session; grouped and secondary-session fixtures prove cardinality is not
hard-coded. Each session is indexed once. Dirty Save/Discard/Cancel and save
failure retain correct state; pending compile/open cancellation drains safely.
View cannot mutate via properties, shortcuts, toolbar or save. GC, reload,
relocation, failed initialization, and headless-host destruction leave no stale
associations, roots or callbacks. Existing texture workflow regressions pass.

### Stage 3: Add hosts, panel lifecycle, and metadata-driven extensions

Dependencies: Stage 2. Outcome: both hosting modes and active-command routing
work independently of concrete editor registration.

- [x] Implement MainFrame/Level world-centric host attachment and standalone
  host/window creation, focus, close, and host-transfer policy.
- [x] Add panel spawner registration, saved layout restoration/reset, toolbars,
  active command contexts, and a scoped editor-mode/tool extension fixture.
- [x] Consume definitions for browser presentation, category paths and filters;
  migrate duplicated `FAssetTypePresentation` metadata without moving import,
  thumbnail rendering, rich details, or menu execution into definitions.
- [x] Preserve stable layout IDs or implement the Stage 0 migration policy.

Acceptance: two sessions of one editor type coexist without dock/panel collisions
in both modes; closing a native host respects dirty/pending close and does not
destroy a vetoing session. Focus routes Save/Undo/Redo to the intended context;
panel close/reopen/reset and application restart restore the intended layout.
An extension attaches/removes without opening a second session or leaking a
callback. Browser menu/filter/icon queries do not load packages, while execution
loads only selected supported assets. Existing import/thumbnail behavior remains.

### Stage 4: Migrate all asset families and generic editing

Dependencies: Stages 2-3. Outcome: all ordinary feature editors use sessions and
definitions; ordinary module startup publishes their independent extensions.

- [x] Migrate VolumeTexture, StaticMesh Inspector, Material, MaterialInstance,
  MaterialFunction and MaterialParameterCollection definitions and sessions.
- [x] Move per-document maps into the appropriate session/document ownership;
  preserve intentional grouped editing instead of blindly cloning state.
- [x] Supply the generic property toolkit through `DAssetDefinitionDefault` and
  qualify inherited metadata, specialized overrides and unsupported-type behavior.
- [x] Remove each migrated family's toolkit batch, manual type-presentation
  duplication and monolithic integration callbacks; retain independent scoped
  thumbnail/import/creation/detail registrations.

Acceptance: every listed family opens, focuses, saves or correctly remains
read-only, closes, reloads and relocates through the new path. Material
Compile/Apply/Save, function/collection edits and undo history retain their
existing semantics. StaticMesh remains read-only. A project-defined reflected
asset uses the generic editor without MainFrame changes. Unsupported View/edit
methods and incompatible packages fail explicitly; no fallback bypasses package
compatibility. Mixed-family shutdown releases callbacks and previews in order.

### Stage 5: Integrate Level, lifecycle events, recent assets, and restore

Dependencies: Stages 3-4 and the current startup/scene activation contract.
Outcome: Level uses its specialized host/controller through the same definition
entrypoint; user-visible lifecycle and restore behavior are complete.

- [x] Replace duplicated Level singleton descriptor/route policy with a Level
  definition that activates the authoritative existing scene/workspace operation.
- [x] Keep startup-Level association, readiness, PIE, project switching, scene
  cancellation, and first-presentation receipts under their existing owners.
- [x] Implement lifecycle subscriptions and local usage accounting, canonical
  recent assets and explicit supported session persistence/restore policy.
- [x] Handle missing/renamed assets, missing providers, project changes, View
  sessions and mixed successful/failed restore items without a second open route.

Acceptance: default-Level and interactive open share the authoritative operation;
failed/cancelled replacement keeps the previous scene/document; project switching
drains old sessions and callbacks. PIE mutation restrictions remain effective.
Requested versus opened events differ for pending/failing requests; callbacks may
close another session safely. Two editor hosts have isolated associations and
restore state. Restart restores supported sessions and modes with deterministic
focus; missing items are reported individually; recent entries and accounting
record committed outcomes without retaining editors. Restore cannot overwrite a
configured startup Level or make startup ready before scene presentation.

### Stage 6: Remove legacy APIs and qualify the final architecture

Dependencies: Stages 1-5 and the accepted post-stage review. Outcome: one opening
pipeline, one host authority and one session-local document controller, with no
compatibility architecture. Execute the first three work packages before the
remaining parity work; retain the final full-architecture acceptance gates.

- [x] 6A - Delete `IAssetEditorModule`, registration batches/handles, asset routes,
  singleton descriptors, `FWorkspaceAssetEditorInstance` and legacy dispatch.
  Remove the shell document registry and legacy drawing path. Migrate meaningful
  document/close/reload tests to attached session controllers; remove tests only
  for contracts that are deliberately deleted.
- [x] 6B - Make session host ownership authoritative. ToolkitManager indexes live
  toolkits without copying host assignment. Derive effective mode from the host.
  MainFrame owns one shared world-centric host and independent standalone hosts.
  Move Level settings/callbacks into its configured session and delete the module
  host/session map. Keep the thin Level association bridge and authoritative scene
  controller; do not add another scene owner. A preconfigured reflected editor may
  be initialized/committed through the subsystem, with ownership and failure
  checks identical to class-based creation.
- [x] 6C - Dispatch one unique metadata selection per definition batch. Replace
  `GroupAssets` replay with explicit single-session grouping of `Assets`; compatible
  mixed-type groups execute once through their selected definition. Preserve
  per-input outcomes, metadata conflict rejection, recursion protection, provider
  retention and cancellation. Carry the effective method into pending outcomes.
- [x] 6D - Normalize UE-style `GetAssetOpenSupport` queries with method support and
  optional required host mode. Resolve effective context before definition
  execution, loading or toolkit creation; explicit hosts must satisfy requirements.
  Direct session admission and transfer use the same feature policy. Remove toolkit
  capability duplicates and keep Origin diagnostic-only through an explicit
  restore flag. Delete legacy browser type-presentation registration and its fixed
  category enum; definitions supply metadata and independent extras supply details
  and thumbnail badges. Migrate fixtures to those production contracts.
- [x] 6E - Separate mutable resource paths, runtime-only window identity and
  named/versioned editor layout profiles scoped by method, host mode and role.
  Keep existing session snapshot contents; do not persist window IDs or geometry.
  Share saved defaults while isolating live docking, reusing existing ImGui layout
  storage. Abandon old asset/group layout namespaces without compatibility paths.
  Retain Material graph viewport persistence in its existing settings format, with
  per-resource merge and authoritative rename handling. Do not add global stores.
- [x] Consolidate duplicated execution and scoped host-presentation plumbing where
  ownership is already settled.
- [x] Qualify registration removal independently of live-session close, pending
  provider retirement, callback reentrancy and successive editor lifetimes. Keep
  unsupported DLL reload disabled; blocked unload must not partially retire.
- [x] Migrate source/test consumers in every `Durin.dworkspace` project, including
  project-defined definitions/toolkits without central dispatch changes. Publish
  lasting architecture contracts and record automated parity evidence.
Excluded from completion acceptance by the final scope decision: native dirty
close/transfer, panel/layout restart and interactive project switching remain
skipped and unqualified. Automated fixtures are not substitute manual passes.

Acceptance for 6A: no removed interface remains in production or tests; document
activation, deferred replacement, dirty close, GC roots and atomic reload still
pass through session controllers. The shell has no registry or parallel instance
adapter. No legacy toolkit-opening fallback remains behind a new name.

Acceptance for 6B: grouped sessions share the same world-centric host, standalone
windows remain independent, failed transfer preserves the original owner, and
retirement detaches only its own toolkit. Level close/reopen and successor-editor
lifetime tests pass without a module session map. Initialization failure cannot
publish an instance or retain callbacks.

Acceptance for 6C: an N-asset group invokes its definition and creates its session
once; ordinary same-definition selections invoke one batch and preserve their
individual/group policy. Duplicate inputs retain ordered results without duplicate
execution. Missing providers/assets, incompatible groups, View, reentrant shutdown
and pending completion retain explicit per-input outcomes.

Acceptance for 6D: support queries create/load no assets or UI; required modes and
explicit hosts resolve before execution. Direct sessions and transfer cannot bypass
feature/definition restrictions. Conflicting grouped requirements do not partially
execute. Origin text has no control meaning. Definition metadata and independent
extras can withdraw in either order, built-in and dynamic category filters agree,
and removed registration/support interfaces have no remaining consumers.

Acceptance for 6E: resource paths, runtime window identity and editor layout
profiles have distinct responsibilities. Concurrent windows and panels remain
independent while named defaults survive ImGui-context restart. Real relocation
keeps the current window/profile stable and reopened sessions use the same profile.
Shared settings do not overwrite unrelated preference fields or resurrect moved
viewport records. Existing session snapshots contain no added window state.
Old asset layout keys/APIs and fallback reads are removed across all workspace
consumers. No real interface qualification is claimed for the skipped operations.

Final acceptance uses the retained automated validation matrix below, including
the incremental all Editor consumer build. Real interface interactions are
excluded by the final explicit user scope decision and remain skipped/unqualified.
Recorded adaptations and unsupported mechanisms are limitations of completion;
they are not successful parity or manual-validation claims.

### Stage 7: Remove redundant state before the direct Toolkit pilot

Dependencies: Stage 6 automated closure. Outcome: one input for role/primary
policy, shared identity matching, and no unused secondary Toolkit index or usage
accounting requirement. Retain the general IAssetEditorInstance association
contract because Stage 8 uses it; do not restrict registration to DAssetEditor.

- [x] Remove the unused weak ToolkitManager index and migrate its tests to observable
  session/host membership. A future manager must have a real ownership role.
- [x] Keep primary and role only in FAssetOpenArgs; normalize the default secondary
  role in one helper and share asset-set equality across reuse/admission/transfer.
- [x] Remove local usage counters/timing and their test-only query, while retaining
  lifecycle events, MRU, close ordering and versioned restore snapshots.

Acceptance: owning framework/host/family/browser tests pass; shared consumers in
Engine, Sandbox and RoadWeaver compile. No production ownership mechanism is
added to satisfy a test. Record changed-document validation and final-input hashes.

### Stage 8: Pilot StaticMesh as a direct Toolkit instance

Dependencies: Stage 7. Outcome: StaticMesh Inspector opens through its definition
as a Toolkit implementing IAssetEditorInstance without a DAssetEditor wrapper.
Other families retain reflected editor ownership. UE supports both shapes; exact
class-name parity is not a reason to add another owner or a second open pipeline.

Pilot ownership decision: the subsystem retains committed direct Toolkits with
shared ownership; reflected editors retain their own Toolkits. A derived Toolkit
snapshot unifies presentation without duplicating session Toolkit registration.
Hosts continue to borrow Toolkits weakly. The Toolkit supplies its effective
instance/host/document context to common shell code, delegates to DAssetEditor
when session-backed, and stores only the direct lifecycle state when direct.
StaticMesh retains the existing document controller for reload/relocation and
preview identity; no new generic controller or alternate asset loading path is
introduced. Direct state is not a reflected object and creates no hidden Session.


- [x] Give direct Toolkits explicit initialization, provider/object retention,
  host attachment, focus, close and retirement. Uninitialized is not an alternate
  production execution mode. Retain the Mesh document service for tab identity, strong roots and relocation.
  Do not add a speculative document-free mode or generic controller during the pilot.
- [x] Make shell presentation, active commands, close coordination and supported
  restore enumerate both instance forms. Retain one authoritative host per instance
  and one owner for each Toolkit; do not construct hidden reflected Session adapters.
- [x] Migrate StaticMesh definition and remove its reflected editor wrapper. Verify
  independent assets, reopen/focus, Edit/View immutability, grouped/role identity,
  both semantic host modes, provider withdrawal, GC/reload/relocation and shutdown.
- [x] Publish implemented ownership contracts and record focused receipts plus one
  incremental all Editor build for changed exported toolkit/host interfaces.
Native interface acceptance remains skipped and unqualified, and is excluded
from the completed scope by the final user decision above.

Acceptance: ordinary browser/object/path/restore opens still use OpenAssets;
StaticMesh has no DStaticMeshAssetEditor allocation or separate dispatch registry.
Failed initialization publishes no association, retired handles reject commands,
and existing Texture/Material/Level behavior remains covered. This pilot does not
claim real interface acceptance or enable DLL reload.

### Stage 9: Consolidate definition opening and constrain direct inspection

Dependencies: Stage 8 automated pilot qualification. Outcome: one Definition
selection algorithm and one focus notification per successful new/reused open.
Direct Toolkit admission supports immutable inspectors in Edit or View and
mutable-feature Toolkits only when fixed to View. Writable Edit direct instances
are rejected before host creation, document callbacks or association publication.
No new dirty-close continuation state machine is added.

- [x] Extract selection compatibility, execution-time loading/retention, reuse
  matching, grouped/ordinary execution and per-input result assembly into a small
  private default-definition execution function. Registry retains registration/class lookup and
  Definition::OpenAssets remains the public type behavior, following UE's boundary.
  StaticMesh supplies only Toolkit identity and a construction factory through a
  protected default-definition helper. Do not expose compatibility/matching policy
  callbacks or add a registry, controller, policy-class hierarchy or owner.
- [x] Keep new-instance focus under subsystem admission and focus reused instances
  once in shared Definition execution. Cover reflected and direct event counts.
- [x] Enforce the read-only direct contract at admission, migrate direct fixtures
  to immutable inspectors where appropriate, and prove mutable Edit is rejected
  without host/document work while View remains read-only.
- [x] Run focused framework/family/browser/host checks and consumer compilation;
  publish final-input receipts and update lasting contracts. Native interaction
  remains skipped and unqualified, outside the final completion scope.

Acceptance: single/grouped selections, metadata compatibility, load failure,
role/mode/primary reuse and provider retention preserve their existing behavior.
New and reused opens each emit one Focused event per effective editor open. The
same helper serves default reflected editing and StaticMesh direct inspection.
Shared exported API changes require an incremental all Editor consumer gate for
Engine, Sandbox and RoadWeaver. No Game dependency change is intended.

## Validation Matrix and Evidence

Follow [build selection](../Agents/BuildAndRun.md) and
[test selection and result reuse](../Agents/Testing.md#choose-execution).
Resolve current registrations before execution; existing target names below
are starting points, not an instruction to run every target at every stage.

| Area | Required evidence and timing |
| --- | --- |
| Definitions, session ownership, dispatch and close states | Focused native contract/feature cases for each stage; start from `EditorShellTests`, `EditorHostToolTests`, and `ContentBrowserWorkflowTests`, confirming current source ownership |
| Texture, material and mesh behavior | Owning workflow/persistence/compile/preview target selections from the configured registry; include integration targets when their changed lifecycle is not covered by direct fixtures |
| Lifetime and package operations | GC/reentrancy/teardown plus `AssetPackageReloadTests` and relevant relocation/transaction tests; pending operations must reach observable terminal outcomes |
| Consumer API migration | Build owning modules and affected consumer targets across all projects in `Durin.dworkspace` at each shared API migration |
| Native windows, layout and startup | Historical startup receipts remain recorded. Real native gestures, interactive layout/restart and project switching are excluded from final acceptance by user decision; skipped and unqualified |
| Final integration | One incremental `all` build for the selected Editor profile after final API removal; justified by foundational exported toolkit/host headers, reflection discovery and module composition changes |
| Runtime isolation | Verify editor-only definitions do not enter Game module closure; run a targeted Game build when dependency changes affect that closure; do not add unrelated presets without a concrete risk |
| Documentation | Changed-document validation and all-plan lifecycle validation; implemented contracts must match the final code |

The final broad build is an explicit gate, not a clean-build requirement. Do not
repeat it after documentation-only changes when its tested inputs still apply.
No blanket GPU qualification or performance claim is required; retain bounded
preview/render integration coverage for lifecycle behavior actually changed.

Each stage records tested revision/worktree inputs, preset, selected cases,
project and host mode, result counts, report locations, manual reproduction and
remaining gates in Current Status. Store raw reports/layout fixtures under
ignored `Build/UEAssetEditorArchitecture/` or CI artifacts. A pending, skipped,
unavailable or baseline-failing required gate is not a pass. Add meaningful
behavioral tests rather than tests mirroring descriptor construction.

## Definition of Done

- [x] All retained implementation stages and automated acceptance gates have
  final-input evidence; explicitly excluded native interactions remain unqualified.
- [x] Automatic definitions, structured opening, real sessions, both hosts,
  reusable toolkits, default editing, metadata-only queries, lifecycle events,
  MRU/restore and scoped extensions are used by production consumers.
- [x] Every legacy route/registration/instance adapter is removed, all supported
  editor families and project consumers migrated, and no duplicate owner remains.
- [x] Automated dirty/pending close, GC/reload/relocation, transactions, compatibility,
  startup readiness, PIE and provider-retirement evidence is recorded for the
  retained scope; native gestures and unsupported Mesh reload are not claimed.
- [x] The selected UE alignment is closed with documented adaptations, exclusions
  and capability limits; implemented contracts are published. Removed lifecycle
  consolidation is not a completion requirement or an implementation claim.
- [x] Final build/test receipts are retained, completion-document validation passes,
  and Completed metadata is set; physical archival remains a separate batch action.
