# Canonical Resave

Summary: Canonicalize reflected identities without reimporting source data.

Last reviewed: 2026-09-14

Use canonical resave when the Asset Compatibility window or a package context
menu says **Resave recommended**. This is maintenance of serialized type names;
it is separate from unsaved authored changes and reimport.

For one asset, open its Content Browser context menu and choose **Resave
Package**. **Save Package** is reserved for loaded assets with ordinary authored
changes. For a multi-selection, choose **Resave Selected Packages**.

Project inspection and batch application are provided by the target-selected
Developer `AssetMaintenance` service shared by the Editor and
`DurinAssetTool`. It is not part of the game Runtime; Runtime retains only the
single-package schema validation and atomic package mechanisms used by the
service.

For project maintenance, open **Tools > Asset Maintenance > Canonical Resave**,
run the read-only audit, review stored/current identities and blockers, then
apply the recommended set. The apply is a sequence of bounded atomic package
units, so cancellation or failure can leave earlier packages complete; the
terminal report is the authority for the outcome.

DurinDevTool uses the configured game project by default and previews without
writing:

```powershell
.\DevTool.bat asset resave /Game
.\DevTool.bat asset resave /Game/Example --apply
.\DevTool.bat asset resave --all
```

Each positional scope selects an exact package when one exists and every package
below that path. Use `--all` instead of scopes for the complete project, and
`--project <descriptor>` only to override the configured default. Add `--json`
for automation. The lower-level host accepts the corresponding
`DurinAssetTool resave --project=<project.dproject> <scope>...` grammar.

Project selection is explicit: the default command does not enumerate sibling
projects. Repeat with `--project` for each project descriptor; shared Engine
content needs only one pass. Project modules are loaded before schema capture
so project-defined asset classes participate in inspection and resave.

The `asset material-functions --apply` command initializes missing reusable
functions and DefaultMaterial. It preserves compatible function implementation
edits and rejects incompatible provenance or interfaces. It does not upgrade
historical material graphs or recreate the retired ImportedSurface template.
Each package save is atomic. Instances and meshes use canonical resave only
when their existing logical schemas are already supported.

Folder scopes and `--all` select recommended identity repairs. To force a plain
load-and-save of an already canonical package, pass its exact package path.
This is required when persisting an in-memory domain upgrade before removing
the corresponding old reader. The workspace corpus is now v10; the current tool
does not read v9. Format upgrades must be applied with a reader that still
supports the source revision before retiring that reader.

Canonical resave always writes the current canonical DURF/DAST v10 closure;
there is no format-selection or legacy-writer option. `--apply` is the only
option that authorizes writes, and package-level rollback is automatic on
verification or catalog-publication failure.
Before apply, check out the reported authored files in source control. After
apply, review the package diffs and rerun the same dry-run; a successful second
scan is empty and a second apply is a no-op.

Blocked packages are never written. Typical blockers are a dirty loaded
package, read-only mount, unsupported package format, stale
fingerprint, incompatible or unknown payload, unavailable reflected type, or
corrupt bytes; non-asset entries such as redirectors are skipped. For uncooked asset families, apply also waits for the PostLoad
recovery started by the ordinary loader; missing source/DDC data or a provider
that does not publish family-ready transient state blocks the save rather than
serializing a partially recovered object. Resolve the named condition and
create a fresh plan; do not invoke an authored reimport merely to canonicalize
reflected identities.

## Texture source recompression

Use the explicit storage operation to compress current authored texture sources
without their original image files. Update executables first: Zstd source codec 2
cannot be read by older tools. Preview and apply use the same package selection:

```powershell
.\DevTool.bat asset resave --all --recompress-texture-sources --json
.\DevTool.bat asset resave --all --recompress-texture-sources --apply --json
```

