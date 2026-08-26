# Terrain Heightmap Asset

Summary: Defines the exact unsigned 16-bit terrain-height authority, regional extrema, source import, DDC, package, and cooked-runtime contracts.

Modules: AssetCore, Engine, AssetForge, AssetForgeBuiltins, DurinEd, LevelEditor

Last reviewed: 2026-08-25

## Asset Contract

`DTerrainHeightmap` is an Engine asset independent from `DTexture` and every
texture usage, color-space, mip, compression, renderer, and collision policy.
Its immutable payload stores one tightly packed row-major `uint16` sample plane.
`(0, 0)` is the top-left source pixel, X selects a column, and Y selects a row.
Samples are normalized unsigned values in `[0, 65535]`; the asset does not assign
world units, spacing, scale, or offset.

Consumers retain `std::shared_ptr<const FTerrainHeightmapPayload>` snapshots.
A snapshot remains valid across reimport, state exchange, duplication, and asset
destruction. Public access supports bounded sample lookup and exact half-open
regional min/max queries; mutable vectors are not exposed by the asset.
Revisions begin at one. A changed sample plane advances the revision exactly
once, while an identical same-source reimport is a semantic no-op. Moving an
identical source updates provenance without changing the sample revision.

## Source Format and Limits

Accepted sources are either a strict PNG16 image or the fixed square RAW16
profile. PNG requires a non-interlaced image with color type 0, exactly one
grayscale channel, exactly 16 bits per sample, and standard PNG compression and
filter methods. RAW requires a headerless file containing exactly `N * N`
unsigned 16-bit little-endian samples for one dimension in `2..16384`; it has
no header, trailer, sidecar, padding, signed values, or manually supplied
dimensions. A 513x513 RAW source is exactly 526,338 bytes.

Both formats preserve the first sample as top-left `(0, 0)` and rows in source
order. Decode does not flip, rotate, transpose, gamma-correct, normalize,
resample, or convert samples. PNG channel/bit-depth/interlace violations and
RAW odd, undersized, non-square, or oversized byte counts fail before payload
publication. RAW dimension admission uses checked integer arithmetic and each
sample is explicitly assembled from two little-endian bytes; host byte order
and alignment never define the source.

The frozen limits are:

| Fact | Limit |
| --- | ---: |
| Width and height | each `2..16384` |
| Canonical samples | `268,435,456` |
| Encoded source | 512 MiB |
| Canonical sample bytes | 512 MiB |
| Serialized hierarchy nodes | 512 KiB |
| Encoded heightmap payload | 513 MiB |
| Peak synchronous decode/build admission | 2,560 MiB |

The maximum canonical sample plane is 536,870,912 bytes. Its 64×64 hierarchy
contains 87,381 nodes (349,524 bytes) plus nine in-memory level records. The
maximum retained canonical payload is 537,220,652 bytes. Direct import and
reimport build inside a detached candidate. Uncooked package reload instead
stages DDC and source recovery through the CPU task system; no worker accesses a
DObject and no Texture2D scheduler is involved.

## Regional Min/Max Hierarchy

Level 0 is a row-major grid of 64×64 sample regions. Right and bottom edge
regions cover only the samples that exist. Each later level is a row-major
ceil-divide-by-two grid and combines up to four children. Levels are stored from
finest to coarsest, ending at one root. Every node contains the exact minimum
and maximum of its covered source rectangle; no averaging or sample replacement
occurs. Arbitrary regional queries combine complete level-0 nodes and scan only
partial boundary regions, returning exact extrema.

## Authored Package and DDC

The authored package retains mounted `FSourcePath` provenance, XXH3-128 source
identity, file-size/time fingerprint, source format facts, dimensions, global
range, revision, retained-byte facts, and the cooked descriptor field. Decoder
identity/version plus a source-format enum and fixed profile version distinguish
PNG16 from RAW16. It does not retain encoded source bytes or decoded proposal
storage.

