# Authored Imported Data and Reimport Plan

Summary: Make authored assets self-sufficient for derived-data rebuilds and reduce source filenames to non-authoritative editor-only reimport hints.

Last reviewed: 2026-08-27

Status: Archived
Completed: 2026-08-27

## Current Status

The current authored model is source-file authoritative for most standalone
families. Texture2D, TextureCube, StaticMesh, and TerrainHeightmap can consult
their persisted filename during uncooked PostLoad and re-run source capture or
decode after a DDC miss. SkeletalMesh and AnimationClip Scene outputs retain a
DDC locator but do not retain enough imported data to rebuild independently.
VolumeTexture is the existing partial counterexample: its normalized voxel
source is authored `FEditorBulkData` and can live in the package companion.

The selected direction is to make every independently loadable authored asset
self-sufficient for editor and Cook builds. Import persists the canonical,
platform-independent imported data in `.dasset` or its DABK companion. DDC
becomes a pure acceleration layer rebuilt only from that canonical data.
Physical source files are read solely by explicit Import, Reimport, and
Reimport From File operations.

Stages 0 through 4 are complete. The repository contains 25 tracked DAST v6
packages; the independently loadable payload-family main assets are six
Texture2D, one TextureCube, two VolumeTexture, zero TerrainHeightmap, six
StaticMesh, one SkeletalMesh, and one AnimationClip. Every payload family now
owns canonical imported data sufficient for source-independent editor and Cook
rebuilds. Editor workflows persist optional schema-2 source hints, expose only
explicit Import/Reimport actions, and no longer probe physical sources during
load, inspection, thumbnail generation, or derived recovery.

The Stage 4 corpus gate is closed: the project baseline reports all 25 packages
as current DAST v6; the compatibility audit reports 25 compatible and zero
incompatible, unsupported, failed, stale, or resave-recommended packages; and
canonical-resave CI reports 25 selected and 25 skipped with zero ready or
blocked. The final schema-2 package and companion hashes are recorded below.
Stages 0 through 5 are complete. Focused data-domain qualification,
Editor/Game builds and smokes, corpus audit, and deployment/module isolation
pass. The native aggregate builds and runs all 83 targets; five Vulkan visual/
fault-injection targets retain the renderer-baseline failures recorded in the
Stage 5 results and are accepted as non-blocking debt outside this plan's
authored-data authority closure.

## Terms and Decision Status

- **Physical source** is an encoded file selected by Import or an explicit
  reimport action. It is never authored or derived-data authority.
- **Canonical imported data** is the family-owned, platform-independent,
  decoder-free input from which Build reconstructs derived payloads. This is
  the only term used below for that authority; "source data" refers only to a
  physical source file.
- **Source hint** is optional editor-only metadata that helps an explicit
  Reimport locate its complete set of physical source roles. A source role is a
  named required input such as a panorama or one of six cube faces.
- **Derived payload** is disposable family output cached in DDC and transformed
  into the cooked DBLK representation where required.
- **Authored bulk** is canonical imported data stored through
  `FEditorBulkData`, inline in DAST or externally in the descriptor-selected
  DABK companion.

The authority boundary, absence of implicit source I/O, family ownership, and
detached-candidate safety are selected invariants. Exact family schemas,
TextureCube representation, source-hint grammar and move behavior, retained
provenance, legacy migration mechanics, and hard-failure recovery UI remain
Stage 0 decisions. Later sections must not be read as selecting those open
details unless they explicitly say **Selected**.

## Goal

- Make an authored asset, its descriptor-selected DABK companion, and its
  ordinary referenced authored dependencies such as Skeleton sufficient to
  rebuild every editor/Cook derived payload after the entire DDC is deleted.
- Remove all physical-source capture, timestamp checks, decoding, and importer
  invocation from ordinary package load, PostLoad, DDC recovery, and Cook.
- Reduce persisted source locations to optional, editor-only hints consumed
  only by an explicit **Reimport** command.
- Provide **Reimport From File...** as the unconditional source-selection path;
  after a successful live-state commit it replaces the canonical imported data,
  import parameters, derived result, and hint set as one candidate.
- Preserve the current direct, family-owned import architecture without adding
  a generic replay graph, import job, source manager, or provider registry.
- Use the existing authored bulk and atomic package-bundle publication
  facilities rather than creating a second permanent payload store.

## Scope

- Define one platform-independent canonical imported-data contract for each
  supported payload family: Texture2D, TextureCube, VolumeTexture,
  TerrainHeightmap, StaticMesh, SkeletalMesh, and AnimationClip.
- Persist small structural fields inline and large immutable byte arrays through
  `FEditorBulkData`, allowing package serialization to choose inline DAST or
  external DABK placement by the existing threshold and descriptor rules.
- Change family build recipes and keys to consume the canonical imported data
  and its content identity rather than reopening or rehashing a physical file.
- Replace the current filename contract with an optional hybrid source hint:
  project-local files are relative to the physical parent directory of the
  owning `.dasset`, while files outside the project use normalized absolute
  paths.
- Split editor actions into **Reimport** and **Reimport From File...**, with
  explicit enablement, diagnostics, selection, live-state commit, package save,
  and rollback behavior.
- Cut Texture2D, TextureCube, TerrainHeightmap, StaticMesh, skeletal/animation,
  and their Cook paths over to source-independent DDC reconstruction.
- Retain VolumeTexture's existing authored-source direction while adapting it
  to the common source-hint and rebuild semantics.
- Ensure creation-only Scene import publishes sufficient canonical imported
  data into each independent generated payload asset; Scene itself remains
  creation-only and gains no aggregate replay or reimport record.
- Migrate or regenerate the repository-owned authored corpus before removing
  legacy source-dependent load behavior.
- Update import, lifecycle, package, Cook, source workflow, module ownership,
  and user-facing documentation after the behavior is implemented.

## Non-Goals

- Copying original PNG, HDR, FBX, glTF, RAW, or other encoded source files into
  Content or preserving their exact encoded bytes merely to support reimport.
- Treating DABK as a permanent copy of platform DDC payloads. It stores the
  platform-independent canonical imported data, not Win64 render/collision
  output.
- Automatically watching, locating, relocating, deleting, source-controlling,
  or synchronizing physical source files.
- Persisting project-root-relative source paths, search roots,
  recent-directory databases, or per-user machine mappings in assets. External
  normalized absolute paths are allowed only as non-authoritative reimport
  hints.
- Reading a source hint during package load, Cook, background DDC recovery,
  inspection, thumbnail generation, or asset validation.
- Whole-scene reimport, output reconciliation, generated-output ownership, or a
  persisted Scene dependency closure.
- Runtime import or deployment of AssetForgeBuiltins, Assimp, image decoders,
  source hints, canonical imported data, DABK, or DDC in cooked Game content.
- Supporting indefinite read-old/write-new compatibility for repository-owned
  legacy packages after the corpus migration gate is closed.

## Selected Direction and Invariants

### Authority model

The authoritative editor pipeline is:

```text
physical source file
  -- explicit Import/Reimport --> canonical imported data
  -- package save ------------> DAST plus optional DABK
  -- family Build ------------> disposable DDC value
  -- Cook --------------------> cooked DAST plus DBLK
```

Only canonical imported data and ordinary editable asset settings
are inputs to a DDC rebuild. Source provenance may describe how the current
value was produced, but neither a source path, source timestamp, nor continued
source availability is part of asset validity. Deleting the source after a
successful package save must not affect load, edit, build, Cook, duplication,
relocation, or DDC recovery.

DDC keys hash the canonical imported-data identity, normalized build settings,
builder/value schema versions, and target facts. They do not reopen a source
file or derive identity from its current contents. A successful local build
remains usable when a best-effort DDC put fails; canonical imported data is the
recovery authority and the cache remains disposable.

### Canonical imported data

Each family owns a bounded, validated, versioned value with no physical source
path, object pointer, DDC path, target platform, RHI handle, or importer
callable:

| Family | Canonical imported data |
| --- | --- |
| Texture2D | Decoded canonical pixels, dimensions, channel/alpha semantics, and source-format interpretation needed by editable build settings |
| TextureCube | Six canonical decoded faces, or a canonical decoded panorama when projection settings remain editable; Stage 0 freezes the selected representation and size bound |
| VolumeTexture | Existing normalized voxel source plus dimensions, format, and source schema, retained through `FEditorBulkData` |
| TerrainHeightmap | Canonical height samples, dimensions, numeric interpretation, and source schema |
| StaticMesh | Canonical vertices, attributes, indices, sections, material-source mapping, import transform facts, and validated bounds needed by render/collision builds |
| SkeletalMesh | Canonical LOD0 geometry, influences, inverse-bind/bind facts, sections, Skeleton compatibility, and stable material mapping |
| AnimationClip | Canonical track identities, times, transforms/typed keys, duration, interpolation facts, and Skeleton compatibility |

The value closest to the build recipe boundary is required. Encoded physical
source bytes are not canonical imported data for this plan: accepting them as
Build input would move source decoding back into PostLoad, DDC recovery, or
Cook. A family that cannot define a bounded, decoder-free canonical value is
blocked in Stage 0 rather than granted a source-backed exception.

Small metadata stays reflected and reviewable in `.dasset`. Large arrays use
`FEditorBulkData`; AssetCore remains the sole owner of inline/external
placement, DABK descriptors, validation, companion-first publication,
rollback, relocation, duplication, and deletion closure.

### Optional hybrid source hint

**Selected:** hints are optional, non-authoritative, editor-only metadata with
an explicit `AssetRelative`, `ProjectRelative`, or `Absolute` base. The base is
stored beside the hint and is never guessed from its spelling. Stage 0 still
owns the exact grammar, containment algorithm, multi-role encoding, and
move/duplication policy.

An asset may persist one or more editor-only source hints for explicit
reimport. It is not an asset path and is never resolved through mounts. The
selected physical file is classified against the normalized active project
directory before Import or Reimport From File commits its live candidate:

- `AssetRelative` stores a normalized UTF-8 path relative to the physical
  parent directory of the owning `.dasset`. Normalized `..` components are
  allowed so an asset under Content can refer to another source directory.
- `ProjectRelative` stores a normalized UTF-8 path relative to the active
  physical project directory and may not resolve outside it.
- `Absolute` stores a normalized absolute physical path, including a
  drive-qualified path when required on Windows. This deliberately trades
  portability for a useful same-workstation Reimport default.
- Automatic classification chooses `AssetRelative` for a project-local source
  and project-local owner, `ProjectRelative` for a project-local source whose
  owner is outside the project, and `Absolute` otherwise. A family may request
  a compatible base explicitly.
- Empty-component, URI-like, non-normalized, NUL-containing, and excessive
  hints are rejected. The stored base makes each accepted representation
  unambiguous.
- Import and Reimport From File compute the hint only after successful immutable
  capture. They never copy or move the file.
- Package move and duplication never move, copy, search for, or probe a
  physical source. They copy `HintBase + Hint` byte for byte: `AssetRelative`
  intentionally rebinds at the destination, while `ProjectRelative` and
  `Absolute` preserve their physical meaning. A `ProjectRelative` escape is
  rejected when explicit Reimport resolves it; Reimport From File repairs it.
- Cook strips the hint. DDC keys and imported-data identities exclude it.
- Source filename, timestamp, byte count, and last-import hash may remain
  bounded editor diagnostics, but they do not gate load or Build. Stage 0 must
  decide which provenance facts still earn their storage cost.

Because hint resolution occurs only after an explicit user command, opening a
package cannot read an arbitrary relative or absolute target supplied by
package data. Editor presentation may show the normalized hint without probing
the target; availability is checked when Reimport is invoked.

### Reimport commands

**Reimport** performs no file selection. It is enabled only when every required
source role has a persisted hint. Invocation resolves each hint relative to the
current package, captures the complete source closure once, and runs the same
family import transaction used by initial import. Missing, malformed, escaped,
or unreadable input fails before live-state commit and therefore changes
neither the live asset nor package bytes.

**Reimport From File...** is offered for every successfully loaded supported
standalone asset regardless of hint availability. It selects the complete
required source set and uses the same transaction as initial Import and
Reimport:

```text
capture immutable source closure
  -> validate and build detached candidate
  -> atomically commit live canonical data, import parameters, derived result,
     and classified hint set
  -> save the package bundle
```

Any failure before live-state commit leaves both memory and disk unchanged.
Live-state commit is the point at which the new canonical data, parameters,
derived result, and hint set become visible together and the asset becomes
Dirty. Package save is a subsequent persistence step: save failure preserves
the prior on-disk DAST/DABK bundle and leaves the complete new live state Dirty
for retry or explicit user discard. The plan uses **live-state commit** and
**package save** for these two success boundaries; the unqualified word
"publication" is avoided for this workflow.

For TextureCube, the command may request either one panorama or all six faces;
partial source-role replacement is excluded unless Stage 0 proves a complete
transaction and unambiguous UI. Scene remains creation-only. Skeleton has no
external payload or standalone reimport command.

Changing build settings is not Reimport. It rebuilds from resident canonical
imported data. Reimport never runs implicitly because a timestamp or hash has
changed, and DDC miss never calls either reimport path.

### Import parameter ownership

Import parameters are persisted by the narrowest owner that must reproduce
their effect; the importer dialog itself is not serialized as a replay request.

| Parameter class | Persistence owner | Identity and replay rule |
| --- | --- | --- |
| Decode/import interpretation | Concrete family import data beside the source hint | Values that affect conversion from encoded source into canonical imported data participate in imported-data provenance and are reused by ordinary Reimport |
| Editable build policy | Runtime asset properties | Values that transform canonical imported data into DDC/cooked output participate in the DDC key and rebuild without source access |
| Auto-detected source facts | Canonical imported data or bounded provenance | Dimensions, channel layout, material mapping, Skeleton compatibility, and similar facts are validated outputs, not user replay parameters |
| One-shot presentation/placement | Editor-local transient state | File-dialog directory, destination package, naming choices, overwrite confirmation, preview toggles, and notification options are not persisted in the asset |

The ownership test is the first deterministic transformation affected by the
parameter. A channel interpretation, coordinate/basis conversion, mesh import
scale baked into canonical vertices, or RAW heightmap layout belongs to family
import data. Mip generation, target format, compression quality, collision
policy, or another transformation that can run entirely from canonical data
belongs to the asset's build settings. A parameter must not be duplicated in
both places.

Family import-data schemas are versioned and bounded. They store typed values,
not dialog indices or labels, generic key/value bags, serialized widgets, or
provider identities. Ordinary Reimport uses the saved family parameters and
current built-in decoder implementation. Reimport From File initializes its
dialog from the saved parameters when compatible, allows the user to change
them, and commits the new parameter set only with the complete successful
live-state candidate. Importer/decoder version may remain provenance for
diagnostics and migration, but it does not request a retired plugin or make the
physical source part of DDC recovery.

### Load, Build, and failure behavior

Authored package load first resolves and validates every required DABK entry,
then constructs the object graph. Missing/corrupt authored bulk data is a hard
authored-package failure and must not fall through to the source hint or DDC.
Existing backup/restore is the baseline recovery path. Ordinary asset-menu
Reimport cannot be claimed as a remedy when the object cannot load. Stage 0
must either select and bound a metadata-only recovery shell that can safely run
Reimport From File, or explicitly make backup/restore the only recovery until a
separate tool is designed.

After a valid authored load, each payload family performs:

```text
validate imported-data schema and bounds
compute build key from imported data plus settings
query and validate DDC
on miss/corruption: build from imported data
publish complete in-memory product
best-effort store in DDC
```

PostLoad and Cook may call family Build modules, but they must not call source
capture, filename resolution, AssetForgeBuiltins import functions, Assimp, or
image/source decoders. Diagnostics distinguish `AuthoredDataMissingOrInvalid`,
`DdcMissOrCorrupt`, `BuildFailed`, and `DdcStoreFailed`; `MissingSource` is a
reimport diagnostic only.

### Compatibility and migration