With this option, scopes and `--all` include compatible current packages even
without reflection resave recommendations. The service processes packages in
sequence and recompresses detached texture source candidates. Preview may load
assets and rebuild disposable derived data but never changes authored storage.
Non-texture and already-identical packages are skipped. Ordinary resave does not
recompress texture sources.

Each report package includes `recompressTextureSources` and `textureSources`.
Source records include exact decoded hash, semantic identity, source descriptors
and build settings, decoded and before/after stored byte counts, registration
GUID, and whether storage would change. Preview reports `Ready` for changes;
apply reports `Resaved`. Compare source identity and decoded hash across reloads;
registration GUIDs are reconstructed on load and are not persistent identity.

Publication reuses canonical resave transactions. Cancellation stops before
publishing the next package; preparation/codec failure cannot install a partial
source. A failed package save restores source storage, and verification failure
restores the package, companion closure and registry. Successful conversion can
move a payload inline and remove its exact old companion through normal package
publication; changed paths include that removed companion. Never clean up
companions with a wildcard. Repeating the pinned policy skips unchanged sources
without rewriting package or companion bytes.

For repository-wide work, enumerate workspace project descriptors and their
enabled mounts and deduplicate physical packages. Engine content is mounted by
normal game projects; `Engine.dproject` itself is not a standalone asset-tool
project because its root list includes the launcher program.

## Explicit PBR material template

Create an optional Metallic/Roughness instance parent at a chosen unused package
path. The command previews by default; `--apply` creates and saves the material:

```powershell
.\DevTool.bat asset material-template /Game/Materials/PBRSurfaceMaterial_MR --project Sandbox/Sandbox.dproject
.\DevTool.bat asset material-template /Game/Materials/PBRSurfaceMaterial_MR --project Sandbox/Sandbox.dproject --apply
```

The template exposes independent value, texture, and UV parameters for every
surface property. Metallic reads B and roughness reads G; map bindings and UVs
remain independent. Normals use the existing decoded-RG sample output. It needs
no material function assets. Instance overrides bind the stable built-in parameter
IDs. Existing destinations are rejected without overwriting their contents.
Neither scene import nor `material-functions` creates this optional template.
The command does not enable a new import-as-instance mode; it provides the
parent authoring entry point for that future workflow.

## Material recipe initialization and reconstruction

Inspect material/function provenance, current schemas, parameter owners and
instance overrides before changing shared material content:

```powershell
.\DevTool.bat asset material-functions --project Sandbox/Sandbox.dproject
.\DevTool.bat asset material-functions --project Sandbox/Sandbox.dproject --apply
```

Apply repeats the inventory before writing. It initializes missing standard
functions and DefaultMaterial from current graph-owned recipes.
Existing functions retain compatible implementation edits; incompatible provenance
or interfaces fail.
Modified or unsupported graphs require explicit reconstruction and are not converted.
Repeated application to current assets is a no-op.

For reconstruction, first inventory exact files and inbound references, preserve a
byte backup or source-control checkpoint, then remove only the approved affected
assets from mounted content and initialize replacements. Rebuild retained instance,
mesh and scene references deliberately and audit every workspace project afterward.
Current material owners use program schema 7 and function schema 3 with a required
ownership marker. Old material data cannot be made current by canonical resave;
there are no historical material graph readers or automatic parameter-table adapters.
Cooked outputs must be regenerated after reconstruction.

Scene imports generate structural parents under the destination mount's
`Materials/ImportedParents` directory; they do not select or initialize the
retired Engine ImportedSurface template. Reimport the source to regenerate its outputs
and select current parent shapes. Reimport replaces edits to generated assets;
keep independent customized copies outside the destination. Existing immutable
parents and outputs removed from the source are retained for references. The
material-functions command maintains the shipped reusable functions and
DefaultMaterial; it does not rebuild scene-specific parents. Already-current
content needs no destructive reconstruction solely for a new structural recipe. See [Material System](../../Runtime/Rendering/MaterialSystem.md)
for ownership and normal sampling contracts.
