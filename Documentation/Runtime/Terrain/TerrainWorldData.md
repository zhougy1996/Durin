# Terrain World Data

Summary: Define the canonical Terrain World lattice, tile products, packages, budgets, and spatial-interest boundary.

Modules: Engine, TerrainBuild, DerivedDataCache

Last reviewed: 2026-08-26

This contract is authoritative for the new Terrain World family. The current
`DTerrainHeightmap`, `DTerrainComponent`, `ATerrainActor`, `FTerrainSceneProxy`,
`TerrainHeightmap` derived-data values, and their serialized packages are a
different, unsupported family. No reader, redirector, converter, or fallback
may interpret those values as Terrain World data.

## Qualified product profiles

All dimensions name cells; sample dimensions are one larger on each inclusive
edge. Storage figures are product targets, not permission to exceed the schema
ceilings later in this document. Latency is measured from an admitted request
until the required immutable generation is available to its consumer on a
Win64 desktop with NVMe storage, 8 physical CPU cores, 16 GiB system memory,
and a Vulkan GPU with 8 GiB device memory.

| Profile | Cells / spacing / footprint | Height envelope | Logical layers | Authored / cooked target | Tiles / packages |
| --- | --- | --- | ---: | ---: | ---: |
| F0 finite qualification | 4,096² at 1 m; 4.096 × 4.096 km | -2,048 to +4,096 m; 0.25 m | 8 | 64 MiB / 192 MiB | 256 / 5 |
| F1 continuous traversal | 16,384² at 2 m; 32.768 × 32.768 km | -4,096 to +6,144 m; 0.25 m | 16 | 2 GiB / 3 GiB | 4,096 / 65 |
| F2 teleport and recovery | 65,536² at 2 m; 131.072 × 131.072 km | -4,096 to +6,144 m; 0.25 m | 32 | 24 GiB / 48 GiB | 65,536 / 1,025 |
| F3 large open world | 131,072² at 4 m; 524.288 × 524.288 km | -4,096 to +6,144 m; 0.25 m | 64 | 96 GiB / 192 GiB | 262,144 / 4,097 |

The package count is one world manifest plus one 8×8-tile region package per
occupied region. Sparse worlds omit empty regions. F2 is the required teleport
case; F3 is a scale and storage qualification case and does not imply that its
entire authored or cooked corpus is installed at once.

| Profile | Peak build memory | Metadata | Height | Surface | Collision | Query | Initial / traversal / teleport |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| F0 | 2 GiB total; 512 MiB/task | 8 MiB | 96 MiB | 192 MiB | 64 MiB | 32 MiB | 1,000 / 50 / 1,500 ms |
| F1 | 4 GiB total; 768 MiB/task | 64 MiB | 384 MiB | 768 MiB | 256 MiB | 128 MiB | 1,500 / 75 / 2,000 ms |
| F2 | 4 GiB total; 768 MiB/task | 128 MiB | 512 MiB | 1,024 MiB | 384 MiB | 192 MiB | 1,500 / 75 / 2,000 ms |
| F3 | 4 GiB total; 768 MiB/task | 256 MiB | 768 MiB | 1,536 MiB | 512 MiB | 256 MiB | 2,000 / 100 / 3,000 ms |

Resident figures are hard upper bounds per Terrain World. `Surface` covers
coverage/cache inputs, not canonical height. Initial activation requires the
minimum complete render and collision set around the initial player. Traversal
is p95 request-to-ready while moving at 40 m/s without a direction reversal;
teleport is p95 replacement of the prior working set after a 50 km discontinuity.
Admission must reject a profile whose configured budgets are lower than its
minimum complete set or higher than platform limits.

## Tile selection

The schema-1 tile dimension is 256 cells and 257×257 stored samples. It is a
versioned world property whose only schema-1 value is 256; a future dimension
requires a new tile-scheme version and rebuilds every product identity.