The repository-owned authored corpus is the supported compatibility baseline.
Stage 0 inventories every legacy package and chooses deterministic regeneration
or an explicit one-time editor migration for each family. A migration may read
the old persisted source only as an explicitly invoked corpus upgrade; package
load must not silently mutate or save an asset.

Cutover is gated on all repository packages that require external editor
payloads containing valid canonical imported data and DABK descriptors. A
temporary legacy reader may exist only while migration is active, must be
read-old/write-new, and is deleted in the same plan. Assets outside the
repository corpus without canonical data require Reimport From File rather
than permanent source-fallback compatibility, but only if they can load or the
selected metadata-only recovery path supports them. Otherwise they require
backup restoration or an explicit external migration tool.

### Stage 0 decision ledger

Stage 0 records its selected answers in this plan before serialized data or
PostLoad behavior changes. Each row must name the concrete schema/API result,
the affected families, and the migration consequence; an inventory without a
selected result does not close the row.

| Decision | Fixed invariant | Stage 0 result required |
| --- | --- | --- |
| Family canonical values | Build input is bounded, versioned, platform-independent, and decoder-free | Exact fields, payload ids, bounds, validation, aggregate content identity, and inline/DABK placement for every family |
| TextureCube | Reimport replaces a complete source closure atomically | Canonical faces versus panorama, editable projection boundary, size limit, and whether replacing fewer than six face selections can still produce a complete unambiguous closure |
| Source hints | Hints are optional, editor-only, excluded from authored/DDC identity, and never probed implicitly | Grammar, normalization, containment, source-role encoding, move/duplication rebasing or byte-copy rule, and invalidation diagnostics |
| Reimport transaction | Candidate work is detached and pre-commit failure is non-mutating | Exact live-state commit API, Dirty/save behavior, cancellation points, and diagnostic ownership |
| Provenance | Provenance never gates load or Build | Retained bounded fields and removal list for timestamp/hash polling |
| Legacy compatibility | No permanent source fallback survives this plan | Per-family reader or regeneration path, corpus command/evidence, cutover gate, and removal point |
| Hard authored-data failure | Missing/corrupt canonical data never falls through to DDC or source hints | Backup-only policy or a bounded metadata-only Reimport From File recovery entrypoint |
| Module ownership | Engine/Build/Cook do not depend on concrete importers or source decoders | Concrete value types, Build APIs, allowed module edges, and enforcement search/test |

### Stage 0 selected results

#### Canonical value schemas and placement

Every value below uses schema version 1, validates before publication, hashes a
canonical little-endian encoding of its inline fields followed by its authored
bulk bytes, and stores those bytes through `FEditorBulkData`. AssetCore's
existing 64 KiB threshold alone chooses inline DAST versus external DABK; a
family never chooses placement. A zero hash, wrong payload id, unknown enum,
non-finite numeric value, count/byte disagreement, invalid reference, or bytes
above the family bound rejects the complete authored package.

| Family | Stable authored payload id | Schema-1 value and bound |
| --- | --- | --- |
| Texture2D | `7f3301ba-7c9f-45c6-8a8a-b67cc85dc65e` | Inline width, height, original channel count, RGBA8 format, alpha semantic, schema; tightly packed RGBA8 pixels in bulk; dimensions 1..16384 and at most 512 MiB |
| TextureCube | `8b2cd073-1965-4a69-8730-d84e54fd72e1` | Inline six-face dimension, per-face original channel/alpha facts, schema; six canonical RGBA8 faces in positive-X, negative-X, positive-Y, negative-Y, positive-Z, negative-Z order in one bulk value; equal square faces and at most 768 MiB aggregate |
| VolumeTexture | existing `6fe21a38-4943-40a7-a304-c2d526f22931` | Existing width, height, depth, normalized voxel format, schema, and tightly packed voxel bulk; the existing dimension and byte bounds remain authoritative |
| TerrainHeightmap | `5a7a7583-8bb7-4d51-a0df-56f52cd48376` | Inline width, height, unsigned-16 interpretation and schema; row-major little-endian uint16 samples in bulk; dimensions 2..16384, at most 268,435,456 samples and 512 MiB |
| StaticMesh | `442898cd-801d-49ed-9345-95334531fc1d` | Canonical encoding of material source-index/name mapping followed by mesh records containing name, transformed positions, normals, tangents, up to four UV sets, colors, indices, and source material index; the existing 100,000,000-vertex, 300,000,000-index and 65,536-section limits apply, with a 1,073,700,000-byte authored encoding ceiling |
| SkeletalMesh | `e1246757-fac3-498d-a4ec-51615d956391` | Canonical LOD0 positions, normals, tangents, UVs, colors, indices, four-slot influences, sections, palette indices, inverse-bind matrices, mesh-node bind transform, stable material mapping, Skeleton compatibility identity and schema; existing count limits apply, with a 1,073,700,000-byte authored encoding ceiling |
| AnimationClip | `87d4a296-6357-4b64-92d3-9c5e2881e292` | Canonical duration, clip name, Skeleton compatibility identity, and ordered track records containing bone index, path, interpolation, times and typed values; existing track/key limits apply, with a 1,073,700,000-byte authored encoding ceiling |

TextureCube deliberately retains six decoded faces, including for panorama
imports. Face dimension, projection version, and exposure are import
interpretation facts: changing them requires explicit reimport. They are not
editable Build settings. Six-face import/reimport always selects all six roles;
partial face replacement is removed. This keeps ordinary Build decoder- and
projector-free and bounds the one canonical representation.

Skeleton remains ordinary reflected package authority and gains no authored
bulk. StaticMesh canonical geometry is the pre-build imported mesh value rather
than render/collision output. SkeletalMesh and AnimationClip canonical values
are their existing validated object-free payload values plus the listed
compatibility/import facts; their DDC/cooked codecs may share field encoders but
the authored payload ids and authority remain distinct.

#### Source-hint grammar and provenance

`FSourceFile::Filename` is replaced in schema 2 by optional `HintBase + Hint`
data. `HintBase` is one of `AssetRelative`, `ProjectRelative`, or `Absolute`;
the base is never inferred from the stored string. A hint set is canonical
stable-identity order with at most eight roles for the families in this plan.
Stable identities and roles use the existing bounded identifier grammar. Hint
text is normalized UTF-8, at most 4096 bytes, contains no NUL, URI scheme,
empty component, backslash, `.` component, trailing slash, or repeated
separator.

- `AssetRelative` resolves from the physical parent of the owning `.dasset`;
  normalized `..` components are permitted.
- `ProjectRelative` resolves from the active physical project directory and
  must remain within that directory.
- `Absolute` stores a normalized absolute platform path. Drive letters are
  uppercase and comparison/containment uses the platform's path case rules.
- Default classification first normalizes the source, owner, and active project
  paths and uses component containment, never string-prefix containment. A
  project-local source with a project-local owner becomes `AssetRelative`; a
  project-local source whose owner is outside the project becomes
  `ProjectRelative`; every other source becomes `Absolute`. Importers may
  explicitly request any compatible base.
- Import and Reimport From File classify only after immutable capture succeeds.
  Reimport resolves only after the user invokes it. No display/status path
  probes the result.
- Move and duplication copy `HintBase + Hint` unchanged and perform no source
  I/O. Consequently `AssetRelative` intentionally rebinds to the new package
  location, while `ProjectRelative` and `Absolute` retain their physical
  meaning. Explicit Reimport reports `HintEscapesProject` if a
  `ProjectRelative` hint resolves outside the project; Reimport From File
  repairs it. Cook strips all import data.

Retained provenance is stable identity, role, display label, exact physical
source byte count, source-content XXH3-128, and decoder/importer/projection
identifier and version. Last-write time is removed. Provenance is excluded
from canonical imported-data identity and every Build/DDC key. Timestamp/hash
polling is removed from Texture2D inspection and thumbnail paths,
TextureCube/VolumeTexture status, Terrain recovery/inspection, StaticMesh
inspection, `SourceReferenceIndex`, and any automatic PostLoad or Cook path.
Explicit Reimport may compare captured bytes only to report a no-op candidate.

#### Reimport transaction and hard-failure policy

