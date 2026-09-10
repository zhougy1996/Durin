# Texture Source Lossless Compression Plan

Summary: Compress authored texture pixels with Zstd and repack repository assets without changing decoded pixels or derived-data identity.

Last reviewed: 2026-09-10

Status: Completed
Completed: 2026-09-10

## Current Status

All stages are complete on Windows. Zstd 1.5.7 is pinned to upstream commit
`f8745da6ff1ad1e7bab384bd1f9d742439278e99`, prepared through the manifest
system and linked privately as a static Engine dependency. Dependency validation
(11 manifests) and `DevTool build --target Engine` passed with the existing
Win64 Debug editor profile. The source CMake integration uses the active host
compiler/architecture and PIC; it requires no platform SDK archive or runtime
codec DLL. macOS compilation/execution is unavailable on this Windows host.

The level 3 policy is confirmed by the following read-only baseline of all eight
external texture payloads (the ninth texture is inline and will be included in
the package-level preview). Measurements use Python zstandard 0.24.0 reporting
Zstd 1.5.7, single-threaded single frames, one warm-up and median of seven runs;
these are host diagnostic timings, not a performance gate or Engine Debug timings.
Every decode was byte-compared against its input. The temporary harness and JSON
receipt are in ignored `Build/TextureCompressionQualification`.

| Payload | Input bytes | Zstd bytes | Encode ms | Decode ms |
| --- | ---: | ---: | ---: | ---: |
| TEXCUBE_PureSky_512x512 | 6291456 | 1800675 | 46.456 | 16.261 |
| TEX_StoneHead | 1048576 | 424996 | 7.575 | 2.264 |
| VT_Cloud_Base_Voronoi_128 | 2097152 | 891310 | 20.326 | 4.596 |
| vintage_lighter_diff_BaseColor | 4194304 | 1325796 | 29.968 | 9.11 |
| vintage_lighter_diff_Opacity | 4194304 | 147 | 1.367 | 1.359 |
| vintage_lighter_metal_vintage_lighter_rough_Metallic | 4194304 | 712076 | 19.219 | 8.729 |
| vintage_lighter_metal_vintage_lighter_rough_Roughness | 4194304 | 888043 | 22.216 | 8.563 |
| vintage_lighter_nor_gl_Normal | 4194304 | 1638437 | 33.047 | 9.158 |

Stage 1 validation: `DevTool test TextureTests` passed all 105 tests (receipt
`20260910-123945-256612-20196-TextureTests.log`). Added byte/floating layered
volume round trips, repeated storage no-op, rollback, retained handles, bulk
GUID/owner and source-key invariance, malformed/truncated/trailing/concatenated,
dictionary/size/window rejection, and identical serialized texture build output.
Existing source-only Cube fixtures now decode through `GetMipData`; explicit
external-companion fixtures retain Raw, while the atlas import case validates
compressed inline placement and package corruption. All explicit Raw preferences
in production Texture2D, Cube, long-lat and Volume preparation now use Zstd;
scene/import adapters feed those common preparation boundaries.

Stage 2 is complete. `DevTool test affected --base 07a6dfd38` selected 88
native targets: 87 passed; the new Cube rebuild test initially attempted to
unload its deliberately dirty package. After adding the required save to that
test, `DevTool test TextureTests` passed all 106 cases. No other target failed.
Receipts: `20260910-125507-273524-8068-ctest.log` and
`20260910-125806-272693-22428-TextureTests.log`.
Normal Sandbox and RoadWeaver project hosts enumerate 20 and 8 packages,
respectively, covering all 22 unique physical packages in Engine, Sandbox and
RoadWeaver content (6 shared Engine packages). All 9 texture sources are in
Sandbox; only their deduplicated exact package paths were applied. Standalone
`Engine.dproject` host startup failed because its enabled roots include the
launcher program; Engine coverage uses its mounts in both normal project hosts.
No authored file changed during preview.

The first real preview exposed Cube/Volume PostLoad rebuilding the source itself.
Their result-application contexts now explicitly preserve installed source during
PostLoad/rebuild; imports retain source replacement. This prevents implicit
compression and registration churn when loading ordinary assets.

The GUID acceptance boundary is in-memory registration: DAST v9 does not persist
bulk instance GUIDs and its existing adapter reconstructs them from the stored
content hash. Recompression and storage-only commits retain the live GUID;
reload checks compare semantic source identity, exact decoded hash, complete
source descriptors and build settings, not that reconstructed registration GUID.
Persisting registration GUIDs would change the chosen package contract and is
outside this plan. This boundary is now documented in Package Bulk Data.

