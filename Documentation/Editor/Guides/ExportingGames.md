# Exporting A Game

Summary: Export a Win64 Game through the engine tool, DevTool, or Editor and identify a completed delivery.
Last reviewed: 2026-11-15

## Prepare the project

Save project and asset changes before exporting and keep the inputs unchanged
until the job finishes. Configure the default Level, GameMode/default data and
AssetManager rules in the project. Cook follows those declarations and serialized
asset references; an ordinary Sandbox export needs no additional pawn-mesh root.
See [Cook policy](../../Runtime/Assets/Build/Cooking.md#primary-asset-policy-and-cooked-discovery).

Use a new output directory for every job. An existing directory is rejected,
including one left by a failed export. Build/Cook caches can still be reused.

## Choose a client

The source engine's `ExportComponent` build target supplies DurinExportTool, its
Cook host, dependencies and backend metadata beside the Editor executable.
The direct engine tool needs absolute project/output paths and selects the
Release Game configuration by default:

```powershell
.\Engine\Binaries\Win64\Debug\Runtime\DurinEditor\DurinExportTool.exe export --project G:/Workspace/DurinLab/architect/Sandbox/Sandbox.dproject --output G:/Workspace/DurinLab/architect/Build/Exports/Sandbox/Run001 --verify --archive
```

This builds compatible Game products incrementally, cooks the project's declared
roots, stages the delivery, runs a bounded startup check and creates Game.zip.
Use `--configuration Debug` for a development Game. Verification checks bounded
startup; it does not replace gameplay or clean-machine qualification.

DevTool forwards the same worker request and resolves relative paths from the
checkout:

```powershell
.\DevTool.bat export --project Sandbox/Sandbox.dproject --output-dir Build/Exports/Sandbox/Run002 --configuration Release --verify --archive
```

Use `--preview` to check configuration/capabilities and show planned phases
without executing the job. A successful preview does not establish product
freshness or closure; those checks run during export. Preview creates no output
and reports no product fingerprint. `--build reuse --cook reuse` explicitly
requires existing compatible native receipts and committed Cook outputs.

In the Editor, open **File > Export Game**, choose the configuration and a new
absolute output path, and select optional startup verification/archive. **Preview**
checks configuration. **Export**, then **Save All and Export**, settles and saves
open asset documents before starting an external worker. Resolve any save failure
before retrying. **Cancel** stops an active job and its owned children; it can leave
partial files for diagnosis.

An installed component supplies its own worker/backend metadata. Its currently
supported capability is compatible prebuilt native and committed Cook reuse;
source compilation and new offline Cook are not provided. The Editor can select
it through an absolute `DURIN_EXPORT_COMPONENT` path. Restart client/worker
processes after updating a tool installation: component admission is cached for
the process lifetime. See [component and backend ownership](../../Runtime/Application/GameExport.md#component-and-backends).

## Use the delivery

A completed export contains `Delivery/`, optional `Game.zip`, and
`ExportComplete.json`. The final marker binds Delivery/Runtime.json and the
requested archive. Treat an absent or mismatched marker as an incomplete job;
choose a new output path to retry. Earlier exports remain untouched.

Run Delivery/RunGame.bat or its Binaries/Win64 executable. Move the entire Delivery
tree together; runtime admission independently checks its manifests and required
files. User configuration, logs and caches go to per-user storage, outside the
installation. Low-level `stage` writes the same runtime tree and final marker
directly in its requested fresh directory, without a Delivery wrapper.

Win64 Release prerequisites include the declared MSVC runtime and a compatible
Vulkan driver/runtime; an archive does not install them. Debug CRT requires an
explicit development environment. See [delivery layout and writable storage](../../Runtime/Application/GameDeployment.md#delivery-layout-and-writable-storage)
and [export completion](../../Runtime/Application/GameExport.md#invocation-and-completion).

For an SDK-independent Debug runtime check, set `DURIN_VULKAN_VALIDATION=off`.
Debug's default `auto` mode uses an installed Vulkan validation layer when
available; the layer is optional development tooling, separate from the Vulkan
driver/runtime prerequisite.