Each family exposes one typed detached candidate containing canonical imported
data, typed import parameters, derived product, and complete hint/provenance
set. Capture, decode, validation, Build, and cancellation precede the live-state
commit. A family-owned exchange token performs one non-failing swap of all four
parts on the game thread. Cancellation is accepted through Build completion;
after commit it is ignored. Commit marks the package Dirty. The editor then
saves the package bundle. Save success finalizes displaced state; save failure
leaves the new complete live state Dirty and the prior on-disk DAST/DABK bundle
unchanged. Explicit discard reloads/discards the dirty package through the
existing package workflow; reimport does not invent compensation after a save
failure. Diagnostics are owned respectively by source capture, concrete
importer, family Build, live exchange, and AssetCore save.

Missing or corrupt required authored bulk uses only AssetCore's exact companion
backup recovery. There is no metadata-only live object and no ordinary asset
menu when construction fails. If backup recovery fails, repository content is
restored from source control or processed by the bounded corpus migration tool;
external packages require backup restoration or Reimport From File after a
loadable prior version is restored.

#### Import-parameter ownership

Texture2D decoder id/version is provenance only because its canonical value is
already RGBA8; usage, sRGB, maximum resolution, compression quality, alpha mip
mode, and coverage threshold remain editable asset Build policy. TextureCube
layout, decoder/projection versions, face dimension, and panorama exposure are
typed import interpretation. VolumeTexture atlas format, channels, slice
dimensions, depth, tile layout, and decoder version are typed import
interpretation; output format and mip filter remain Build policy.
Terrain source format/profile and decoder version are typed import
interpretation. StaticMesh basis axes and importer version are typed import
interpretation; normalized size and collision policy remain asset Build
policy. Skeletal/animation coordinate conversion and scene decoder versions
are import provenance/interpretation committed by Scene; Skeleton identity,
detected material mapping, dimensions, bounds, and track facts are validated
canonical outputs. Dialog indices, selected directories, destination names,
overwrite choices, and preview state remain transient.

#### Compatibility, corpus, and module ownership

The cutover keeps DAST v6/DABK v2 and changes reflected family schemas, not the
package envelope. The temporary migration command loads each current package
while old reflected fields still exist, obtains canonical data from the
available physical source or current validated DDC/product, publishes the new
value, and performs canonical resave. It is explicit and never runs during
ordinary load.

The tracked family corpus and selected path are: six Texture2D (five
VintageLighter Scene outputs regenerate from the tracked glTF; TEX_StoneHead
uses its current validated decoded/DDC product), one TextureCube (tracked HDR),
two VolumeTexture (already canonical, tracked PNGs retained only for explicit
reimport), zero TerrainHeightmap, six StaticMesh (RiggedSimple and
VintageLighter regenerate from tracked Scene sources; Box, Sphere, SplineBox,
and GrayboxPawn encode their current validated imported/render geometry), one
SkeletalMesh and one AnimationClip (both use an explicit read-old/write-new
migration of their validated schema-1 DDC products; the tracked RiggedSimple
GLB remains their deterministic creation source). The migration gate is a
17/17 current-schema report followed by cold-DDC/no-source load and Cook.
Temporary skeletal readers are removed immediately after their Stage 3 corpus
cutover; the remaining family migration entrypoints are removed in Stage 4.

Engine owns reflected canonical values, validation, live exchange, and cooked
publication. TextureBuild, TerrainBuild, StaticMeshBuild, and the existing
skeletal build code own decoder-free canonical-to-derived recipes and keys.
AssetCore alone owns bulk placement and package transactions.
AssetForgeBuiltins owns physical capture, decoding/Assimp/Scene translation,
hint classification, and construction of typed candidates. Family editor
modules own command enablement, dialogs, save, and diagnostics. Allowed edges
are Editor -> Build -> Engine -> AssetCore; Engine, Build, and Cook may not
include or link AssetForgeBuiltins, ImageDecoder, Assimp, or physical-source
helpers. Stage 5 enforces those edges with descriptor and include searches.

## Current Foundations and Gaps

| Area | Foundation to preserve | Gap closed by this plan |
| --- | --- | --- |
| Package storage | DAST v6 payload directory, `FEditorBulkData`, DABK v2, stable companion naming, hash validation, backup recovery, and atomic bundle publication | Only VolumeTexture currently uses authored bulk as normalized build authority |
| Direct import | Family-owned capture/decode/build/publication and finite editor dispatch | Most family import products keep normalized data only transiently |
| DDC | Family-owned canonical keys, typed validation, disposable cache storage, and build sessions | Several PostLoad paths recover by reopening source files; some scene outputs cannot recover at all |
| Source metadata | Concrete family import data and immutable single-attempt capture | Current relative paths use the project rather than owning asset as their base, and filenames remain overloaded as rebuild authority |
| Reimport | Explicit family reimport functions with detached-candidate safety | Current UI primarily assumes one persisted current source and lacks a uniform Reimport From File split |
| Cook/runtime | Cooked packages use required DBLK payloads and exclude import modules from Game | Cook can still depend indirectly on authored DDC recovery that assumes source availability |
| Corpus | Repository-bounded compatibility policy and existing asset qualification tooling | Legacy packages do not uniformly contain canonical imported payloads or source-independent rebuild inputs |

## Implementation Stages

### Stage 0: Freeze schemas, source-hint semantics, and migration

Dependencies: current DAST/DABK authored-bulk contract and completed direct
asset-import simplification milestones.

- [x] Inventory every family PostLoad, Cook, explicit rebuild, reimport,
  inspection, thumbnail, test, and editor action that resolves or probes a
  physical source filename.
- [x] Inventory every repository-authored Texture2D, TextureCube,
  VolumeTexture, TerrainHeightmap, StaticMesh, SkeletalMesh, and AnimationClip;
  record current package version, DDC-only payloads, existing authored bulk,
  source availability, and deterministic regeneration path.
- [x] Freeze the exact canonical imported-data schema, stable payload id,
  bounds, validation, content-hash encoding, and inline/DABK placement for each
  family in the table above.
- [x] Resolve TextureCube's canonical representation and whether partial
  six-face reimport remains supported; record the selected size and editability
  tradeoff before implementation.
- [x] Freeze source-hint grammar, project containment classification,
  package-relative versus external-absolute encoding, `..` handling,
  multi-source roles, package move/duplication rebasing or byte-copy behavior,
  post-move invalidation, and cook stripping.
- [x] Freeze the Reimport live-state commit and package-save state machine,
  including candidate ownership, cancellation, Dirty state, user discard, and
  the exact state retained after save failure.
- [x] Decide whether hard authored-data failure supports a bounded metadata-only
  Reimport From File recovery shell or only backup/external migration; do not
  describe an ordinary asset-menu action as available before object load.
- [x] Decide which non-authoritative provenance fields remain and identify every
  timestamp/hash polling path that later family stages must remove from load or
  status inspection.
- [x] Classify every current importer option as decode/import interpretation,
  editable build policy, auto-detected output, or transient editor state;
  eliminate duplicate and generic replay fields before freezing family
  schemas.
- [x] Select the exact read-old/write-new or regeneration path for every legacy
  family and prove the migration can complete before source fallback is
  removed.
- [x] Freeze required module/API ownership so Engine values and family Build
  modules consume imported data without depending on AssetForgeBuiltins or
  concrete source decoders.

#### Acceptance Gate

- Every supported family has one bounded canonical imported-data input, one
  content identity, one DDC reconstruction path, and one corpus migration path.
- No source-hint, TextureCube, transaction, hard-failure recovery,
  compatibility, package-version, module-edge, or bulk-placement decision
  remains unresolved before serialized data changes.

### Stage 1: Establish source-independent canonical data with Texture2D

Dependencies: Stage 0 schemas and migration decisions complete.

- [x] Make the Texture2D canonical decoded pixels a reflected, versioned value
  stored through `FEditorBulkData`; retain only lightweight dimensions and
  interpretation inline.
- [x] Serialize and validate Texture2D imported data through DAST/DABK with
  bounded allocation, exact payload-id/size/hash agreement, and no platform or
  DDC facts in the authored payload.
