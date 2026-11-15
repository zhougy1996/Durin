# Game Deployment Bootstrap And Staging

Summary: Explicit source Game and Runtime.json v2 startup paths and the native preview/stage worker.

Modules: Core, Launch, Projects, GameExport

Last reviewed: 2026-11-15

## Startup modes

Source Editor, Project Browser and headless asset tools retain their source host
setup. Source Game requires `--source-game`, an absolute `--engine-root` pointing
to Engine, and an absolute `--project` descriptor. DevTool run and newly generated
VS Code Game launch configurations provide these arguments. Existing user-owned
launch configurations need the same explicit arguments.

Source Game still consumes cooked assets and Shader data. It can select an
independent committed Cook generation with `--cook-root=<absolute-directory>`;
without that argument its existing executable-directory Cook binding remains.
Neither launch mode builds or cooks implicitly.

```powershell
.\DevTool.bat run --preset Win64-Debug-DurinGame --project Sandbox/Sandbox.dproject -- --cook-root=G:/Workspace/DurinLab/architect/Build/Exports/Stage3/SandboxCook --exit-after-ticks=30
```

Standalone Game defaults to staged mode. `RunGame.bat` supplies an absolute
`--runtime-manifest` argument. Direct executable launch without that argument
uses only `../../Runtime.json` relative to the executable directory, the stable
layout location. Staged launch rejects source project arguments;
Editor rejects Game path-mode arguments.

Core reads Runtime.json schema v2 before profiling-session setup, logging,
configuration or writable-storage consumers. It resolves relative bindings from
the manifest's canonical parent, checks target identity, executable binding,
required file hashes, duplicate paths/keys and canonical containment, then
publishes one immutable path context. Failure reports to stderr and returns 2;
it never searches cwd or an enclosing source checkout. Aliases escaping the
deployment root and conflicting Cook/Shader bindings are rejected.

These diagnostics require entering the engine. Windows can reject a missing
bootstrap dependency first: a missing Core DLL returns loader status `0xC0000135`
before an engine log is available. Missing declared modules/configuration/Cook
files detected after entry use the engine's exit-2 admission diagnostics.

Projects admits the detached `Project` view and existing frozen plugin selection
without a deployed `.dproject` or workspace lookup. It validates native identity,
configuration and plugin phase/mount bindings before freezing the existing module
loader. Core/Launch use declared engine, project, binary, third-party, content,
config and Cook roots. Source-only module lookup is disabled in staged mode.
CookManifest remains the asset checksum/catalog authority.

## Native worker and source client

`GameExport` is a tools-only service linked by `DurinExportTool`. They are built
with the Editor tool distribution and excluded from the Game build closure.
DevTool `stage` discovers that existing worker, supplies source backend metadata
and forwards one request. It does not run asset inspection, select native closure
or copy/publish delivery files. The worker never invokes DevTool recursively and
does not run a Game loop or a Cook host in memory.

The worker supports direct preview/stage and orchestrated export, plus JSON-line
protocol v1. stdout carries progress and one terminal result; stderr carries logs.
Clients keep stdin open until the result and use correlated cancel frames. Low-level
Stage has no children; export owns Build/Cook/verify/archive children. See
[Game export](GameExport.md) for source/prebuilt components, client lifetime and
fresh invocation completion and shared fresh-directory publication.

Source backend metadata uses
[`source-stage-v1.schema.json`](../../../Engine/Source/Developer/GameExport/Contracts/source-stage-v1.schema.json).
It supplies absolute target-receipt, artifact-root and dependency-root locations, either directly
or as a list of configured product sets. The service chooses exactly one matching
configuration and optional explicitly selected preset. Missing products require
an explicit build. Committed Cook files are decoded and checked through the
existing Engine Cook manifest API; current source selection is resolved through
Projects; target-receipt metadata validates the selected plugin build compatibility.

