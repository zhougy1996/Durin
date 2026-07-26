# Asset Data Lifecycle and Storage

Durin separates asset identity, authoring input, rebuildable derived data, and
deployable runtime data. File suffixes describe those lifecycle contracts, not
merely whether a file contains binary bytes.

## Storage Classes

| Class | Typical location | Suffix | Authoritative for | May be deleted locally |
| --- | --- | --- | --- | --- |
| Source input | `SourceAssets/` or a supported legacy import location | Source-specific | Reimport and rebuilding | No |
| Object package | Mounted `Content/` | `.dasset` | Asset identity and editable object state | No |
| Derived data | `DerivedDataCache/` | `.bin` | Nothing; it accelerates editor and cook work | Yes |
| Cooked package | `Cooked/<Platform>/...` | `.dasset` | Runtime object metadata for that cook | No |
| Cooked bulk data | Beside its cooked package initially | `.dbulk` | Runtime payload bytes for that cook | No |
| Local state | `Saved/` | Format-specific | Diagnostics, sessions, and user-local state | Yes |

`FAssetPath` and reflected asset references always identify the main asset in a
`.dasset` package. They never identify a source file, DDC key, `.bin` object,
`.dbulk` file, byte offset, or physical workstation path.

## Authored Packages

An editor `.dasset` contains compact, review-worthy object state:

- reflected properties and cross-package asset references;
- portable source provenance needed for reimport;
- source content identity and lightweight diagnostics;
- build settings that contribute to derived-data keys.

Large platform render payloads do not belong in the authored package. Keeping
them external avoids rewriting source-controlled packages when a builder,
platform, or quality policy changes. Persistent source paths must be normalized
project- or engine-relative paths; absolute workstation paths are invalid.

The source input is authoritative for rebuilding but is not a runtime asset.
New shared source art belongs under project or engine `SourceAssets`, which is
versioned but not mounted as runtime Content. An asset type may temporarily
support legacy colocated sources as an explicit migration rule.

## Derived Data Cache Objects

Generic content-addressed DDC objects use `.bin` and let their owning subsystem
provide a magic value and versioned schema to identify the payload type. Domains
whose native artifacts have their own strict validation may retain those
formats beneath a namespaced DDC subtree; shader SPIR-V and reflection sidecars
are examples. The lifetime contract remains the same: every entry is disposable
and its authored inputs are authoritative.

A DDC key must be built from a canonical byte encoding of every input that can
change the output, including:

- source content hash;
- normalized build and import settings;
- payload schema and builder versions;
- target platform and any relevant feature profile.

File timestamps and absolute paths can be used to avoid rehashing an unchanged
source, but they are not content identity. DDC paths are derived from keys and
must never be serialized into `.dasset`. Missing, incompatible, truncated, or
corrupt objects are safe cache misses when rebuild inputs are available.

DDC writes use Core's shared atomic byte-publication API: a fixed-length
same-directory temporary file is flushed and closed before replacement. The
temporary name is independent of the destination name, and DDC round trips are
supported beyond the traditional Windows `MAX_PATH` boundary under the
[physical file I/O contract](../Core/FileIO.md).
Readers validate magic, versions, declared sizes, allocation limits, structural
invariants, and checksums before publishing data. A cache write failure does not
invalidate a complete in-memory build result.

## Cooked Packages and Bulk Payloads

Cook produces a platform-qualified runtime view under
`Cooked/<Platform>/`. It writes a cooked `.dasset` containing runtime object
metadata and moves large, already-built payloads into `.dbulk`. Source files,
source-only editor metadata, DDC keys, and DDC paths are not runtime
dependencies.

The initial loose-file convention is one companion bulk container per package:

```text
Content/Textures/T.dasset
Cooked/Win64/Game/Textures/T.dasset
Cooked/Win64/Game/Textures/T.dbulk
```

The companion name is derived from the cooked package's mount-relative path by
replacing `.dasset` with `.dbulk`. Case and path normalization follow the
package path rules. A cooked package must not persist that physical path.

A package refers to each external payload with a logical descriptor equivalent
to:

```text
PayloadId
LocationKind
Offset
StoredSize
UncompressedSize
Alignment
PayloadHash
PayloadSchemaVersion
TargetPlatform
CompressionMethod
```

`PayloadId` is unique within the package and remains stable when the payload is
moved from a loose companion into a future archive. `LocationKind` initially
selects the package companion; future kinds may select an archive or install
chunk without changing asset references or the asset-specific payload schema.
Offsets and sizes are unsigned, explicitly encoded values rather than native
structure layouts.

