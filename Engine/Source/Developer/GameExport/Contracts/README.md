# Game export migration contracts

Summary: Selected wire and deployment formats for the GameExportPipeline migration.

Last reviewed: 2026-11-15

These are the selected pipeline contracts. Stages 1-4's implemented subset and
temporary migration bindings are documented in
[Game deployment](../../../../../Documentation/Runtime/Application/GameDeployment.md).
The complete target contract is not yet implemented. JSON Schema checks
structure; the semantic checks below are mandatory for native readers and writers
for each implemented reader/writer. Fixtures use synthetic hashes and do not constitute runnable
exports. The active [plan](../../../../../Documentation/Plans/GameExportPipeline.md)
owns implementation and qualification status.

## Owners and component

`GameExport` is a registered tools-only Developer service and `DurinExportTool`
is its headless program. Both use the existing build owners.
GameExport may depend on Core, Projects and existing domain interfaces; it must
not depend on Launch's Game loop, Editor presentation or DevTool. Cook executes
in DurinAssetTool, not in the worker. The program owns process handles, stdin/stdout
and child lifetime. Core owns generic immutable bootstrap values and may parse
the generic Runtime subset without linking Projects or GameExport. Projects owns
project/plugin admission. Neither export target belongs in Game's runtime closure.

`component-v1.schema.json` describes a completed tool component relative to its
engine installation root. `Worker`, all schemas, providers and backend metadata
must be present in the hashed `Files` closure, together with worker/service native
dependencies and resources. Reject duplicate destinations and escaping paths.
`Capabilities` describes actual shipped functionality, not future intentions.
`build-source` requires source metadata, DHT, CMake and the declared toolchain;
`export-prebuilt` can reuse matching products without these. Missing/stale products
on a prebuilt backend fail with `capability`, never invoke DevTool or install tools.
Editor discovers this component through engine metadata. No `.agents`, Python
environment, DevTool profile or source checkout is part of the installed contract.

## Runtime envelope and bootstrap

`runtime-v2.schema.json` is deployment-root `Runtime.json`. Its canonical parent
directory is the deployment root; no redundant root string or cwd-based binding
is permitted. All physical paths use normalized forward-slash relative components.
`.` is allowed only for root bindings. Reject lexical escapes, absolute/drive/UNC
paths, streams, empty/dot components, trailing dots/spaces, Win32 device names and
aliases/reparse points escaping the canonical root. Validate target/host identity,
executable binding, configuration/mount bindings and required files before
publishing any FPaths value. Missing/corrupt files fail on stderr before logging
or Saved creation. Case comparisons follow platform semantics; Win64 DLL identity
and deployment collisions are case insensitive. Different hashes under the same
loader basename are an error, even if destinations differ.

`Identity` extends existing plugin build identity with `Project` and `Target`.
EngineBuild, platform, architecture, configuration, runtime variant and ABI remain
exact compatibility fields; they are not inferred from filenames. Win64/x64
Debug/Release are the initial capabilities. Mac/arm64 fields preserve generic
format semantics but do not authorize macOS staging. Unknown versions fail with
recook/restage guidance; no legacy-layout switch or source fallback is provided.

Standalone Game defaults to staged mode. `--runtime-manifest=<absolute-file>`
selects the envelope explicitly. Without it, the only location is
`<executable-directory>/../../Runtime.json`, canonicalized once (the stable
`Binaries/Win64` layout). Manifest `Executable`
must identify the running executable. Do not search ancestors, project history,
workspace descriptors or cwd. `--project` cannot select a staged project.

Source Game requires `--source-game --engine-root=<absolute-engine-directory>
--project=<absolute-dproject>`. Reject mixed source/manifest arguments, absent
source roots and source flags in a staged invocation. DevTool run supplies these
for Game only; migrate startup probes and direct-launch documentation in Engine,
Sandbox and RoadWeaver. Source Editor, Browser and tools retain their existing
host setup and may use current source root discovery. Launch selects mode before
config/logger/storage access. One immutable Core context is published once;
Projects fills existing project/mount admission through its existing lifecycle.