```powershell
Engine/Binaries/Win64/Debug/Runtime/DurinEditor/DurinExportTool.exe preview --engine-root G:/Workspace/DurinLab/architect/Engine --backend-metadata G:/Workspace/DurinLab/architect/Build/GameExport/Stage3/source-backend.json --project G:/Workspace/DurinLab/architect/Sandbox/Sandbox.dproject --platform Win64 --configuration Debug --cook-dir G:/Workspace/DurinLab/architect/Build/Exports/Stage3/SandboxCook --output G:/Workspace/DurinLab/architect/Build/Exports/Stage3/DirectGame
```

Preview checks configuration and reports planned phases without creating output or
claiming product identity; execution checks committed inputs and the full closure.
Stage exclusively creates a fresh output directory, copies and hashes the declared
files, validates Core bootstrap and Projects admission, then writes ExportComplete.json
last. Existing destinations are rejected. Failure/cancellation retains an unmarked
partial directory; retry uses another path. There are no Stage candidates, backups,
replacement, rollback or separate writer-lock directories. Evidence is retained in
the adjacent `<output>.evidence` directory, with no generation indirection.

## Delivery layout and writable storage

A staged Win64 export has this stable layout, independent of build configuration:

```text
RunGame.bat
ExportComplete.json
Runtime.json
Binaries/Win64/<ProjectName>.exe + native DLLs/runtime files
Config/Project.yaml
Config/DurinGame.yaml
Config/Defaults/DurinGame.yaml
Config/PrerequisitePolicy.json
Content/CookManifest.bin
Content/RuntimeCook.json
Content/Engine/...
Content/Game/...
Content/Plugins/<Id>/...
Content/Shaders/ShaderLibrary.dslb
```

