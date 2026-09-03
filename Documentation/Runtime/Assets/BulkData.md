# Package Bulk Data

Summary: Define reflected BulkData values, canonical DAST v9 placement, package-resource range access, and raw package segments.

Modules: Engine, CoreDObject, AssetRegistry

Last reviewed: 2026-09-03

BulkData is a reflected field contract. The field owns bounded logical storage
facts and optional memory; the package owns physical placement and integrity;
the asset family owns the meaning of the bytes. A BulkData value never stores a
physical filename, DDC key, target platform, asset schema, or source-control
policy.

## Runtime Field State

`FBulkData` has these observable states:

- Empty: zero size, no resource, and no allocation.
- Attached: unloaded immutable package-resource range.
- Loading: one admitted asynchronous range read.
- Resident: unlocked allocation whose package source remains available.
- ReadLocked or WriteLocked: one or more shared read locks or one exclusive
  write lock.
- Detached: resident mutable storage without a package source.
- Failed: attached storage whose latest explicit read failed and may be retried.
- Retired: a range whose package resource admits no new work.

Only Resident or Detached storage may acquire locks. Reading Attached first
loads an immutable snapshot. Writing is admitted only for Detached or Empty and
detaches shared storage before returning. Resize is legal only while
write-locked, uses checked `uint64` arithmetic, and may not exceed 1 GiB.
Conflicting locks, unmatched unlock, resize outside a write lock, unload while
locked/loading, and access after retirement fail without changing state.

Copies snapshot metadata, resource ownership, and resident bytes but begin
unlocked. Immutable allocations may remain shared until one copy takes a write
lock. Moves transfer the snapshot and leave an Empty source. Every admitted
asynchronous request publishes exactly one terminal Success, Failed,
Cancelled, or Retired result.

Runtime field metadata is logical size, stored size, storage flags, segment
offset, alignment, and a package-resource handle. Current storage supports
flags zero, equal logical/stored sizes, power-of-two alignment from 1 through
4096, and an exact inline or external package range. Physical content identity
belongs to the package descriptor, not mutable runtime state.

## Editor Field State And Identity

`FEditorBulkData` is independent of `FBulkData` and has no lock API. It owns an
instance GUID, XXH3-128 content ID, logical size, and either immutable memory or
an immutable package-resource range. Copies retain a complete immutable
snapshot. `UpdatePayload` validates and owns the complete candidate before
atomically replacing content facts; failure preserves the prior value.

The content ID is XXH3-128 over canonical uncompressed payload bytes, encoded
as little-endian `HashLow` then `HashHigh`. Empty bytes use the ordinary empty
span digest; there is no sentinel. Content equality uses content ID plus size
without loading. Instance identity is registration identity only and does not
enter equality or build keys. Updating an existing value retains its instance
GUID while replacing content identity.

`GetPayload` returns an immutable owned/shared buffer request. Memory sources
may complete inline, but package sources use the same terminal contract through
the resource manager. A failure never erases content identity or size.

## Package-Resource Ownership

`FPackageResourceRange` stores a ref-counted logical resource handle, offset,
stored size, flags, and alignment. Its validator checks flags, alignment,
overflow, caller limits, and the resource's validated segment extent. It owns
no hash, GUID, DDC key, schema, target, asset path, or physical path.

Only the loose backend stores the mounted `.dasset` path and derives the stable
`.dbulk` sibling. At package admission it validates the complete external
segment against the v9 Registry extent and XXH3-128 digest, every external
field digest, and zero alignment padding in one sequential pass using 64 KiB
scratch and the 1 GiB package limit. It then exposes immutable ranges from the
already validated Bulk Directory. Backup recovery uses a bounded-memory atomic
file copy. Metadata-only Registry inspection reads no segment bytes and does
not create a live resource.

A read is admitted only while the resource is Active and its checked range is
inside the validated extent. Retirement enters Retiring, rejects new requests,
requests cancellation for admitted work, waits for one terminal result per
request, then becomes Retired. Package unload retires the resource before
withdrawing object publication; Engine shutdown retires all resources before
filesystem and task services stop.

The backend reports InvalidRange, MissingSegment, TruncatedSegment,
SegmentDigestMismatch, Cancelled, Retired, and IoError distinctly. A range read
checks before/after physical size and rejects a changed segment rather than
returning a mixed generation.

## DAST v9 Authored Placement