| Cells | Repeated-border overhead | Tile/request count | 32-cell patches | Relative rebuild and collision work | Decision |
| ---: | ---: | ---: | ---: | ---: | --- |
| 128 | 1.5686% | 4× | 4×4 | 0.25× | rejected: request/package pressure |
| 256 | 0.7828% | 1× | 8×8 | 1× | selected balance |
| 512 | 0.3910% | 0.25× | 16×16 | 4× | rejected: rebuild and safety-working-set amplification |

The overhead is `(N + 1)² / N² - 1`. The comparison holds footprint constant;
package grouping is independent and therefore does not use larger tiles to
reduce file-open count.

## Canonical world and coordinate values

- `FTerrainWorldId` is a nonzero 128-bit UUID. Its RFC 4122 byte sequence is
  identity and comparison order; string spelling and package path are not.
- `FTerrainGlobalSample` is signed `int64 X, Y`. A definition limits each axis
  to an inclusive sample extent with `Max > Min` and no more than 2³¹ cells.
- `FTerrainTileKey` is `(WorldId, int64 TileX, int64 TileY, SchemeVersion)`.
  World ID then signed X then signed Y then version define comparison order.
- `FTerrainLocalSample` uses unsigned `uint16 X, Y` in `[0, 256]` only while
  addressing a tile payload. A cell-local coordinate is in `[0, 255]`.
- `OriginMeters` is a finite `float64 X, Y, Z`; `SampleSpacingMeters` is one
  finite `float64` in `[0.01, 4096]`; `HeightDatumMeters` is finite `float64`;
  `HeightQuantumMeters` is schema-1 constant `0.25`.
- Canonical height is signed `int32` but schema 1 admits only
  `[-32768, 32767]` quanta. The tile encoding is little-endian signed `int16`.
  World Z is `Origin.Z + HeightDatum + Height * HeightQuantumMeters`.
- World X/Y are `Origin.X/Y + GlobalSample.X/Y * SampleSpacingMeters`, evaluated
  with checked integer-to-double conversion. Each result must be finite and the
  absolute lattice contribution must not exceed 2⁴⁰ m. Render code cancels a
  double-precision presentation origin before narrowing to float.

All integers in canonical byte encodings are little-endian two's-complement.
Floating values encode their IEEE-754 binary64 bits after rejecting NaN,
infinity, negative zero for positive-only fields, and noncanonical NaN payloads.
Counts precede arrays as `uint32`; byte lengths are `uint64`. Reserved fields
must be zero. Readers reject unknown required flags, trailing bytes, duplicate
map keys, noncanonical order, invalid UTF-8, and any arithmetic overflow before
allocating or publishing.

### Division, extents, and boundaries

`floor_div(a, 256)` rounds toward negative infinity and
`floor_mod(a, 256) = a - 256 * floor_div(a, 256)` is always `[0, 255]`.
For an ordinary sample, canonical lookup uses that tile and local coordinate.
A tile `(tx, ty)` stores the inclusive rectangle
`[tx*256, (tx+1)*256] × [ty*256, (ty+1)*256]`; its east/north edge therefore
repeats the west/south edge of its neighbor.

A world sample extent is inclusive `[MinSample, MaxSample]`; its cell extent is
half-open `[MinSample, MaxSample)`. At the maximum world sample only, lookup
selects the tile containing the preceding cell and local coordinate 256. Empty
extents, one-sample extents, a cell outside the half-open extent, multiplication
overflow, and a tile rectangle that does not intersect the world fail.

Global +X is east/right, +Y is north/forward, and +Z is up. Payload rows advance
+Y and samples within a row advance +X. Cell `(x,y)` uses counter-clockwise
winding viewed from +Z: `(00,10,11)` and `(00,11,01)`. Imported image origins
must be normalized to this convention before the build input exists.

## Border ownership and dependency rules