- [x] Compute Texture2D keys and detached build requests from the canonical
  imported-data identity and current build settings without a physical source.
- [x] Replace Texture2D PostLoad source inspection/recovery with DDC query and
  local build from authored pixels; make a DDC put failure diagnostic rather
  than invalidating the complete in-memory product.
- [x] Make Texture2D Cook build from authored pixels after a cold or corrupt
  DDC without AssetForgeBuiltins, filename resolution, or image decoding.
- [x] Add hybrid optional source-hint serialization and the separate Texture2D
  Reimport/Reimport From File entrypoints using one detached import transaction.
- [x] Persist Texture2D decode/import parameters in concrete family import data,
  keep mip/compression policy on the asset, and prove Reimport From File updates
  neither parameter set until live-state commit succeeds.
- [x] Implement the selected Texture2D read-old/write-new or regeneration path,
  migrate the repository-owned Texture2D corpus, and record the count and
  qualification evidence while any temporary reader still exists.
- [x] Cover inline and external authored storage, DABK loss/corruption/backup,
  DDC deletion/corruption/store failure, missing/changed source, absent,
  project-local relative, external absolute, and cross-volume hints, package
  move/duplication, pre-commit reimport failure, post-commit save failure, Cook,
  and cooked runtime loading.

#### Acceptance Gate

- A Texture2D whose original source and DDC are both absent loads, rebuilds,
  edits, and Cooks from DAST/DABK alone.
- Package load and DDC recovery perform no source probing or decoding; only the
  two explicit reimport actions can read a physical Texture2D source.
- Every repository-owned Texture2D is migrated or regenerated, and its recorded
  qualification no longer depends on a temporary source fallback.

#### Stage 1 qualification evidence

- The Texture2D authored schema is version 1 with payload id
  `7f3301ba-7c9f-45c6-8a8a-b67cc85dc65e`, a 16384-by-16384 dimension bound,
  and a 512 MiB decoded-byte bound. Its identity covers the schema,
  interpretation fields, and exact RGBA8 bytes; platform and provenance facts
  are excluded.
- `TextureBuild` owns uncooked PostLoad, DDC query, local reconstruction, Cook
  input, and best-effort DDC diagnostics. Searches find no source resolver or
  image decoder below that path. `AssetForgeBuiltins` owns only Import,
  `Reimport`, and `Reimport From File` source capture and decoding.
- Six tracked Texture2D packages were regenerated and canonical-resaved. Each
  has one LFS-managed DABK companion: `TEX_StoneHead` contains 1,048,576
  authored bytes; each VintageLighter texture contains 4,194,304 authored
  bytes. After deleting `Sandbox/DerivedDataCache/Textures` and temporarily
  moving both physical source directories outside their expected locations,
  all six loaded, rebuilt, and canonical-resaved successfully; the source
  directories were restored afterward.
- Final SHA-256 qualification pairs (`.dasset`, `.dabulk`) are:
  `TEX_StoneHead` (`0b4ef87bb362cef2efd56684d354109ac7c4486ee6df16cd22247ae50c9f435e`,
  `8b281231a2c3df1e0eaef43000599bcbdeb6765bfdd7bf1809d9a321ddc9d4f1`),
  `diff_BaseColor` (`3f749ee1d1e29890d074b6ad289d6eef0beec699ed996d1bc20ee9990e5bf16e`,
  `7a602efcee3959c2b530caacd78452ce6daa4f0a169c866063f3290600d74989`),
  `diff_Opacity` (`9d3a30359f7aa147572d23221ea202aeac410194a4b35cb5a87f77ea53d8819f`,
  `8144a41588c1c2d96f3fdea383559c460dcf6ca85472c70b45327d7746412bdc`),
  `Metallic` (`ffcc1a139c63e674c76d07bf5d77bb4701adf4c201c73362d7e2371f9f01601b`,
  `0569ace1c562db7645b836e63e53e9a8b46dc86bd835848b3439f2c2b8b032fd`),
  `Roughness` (`5dc068762468778554af97db97f760ea8dceed91666b4fda09eb8b2175c57078`,
  `3b0a1b6e30aaf0bca852b38c751c497eaa1c51f0ec57dc26e0cce35ee5204f0f`),
  and `Normal` (`87bcbeef372cf9de9739870bfb6fe21e23d0a7bba0737e1e2b901925963e7e80`,
  `ffb00cafdc1607741ca61ffcf1285105e6568db4d7dba4cdd0758172bdb4772f`).
- Focused evidence: `AssetImportDataTests` 3/3, `TextureTests` 77/77, and
  `FTextureCookTests.ColdCookRebuildsFromAuthoredPixelsWithoutSourceOrDdc`
  1/1. The family matrix covers external DABK recovery and hard failure,
  corrupted DDC rebuild, best-effort DDC store failure, changed/missing source,
  detached reimport failure, source-hint rebasing, and cooked output.

### Stage 2: Migrate direct texture, Terrain, and StaticMesh families

Dependencies: the Texture2D vertical slice and common source-hint semantics are
qualified.

- [x] Persist TextureCube canonical imported data and rebuild all projection,
  mip, compression, and platform products from it; implement the selected
  complete panorama/six-face reimport transaction.
- [x] Retain VolumeTexture's normalized authored voxels while removing legacy
  project/absolute filename assumptions and aligning its Build, Cook,
  diagnostics, and reimport actions with the new contract.
- [x] Persist TerrainHeightmap canonical samples and replace asynchronous and
  synchronous source-recovery workers with imported-data build workers that
  never capture or decode a file.
- [x] Persist StaticMesh canonical imported geometry and material-source
  mapping; rebuild render and collision data from it without invoking Assimp or
  `BuildStaticMeshFileProduct` during PostLoad or Cook.
- [x] Ensure property edits and explicit Rebuild commands for all four families
  consume resident canonical data and never reimport implicitly.
- [x] Convert their DDC store policy to best effort after successful build and
  retain bounded family-specific diagnostics and cancellation behavior.
- [x] Apply the selected per-family compatibility path, migrate the
  repository-owned TextureCube, VolumeTexture, TerrainHeightmap, and StaticMesh
  corpus, and record counts before removing their source-backed fallbacks.
- [x] Add focused family tests matching the Texture2D source-independent matrix
  and assert that cache recovery does not set importer-invoked diagnostics.

#### Acceptance Gate

- TextureCube, VolumeTexture, TerrainHeightmap, and StaticMesh rebuild after a
  complete DDC deletion with all physical sources unavailable.
- In addition to initial Import, each family has exactly two explicit reimport
  actions and no PostLoad, Cook, inspection, thumbnail, or property-edit source
  dependency.
- The repository-owned corpus for all four families is migrated and accounted
  for before their temporary source-backed recovery paths are removed.

### Stage 2 selected results

- TextureCube now persists the six projected RGBA8 faces as canonical authored
  data. Panorama projection occurs only during explicit import or reimport;
  TextureBuild owns decoder-free PostLoad, DDC, and Cook recovery. The sole
  tracked cube, `/Game/Textures/TEXCUBE_PureSky_512x512`, was migrated with
  package SHA-256 `e48e6e043f55f8a66c826292840d82988f4073c856e4f5bd4ddcb1065d2ec64d`
  and DABK SHA-256 `2817c5de9b18b8021e69463ef66dd434039ccc990d6610393ce9725c9e292af1`.
- Both tracked VolumeTexture packages already contained normalized authored
  voxels and remain canonical: `VT_Cloud_Base_Voronoi_128` SHA-256
  `5563db5751c7c3b80f62d4133d7c294b2563b5b09f415ad55a4f290e49ab03fd`
  and `VT_Cloud_Detail_Voronoi_64` SHA-256
  `103448e606c24b3a07cd39b452bb7a306202e5cc227d86d2fb67d71163d74410`.
  Their DDC publication is best effort and Cook/PostLoad rebuild from resident
  voxels.
- TerrainHeightmap now stores row-major uint16 authored samples and TerrainBuild
  exclusively owns its source-free PostLoad feature. The tracked corpus count
  remains zero.