The `.dbulk` container has its own `DBLK` magic, container-format version,
target-platform identifier, and bounded payload table. Payload ranges must be
aligned, non-overlapping, contained by the file, and validated before
allocation. Each payload is independently checksummed so corruption is reported
against the owning asset and `PayloadId`.

Asset-specific codecs own the bytes inside a payload. For example, a texture
codec owns mip records and GPU block-compressed bytes, while a static-mesh codec
owns vertex and index streams. `AssetCore` owns container lookup, bounded I/O,
and descriptor validation but does not interpret those bytes. C++ object
memory, STL layouts, pointers, and RHI handles are never serialized.

The initial texture payload uses no additional container compression because BC
texture data is already compressed and must remain independently addressable by
mip. Other payload types may select an explicit compression method when their
codec and loading policy support it.

## Cook and Publication Rules

Cooking must be deterministic for identical source bytes, settings, builder and
schema versions, and target platform. It may reuse a validated DDC payload, but
the result is copied into cooked ownership; the runtime never follows a DDC
reference.

The cooker writes bulk data to a temporary file, flushes and closes it, validates
the completed container, and publishes it before publishing the cooked package
that references it. Failed cooks remove their temporary output. A stale
unreferenced bulk file is harmless and can be removed by manifest-driven output
cleanup; a package must never reference a partially written container.

Cook output and its deployment manifest are a consistency unit. The manifest
must include both the cooked `.dasset` and every required `.dbulk`. Packaging,
patch generation, installation, and cleanup operate from that manifest rather
than by assuming that every `.dbulk` in a directory is live.

## Load and Failure Policy

Editor loading may use a valid DDC object and rebuild from source on a safe miss.
Cooked runtime loading has no source or DDC fallback:

1. load the cooked package and resolve its logical bulk location;
2. validate the container, descriptor, platform, schema, ranges, and hash;
3. decode the complete asset-specific payload transactionally;
4. publish runtime data only after all required validation succeeds.

A missing or invalid required cooked payload is an asset-qualified hard load
failure and indicates an incomplete or corrupt installation. Optional payloads
must be explicitly marked by the asset schema; absence is not inferred as
optional merely because a file cannot be found.

Neither editor nor runtime readers trust counts, offsets, sizes, compression
ratios, enum values, or cross-record references from disk. Readers enforce
per-payload and process-appropriate allocation limits and reject integer
overflow, overlapping ranges, trailing required data, and decompression bombs.

## Versioning

The following versions are independent and change for different reasons:

- `.dasset` package format version: object-package envelope changes;
- bulk container version: descriptor table or container framing changes;
- asset payload schema version: texture, mesh, audio, or another payload layout
  changes;
- builder version: output semantics change without necessarily changing its
  readable disk layout;
- target platform/profile: output compatibility differs by runtime target.

A reader may retain deliberate backward compatibility for old package,
container, or payload schemas. A builder-version change normally creates a new
DDC key and cooked output but does not by itself require the runtime reader to
reject an otherwise supported payload schema.

## Naming Decision

`.bin` and `.dbulk` may contain identical asset-specific payload bytes, but they
are not interchangeable:

- `.bin` means a content-addressed, rebuildable cache object with no persistent
  asset reference.
- `.dbulk` means manifest-owned, deployable bulk data referenced logically by a
  cooked package and required by runtime.

The dedicated `.dbulk` suffix lets cook, staging, patching, deployment,
diagnostics, and cleanup distinguish required runtime payloads from arbitrary
binary caches without inspecting every file. A future archive may absorb loose
`.dbulk` containers; `.dbulk` is the loose cooked representation, not a promise
that distribution will always ship one operating-system file per package.

## Repository Policy

Authored `.dasset` packages and source inputs are versioned according to the
content storage policy. `DerivedDataCache`, `Cooked`, and `Saved` remain ignored
generated directories. Cooked output is nevertheless authoritative within a
specific staged build: ignored means reproducible distribution output, not
disposable while that build is running or installed.

## Current Implementation Status

Authored `.dasset` packages and project-local DDC stores exist. Texture DDC
objects already follow the `.bin`, content-addressed, versioned, checksummed,
safe-miss rules above. The shared `.dbulk` container, cooked package
descriptors, deployment manifest, and runtime-only load mode are selected
contracts but are not yet implemented.

## Related Documentation

- [Asset Packages](AssetPackages.md)
- [Texture System](../Rendering/TextureSystem.md)
- [Content Version Control](../../Development/VersionControl/ContentVersionControl.md)
- [Texture Support Plan](../../Plans/TextureSupport.md)
- [Asset Derived Data and Cooking Plan](../../Plans/AssetDerivedDataAndCooking.md)