Global sample identity is the sole authority. Repeated payload borders are
copies with no independent authorship. Every pair of present neighbors must
decode a bit-identical shared edge; all four participants at a shared corner
must decode the same value. A mismatch is `BorderMismatch`, never averaging,
resampling, a skirt, or a warning-only condition.

Neighbor order in identity and diagnostics is `SW, S, SE, W, E, NW, N, NE`.
A normalized input may carry a one-sample halo outside its 257² stored rectangle.
Missing halo is legal only at the world extent and uses clamped canonical edge
samples. Builders declare the following exact consumption:

| Product or calculation | Stored interior/edges | Halo | Neighbor invalidation |
| --- | --- | --- | --- |
| Canonical height | 257², including repeated edges/corners | none | changed shared edge invalidates both tiles |
| Metadata bounds and geometric error | 257² | one sample for conservative derivatives | eight neighbors |
| Surface coverage and future normals | 257² | one sample | eight neighbors |
| Collision | 257² source; builder may downsample deterministically | none | shared-edge neighbor only |
| Query | 257² plus min/max hierarchy | one sample for normals | eight neighbors |

Patch and LOD identities are derived from global cell rectangles. A patch may
not own or renumber border samples. Coverage uses the same repeated-edge rule;
palette indices may differ between tiles, but stable layer IDs and decoded
weights at an edge must match.

### Golden vectors

T1 codecs and builders must encode these vectors as executable fixtures:

| Global coordinate | Tile | Local |
| ---: | ---: | ---: |
| -257 | -2 | 255 |
| -256 | -1 | 0 |
| -1 | -1 | 255 |
| 0 | 0 | 0 |
| 255 | 0 | 255 |
| 256 | 1 | 0 |
| 257 | 1 | 1 |

The asymmetric seam fixture uses tile `(-2, 3)`, whose inclusive sample X/Y
ranges are `[-512,-256]` and `[768,1024]`, and corner heights
`SW=-32768, SE=-17, NW=23, NE=32767`. East, north, and northeast fixtures repeat
the appropriate complete edge/corner. A non-square extent uses
`Min=(-513,769), Max=(259,1282)`. Additional required rejects are
`Min==Max`, 2³¹+1 cells, `INT64_MIN / -1`, tile multiplication overflow,
height -32769/+32768, zero/NaN spacing, infinite origin, and a world-position
contribution above 2⁴⁰ m.

## Authored definition and normalized input

The authored `DTerrainWorldDefinition` schema contains only:

- world ID, coordinate values, inclusive sample extent, tile-scheme version,
  and selected profile/budgets;
- up to 64 stable nonzero 128-bit layer IDs with unique display names and
  physical-surface references;
- up to 1,024 canonically ordered composition sources, each with stable ID,
  source reference/content hash, affected global rectangle, blend operation,
  integer strength, enabled state, and a required Height/Coverage domain mask;
- build-policy ID/version, target platform/profile, and package-region policy.

The package is capped at 4 MiB and 16 levels of composition nesting. Paths are
references, never product identity. Local source formats, editor previews, DDC
locations, cooked locations, cache hits, timestamps, and workstation paths are
excluded.

Schema-1 composition uses wire values `Replace=1`, `Add=2`, `Minimum=3`, and
`Maximum=4`; other values reject. Strength is an integer in `[0,255]`. Height
Replace evaluates `(current*(255-strength) + source*strength) / 255`; Add adds
`source*strength/255`; Minimum and Maximum compare against that scaled source.
Signed division rounds to nearest with half values away from zero, and a result
outside the signed schema-1 height envelope is `Overflow`. Sources execute in
canonical source-ID order and affect only their inclusive clipped rectangle.
Schema-1 logical coverage uses full-strength ordered Replace; other coverage
blend/strength pairs reject until a later schema freezes component-wise weight
composition. Decoded source contributions must match the authored source ID,
content hash, and 257² tile sample count before composition.
The domain-mask wire bits are `Height=1` and `Coverage=2`; zero and unknown bits
reject. Product build keys include only sources in the product's declared
domain. Metadata and Collision consume Height, Coverage consumes Coverage, and
Query consumes both.