| Accessor/binding | Source mode | Staged mode |
| --- | --- | --- |
| `LaunchDir` | Actual executable directory | Actual executable directory |
| `RootDir` | Source workspace root | Runtime.json parent |
| `EngineDir` | Selected engine source root | `Paths.Engine` |
| `ProjectDir` | Authored descriptor parent | `Paths.Project` |
| `ProjectFile` | Authored descriptor | Empty; detached `Project` view |
| `EngineContentDir` | Engine/Content | `Paths.EngineContent` |
| Engine binaries and third-party accessors | Existing build bindings | `Paths.Binaries`, `Paths.ThirdParty` |
| Project content/mount bindings | Authored mounts | `Paths.ProjectContent`, `Mounts` |
| Cook root and Shader library | Existing source/tool policy | `Paths.Cook`, `Cook.ShaderLibrary.Path` |
| Frozen configuration/template | Existing source inputs | `ConfigurationFiles` under `Paths.Config` |
| Launch Saved/config/logs and DDC | Existing source policy | Per-user Saved/Configs, Saved/Logs, Cache |

Audit callers appending source-only suffixes to these accessors. Staged EngineDir
and ProjectDir may both be `.`; their content, binary and config accessors remain
explicit. They do not imply a physical source Engine or Project directory.
No sentinel directories or authored `.dproject` are required. `/Engine/`, `/Game/`
and `/Plugins/<Id>/` identities remain unchanged. Mount paths must match declared
content paths, names must be unique, plugin owners must be selected, and references
must resolve to active mounts. Only Runtime plugin modules appear in `Binaries`;
provider/EngineReady phase order remains the existing plugin contract. Editor
modules can exist in the authored fixture but never in Game admission.

`Files` binds executable, native libraries/data, configuration, Cook manifest,
runtime snapshot, Shader library and licenses by SHA-256. Do not list every asset:
CookManifest owns asset entries and checksums. Every referenced runtime file must
occur exactly once in `Files`; directories are not file records. Runtime.json
does not hash itself. `NativeReceiptSha256` binds the canonical completed receipt
in external export evidence. `Cook.SelectionFingerprint` must match the snapshot,
receipt's frozen deployment selection and admitted plugins; the available build
inventory may be a superset. Reject mismatches before publication or admission.
This new selection digest hashes canonical frozen selection bytes with SHA-256;
it does not replace the existing plugin resolver's internal fingerprint algorithm.

## Target receipts and Cook snapshot

`target-receipt-v1.schema.json` records only a successfully completed target.
Configure-time inventory is not a receipt. `Products.Source` resolves against
explicit Engine, Project, Artifacts or Dependencies roots; `Destination` is relative to
deployment root. `Owner` is Engine, project or selected plugin identity. Every
product has exact bytes, category and a Runtime host. Modules declare linked
and dynamic edges separately; both contribute to required build/receipt closure,
only linked edges introduce linker dependencies. `RuntimeFiles` names product
destinations. Reject unresolved/inactive/Editor edges and stale/missing inputs.
`InputFingerprint` covers the backend's native availability inputs; selection-only
overrides change `SelectionFingerprint`, not native availability or build-all
plugin policy. A receipt superset is permitted; deployment selects only the
required target/module/file closure. Stage does not authorize DLLs by directory
globbing or import discovery. Ordinary and delay PE imports check the declared
closure; string loads and data require declarations.

The source backend consumes TargetReceipt, ArtifactsRoot and
DependenciesRoot, directly or per configured Game product set. It has no GameRuntime
field or source-output naming fallback. Available build receipts may be supersets;
the selected external TargetReceipt.json retains only reachable module/product
records and binds Cook's SelectionFingerprint. Runtime.json hashes those exact
selected receipt bytes; AvailabilityReceipt.json preserves the original.
The selected projection retains CookCompatibility as well as the original input
fingerprint and provenance; both receipt forms satisfy target-receipt-v1.

`BuildInputs` retains the catalog's logical Path/Hash descriptor fingerprints,
independent of startup-only overrides. Selected plugin/ordinary descriptor inputs
must match current selection, preserving existing plugin admission checks without
reading PluginBuild.json. Plugin module records additionally bind Binary (a product
destination owned by that module) and Phase. The main product must be NativeLibrary;
Runtime dependencies follow the receipt graph and never resolve through inventory
copies or filename guesses. Old receipts lacking this capture require rebuilding.

