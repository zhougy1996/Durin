# Terrain RAW16 Heightmap Import Plan

Summary: Add exact Gaea/Unity-style square little-endian unsigned RAW16 sources to the existing Terrain Heightmap import, reimport, DDC, and Cook lifecycle without adding a third-party decoder.

Last reviewed: 2026-08-14

Status: Archived
Completed: 2026-08-14

## Current Status

Completed on 2026-08-14. `DTerrainHeightmap` accepts strict PNG16 and the fixed
square U16LE `.raw` profile through one source-format-neutral build,
publication, reimport, DDC, Cook, renderer, and collision lifecycle. The RAW
decoder uses bounded file admission, checked integer square inference, and
explicit little-endian assembly without a third-party decoder. Persisted
provenance and version-2 DDC keys carry decoder, format, and profile identity.

Evidence: the generated asymmetric 513x513 fixture is exactly 526,338 bytes
and records the Gaea 2.3 Unity RAW top-left row-major U16LE oracle. Focused
`TerrainHeightmapTests` (9/9), `TerrainHeightmapCookTests` (1/1), the `@terrain`
domain, the ordinary native `test all` aggregate, Editor and Game `all` builds,
and hidden-window three-tick smoke runs for both variants passed. Changed
documentation validation passed; final all-plan validation accompanies
completion.

## Goal

Import and reimport a Gaea/Unity-style square U16LE `.raw` file as the same
exact canonical `DTerrainHeightmap` payload produced by PNG16, while preserving
transactional publication, source provenance, DDC behavior, Cooked Runtime
loading, editor usability, and existing renderer and collision consumers.

For the initial qualified profile:

- the file contains no header and no trailing bytes;
- every sample is an unsigned 16-bit little-endian value;
- width equals height and is inferred exactly from `file_size / 2`;
- dimensions remain within the existing `2..16384` asset limits;
- the first sample is canonical `(0, 0)` and rows remain top-to-bottom without
  flip, transpose, normalization, gamma conversion, or resampling;
- a 513x513 source is exactly 526,338 bytes and publishes 263,169 exact samples.

## Scope

- A bounded square U16LE RAW decoder implemented with repository and standard
  C++ facilities only.
- Extension-based PNG16 versus RAW16 source dispatch in the existing Terrain
  Heightmap translator.
- Decoder identity/version and source-format participation in deterministic
  DDC keys and persisted source provenance.
- Mounted-source copy, import, reimport, relocation, rollback, semantic no-op,
  and warm-DDC behavior for `.raw` sources.
- The existing Terrain Heightmap dialog with `.png` and `.raw` selection,
  format-aware validation, and concise source-contract guidance.
- Focused structural, malformed-input, lifecycle, DDC, Cook, editor-routing,
  renderer-consumer, and collision-consumer validation.
- Updates to the lasting Terrain Heightmap asset and editor workflow contracts.

## Non-Goals

- Signed, floating-point, big-endian, 8-bit, 24-bit, 32-bit, interleaved, or
  header-bearing RAW variants.
- Rectangular RAW, manually entered dimensions, import presets, or arbitrary
  per-file byte-order and row-orientation controls.
- Consuming Gaea `definition.json` scale, height, or unit metadata; Terrain
  Component spacing and height scale remain independently authored.
- Automatically resizing 512 to 513, 1024 to 1025, or otherwise changing the
  authored sample plane.
- Changing PNG decoding, canonical payload layout, hierarchy policy, DDC
  storage format, THPL schema, renderer height upload, collision geometry, or
  the 1025x1025 render/collision ceiling.
- Making RAW the only or preferred source; strict PNG16 remains supported.
- Terrain draw batching, streaming, sculpting, or other rendering scalability
  work owned by the separate draw-submission plan.

## Design Decisions and Invariants

### RAW decoding is small, explicit, and dependency-free

- The translator admits `.raw` case-insensitively and reads through the same
  bounded mounted-source file path used by PNG.
- File size must be even, at least eight bytes, no greater than the existing
  encoded-source ceiling, and equal to `2 * N * N` for one integer `N` in the
  existing dimension limits. Checked division, integer square-root validation,
  and checked multiplication prove the shape; floating-point inference is not
  authoritative.