- All six tracked StaticMesh packages were atomically canonical-resaved and the
  temporary Assimp/source-backed migration bridge was removed. SHA-256 values
  are: Box `e7f97e585dd8d8d1f4fcf5a5b94f9e6bc751eda2b89422ae5fba2fb3df7974bb`,
  Sphere `c4a810d74b831808d8d551545c9622ed37d12fc5612bf56203347fa93b4e08db`,
  SplineBox `7cce1112ad34c65bdf2828b338b0209a6eb2702feec85a00a2a0d5a142abd53b`,
  RiggedSimple `39dfdacbd02a20f13e62ad6425793a567819bc5d710d9831da441495718f6d76`,
  GrayboxPawn `3f6be2a261dcc8bb90379694f500c1b3df0a72702304a1ec3cc748ed0b33e180`,
  and vintage_lighter_1k
  `c4093f6704992358687bf430042eaa0a10554b61eae9742d834df12c5d6572b5`.
- Focused evidence: `TextureTests` 77/77, `TerrainHeightmapTests` 11/11,
  `TerrainHeightmapCookTests` 1/1, and `StaticMeshTests` 73/73. Their cold or
  corrupt DDC and missing-source cases rebuild from canonical data and assert
  that automatic recovery does not set source-importer diagnostics. Production
  searches under Engine, TextureBuild, TerrainBuild, and StaticMeshBuild find
  no source capture, filename resolution, ImageDecoder, or Assimp dependency
  for these Stage 2 families.

### Stage 3: Make skeletal and animation outputs independently authored

Dependencies: common canonical imported-data pattern qualified for direct
families.

- [x] Persist the selected canonical SkeletalMesh geometry/influence/bind data
  and AnimationClip track/key data in each independent output package or DABK
  companion during standalone/private Scene orchestration.
- [x] Keep Skeleton as package-only structural authority with no unnecessary
  DDC or bulk payload; validate referenced Skeleton compatibility before
  skeletal/animation Build.
- [x] Re-key and rebuild skeletal/animation DDC values from their own authored
  canonical data, build settings, output identity, and Skeleton compatibility
  rather than a transient captured Scene closure.
- [x] Remove cache-only authored load failure for Scene outputs and prove that
  no aggregate Scene recipe, root-source hint, peer-output graph, or importer
  replay record is introduced.
- [x] Preserve atomic multi-package Scene publication while including every
  output's authored bulk companion in preflight, save, rollback, relocation,
  and cleanup closure.
- [x] Apply the selected skeletal/animation compatibility path, migrate every
  repository-owned independent Scene output, and record counts while temporary
  readers and original Scene sources are still available.
- [x] Cover DDC deletion/corruption, DABK failure, Skeleton mismatch, package
  relocation, Scene transaction rollback, Cook, and runtime payload loading.

#### Acceptance Gate

- Every independently loadable SkeletalMesh and AnimationClip can reconstruct
  its derived payload from its own package/companion plus referenced Skeleton,
  with neither Scene source nor Scene orchestration available.
- Scene remains creation-only and its outputs remain ordinary peer assets.
- Every repository-owned skeletal/animation output is migrated and accounted
  for before temporary Scene/source-backed recovery is removed.

#### Stage 3 selected results

- `FSkeletalMeshImportedData` and `FAnimationClipImportedData` own schema-1
  canonical payloads through `FEditorBulkData`, using payload ids
  `e1246757-fac3-498d-a4ec-51615d956391` and
  `87d4a296-6357-4b64-92d3-9c5e2881e292`. Skeleton remains reflected
  structural authority and contributes only its compatibility identity.
- Skeletal DDC key schema 2 contains the canonical provider recipe,
  imported-data identity, payload fingerprint, current output object path,
  Skeleton compatibility, and target. It has no source closure, Scene state,
  source hint, peer graph, or importer replay field. PostLoad decodes only the
  owning asset's canonical bulk and treats DDC storage as best effort.
- Scene capture remains creation-only. Detached candidates capture each
  output's canonical bulk before the existing atomic multi-package save, so
  AssetCore includes inline/DABK payloads in preflight, rollback, relocation,
  duplication, deletion, and cleanup closure without an aggregate Scene asset.
- The repository corpus contains one independent SkeletalMesh and one
  AnimationClip. A bounded temporary schema-1 DDC reader migrated both
  read-old/write-new, was deleted before final validation, and the final tool
  canonical-resaved both packages without legacy support. Their payloads are
  below the external threshold, so no tracked DABK is required. SHA-256:
  Animation_0
  `80dd5d529f79962bafda9f95ea77315d5d3abd7ad4c09a860ebc5c872a671d30`;
  Cylinder
  `ddb9f9dedd9112bcde6f038467ca211b2c2c804ee7a10dbd5010b735e6f2e7f7`.
- Focused evidence: `SkeletalAssetTests` 35/35,
  `SkeletalSceneLifecycleTests` 1/1, and `SceneImportTests` 4/4. Coverage
  includes DDC miss/corruption/warm hit, external DABK hard failure,
  Skeleton mismatch, relocation re-keying, duplication, Scene rollback, Cook,
  and authored/runtime reload. Production searches find no legacy repair
  loader, source-closure key field, source capture, Assimp, or AssetForge
  dependency in Runtime skeletal PostLoad or SkeletalBuild.

### Stage 4: Cut over editor workflows and migrate the corpus

Dependencies: all family authored schemas and source-independent builds are
implemented.

- [x] Replace `FSourceFile::Filename` semantics with the selected optional,
  explicitly based asset-relative/project-relative/absolute hint
  representation and update concrete import-data schemas without introducing
  a generic source manager.
- [x] Present **Reimport** only when the required hint set exists and present
  **Reimport From File...** for every supported standalone family regardless
  of hint availability; use clear terminal diagnostics for resolution,
  selection, decode, build, live-state commit, and save failures.
- [x] Remove change-source/reference-only actions whose result can leave the
  asset pointing at unimported data; a new file selection can affect canonical
  data only through a successful Reimport From File live-state commit.
- [x] Remove load-time source availability, timestamp, and changed-content
  status from inspectors, thumbnails, details panels, and build status; retain
  an explicit reimport-hint display without automatic filesystem probing.
- [x] Run a corpus-wide closure audit, migrate any recorded stragglers, verify
  source-control/LFS classification, and reconcile the per-family counts and
  hashes recorded in Stages 1-3 with the existing qualification data.
- [x] Remove temporary legacy readers, old filename conversion helpers and
  schema branches, source-backed DDC recovery entrypoints, and obsolete tests
  after the corpus passes the new validator.
- [x] Verify package move, duplication, deletion, redirector fix-up, source
  control, and companion cleanup continue to operate on package/companion
  closure only and never mutate source files.

#### Acceptance Gate

- The repository contains no authored package that requires a physical source
  or DDC entry to load, edit, rebuild, or Cook.
- Production searches find no source resolution beneath PostLoad, DDC recovery,
  Cook, thumbnail, inspection, or automatic asset-load paths; legacy source
  schemas and compatibility code are gone.

#### Stage 4 selected results

- `FSourceFile` schema 2 stores an explicit `HintBase + Hint` pair and retained
  provenance. `SourceFilename.h`, the schema-1 filename conversion path, and
  the family source-backed recovery entrypoints are removed. Package move and
  duplication copy the hint representation without touching a physical source;
  Cook strips import data.
- Texture, StaticMesh, Terrain, and Level editor workflows route Reimport and
  Reimport From File through family-owned detached candidates. Reimport is
  enabled only for a complete hint set; Reimport From File remains available
  without one. The former reference-only source replacement actions and
  automatic source status/probing in thumbnails, inspectors, and build status
  are removed.
