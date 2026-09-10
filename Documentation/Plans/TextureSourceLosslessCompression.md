# Texture Source Lossless Compression Plan

Summary: Compress authored texture pixels with Zstd and repack repository assets without changing decoded pixels or derived-data identity.

Last reviewed: 2026-09-10

Status: Active
Completed:

## Current Status

Implementation has not started. Repository inspection found Raw and byte-run
RunLength codecs in `FTextureSource`, but no registered Zstd dependency.
`GetMipData` already centralizes decoding and canonical pixel-hash validation.
`GetIdentity` excludes storage compression, and existing tests assert that Raw
and RunLength representations have equal source identities. These boundaries
should be retained.

The preceding asset cleanup resaved all repository content, but did not compress
source pixels. The VintageLighter BaseColor companion contains 4,194,304 bytes.
The repository currently contains nine texture assets across Sandbox texture,
model and volume content. Implementation must rediscover all mounted projects;
this count is a baseline, not a selection filter.

This work spans dependency preparation, persistent codec validation, import
defaults, asset-maintenance transactions and repository conversion. It is
recorded as a staged plan under the user's explicit fallback authorization.

## Goal

Reduce authored texture storage while preserving exact decoded pixels, texture
descriptors, asset paths, references, source identities and build settings.
Original external image files must not be required for conversion.

## Selected Design

- Own compression in `FTextureSource`. Do not add a generic package compression
  layer or change DAST v9. BulkData continues to store opaque bytes; its content
  digest describes stored bytes, while the source canonical hash describes
  decoded pixels. Clarify this distinction in the owning documentation.
- Add an explicit Zstd codec value without renumbering Raw or RunLength. Keep
  both existing codecs as valid current storage choices. Do not restore retired
  TextureSource schemas or migration fields. A new codec is unreadable by older
  executables; update tools before converting assets.
- Pin a Zstd release and source revision through the existing dependency
  preparation system. Link it privately and statically to Engine; expose no
  Zstd types through public engine headers. Preserve its license notices.
- Start with Zstd level 3, a single frame, no dictionary and no worker threads.
  Use it as the default preference for new source initialization. Fall back to
  Raw when successful compression does not reduce stored bytes. Report codec
  errors without replacing the old source.
- Bound decoded size and decoder memory by the existing source-size limit.
  Reject unsupported codecs, malformed or truncated frames, trailing bytes,
  concatenated frames, dictionaries, inconsistent lengths and hash mismatches.
  Validate descriptors and sizes before expensive allocation or compression.
- Add an explicit storage-recompression operation which builds a candidate,
  verifies decoded equality and commits only on success. Preserve the existing
  bulk instance GUID, owner and source identity. Invalidate only replaced
  source residency; previously acquired byte buffers remain valid.
- Perform compression on detached source data in existing import/maintenance
  work, respecting existing thread ownership. Do not recompress in PostLoad,
  rendering, ordinary save or every pixel read. Read-only audits never mutate.
- Expose an explicit texture-source recompression option through DevTool asset
  maintenance with preview and apply behavior. An all-assets selection for this
  operation must include compatible current packages, not just packages with
  reflection resave recommendations. Reuse package publication and recovery.
- Keep GPU formats, mip generation, Cook outputs and DDC key construction out
  of scope. Source storage compression must not change their semantic inputs.

## Implementation Stages

### Stage 0: Register and qualify the codec dependency

- [ ] Select and pin an official Zstd release, register dependency preparation,
  license handling and the private static Engine dependency using repository
  conventions. Do not depend on another library's transitive codec copy.
- [ ] Verify preparation/configuration and build on the available host; review
  supported Windows and macOS target/SDK dependency propagation and record any
  host validation that is unavailable.
- [ ] Measure level 3 compression size and encode/decode time on the existing
  texture payloads, including the HDR/cube and noise volume inputs. Record the
  baseline and confirm the codec policy before proceeding.

Completion: reproducible dependency integration and measured repository results.

### Stage 1: Implement source compression and bounded decoding

- [ ] Add the codec, initialization default, bounded decode and transactional
  recompression API while retaining all source identity inputs.
- [ ] Audit Texture2D, cube, long-lat cube, volume and scene import callers for
  explicit Raw preferences and ensure the intended default reaches each path.
- [ ] Extend existing texture tests for exact pixel round trips across byte,
  floating-point, layered, multi-mip and volume sources; Raw fallback; corrupt
  input rejection; unchanged source identity and failed-operation rollback.
- [ ] Verify bulk instance identity preservation, release/reload behavior and
  existing detached-buffer lifetime guarantees.
- [ ] Verify the same source/build settings produce the same derived-data key
  and cooked texture content before and after recompression.

Completion: focused tests pass and storage changes are semantically invisible
to consumers. Codec errors cannot publish partially replaced source state.

### Stage 2: Add maintenance and convert all repository textures

- [ ] Add the explicit maintenance option, JSON preview/result reporting and
  tests for current-package selection, cancellation and save failure handling.
- [ ] Enumerate every project and enabled content mount. Include Engine,
  Sandbox and RoadWeaver; deduplicate shared physical packages. Use the normal
  project module loading path and process packages sequentially to bound memory.
- [ ] Capture per-asset decoded hashes, identities, descriptors and package plus
  companion sizes. Preview, then apply recompression to every texture source.
- [ ] Reload saved assets and verify captured semantic facts. Report actual
  aggregate disk savings, including inline/external payload placement changes.
- [ ] Run the operation again and confirm package and companion bytes remain
  unchanged for the pinned codec and policy. Confirm recovery/cleanup respects
  packages whose payload moved inline; never delete companions by wildcard.
- [ ] Update the implemented source-storage and maintenance documentation,
  run affected tests and the required full editor build through DevTool, and
  audit each project's content. Commit code and converted assets with evidence.

Completion: all discovered repository textures are processed, exact decoded
content is preserved, repeat application is stable and validation passes.

## Related Code and Guidance

- `Engine/Source/Runtime/Engine/Public/Texture/TextureSource.h`
- `Engine/Source/Runtime/Engine/Private/Texture/TextureSource.cpp`
- `Engine/Source/Runtime/Engine/Private/Asset/EditorBulkData.cpp`
- `Engine/Source/Editor/AssetForgeBuiltins/Private/Texture2DImport.cpp`
- `Engine/Source/Programs/DurinAssetTool/Private/AssetToolMain.cpp`
- `Engine/Tests/Native/EngineTests/Private/Texture/TextureBuildTests.cpp`
- `Engine/CMake/ThirdParty/CMakeLists.txt`
- `Tools/DurinDevTool/durin_dev_tool/bootstrap/manifests.py`
- [Asset data lifecycle](../Runtime/Assets/AssetDataLifecycle.md)
- [Package bulk data](../Runtime/Assets/BulkData.md)
- [Canonical resave](../Editor/Guides/CanonicalResave.md)
- [Build instructions](../Agents/BuildAndRun.md)
- [Test instructions](../Agents/Testing.md)
