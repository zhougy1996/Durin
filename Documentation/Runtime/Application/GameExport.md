# Game Export Service And Clients

Summary: Define engine-owned Game export orchestration, isolated completion, source/prebuilt components, and shared worker clients.
Modules: GameExport, Core, MainFrame, DurinEd
Last reviewed: 2026-11-15

## Invocation and completion

For client commands and delivery usage, see [Exporting a Game](../../Editor/Guides/ExportingGames.md).

DurinExportTool hosts GameExport outside the Editor. The export operation runs
Build, Cook, Stage and optional bounded startup verification/archive in order.
It invokes CMake and DurinAssetTool directly, never DevTool. Existing receipt,
CookManifest, native closure, configuration and runtime admission checks retain
their owners; [Game deployment](GameDeployment.md) owns the low-level Stage layout.

Each export and low-level Stage exclusively creates a new requested directory.
Existing directories are rejected, including incomplete invocations. Delivery contains the
ordinary Runtime.json/Binaries/Config/Content tree; Game.zip, if requested, contains
that Delivery tree. Logs, Cook caches and isolated verification user data remain
outside Delivery. Source backend metadata selects the per-project/configuration
Cook cache unless the caller supplies a committed Cook directory.

ExportComplete.json is written atomically and last, after requested phases and
owned children finish. Version 1 binds RuntimeManifestSha256 and optional
ArchiveSha256. ZIP timestamps can vary; cross-client content identity compares the
delivery manifest and archive payload, while each marker binds its own archive bytes.
It is completion metadata, not runtime load authority or a substitute
for CMNF/receipt/file validation. Only a matching completion marker denotes success.
A crash or failure can leave partial files; there is no cross-phase transaction,
backup restoration, automatic cleanup or resume. Retry uses another directory and
existing verified build/Cook caches. Earlier exports remain untouched. Low-level
Stage writes the payload directly in the requested directory, with its own
ExportComplete.json, using the same fresh directory and final marker rule.

Inputs must be saved and stable until exit. Source edits during export are
unsupported. No editing locks or source snapshots are introduced. Output/input
aliases and overlapping destinations remain errors. A late cancellation after
completion marking does not undo success.

## Component and backends

CMake generates ExportBackend.json from resolved visible Game presets and actual
binaryDir values, with platform/project/configuration product sets. DevTool Cook
and Stage read this file from the selected tool-host build; Export uses the same
product sets in the published component backend. Clients do not construct receipt
paths or generate temporary product catalogs.

The CMake ExportComponent target builds the worker, Cook host and declared provider
DLLs and publishes a hashed ExportComponent.json beside the worker. Its file set
follows concrete link/dynamic dependencies, the asset tool's explicit provider
build dependencies, non-Engine project Game roots and Runtime plugin providers.
Editor-only companion roots and unrelated/test DLL targets are not inventory roots;
Editor libraries required by actual tool links remain dependencies. The generated
ExportBackend.json supplies source CMake/compiler environment, declared project/
configuration product sets, Cook host and prerequisite policy. Tool paths are
explicit; project/target selection stays in GameExport, not client profiles.
Source Build=auto invokes the existing incremental GameReceipt target. Build=reuse
requires compatible completed receipts; Cook=auto runs the existing incremental
Cook host. Missing backend tools or incompatible receipts fail explicitly during execution.
The external worker boundary parses requests and emits protocol events once.
Orchestration resolves backend/project/preset paths once into a plain FExportTarget
and calls Stage with that value and ordinary progress callbacks. Stage owns Cook/
receipt/closure validation and file copying/admission; it does not reload backend
metadata or select a target again. The descriptor comparison uses the already
captured canonical project document. Native receipt parsing remains single-pass.
Before package-path decoding, the worker initializes Core's existing path context
from explicit EngineRoot. Installed components therefore also work with the source
Engine Content/Shaders directories unavailable.

Engine/Scripts/Build/export_component.py can package an installed-style component
from that recipe, a completed Game receipt, project catalog metadata and committed
Cook input. It copies only declared native products and tool files, plus logical
project/module/plugin descriptors; native C++/headers, DevTool, .agents and a Python
environment are not runtime dependencies. The installed component declares
Build=reuse and Cook=reuse, not source build or new offline Cook capability.
Packaging does not establish a complete installed Editor/SDK distribution.

Component metadata and its hashed inventory are validated once per canonical path
per client/worker process; successful immutable metadata is retained. Failed
validation is not cached. Tool installations must stay stable while those
processes are running; restart them after updating the tools.

Prebuilt admission checks the component's file inventory, exact Cook/target receipt
binding, input fingerprint, logical selection and product hashes. Engine/SDK source
and host compiler inputs are recorded provenance rather than required installed
files. Present project-native inputs still undergo freshness checks. Source-backed
admission retains its full physical native-input validation. A prebuilt backend
cannot silently turn into a source backend or build missing/stale native products.

## Shared clients and process lifetime

Direct CLI export needs an absolute project and fresh output path; component
metadata supplies backend/tool locations and the default Release configuration.
DevTool export forwards the same protocol request through the existing worker
client. Low-level stage/preview commands remain available with reuse-only policy
and fresh-directory publication.

The Editor File > Export Game action offers configuration, output, optional verify/
archive, preview, progress and cancel. Save All and Export first settles/saves open
documents through existing document managers; cancelled prompts, pending operations
or save failure dispatch no worker. The UI owns a background process client, not a
second export pipeline. Component lookup defaults beside the Editor executable;
an explicit absolute DURIN_EXPORT_COMPONENT path can select an installed component.
Editor shutdown cancels and joins it before session teardown.

Core ExecuteControlledProcess reuses structured process launch options and owns a
Win64 Job Object for every controlled child tree. The Editor client sends correlated
cancel frames to its worker; export phases retire their children before cancellation
acknowledgement/result. EOF, disconnect and shutdown request the same cancellation.
Partial export files need not be removed before acknowledgement. See
[platform processes](../Core/PlatformProcesses.md) and the versioned
[wire contracts](../../../Engine/Source/Developer/GameExport/Contracts/README.md).

All preview entry points (Operation=preview or Preview=true) check request,
backend capability, tool availability and output/input paths, then report planned
phases. They do not check Cook/native freshness or closure, execute child tools,
or claim product hashes/a product fingerprint. Execution performs full admission.
Preview allocates no invocation, lock, marker or verification data. Result fingerprints/content identities
exclude job paths and correlation IDs. Raw logs and reproduction receipts belong
under ignored Build outputs or CI artifacts; qualification status belongs to the
[active plan](../../Plans/GameExportPipeline.md).