One immutable `FTerrainNormalizedTileInput` owns copied values for the tile key,
clipped source rectangles, canonical signed height quanta, decoded layer IDs and
weights, one-sample halo, ordered neighbor evidence, composition policy,
builder/schema/platform IDs, and a cancellation token. At most 64 sources and
16 logical layers may overlap one tile; at most four layers have nonzero weight
at one sample and their `uint8` weights sum exactly 255. Workers never inspect
reflected objects or read source files.

The implemented normalized value carries separate 259² Height and Coverage
halos. A completely omitted halo is legal only when the tile contains the
whole world extent; otherwise both arrays are required. Query edge differences
and Metadata conservative geometric error use Height halo samples, while
Coverage and Query keys consume Coverage halo values and ordered neighbor
evidence.

## Tile products and generation

Each value uses the same four-byte `TWPD` Terrain World Product Data magic,
followed by `uint16` schema version, `uint16 ProductClass`, required/optional
flags, world and tile identity, generation ID, logical/stored sizes, and
XXH3-128 of the exact canonical body. Schema-1 products are:

| Product | Class | Per-tile ceiling | Required dependencies | Authority |
| --- | ---: | ---: | --- | --- |
| Metadata | 1 | 16 KiB | height, neighbor evidence | bounds, extrema, geometric error, product directory |
| Height | 2 | 160 KiB | normalized height input | canonical 257² signed quanta |
| Coverage | 3 | 320 KiB | layer library, coverage input, neighbors | stable logical weights, not shader output |
| Collision | 4 | 96 KiB | exact height identity, policy | rebuildable physics value |
| Query | 5 | 160 KiB | height, coverage, neighbors | height/normal/layer/min-max query value |

The aggregate maximum is 752 KiB per tile. A region package is capped at
64 MiB stored and 256 MiB logical bytes; an individual product is independently
addressable and checksummed. Counts are capped at 64 products per product class
per region, five known required classes, eight neighbors, 64 dependencies per
product, and 1,024 manifest regions per lookup page.

Build functions may compute and cache products independently. Publication of a
new tile generation is atomic: its manifest record becomes visible only after
all five required products validate, every present neighbor border agrees, and
all hashes and dependency identities are fixed. Failure retains the prior
complete generation. Runtime residency is independent by product class, but a
single consumer handle cannot mix generation IDs.

The deterministic build key is XXH3-128 over a tagged canonical encoding of
world/tile/scheme, normalized source content and composition, rectangle/halo,
layer identities and weights, ordered neighbor evidence, builder and product
schema versions, target platform/profile, and output policy. Scheduling order,
worker count, cache path, package placement, and cancellation timing are absent.

### Implemented schema-1 product layout

`TerrainBuild` implements every schema-1 product as one canonical envelope.
The fixed 108-byte prefix is followed by zero to 64 dependency hashes and then
the canonical body. Fields occur in this order:

| Field | Encoding |
| --- | --- |
| magic, schema, product class | `TWPD` `uint32`, `uint16`, `uint16` value 1-5 |
| required, optional flags | `uint32` value 1, zero `uint32` |
| tile identity | RFC 4122 world UUID, signed `int64 X/Y`, `uint16` scheme |
| tile-reserved, generation | zero `uint16`, RFC 4122 generation UUID |
| logical and stored body bytes | two `uint64`; equal in schema 1 |
| body checksum | XXH3-128 low then high `uint64` |
| dependency count, reserved | bounded `uint32`, zero `uint32` |
| dependencies and body | ordered XXH3-128 values, then exact body bytes |

Compatibility inspection reads only the unified magic, schema, and product
class before any body or dependency allocation. The implemented bodies are:

- Height: `uint16 Width=257`, `uint16 Height=257`, then row-major signed
  little-endian `int16` canonical heights.