- Final schema-2 DAST SHA-256 values supersede the earlier per-stage DAST
  qualification hashes for the 15 packages whose import data was rewritten:
  Texture2D BaseColor
  `4f12ddc0a86ef371ef2ee50c60090763526f4c414b26d7c1e9a9f2477b7056d9`,
  Opacity
  `cc9a02af165da776f4ed6cf1b1346e55e1f423f302d003b6b8fee93b80a9c821`,
  Metallic
  `f9a0366e8760b8e08a7130aeee8e2f1cd4fe8abd221b08ccff170cd7bc66172f`,
  Roughness
  `40710932592253af7009924d9033ce69aa6c213c18475f392ee6d89fe271835e`,
  Normal
  `870684b1863994f34282c4586d7745f219c23e9f3d3a0e85cbec89334b2956ea`,
  and StoneHead
  `768f5467ac93ef8d4085bad1ac90f0f2331874ee44fcf1a3131d79bf916bec4a`;
  TextureCube
  `1aa6c300c3e8cef311d0f0cb45866f533aeac46f8ef93b6bb1d96a90b786b121`;
  VolumeTexture Base
  `2ffe6d2096182baf6249f281e8ec2d2a26bfcaff468c9e2a1916f60ff498fe8a`
  and Detail
  `df03dad2959d5689b1c8e5ae0bd0a2ada119a79efec3510a094c1dc3ad3a2478`;
  StaticMesh Box
  `a3358bbf5d03e209452535f7097fcfedd205cf9281459278abd04692b14e4ead`,
  Sphere
  `087e190c83ce10265913650febca24854545a608f3449430944dab2a5d3fabf4`,
  SplineBox
  `9b590c6232864eede7eb0e93612897e1f87c5c4aa38a7826546a40f42a1aeba7`,
  RiggedSimple
  `4e31b27f98dedb2e64ba86fd3b9bd740078a4cbb9bb67e4c02fc7b676235f3f2`,
  GrayboxPawn
  `1d63b5972e2e39714834e7da3b35b062b0cd56aeeca04a39e71bfad0abff5611`,
  and vintage_lighter_1k
  `cea2ad2470c375e53dfd9f3ce796d8f500e596c13eb4eeadf17d48c579da1209`.
- The nine final DABK companion SHA-256 values are BaseColor
  `7a602efcee3959c2b530caacd78452ce6daa4f0a169c866063f3290600d74989`,
  Opacity
  `8144a41588c1c2d96f3fdea383559c460dcf6ca85472c70b45327d7746412bdc`,
  Metallic
  `0569ace1c562db7645b836e63e53e9a8b46dc86bd835848b3439f2c2b8b032fd`,
  Roughness
  `3b0a1b6e30aaf0bca852b38c751c497eaa1c51f0ec57dc26e0cce35ee5204f0f`,
  Normal
  `ffb00cafdc1607741ca61ffcf1285105e6568db4d7dba4cdd0758172bdb4772f`,
  TextureCube
  `2817c5de9b18b8021e69463ef66dd434039ccc990d6610393ce9725c9e292af1`,
  StoneHead
  `8b281231a2c3df1e0eaef43000599bcbdeb6765bfdd7bf1809d9a321ddc9d4f1`,
  Volume Base
  `8675d09f629f52c0fa5b1844c26451036a131b29ff09a3e6dd93181b417b70f4`,
  and Volume Detail
  `384b1373cf161f214392194fe309430d14a5c14166f19af7973ed13632f0d7a0`.
  Schema-2 resave did not rewrite these canonical bulk payloads.
- Project-level evidence is `DevTool asset baseline --project
  Sandbox/Sandbox.dproject`: 25 current DAST v6 packages; `DevTool asset audit`:
  25 compatible and zero incompatible, unsupported, failed, stale, or resave
  recommended; and canonical-resave `--project-scope --ci`: zero ready, zero
  blocked, 25 skipped, 25 selected. This is zero migration debt.
- Git attributes resolve `.dasset` through `binary` (`-text`, unset text
  diff/merge, no filter) and `.dabulk` through
  `filter=lfs diff=lfs merge=lfs -text`. Focused Stage 4 evidence is
  `AssetImportDataTests` 2/2, `TextureTests` 77/77,
  `TerrainHeightmapTests` 11/11, `StaticMeshTests` 73/73,
  `EditorAssetWorkflowTests` 32/32, `ContentBrowserWorkflowTests` 59/59 with
  one capability skip, `TextureThumbnailTests` 9/9, and `ThumbnailTests` 58/58.

### Stage 5: Qualify closure and publish lasting contracts

Dependencies: corpus migration and removal complete.

- [x] Run focused authored-bulk, package transaction, import/reimport, Texture,
  StaticMesh/collision, Terrain, skeletal/animation, Scene, DDC, and Cook tests
  using the repository testing workflow.
- [x] Clear the complete DDC, make all recorded physical source hints
  unavailable, and qualify representative packages from every family through
  editor load, derived build, save, Cook, and cooked runtime load.
- [x] Inject missing/corrupt/stale DABK, corrupt DDC, failed DDC put, failed
  package save, canceled build, unsupported source, missing hint, and
  cross-volume Reimport From File cases; verify the selected authority and
  rollback classifications.
- [x] Build the default Editor target, run the native aggregate and hidden-window
  editor smoke, and qualify Game/deployment exclusion of source hints, authored
  data, DABK, DDC, AssetForgeBuiltins, and offline source decoders.
- [x] Audit module descriptors and includes to prove Engine/Build/Cook paths do
  not acquire an AssetForgeBuiltins or importer dependency.
- [x] Update Asset Data Lifecycle, Asset Packages, Asset Import Architecture,
  Source File Workflows, family contracts, Code Modules, and content
  version-control guidance with only implemented lasting behavior.
- [x] Run changed/all documentation, all-plan, and all-roadmap validation and
  record exact test, build, corpus, search, and deployment evidence before
  completing this plan.

#### Acceptance Gate

- The full editor/Cook qualification succeeds from authored packages with both
  DDC and physical sources unavailable, while explicit Reimport paths retain
  detached-candidate and package-save safety.
- Lasting documentation contains one consistent authored/imported/derived/
  cooked authority model and no source-authoritative fallback language.

#### Stage 5 qualification results

- Focused qualification passes: `AssetBulkContainerTests` 11/11,
  `AssetPackageTests` 125/125, `AssetImportTests` 17/17, `AssetCookTests`
  13/13, `DerivedDataCacheTests` 10/10, `TextureTests` 77/77,
  `StaticMeshTests` 73/73, `TerrainHeightmapTests` 11/11,
  `SkeletalAssetTests` 35/35, `SkeletalSceneLifecycleTests` 1/1,
  `SceneImportTests` 4/4, and `TerrainHeightmapCookTests` 1/1. These suites
  cover cold/corrupt DDC, absent source hints, canonical rebuild, authored-bulk
  integrity and transaction failure, detached reimport candidates, cancellation,
  save rollback, Cook publication, and cooked package load.
- Editor qualification passes the full `Win64-Debug-DurinEditor` `all` build
  and a Sandbox hidden-window, eight-tick smoke. Game qualification passes the
  full `Win64-Debug-DurinGame` `all` build and equivalent hidden-window smoke.
  The Game deployment tree and run log contain no AssetForge, ContentBrowser,
  DurinEd, LevelEditor, TextureEditor, StaticMeshEditor, Assimp, ImageDecoder,
  source-hint, or DABK artifacts.
- Descriptor/include searches find no importer dependency in Runtime or
  developer Build modules and no legacy source-filename API. The remaining
  `AssetForgeBuiltins` program dependency is confined to the editor-only
  `DurinAssetTool` canonical-resave host; Core's generic `ImageDecoder` is not
  referenced by family Build or Cook paths. The unused
  `IStaticMeshBuildFeature` boundary and its test adapter were removed so
  explicit import is the only static-mesh file-build route.
- The final corpus rerun reports 25 current DAST v6 packages, 25 compatible
  packages with every debt category at zero, and canonical-resave CI with 25
  selected, 25 skipped, zero ready, and zero blocked. Git attributes resolve
  `.dasset` as binary and `.dabulk` through LFS.
- Documentation validation passes for 15 changed and 138 total documents,
  together with all 273 plans (4 active, 4 completed, 265 archived) and all 23
  roadmaps (2 active, 1 completed, 20 archived).