After conversion, all 9 saved textures were loaded in fresh asset-tool processes
and passed those semantic comparisons. Their packages plus companions decreased
from **30,695,804 to 7,880,874 bytes**, saving **22,814,930 bytes (74.33%)**.
Opacity moved inline and its exact `.dbulk` was removed. A second apply skipped
all 9 textures; all 29 remaining repository package/companion files were
byte-identical by SHA-256. Sandbox and RoadWeaver audits report all 20/8 packages
compatible, ready and current. Ignored receipts and per-asset descriptors are in
`Build/TextureCompressionQualification/{before-semantics,before-files,after-files,
sandbox-preview,sandbox-apply,sandbox-reload,sandbox-repeat,roadweaver-preview,
roadweaver-reload,sandbox-audit,roadweaver-audit}.json`.

Maintenance tests cover current-package selection, cancellation, read-only
preview, serialization/publication/verification failure with source and file
rollback, inline companion removal and repeated-apply stability. Six focused
maintenance tests and 28 DevTool parser/forwarding tests passed. The full editor
`all` build passed (`20260910-124829-334795-7180-cmake.log`).

Per-texture conversion receipt (package plus companion bytes):

| Texture | Before | After | Exact decoded XXH3-128 |
| --- | ---: | ---: | --- |
| `vintage_lighter_diff_BaseColor` | 4,197,237 | 1,328,729 | `a2a5b0f70a84a1963ce049ec1d31a90d` |
| `vintage_lighter_diff_Opacity` | 4,197,329 | 3,172 | `a7fa2fde863813638bbf51c6fa494e2f` |
| `vintage_lighter_metal_vintage_lighter_rough_Metallic` | 4,197,449 | 715,221 | `29ea02cbcc6f386914bc4cfc12c90863` |
| `vintage_lighter_metal_vintage_lighter_rough_Roughness` | 4,197,453 | 891,192 | `3cc72c9163546052a100c2daa240f69f` |
| `vintage_lighter_nor_gl_Normal` | 4,197,236 | 1,641,369 | `d7e919cfc6160175c88ff948ce118662` |
| `TEXCUBE_PureSky_512x512` | 6,294,253 | 1,803,472 | `fa0b83bfc6bc74b61b4065ff87c7641f` |
| `TEX_StoneHead` | 1,051,317 | 427,737 | `893fb8249da8e173c1313405c27a69b8` |
| `VT_Cloud_Base_Voronoi_128` | 2,099,268 | 893,426 | `082ef5bec38851fbdeb74b1390e2a348` |
| `VT_Cloud_Detail_Voronoi_64` | 264,262 | 176,556 | `ccb989826d24e3a3cc6f96b944345296` |

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

- [x] Select and pin an official Zstd release, register dependency preparation,
  license handling and the private static Engine dependency using repository
  conventions. Do not depend on another library's transitive codec copy.
- [x] Verify preparation/configuration and build on the available host; review
  supported Windows and macOS target/SDK dependency propagation and record any
  host validation that is unavailable.
- [x] Measure level 3 compression size and encode/decode time on the existing
  texture payloads, including the HDR/cube and noise volume inputs. Record the
  baseline and confirm the codec policy before proceeding.

Completion: reproducible dependency integration and measured repository results.

### Stage 1: Implement source compression and bounded decoding

- [x] Add the codec, initialization default, bounded decode and transactional
  recompression API while retaining all source identity inputs.
- [x] Audit Texture2D, cube, long-lat cube, volume and scene import callers for
  explicit Raw preferences and ensure the intended default reaches each path.
- [x] Extend existing texture tests for exact pixel round trips across byte,
  floating-point, layered, multi-mip and volume sources; Raw fallback; corrupt
  input rejection; unchanged source identity and failed-operation rollback.
- [x] Verify bulk instance identity preservation, release/reload behavior and
  existing detached-buffer lifetime guarantees.
- [x] Verify the same source/build settings produce the same derived-data key
  and cooked texture content before and after recompression.

Completion: focused tests pass and storage changes are semantically invisible
to consumers. Codec errors cannot publish partially replaced source state.

### Stage 2: Add maintenance and convert all repository textures

- [x] Add the explicit maintenance option, JSON preview/result reporting and
  tests for current-package selection, cancellation and save failure handling.
- [x] Enumerate every project and enabled content mount. Include Engine,
  Sandbox and RoadWeaver; deduplicate shared physical packages. Use the normal
  project module loading path and process packages sequentially to bound memory.
- [x] Capture per-asset decoded hashes, identities, descriptors and package plus
  companion sizes. Preview, then apply recompression to every texture source.
- [x] Reload saved assets and verify captured semantic facts. Report actual
  aggregate disk savings, including inline/external payload placement changes.
- [x] Run the operation again and confirm package and companion bytes remain
  unchanged for the pinned codec and policy. Confirm recovery/cleanup respects
  packages whose payload moved inline; never delete companions by wildcard.
- [x] Update the implemented source-storage and maintenance documentation,
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