Content directories exist only when retained Cook outputs require them. Engine and
project roots may both bind to `.`; consumers use explicit binary/config/content
bindings. No discovery-only Engine directory or authored `.dproject` is deployed.
The common Win64 executable manifest declares the
[UTF-8 active code page](https://learn.microsoft.com/en-us/windows/apps/design/globalizing/use-utf8-code-page)
so native path/argument handling supports spaces and non-ASCII relocation paths.

`UserData.Policy=LocalAppData` resolves Windows' known folder before the first
write. `Export.ApplicationId` in authored `Configs/Project.yaml` selects the stable
identity; absent a setting it defaults to `org.durin.<ProjectName>`. Set and retain
an explicit identity when renaming the project. Sandbox authors `org.durin.Sandbox`.
Neither output location nor Debug/Release changes this identity. IDs start with an
ASCII letter and contain at most 128 letters, digits, dots, underscores or hyphens.
The application owns these paths below LocalAppData:

```text
Durin/<ApplicationId>/Saved/Configs/DurinGame.yaml
Durin/<ApplicationId>/Saved/Logs/
Durin/<ApplicationId>/Cache/
```

Launch creates a new user override as `{}` rather than copying inherited defaults.
Before logging, configuration loads frozen engine defaults, frozen effective Game
configuration and user overrides in that order. Frozen Config files remain inside
the read-only delivery. Existing overrides survive updates and relocation.
`--user-data-root=<absolute-directory>` explicitly substitutes the LocalAppData
base for isolated verification. It must resolve outside the installation.
`--migrate-user-config=<absolute-file>` optionally copies known legacy user
configuration only when the destination is absent; it never overwrites edits.
These arguments are staged-Game only. Source Saved behavior is unchanged.
Unresolvable/unwritable user storage fails explicitly, without install/cwd fallback.
The old `ExecutableSaved` manifest policy is rejected.

Cook atomically commits both full `ExportEvidence.json` and reduced schema-v2
`RuntimeCook.json` as existing PluginSnapshot auxiliary outputs. The reduced
snapshot contains project/base-module identity, target/ABI, stable application ID,
frozen Runtime plugin/module selection, mount permissions and configuration hashes.
It excludes source descriptors, source paths and embedded YAML. Its selection digest
hashes the serialized minimal selection. Stage checks this projection against the
full committed evidence. Delivery retains the original reduced snapshot bytes and
projects CookManifest by removing only the evidence record; all asset records and
checksums remain unchanged. Old full-only Cook generations require recooking.

Full source capture, backend metadata, source/destination hashes, original
CookManifest and native input evidence are retained directly at
`<output>.evidence/`, outside the runnable tree. A failed invocation can leave
partial evidence; only the final completion marker denotes successful publication.

## Native receipt selection

GameExport reads the availability receipt once into an owning document and concrete
module/product values. ReadNativeReceipt validates target/ABI, prerequisites,
source input hashes and logical BuildInputs captured from the build catalog.
Plugin modules carry explicit Binary and Phase bindings in that same receipt.
Selected descriptor fingerprints, owners, Runtime hosts and phases must match;
older receipts without BuildInputs require rebuilding GameReceipt.

SelectNativeDeployment performs graph traversal and selected receipt projection
without filesystem access. ValidateNativeDeployment checks selected artifact hashes,
executable/Core identity and loader-name collisions; PE inspection then verifies
imports. Preflight fixes final destinations, required files and native library
categories. Publication executes this file plan and rechecks bytes before copying,
then validates the copied delivery before final completion marking. Exclusive directory
creation establishes writer ownership; cancellation retains unmarked partial files.
The selected document is serialized for evidence without being parsed again.
The original availability bytes are captured at read time and retained unchanged.

Export has no dependency on PluginBuild.json or its copied Runtime files. Existing
source Editor/plugin admission continues to use that inventory; it is not a second
export product authority. Plugin main paths and runtime dependency paths are derived
from receipt products and their reachable graph, without basename lookup.

Stage consumes the completed target receipt; it does not enumerate native DLL
folders or infer project products from source output naming. Starting from the
launcher, frozen project roots and selected plugin Runtime modules, it follows
linked/dynamic edges and retains their declared runtime files. Availability may
contain disabled plugins, but a selected edge into an inactive owner or Editor
module is rejected. Missing/changed native inputs or selected artifacts, wrong
project/target/ABI, duplicate destinations and native loader basenames fail before
publication. RuntimeData and License files are copied and hashed according to their
categories; they cannot satisfy PE imports as native libraries.

Ordinary/delay-load x64 PE imports validate the declared native closure against
win64-system-v1. The OS/API-set allowlist, configuration-specific CRT list and
Vulkan prerequisite are explicit. Unknown imports fail even if a matching file is
present beside the executable or in System32. PE inspection never expands closure.
The actual MSVC toolset requirement and redistribution provenance come from the
receipt. The policy file is copied to Config and hashed.

`NativeReceiptSha256` binds external `TargetReceipt.json`, the selected projection
of the completed availability receipt. External `AvailabilityReceipt.json` preserves
the original. SelectionFingerprint binds the same reduced Cook selection as
Runtime.json. Source/build inputs use explicit Engine, Project, Artifacts and
Dependencies roots, plus Workspace for shared build rules. Host-only SDK/import
inputs carry absolute paths in external evidence; Host and Workspace are forbidden
as product roots. No source/host input capture enters the runnable tree.

Runtime.json's NativeLibraries lists only the selected native-library products.
Core resolves them against verified Files once during bootstrap, caches canonical
paths and hashes, and ModuleManager checks that cache before explicit loading.
A data file or planted DLL cannot become a module just because it exists on disk.
Replacing binaries in older exports requires restaging with their matching manifest.

## Cook target binding

Project Cook takes a completed target Game receipt separately from its Editor/tool
host. The copied target identity supplies configuration and GameBuild; the exact
receipt hash and schema/provider compatibility digest are committed with Cook.
Stage requires that receipt and Game identity, so a different build/configuration
requires recooking. Host identity stays in external ExportEvidence.json. The runtime
snapshot admits only the matching Game build. See
[Cook input ownership](../Assets/Build/Cooking.md#coordinator-and-input-ownership).

## Remaining migration boundary

Stage and orchestrated export share [fresh invocation directories and completion markers](GameExport.md#invocation-and-completion),
with no replacement or rollback. Editor/client and installed-style component behavior
are described in that contract. Full SDK distribution, clean-machine qualification
and remaining acceptance gates stay with the [active plan](../../Plans/GameExportPipeline.md).
Debug tests do not redistribute
Debug CRT or establish clean-machine prerequisites. The initial receipt backend
supports Win64 MSVC Debug/Release. Stage 4 qualifies Debug-host Cook and local
Debug/Release staging and startup; clean-machine redistribution remains open.