- Each sample is assembled from two bytes as little-endian `uint16`. Host
  endianness, alignment, and `reinterpret_cast<uint16*>` never define source
  meaning. Decoding allocates the final sample vector once after admission.
- The initial contract is no-flip top-left row-major. An asymmetric golden
  fixture freezes corners and interior samples. If a real Gaea 2.3 oracle
  disproves that orientation, Stage 0 changes the selected fixed decoder
  contract and its identity before publication code lands; production does not
  guess orientation from terrain content.

### Canonical payload and consumers remain source-format neutral

- PNG16 and RAW16 decoders both produce width, height, and owned exact
  `uint16` samples for the existing `BuildTerrainHeightmap` entry point.
- Hierarchy construction, extrema, revisioning, snapshots, renderer uploads,
  collision publication, package serialization, and THPL Cook remain unchanged.
- Identical canonical samples from different source encodings may be semantic
  sample no-ops on reimport, while source provenance still updates according to
  the existing moved-source rules.

### Source identity is explicit across reload and DDC

- Persisted provenance distinguishes the PNG and RAW decoder IDs and versions.
  Inspection reports the selected source format without inferring it from a
  relocated filename after publication.
- The DDC builder contract includes decoder identity/version and the fixed RAW
  format/orientation profile in addition to encoded source hash and existing
  target facts. Stage 1 bumps the builder/key version deliberately; old PNG DDC
  entries miss and rebuild rather than being interpreted under a new key ABI.
- Width and height are validated again when restoring DDC and THPL through the
  existing canonical payload validators. RAW source bytes and sidecars are
  never stored inside authored packages or cooked companions.

### Existing import lifecycle is extended, not forked

- Source destination defaults preserve the selected `.png` or `.raw`
  extension under `TerrainHeightmaps`; an explicitly requested destination
  must use the same admitted extension as its source.
- Initial import builds a detached candidate before package publication.
  Reimport, relocation, save rollback, source reference indexing, and loaded
  consumer recreation reuse their existing paths and failure atomicity.
- The explicit Terrain Heightmap action accepts PNG16 and RAW16. Ordinary image
  import continues routing PNG to `DTexture2D`; generic asset import does not
  reinterpret every `.raw` file as Terrain without an explicit Terrain target.

### Diagnostics are deterministic and actionable

- RAW rejection distinguishes odd byte count, too small/large source,
  non-square sample count, dimension limit, read failure, and checked-arithmetic
  failure without publishing a partial asset.
- UI text states the exact supported RAW profile and does not imply support for
  arbitrary RAW files. Diagnostics remain within the existing 2,048-byte asset
  bound.

## Current Foundations and Gaps

| Area | Existing foundation | Gap this plan owns |
| --- | --- | --- |
| Source translation | Mounted PNG source copy, bounded read, strict grayscale16 decode, detached build and rollback | Extension-preserving destination and exact square U16LE decode/dispatch |
| Canonical build | Source-neutral `uint16` sample request, hierarchy, immutable payload, DDC publication | Decoder/profile identity in the build key and publication context |
| Asset lifecycle | Provenance, source hash, reimport, relocation, semantic no-op, snapshot lifetime | RAW decoder facts and RAW source-reference coverage |
| Editor | Explicit Terrain Heightmap dialog and destination validation | RAW filter, wording, extension validation, and focused UI tests |
| Cook/runtime | Source-free THPL companion reconstructed into the canonical payload | Prove a RAW-authored asset follows the unchanged Cook/runtime path |
| Consumers | Renderer and collision consume only canonical payload/revision | Regression proof; no consumer implementation change expected |
| Dependencies | Standard file loading, hashing, checked build limits, archive endian helpers | A small decoder function; no third-party package or CMake dependency |

## Implementation Stages

### Stage 0: Freeze the RAW profile and golden fixtures

- [x] Add a deterministic asymmetric square RAW fixture with known corners,
  interior samples, byte order, row order, dimensions, and exact byte count.