- Coverage: 257² dimensions, a one-byte palette count and RFC 4122 layer IDs,
  then one to four sorted `(palette uint8, weight uint8)` pairs per sample;
  every sample sums to 255.
- Collision: deterministic 129² even-coordinate samples, including coordinate
  256, from the exact canonical height input.
- Query: deterministic 129² records containing height, signed X/Y central
  differences, and the dominant logical-layer palette index.
- Metadata: signed extrema, geometric range, canonical world-space XYZ bounds,
  and the ordered five-class product directory with each schema ceiling.

The five TerrainBuild functions use the DerivedDataCache Build Framework to
cache and validate canonical bodies
independently. Generation envelopes are applied only at atomic publication, so
one body can be reused without putting generation or package placement in its
build identity. A generation publisher accepts exactly the five checked
classes, verifies Height/Collision/Query dependencies, rejects stale request
tokens, and retains the previous complete generation on every failure.

## DDC, Cook, manifests, and compatibility

Authored intent remains `.dasset`; rebuildable product values remain DDC; Cook
places deployable values in manifest-owned raw region segments. The world manifest package
maps `(WorldId, TileKey, Generation, ProductClass)` to a region-package asset,
exact logical/stored range, XXH3-128, dependencies, and compatibility
tuple. Entries are sorted by tile Y, tile X, product class. Physical paths and
offsets never enter product or build identity.

An 8×8 tile region is keyed by floor division of tile coordinates by eight.
One region package owns all installed products for its occupied tiles. Partial
installation is region-granular; a manifest marks absent regions explicitly.
Opening a region reads one bounded directory and only requested bulk ranges.
An open/resident product, its manifest generation, any dependent collision/query
value, or an admitted load blocks package unload.

Cook follows authored references and explicit selected regions, requires a
complete generation, validates DDC hits identically to local builds, and writes
no source provenance into runtime bulk. A cooked runtime must load with source
and DDC unavailable. Missing region, missing product, corrupt range, checksum
mismatch, and incompatible platform/schema are distinct terminals.

Readers require the unified `TWPD` envelope and inspect its magic, schema, and
product class before reading a body. The retired per-class product magics, old
`DTerrainHeightmap` class, DAST Terrain packages, `TerrainHeightmap` build keys,
legacy cooked payload IDs, and old component/actor fields return
`UnsupportedLegacySchema`; no partial decode, alias, or dependency lookup occurs.

`TerrainBuild` materializes this contract through a sorted `TWMF` world
manifest and headerless opaque region segments. Installed region packages
contain five independently addressable raw ranges per complete tile;
uninstalled occupied regions remain explicit manifest records with no product
directory. Each installed record carries the exact manifest-owned range
(offset, stored/logical size, target, profile, and checksum),
the full product checksum, and ordered dependencies. Runtime loading first
validates the region extent/hash and `TWMF`, then the selected range, product
hash, product envelope, identity, generation, and dependencies. The returned handle
co-owns its immutable manifest generation and decoded product storage, which
prevents package storage from being released while a product is open.

## Runtime handles and spatial interest

Runtime public values are `FTerrainWorldHandle`, `FTerrainTileHandle`,
`FTerrainProductHandle`, `FTerrainGenerationHandle`, and
`FTerrainInterestHandle`. They contain stable IDs, product class, generation,
and immutable status only; none retains a `DObject`, component, actor, scene
proxy, package path, DDC key, GPU resource, or physics object.

An interest request contains exactly one of a finite world-space AABB or stable
Terrain sample region, consumer class, product-class mask and quality, signed
priority, safety class, optional deadline, requested generation policy, and a
lifetime/cancellation identity. Consumer classes are `RenderVisibility`,
`PhysicsSafety`, `Query`, `EditorFocus`, and `ExplicitPreload`.

The producer owns interest lifetime and priority. Terrain owns checked
world-to-tile mapping, coalescing, dependency expansion, admission, scheduling,
typed residency, fallback, and eviction. A camera/player/editor producer and a
future World Partition producer use the identical value boundary. A World cell
may cover many Terrain tiles and one Terrain tile may serve many producers; no
mapping or package API assumes equal grids or one-to-one ownership.