DDC objects live under `TerrainHeightmap/Objects`. Version-2 keys hash the
builder identity `Durin.TerrainHeightmap.Builder.V2`, source hash, decoder ID
and version, source-format enum and profile version, unsigned 16-bit format,
top-left row-major orientation, 64-sample base region, builder and payload
versions, and target platform/profile. The version bump deliberately misses
version-1 PNG entries. A warm hit validates and restores the immutable payload
without opening source. A missing, corrupt, or incompatible object rebuilds
only when mounted source is available; otherwise PostLoad reports
`SourceUnavailable` and does not invent a flat payload.

Uncooked PostLoad publishes the reflected object graph immediately in `Loading`
and moves DDC query, object read, payload decode, source capture/decode, canonical
build, and DDC store to worker execution. Query, read, decode, capture, and
build/store timings remain separate in the bounded diagnostic. Publication is
always deferred to GameThread and revalidates the object handle and authoring
load generation. A normal publication drives the existing payload revision
context, so registered render and collision consumers rebuild through their
ordinary invalidation path.

Requests with the same derived-data key coalesce onto one worker. Admission is
bounded to two distinct retained load works and 64 subscribers, which bounds the
worst-case canonical result retention to approximately 1,026 MiB. Unload,
replacement, a newer publication, provider shutdown, or task-system shutdown
invalidates or cancels stale work. Editor rendering reports payload loading and
publishes no partial proxy. Required gameplay collision waits at its explicit
activation barrier; missing/corrupt payload recovery failure rejects play.

## Payload and Cook

The terrain owner selects `TerrainHeightmapPrimaryCookedPayloadId`, payload
schema 2, and builder 3. The value uses 16-byte section alignment, no bulk
compression, and a 96-byte
little-endian header records platform/profile, dimensions, hierarchy policy,
counts, global range, table/section offsets, stored size, and XXH64 body
checksum. Each 24-byte level record stores level dimensions, node offset,
sample-region size, and a zero reserved field. Canonical samples follow as
little-endian `uint16`; hierarchy nodes follow as little-endian min/max pairs.

Decode validates target identity, versions, ceilings, checked counts and
ranges, alignment, non-overlap, checksum, level topology, sample extrema, and a
complete independently rebuilt hierarchy before publication. Cook emits one
package companion entry transactionally and strips source-only fields. Cooked
runtime requires the descriptor and companion; it never falls back to source,
DDC, `DTexture2D`, or zero height.

## Import, Reimport, and Inspection

The Content Browser exposes an explicit **Terrain Heightmap** import action for
`.png` and `.raw`. Ordinary PNG import remains `DTexture2D`, and generic import
does not reinterpret arbitrary RAW files; the AssetForgeBuiltins heightmap
planning pass and builder are selected only for a `DTerrainHeightmap` target. The copied mounted
source preserves its admitted extension, and an explicit source destination
with a different extension is rejected. Import builds and
persists a detached candidate before package publication. Standard reimport
uses a reversible whole-state exchange, preserves object identity, rolls back
on save failure, and updates reflected `FSourcePath` provenance for the source
reference index and relocation workflow.

`TranslateTerrainHeightmapSource` is the sole encoded-PNG interpretation
authority. Direct import, provider candidates, source changes, repair, and the
independently reversible `TerrainHeightmapAuthoringPolicy` consume one immutable
source capture and pass exact owned samples to TerrainBuild. Provider and
PostLoad orchestration do not decode or hash the source themselves.

Generic reflected inspection exposes source format facts, dimensions, global
range, revision, sample/hierarchy/retained bytes, status, DDC identity, cooked
descriptor versions, and a diagnostic capped at 2,048 bytes. The asset has no
dedicated editor, rendered thumbnail, renderer resource, or collision object.

## Validation

`TerrainHeightmapTests` covers asymmetric orientation and exact samples,
non-square and odd hierarchy edges, extrema and limits, deterministic key and
payload round trips, corruption rejection, strict PNG and square RAW16
acceptance, explicit
import versus default Texture2D routing, no-op and changed reimport, rollback,
source-reference indexing, duplication/snapshot lifetime, and warm DDC reload.
`TerrainHeightmapCookTests` removes source and DDC before loading the published
cooked package and verifies exact samples.

Renderer terrain and Physics heightfield consumers may depend on this payload
and revision contract. They may not reopen source or redefine coordinates,
height normalization, hierarchy coverage, or runtime fallback behavior.