The service parses availability bytes once, decodes module/product values, selects
closure without I/O, then verifies selected files and PE imports before publication.
It serializes the selected document directly and preserves the originally read
availability bytes in evidence. Source Editor/plugin inventory admission is unchanged.

`Inputs` is a sorted native-availability file inventory. Engine, Project, Artifacts,
Dependencies and Workspace paths are relative to explicit roots. Host-only SDK
inputs use absolute Host paths; Host/Workspace never authorize deployed products.
InputFingerprint is SHA-256 of UTF-8 `Root/Path:Sha256\n` records sorted by root/path.
Generated Build.inputs binds graph membership; CMakeCache.txt binds configured
compiler/tool settings, alongside recorded source/header/generated/build files. Startup-only overrides are excluded.
Products optionally carry License and Provenance together for redistribution.
NativeLibraries in Runtime.json projects only NativeLibrary products and is cached
by Core after Files validation; RuntimeData/License products do not authorize loads.

CookCompatibility is the configuration-independent runtime/provider and DHT/schema
source contract generated by the build and embedded in Projects. A Cook host must
match it, engine version, native compiler ABI, platform/architecture and project.
This admits Debug-host/Release-target work without loading target DLLs. Host identity
is recorded only in full evidence. The target GameBuild and exact target-receipt hash
are committed in the runtime snapshot; Stage verifies both before native selection.
Older snapshots/receipts missing these bindings require rebuild/recook.

`runtime-cook-v2.schema.json` replaces the full authored Cook snapshot in delivery.
Its implemented flat v2 fields are EngineVersion, platform/architecture,
configuration/ABI, ApplicationId, Project name/base modules, Selection,
SelectionFingerprint, Mounts and ConfigurationHashes. Selection is a serialized
minimal v1 object containing plugin IDs and ordered Runtime modules (name, owner,
host and phase); its SHA-256 binds the exact serialized bytes. ConfigurationHashes
binds frozen Project, Game and engine defaults. Completed native identity is bound by the receipt. GameBuild, TargetReceiptSha256 and CookCompatibility bind the exact admitted
target independently from the host. It contains no authored descriptors, machine paths or tool
host identity. Full Cook input capture stays in external evidence; retain current
Cook transaction and provider lifetime. Target configuration is independent of
the host configuration. Compatible provider/schema checks precede Cook; the host
never loads target Game DLLs. Full ExportEvidence.json and reduced RuntimeCook.json commit together as existing
PluginSnapshot auxiliary outputs, with their Cook manifest. Delivery projects out
only the full evidence record and keeps the original runtime snapshot bytes.

SHA-256 strings are lowercase hex. Canonical JSON identity bytes are UTF-8 without
BOM, object keys sorted ordinally, no insignificant whitespace or terminal newline,
and non-ASCII emitted directly. Inputs use integer/bool/string values only; reject
duplicate keys. Set-like arrays sort by declared identity/path, while ordered
module/phase arrays preserve admitted order. A digest field is excluded when
hashing its containing identity; no circular manifest/receipt hashes are allowed.

## Writable identity and Win64 prerequisites

`ApplicationId` comes from authored `Export.ApplicationId` in Configs/Project.yaml,
defaulting to `org.durin.<ProjectName>`. Authors should persist an explicit value
before renaming a project; export does not silently mutate source settings. Retain
it when renaming project/output/configuration.
Do not derive it from package location, engine build or Debug/Release. Win64
`LocalAppData` resolves through the platform known-folder API, then
`Durin/<ApplicationId>/Saved/{Configs,Logs}` and `Durin/<ApplicationId>/Cache`.
Staged verification supplies `--user-data-root=<absolute-directory>` as an isolated
LocalAppData base outside delivery. `--migrate-user-config=<absolute-file>` performs
explicit legacy user-only migration when the destination is absent. The runtime
rejects the old ExecutableSaved policy; source storage is unchanged.
Portable storage is not exposed in v1. Failure to resolve/create user storage
fails explicitly without installation/cwd/source fallback. Existing source Saved
data is untouched. Migrate known legacy user overrides only with an explicit
source location and absent destination; never overwrite user edits or seed saved
config from merged inherited project/plugin defaults. New user config contains
only user overrides; frozen Config remains immutable.