Coalescing takes the maximum priority and strictest safety/deadline for equal
world/region, generation policy, and product class. Physics safety outranks
render visibility at equal priority. Earlier deadlines then stable request ID
break ties. Cancellation removes only that producer's contribution;
supersession transfers demand to the newer generation before retiring the old
complete set. Every admitted request ends exactly once.

Minimum complete fallbacks are typed: metadata may report unavailable bounds;
render uses a visible coarse complete ancestor or an explicit hole marker;
surface uses the default logical layer; query reports unavailable rather than
inventing height; collision retains the prior complete safe generation or
blocks entry. Render residency never authorizes physics traversal.

## Limits, outcomes, and observability

Global ceilings are 2³¹ cells/axis, 262,144 tiles/profile, 4,097 installed
packages/world, 64 layers/world, 1,024 sources/world, 64 sources/tile,
16 layers/tile, four active layers/sample, eight neighbors, five product
classes, 64 dependencies/product, 256 admitted build tasks, 4,096 queued product
requests, 16,384 live interests, and the byte limits above. One-past-limit
input is rejected before allocation. The configured resident and peak-build
budgets may be lower but never higher than platform policy.

Every operation has one terminal status from `Ready`, `Unavailable`,
`InvalidDefinition`, `UnsupportedLegacySchema`, `MissingDependency`,
`BorderMismatch`, `Overflow`, `BudgetRejected`, `Cancelled`, `Superseded`,
`Corrupt`, `Incompatible`, or `PublicationFailed`. Validation, DDC query/local
build, Cook, package load, publication, interest admission, cancellation, and
shutdown use the same counted vocabulary and retain the previous complete
generation where one exists.

Diagnostics expose world/tile/product/generation IDs; coordinate extents;
logical/stored/resident/peak bytes by product; source, dependency, neighbor,
task, queue, and request counts; build/cache/Cook/load origin; bounded phase
timings; checksum/schema/platform; fallback; rejection reason; and exactly-once
terminal totals. Physical cache paths and source contents are excluded. At
shutdown, admitted equals the sum of every terminal and resident/retained/task
bytes return to zero.

## Required implementation qualification

T1 must add structural and round-trip codecs; asymmetric coordinate/seam
vectors; truncation, trailing, oversized, checksum, version, and legacy rejects;
deterministic cold/warm and reordered builds; cancellation/supersession;
manifest, reachability, partial-install, source/DDC-free Cook load, corruption,
unload, and shutdown cases; plus one-past-limit memory and package tests. T2 and
T4 extend the same vectors through render, collision, query, unequal-grid
interest, traversal, teleport, failure fallback, and the four product profiles.

## Related documentation

- [Asset Data Lifecycle and Storage](../Assets/AssetDataLifecycle.md)
- [Asset Packages](../Assets/AssetPackages.md)
- [Asset Catalog and Mutation](../Assets/AssetCatalogAndMutation.md)
- [Terrain World System Roadmap](../../Roadmaps/TerrainWorldSystem.md)
- [Current Terrain Heightmap Asset](TerrainHeightmapAsset.md)
- [Current Finite Terrain Rendering](../Rendering/TerrainRendering.md)

## Related code

- `Engine/Source/Developer/TerrainBuild/Public/Terrain/TerrainWorldTile.h`
- `Engine/Source/Developer/TerrainBuild/Public/Terrain/TerrainWorldCook.h`
- `Engine/Source/Developer/TerrainBuild/Private/Terrain/TerrainWorldBuildFunctions.cpp`
- `Engine/Source/Editor/AssetForgeBuiltins/Private/TerrainWorldBuildAdapter.h`
- `Engine/Tests/Native/EngineTests/Private/Terrain/TerrainWorldBuildTests.cpp`