CoreDObject receives BulkData as detached linker values. Each value includes
logical bytes, element size, power-of-two alignment, and explicit Inline or
External placement. The v9 writer owns placement; it never writes offsets,
handles, residency, or resource state back into the live field.

The package contains two physical payload domains:

- Inline Bulk is the ninth `.dasset` section and holds canonical aligned inline
  payload ranges.
- External bulk is one headerless raw `.dbulk` sibling containing aligned
  payload ranges and zero padding only.

Bulk Directory version 1 has one canonical record for every BulkData value and
binds its export/property identity, storage kind, offset, extent, element size,
alignment, and XXH3-128 content digest. Records use frozen linker order. Inline
and external offsets are relative to their respective segment starts. Empty
values have zero extent; a package with no external bytes has no `.dbulk`
sibling and Registry declares zero extent and a zero whole-segment digest.

Validation requires each record to resolve to one `BulkData` property tag and
requires its tag placement facts and byte content to match. Ranges are ordered,
nonoverlapping, correctly aligned, digest-verified, and separated only by zero
padding. Every byte in Inline Bulk and the external segment is consumed by one
range or required alignment padding; leading/trailing undeclared bytes and a
missing or extra stable companion are invalid. The raw companion has no DURF
envelope, nested metadata header, generation id, target, schema, or trailer.

The Registry section binds the external segment by exact extent and whole-file
XXH3-128. Header validation checks declared extent against physical extent;
complete package validation checks the whole digest and every directory range
before linker publication or object construction.

Runtime loose loading uses the resource-backed v9 reader. External linker
values carry only directory offset, extent, alignment, element size, and
content digest; they do not own payload bytes. Canonical main-package
validation reconstructs `.dasset` from those descriptors without emitting a
canonical `.dbulk` buffer. Full closure validation and mutation tools retain
the owning-byte reader when byte-for-byte external reconstruction is required.

## Cooked Projection

A cooked Archive dispatches `DObject::SerializeCooked`, supplies exact
platform/profile facts, filters editor-only state, and captures runtime
`FBulkData` into the same v9 linker value and placement contract. Detached live
fields remain detached while capture copies immutable bytes. The writer assigns
physical offsets only inside detached output.

`FCookContext::AddPackage(VirtualPath, Package)` carries canonical identity and
exact v9 main/bulk bytes through reachability, pruning, output planning, and
CMNF publication. Cooked load validates the closure first, then attaches
external fields to the package resource. Metadata load issues no range request;
first access requests exactly the declared range. There is no Cook-only package
raw-segment metadata grammar.

Opaque non-package Cook segments remain a separate explicit plan kind. They
are validated by their own extent/digest and are never passed to the v9 package
reader or misidentified as a `.dbulk` closure.

## Publication And Companion Ownership

Authored save snapshots all payloads into the detached closure and follows
[package publication](AssetPackages.md#production-save-and-load), including the
distinction between pre-commit rollback and post-commit Registry reconciliation.
The prior stable segment remains recoverable until the new closure commits.
Inline-only saves publish no empty segment and remove a stale prior companion
only after the new main package is committed. Cook publication and rollback
follow [Asset Data Lifecycle](AssetDataLifecycle.md#cook-and-publication-rules).

Move, duplicate, delete, inventory, orphan detection, source-control closure,
and canonical resave derive companion ownership from validated v9 Registry and
Bulk Directory facts. A suffix scan is never authority. Atomic temporaries and
`.durin-backup` files are recovery state, not authored companions. Git LFS
pointer text, absent content, truncated companions, and partial clones fail
closure validation and never publish a live package.

## Qualification Budget

`FPackageAssetTests.V8FieldBulkClosureMeetsBoundedLooseFixtureBudgets` freezes a
4 MiB uncompressed external field and enforces bounded metadata load, first
access, and save. Metadata load retains zero payload bytes and issues no range
request; admission reads exactly one segment extent with at most 64 KiB
validation scratch. First access returns one owned 4 MiB buffer through one
exact request; ordinary save preserves a 4 MiB headerless segment; unload
returns the resource count to zero. These are regression ceilings, not hardware
benchmarks.

## Related Documentation

- [Asset Packages](AssetPackages.md)
- [Asset Data Lifecycle](AssetDataLifecycle.md)
- [File I/O](../Core/FileIO.md)
- [Serialization](../Core/Serialization.md)