[`win64-system-v1`](win64-system-v1.json) targets Windows desktop x64 version
10.0.19041 or later. It freezes the baseline OS DLL and UCRT API-set allowlist;
matching is by exact case-folded basename, not System32 existence or wildcard
API-set prefixes. MSVC runtime, graphics drivers/Vulkan loader and optional OS components are not blanket OS
guarantees. Each must be a declared deployment file or prerequisite with provider,
version and licensing provenance. Stage applies this allowlist against
ordinary and delay-load native import evidence; unknown imports fail, rather than expanding it by probing
the build machine. This is the selected classification rule, not a claim that the
old Python classifier already implements it. The UCRT is an
[OS component on Windows 10 and later](https://learn.microsoft.com/en-us/cpp/windows/universal-crt-deployment);
[API-set availability](https://learn.microsoft.com/en-us/windows/win32/apiindex/detect-api-set-availability)
is not guaranteed merely by an api-/ext- prefix.

Release uses installed x64 VC++ Redistributable with `MinimumVersion` captured
from the actual toolchain requirement, at least the version of the MSVC build
tools used. Do not bundle an installer or copy CRT from System32. The prerequisite
diagnostic/link is external launcher/export evidence because a missing static
CRT import can prevent Game startup before its own diagnostics. Debug uses
`developer-debug-crt`, requires explicitly provisioned licensed developer/test
runtime, and never copies debug CRT into delivery. Debug clean-machine tests must
use that declared prerequisite; they cannot claim ordinary Redistributable support.
Each redistributed third-party file carries receipt ownership, provenance and
license evidence. Microsoft documents
[redistribution requirements](https://learn.microsoft.com/en-us/cpp/windows/redistributing-visual-cpp-files),
[runtime version compatibility](https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist/)
and [Debug test-machine prerequisites](https://learn.microsoft.com/en-us/cpp/windows/preparing-a-test-machine-to-run-a-debug-executable).

## Worker protocol v1

Direct CLI flags normalize to `request-v1.schema.json`; Editor and DevTool send
the identical request. All client paths resolve to absolute paths before dispatch.
Worker validates them; no client closure/target selection is authoritative.
`EngineRoot` and `BackendMetadata` are explicit trusted capability inputs.
`RequestId` is correlation only, excluded from normalized plan identity. Freeze
project/target, effective selection, inputs, layout, prerequisites, capabilities,
reuse/verify/archive policy into the plan. Output paths belong in reporting but
not content identity, allowing relocation. `preview` never builds, cooks, creates
work roots/user data, locks outputs or publishes. `stage` requires committed Cook
and receipt inputs with reuse-only policy. `export` permits service-selected
incremental build/Cook; reuse requires full freshness checks. Prebuilt backend
cannot construct missing native inputs. No worker calls DevTool recursively.

Production clients start one owned worker per request with `--protocol=1` and
send one newline-terminated request on stdin, keeping the pipe open for cancel.
Direct CLI adapts flags and console cancellation internally. stdout contains only
UTF-8 JSON-line `event-v1.schema.json` messages; logs go to stderr. Maximum request,
cancel and event frame size is 1 MiB each, no BOM, no embedded newline framing.
Sequence starts at zero and increases strictly per request. Progress counters have
`0 <= Completed <= Total`; Total zero denotes unknown. Events share RequestId.
Exactly one terminal result is emitted after owned child processes retire.
No event follows it. Unknown versions/fields, duplicate keys and oversized/malformed
frames fail `protocol`; malformed requests without a usable ID use `InvalidRequest`.
After a valid result, exit with its ExitCode; absence of result is a client-side
worker failure. stderr logging failure must not corrupt stdout framing.

`cancel-v1.schema.json` requests cancellation. Do not acknowledge receipt as
completed cancellation. Acknowledge only after stopping owned build/Cook/verify
children, then emit the cancelled result with
`ChildrenRetired=true`. Repeated cancel for the same job is idempotent. Wrong IDs
fail protocol. EOF/disconnect or Editor shutdown requests the same cleanup; pipe
failure may prevent acknowledgement delivery but never releases live children.
Use existing process ownership facilities and a bounded 30-second graceful child
stop, then terminate/reap the owned tree before acknowledgement. Cancellation
before export completion leaves an unmarked incomplete invocation and earlier
successful directories untouched. No cross-phase rollback or partial-directory
deletion is required before acknowledgement. Completion-marker publication is a
short non-cancellable final step: a late cancel after marking returns success.
No detached jobs. Low-level Stage also requires a fresh directory and final
completion marker; no candidate/replacement transaction or rollback is retained.

| Category | Exit | Meaning |
| --- | --- | --- |
| none | 0 | Completed preview/stage/export |
| protocol, request | 2 | Wire/argument/bootstrap validation |
| capability | 3 | Missing component/backend capability |
| prerequisite | 4 | Unavailable declared runtime/tool prerequisites |
| identity | 5 | Stale/incompatible receipt, selection, ABI or Cook inputs |
| build, cook, stage | 6, 7, 8 | Owned phase failure |
| verify, archive, publication | 9, 10, 11 | Verification, archive or publication/marker failure |
| internal | 70 | Unexpected worker failure |
| cancelled | 130 | Cancellation completed and children retired |

Results carry a stable Code, actionable Message and optional plan fingerprint.
Failures/cancellations have null Output; incomplete invocation files may remain.
Preview success also has null Output. An export result names the fresh invocation
directory containing Delivery, optional Game.zip and ExportComplete.json. The marker
uses export-complete-v1.schema.json and binds RuntimeManifestSha256 plus optional
ArchiveSha256. Only checked, marked invocations are completed exports. Build and
Cook keep their existing output/cache semantics; the worker adds no cross-phase
transaction or rollback. Existing destinations are rejected; retry uses a new path.
Low-level stage remains reuse-only and writes its payload and completion marker
directly in the requested fresh directory.

All operations accept Preview=true; Operation=preview is the reuse-only alias.
Preview checks configuration/capabilities/tools/paths and reports planned phases,
without checking Cook/native freshness or closure, running children or claiming
product hashes. Preview results have null PlanFingerprint and Output. Component
metadata now supplies scalar BackendMetadata, EngineRoot and DefaultBackend; its
hashed Files include worker/tools, policy and schemas. The source-stage backend
supports project/configuration product sets and source build/Cook tools. A
prebuilt-stage backend requires its hashed component and compatible committed Cook,
with no build or new offline-Cook capability. See
[implemented export ownership](../../../../../Documentation/Runtime/Application/GameExport.md).

The optional request Preset selects a named generated Game product set (direct
CLI --game-preset and DevTool stage --game-preset). Games records optionally carry
Platform; legacy single-platform metadata remains readable. Native target
resolution filters platform/project/configuration/preset once and passes resolved
paths to Stage. Source metadata is generated from declared resolved CMake presets;
Cook/Stage clients consume those paths rather than recreating them.

## Portable fixtures and validation

[Fixtures](../../../../Tests/Fixtures/GameExport) cover no-plugin, content-only,
dependent Runtime+Editor authored selection with Runtime-only delivery, and
dynamic native/data declarations. The fixture manifest hashes are symbolic test
identities, not checksums of real built products. `cases.json` encodes negative
schema cases and the Stage 1 bootstrap execution matrix; schema tests do not claim
that native bootstrap failures or reparse-point validation already pass.
`baseline.json` preserves historical Sandbox Debug/Release receipt identities,
bounded outcomes and raw report hashes; original reports remain under ignored
Build outputs. Those receipts cannot qualify the new schema/layout or a clean OS.

Run the portable contract suite with the checkout's prepared Python:

```powershell
.\.venv\Scripts\python.exe -m pytest Engine/Source/Developer/GameExport/Tests -q
```

The suite requires the existing pytest/jsonschema dependencies; it does not build
native targets or install dependencies. Subsequent stage readers/writers must
consume these same goldens, including structural and semantic rejection cases.