- [x] Compare the selected top-left row-major contract with one Gaea 2.3 Unity
  RAW oracle and record the result in the fixture generator or test provenance.
- [x] Freeze decoder ID, decoder version, source-format enum/profile, and exact
  malformed-input diagnostics before changing persisted provenance or DDC keys.
- [x] Add fixture generation/checking that writes explicit bytes rather than
  depending on host endianness.

#### Acceptance Gate

- The golden fixture independently proves sample order and U16LE byte meaning.
- Exact accepted and rejected RAW profiles are unambiguous; no production
  orientation or dimension heuristic remains unresolved.

### Stage 1: Add bounded RAW decoding and canonical build integration

- [x] Add a private or StandardAssetImport-owned RAW decode result parallel to
  the existing grayscale16 PNG result without introducing a public generic
  image-decoder dependency.
- [x] Implement checked square-dimension admission, one-allocation decoding,
  and explicit little-endian sample assembly under existing source/sample
  ceilings.
- [x] Refactor Terrain source translation to dispatch `.png` and `.raw` into
  the same `BuildTerrainHeightmap` and publication path.
- [x] Preserve the source extension in default and explicit mounted-source
  destinations and reject extension mismatches transactionally.
- [x] Persist decoder identity/version and add decoder/profile facts to the DDC
  key ABI with an intentional builder/key version bump.
- [x] Cover exact samples, extrema, retained-byte facts, DDC miss/build/warm
  hit, and every malformed RAW admission failure.

#### Acceptance Gate

- PNG fixtures retain exact existing behavior.
- Valid square U16LE RAW publishes the expected canonical payload; malformed,
  oversized, ambiguous, and truncated files publish nothing.
- PNG and RAW DDC keys are deterministic, format-aware, target-aware, and old
  keys cannot be mistaken for the new ABI.

### Stage 2: Extend import, reimport, relocation, and editor workflow

- [x] Extend `FTerrainHeightmapImportSettings` only if the frozen fixed profile
  requires an explicit source-format field; keep the settings payload empty if
  extension dispatch is sufficient.
- [x] Update the explicit import handler and Terrain dialog to accept `.png`
  and `.raw`, preserve the chosen extension, and present format-specific help.
- [x] Keep ordinary PNG-to-Texture2D routing unchanged and require an explicit
  Terrain Heightmap target for RAW.
- [x] Add RAW initial import, identical reimport, changed reimport, moved-source
  reference, package-save rollback, missing source, corrupt source, and loaded
  renderer/collision consumer revision tests.
- [x] Expose persisted decoder/source-format facts through existing reflected
  inspection without adding a dedicated asset editor.

#### Acceptance Gate

- A user can select a Gaea `.raw`, create one Terrain Heightmap asset, reimport
  it, relocate its mounted source, and receive precise validation failures.
- All operations preserve existing transaction, revision, package, source
  index, and consumer recreation contracts.

### Stage 3: Qualify Cooked Runtime and complete lasting documentation

- [x] Cook one RAW-authored heightmap, remove source and DDC, load the cooked
  package in Game, and verify exact dimensions, samples, hierarchy, revision,
  renderer proxy creation, and collision publication.
- [x] Prove THPL bytes/schema are unchanged for equal canonical payload and
  target facts regardless of whether the authoring source was PNG or RAW.
- [x] Update the Terrain Heightmap asset contract and Terrain workflow guide
  with the supported RAW profile, no-third-party design, limitations, and
  source-format-neutral Cook behavior.
- [x] Run focused Terrain import/Cook tests, Terrain domain tests, documentation
  validation, the ordinary native aggregate, required Editor/Game builds, and
  hidden-window smoke validation according to repository build/test guidance.

#### Acceptance Gate

- RAW-authored Cooked Runtime needs neither source, sidecar, nor DDC and is
  indistinguishable to renderer/collision consumers from an equivalent PNG
  canonical payload.
- Lasting contracts own the implemented behavior, all required validation
  passes, and the plan records evidence sufficient for completion.

## Validation Matrix