- `DevTool test all --agent` builds and runs all 83 native targets. Seventy-eight
  pass; `TerrainRenderVulkanTests`, `SceneImportVulkanTests`,
  `SkyBoxVulkanIntegrationTests`, `EditorGridVulkanTests`, and
  `TextureCookIntegrationTests` fail existing Vulkan pixel or injected-device-
  failure assertions after their data/package setup succeeds. Their focused
  non-rendering authority, Cook, package, Scene, Terrain, Texture, and lifecycle
  counterparts above pass. The five renderer-baseline failures are accepted as
  non-blocking debt outside this plan's authored-data authority closure; they
  are retained here so plan completion does not imply an inaccurate 83/83
  native-test result.

## Validation Matrix

Follow [Agent Build And Run](../../../Agents/BuildAndRun.md) and
[Agent Testing](../../../Agents/Testing.md); use focused targets before aggregates and
never overlap native build process trees.

| Concern | Required evidence |
| --- | --- |
| Authored authority | Every payload family validates and rebuilds from DAST/DABK after complete DDC and source removal |
| Source isolation | Instrumented/search-backed proof that source resolution and decoding occur only inside explicit Import, Reimport, and Reimport From File entrypoints |
| Source hints | Same-directory, parent-relative, external absolute, missing, malformed, cross-volume, moved, and duplicated package behavior matches the Stage 0 rebasing/byte-copy and invalidation decisions |
| Reimport safety | Reimport and Reimport From File capture once, atomically commit only a complete live candidate, update canonical data/parameters/result/hints together, and preserve prior disk state while retaining the new Dirty live state on save failure |
| Parameter ownership | Every importer option has one typed owner; ordinary Reimport reproduces import interpretation, while build-setting edits rebuild without source access |
| Bulk storage | Inline/external threshold, payload ids, bounds, hashes, DABK backup recovery, companion-first publication, rollback, move, duplicate, and deletion pass |
| DDC | Warm hit, cold miss, corrupt/incompatible entry, failed put, cancellation, and rebuild-key determinism use canonical imported data only |
| Family semantics | Texture projection/mips/compression, mesh render/collision, Terrain payloads, skeletal compatibility, and animation tracks preserve validated output behavior |
| Scene | Independent outputs rebuild without a Scene source, aggregate recipe, or peer-output replay record; multi-package publication remains atomic |
| Cook/runtime | Cold Cook needs no source/import module; cooked packages load only required DBLK data and deploy no editor authority or decoder dependency |
| Compatibility | Entire repository corpus is migrated, temporary legacy readers are removed, and authored storage qualification reports no old source-dependent package |
| UI/diagnostics | Commands are enabled predictably and distinguish missing or invalid hint/source, invalid authored data, unavailable hard-failure recovery, DDC failure, build failure, and save failure |
| Documentation | Changed/all document validation, all-plan validation, all-roadmap validation, and lasting contracts agree with implementation |

## Definition of Done

- Every authored asset contains the platform-independent canonical imported
  data required by all supported editor and Cook builds, inline or in its
  validated DABK companion.
- DDC is wholly disposable: clearing it never requires an original source file
  or source decoder to load, rebuild, edit, or Cook an authored asset.
- Physical source files are read only by explicit Import, Reimport, and
  Reimport From File operations.
- Persisted source locations are optional hints: project-local sources use
  owning-asset-relative paths and external sources use normalized absolute
  paths; neither form is authored or build authority.
- Reimport uses the current hint, Reimport From File selects and adopts a new
  classified hint, and absence of a hint never invalidates the asset.
- Family-specific import parameters required to reproduce canonical conversion
  are persisted and reused; build settings and transient UI choices remain in
  their separate owners with no generic replay parameter bag.
- Texture2D, TextureCube, VolumeTexture, TerrainHeightmap, StaticMesh,
  SkeletalMesh, and AnimationClip use family-owned canonical schemas and
  source-independent DDC recipes; Skeleton remains package-only.
- Scene outputs rebuild independently without acquiring whole-scene replay or
  reimport behavior.
- Authored companion, package transaction, relocation, duplication, deletion,
  source-control, Cook, and cooked runtime contracts remain validated.
- Repository packages and qualification data are migrated, all temporary
  compatibility code is removed, focused/aggregate tests and Editor/Game/Cook
  gates pass, and lasting documentation is updated.

## Deferred Follow-ups

- Content-addressed or archive-hosted authored companions beyond the current
  stable sibling DABK contract.
- Optional per-user recent-source directories, file-dialog bookmarks, or local
  relink databases; these must remain outside authored package identity.
- Automatic source watching and changed-file notifications. If later desired,
  they must be opt-in editor conveniences and must not affect load or Build.
- Embedding exact encoded source bytes for round-trip export or external-tool
  interoperability; canonical build authority does not imply source archival.
- Whole-scene reconciliation, generated-output ownership, or batch reimport.
- Remote DDC or authored payload services and cache promotion policy.

## Related Documentation

- [Asset Data Lifecycle and Storage](../../../Runtime/Assets/AssetDataLifecycle.md)
- [Asset Packages](../../../Runtime/Assets/AssetPackages.md)
- [Asset Import Architecture](../../../Editor/Architecture/AssetImportFramework.md)
- [Source File Workflows](../../../Editor/Guides/SourceFileWorkflows.md)
- [Asset Compilation](../../../Runtime/Assets/AssetCompilation.md)
- [Volume Textures](../../../Runtime/Assets/VolumeTextures.md)
- [Asset Import Simplification Roadmap](../../../Roadmaps/Archive/2026-08/AssetImportSimplification.md)
- [Code Modules](../../../Workspace/CodeModules.md)
- [Agent Build And Run](../../../Agents/BuildAndRun.md)
- [Agent Testing](../../../Agents/Testing.md)

## Related Code

- [`AssetImportData.h`](../../../../Engine/Source/Runtime/Engine/Public/Asset/AssetImportData.h)
- [`SourceHint.h`](../../../../Engine/Source/Runtime/Engine/Public/Asset/SourceHint.h)
- [`EditorBulkData.h`](../../../../Engine/Source/Runtime/AssetCore/Public/Asset/EditorBulkData.h)
- [`EditorBulkDataStorage.cpp`](../../../../Engine/Source/Runtime/AssetCore/Private/EditorBulkDataStorage.cpp)
- [`AssetPackageOperations.cpp`](../../../../Engine/Source/Runtime/AssetCore/Private/AssetPackageOperations.cpp)
- [`Texture2D.h`](../../../../Engine/Source/Runtime/Engine/Public/Texture/Texture2D.h)
- [`TextureCube.h`](../../../../Engine/Source/Runtime/Engine/Public/Texture/TextureCube.h)
- [`VolumeTexture.h`](../../../../Engine/Source/Runtime/Engine/Public/Texture/VolumeTexture.h)
- [`TerrainHeightmap.h`](../../../../Engine/Source/Runtime/Engine/Public/Terrain/TerrainHeightmap.h)
- [`StaticMesh.h`](../../../../Engine/Source/Runtime/Engine/Public/StaticMesh/StaticMesh.h)
- [`SkeletalMesh.h`](../../../../Engine/Source/Runtime/Engine/Public/SkeletalMesh/SkeletalMesh.h)
- [`AnimationClip.h`](../../../../Engine/Source/Runtime/Engine/Public/Animation/AnimationClip.h)
- [`TextureBuildModule.cpp`](../../../../Engine/Source/Developer/TextureBuild/Private/TextureBuildModule.cpp)
- [`AssetForgeBuiltinsAssetFeatures.cpp`](../../../../Engine/Source/Editor/AssetForgeBuiltins/Private/AssetForgeBuiltinsAssetFeatures.cpp)
- [`BuiltinImportProviderCommon.h`](../../../../Engine/Source/Editor/AssetForgeBuiltins/Private/BuiltinImportProviderCommon.h)
- [`Texture2DImport.cpp`](../../../../Engine/Source/Editor/AssetForgeBuiltins/Private/Texture2DImport.cpp)
- [`TextureCubeImport.cpp`](../../../../Engine/Source/Editor/AssetForgeBuiltins/Private/TextureCubeImport.cpp)
- [`StaticMeshImport.cpp`](../../../../Engine/Source/Editor/AssetForgeBuiltins/Private/StaticMeshImport.cpp)