| Concern | Required evidence |
| --- | --- |
| Exact decode | Asymmetric U16LE fixture proves corners, rows, interior values, extrema, and 513x513 byte count |
| Admission | Odd, empty, undersized, non-square, over-limit, truncated/read-failed, wrong extension, and extension-mismatch cases fail atomically |
| PNG regression | Existing strict grayscale16 PNG acceptance/rejection and Texture2D routing remain unchanged |
| DDC | Decoder/profile-aware deterministic keys, intentional old-key miss, cold build, warm source-free hit, corruption rejection |
| Lifecycle | Initial import, no-op/changed reimport, relocation, source index, duplication, snapshot lifetime, and save rollback |
| UI | PNG/RAW filters, format-specific text, destination extension, missing/stale source, explicit Terrain routing |
| Cook | RAW-authored source/DDC-free THPL load with exact payload and unchanged schema |
| Consumers | Render proxy/upload and HeightField collision consume exact RAW-authored revision without source access |
| Limits | Existing encoded, sample, hierarchy, retained, and synchronous peak ceilings remain enforced |
| Build/lifecycle | Focused targets, native aggregate, Editor/Game builds, smoke, shutdown, and documentation validators pass |

## Definition of Done

- `.png` and qualified square `.raw` sources import and reimport through one
  Terrain Heightmap lifecycle with exact documented samples.
- RAW decoding uses no third-party library and has no host-endian, alignment,
  floating-point dimension, gamma, normalization, resampling, or implicit-flip
  dependency.
- Persisted provenance and DDC keys identify the decoder/profile; old keys
  rebuild safely and warm new keys restore without source.
- UI, rollback, relocation, Cooked Runtime, renderer, and collision contracts
  are qualified without parallel RAW-only asset or consumer paths.
- Long-lived behavior is documented in the Terrain Heightmap asset and Terrain
  workflow contracts; all plan checklists and acceptance gates have evidence.

## Deferred Follow-ups

- Optional Gaea `definition.json` validation and deliberate import of terrain
  physical scale into separately authored Component settings.
- Rectangular RAW with an explicit sidecar or import-settings UI.
- Named byte-order/orientation presets and user-controlled vertical flip.
- Other established raw extensions such as `.r16` after their routing and
  collision with generic import workflows are specified.
- Import-time power-of-two to power-of-two-plus-one resampling; this requires a
  separate authored-data and world-extent policy.

## Related Documentation

- [Terrain Heightmap Asset](../../../Runtime/Terrain/TerrainHeightmapAsset.md)
- [Terrain Workflow](../../../Editor/Guides/TerrainWorkflow.md)
- [Terrain Rendering](../../../Runtime/Rendering/TerrainRendering.md)
- [Runtime Collision](../../../Runtime/Physics/Collision.md)
- [Agent Build and Run Workflow](../../../Agents/BuildAndRun.md)
- [Agent Testing Workflow](../../../Agents/Testing.md)
- [Terrain Draw Submission Scalability](TerrainDrawSubmissionScalability.md)

## Related Code

- `Engine/Source/Runtime/Engine/Public/Terrain/TerrainHeightmap.h`
- `Engine/Source/Editor/StandardAssetImport/Public/TerrainHeightmapSourceTranslation.h`
- `Engine/Source/Editor/StandardAssetImport/Private/TerrainHeightmapSourceTranslation.cpp`
- `Engine/Source/Editor/StandardAssetImport/Private/StandardAssetImportProviders.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Assets/TerrainHeightmapImportDialog.h`
- `Engine/Source/Editor/LevelEditor/Private/Assets/TerrainHeightmapImportDialog.cpp`
- `Engine/Source/Developer/GeometryBuild/Public/Terrain/TerrainHeightmapBuildKey.h`
- `Engine/Source/Developer/GeometryBuild/Public/Terrain/TerrainHeightmapBuildOperations.h`
- `Engine/Source/Developer/GeometryBuild/Private/Terrain/TerrainHeightmapDerivedData.cpp`
- `Engine/Tests/Native/EngineTests/Private/Terrain/TerrainHeightmapTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/Terrain/TerrainHeightmapCookTests.cpp`
