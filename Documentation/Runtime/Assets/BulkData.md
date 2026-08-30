# Package Bulk Data

Summary: Define field-level BulkData values, package-resource range access, and authored/cooked raw package segments.

Modules: Engine, CoreDObject, AssetRegistry

Last reviewed: 2026-08-30

BulkData is a reflected field contract. The field owns bounded logical storage
facts and optional memory; the package owns physical placement and integrity;
the asset family owns the meaning of the bytes. A BulkData value never stores a
physical filename, DDC key, target platform, asset schema, or source-control
policy.

## Runtime field state

`FBulkData` has the following observable states. Empty has zero logical and
stored size, no resource and no allocation. Attached is an unloaded immutable
package range. Loading is an admitted asynchronous read. Resident is an
unlocked allocation. ReadLocked and WriteLocked are resident allocations with
one or more read locks or exactly one write lock. Detached is resident storage
without a package source. Failed is an attached field whose most recent read
failed; a later explicit reload may retry. Retired is an attached field whose
package resource no longer admits work.

Only Resident or Detached may acquire locks. A read lock on Attached first
loads a detached immutable snapshot; a write lock is admitted only for Detached
or Empty and makes the allocation detached before returning. Read locks may be
nested; a write lock is exclusive. Resize is legal only while write-locked,
uses checked `uint64` arithmetic, and may not exceed 1 GiB. Unlock without a
matching lock, a read/write conflict, resize outside a write lock, unload while
locked or loading, and access after retirement fail without changing state.
Unload drops an unlocked resident allocation only when a package source exists;
a detached allocation cannot be re-created and therefore cannot unload.

Copies snapshot metadata, source ownership, and resident bytes but start
unlocked. Immutable resident allocations may be shared until either copy takes
a write lock, which detaches it. Moves transfer the same snapshot and leave an
Empty source. Destruction never waits while holding a field lock. Every admitted
asynchronous request publishes exactly one terminal Success, Failed, Cancelled,
or Retired result.

The runtime field metadata is `(storage flags, logical size, stored size,
segment-relative offset, alignment, package-resource handle)`. M1 supports only
flags zero, equal logical/stored sizes, power-of-two alignment from 1 through
4096, and the Bulk segment. The metadata contains no content identity.

## Editor field state and identity

`FEditorBulkData` is independent of `FBulkData`. It has no lock API. It owns a
random instance GUID, a content ID, logical size, and either immutable memory or
an immutable package-resource range. Copies retain the complete immutable
snapshot, including instance identity and source, and later updates to one copy
do not affect another. `UpdatePayload` first validates and copies/takes the
complete candidate, then atomically replaces all identity, size, source, and
memory facts. A failed update leaves the old value unchanged.

The content ID encoding is XXH3-128 over the canonical uncompressed payload
bytes and is written as two little-endian `uint64` words (`HashLow`, then
`HashHigh`). Algorithm version 1 is fixed by DAST v7. Empty bytes therefore use
the ordinary XXH3-128 digest of an empty span; there is no sentinel identity.
Content equality compares content ID and byte size without loading. Instance
identity is registration identity only and does not participate in equality or
build keys. A new memory value receives a new instance GUID; a loaded value
retains the persisted instance GUID; updating an existing value retains its
instance GUID and replaces its content ID.

`GetPayload` returns a request for an immutable owned/shared byte buffer. Memory
sources may complete inline, but callers use the same request contract. Package
sources complete through the resource manager. Cancellation is advisory until
the backend reaches a cancellation point; it still produces one terminal
result. A source failure does not erase the field's content ID or size and may
be retried. No request callback or backend object may escape the request.

## Package-resource ownership

`FPackageResourceRange` stores only a ref-counted logical resource handle,
segment offset, stored size, storage flags, and alignment. Its shared validator
checks flags, alignment, arithmetic, the caller's size bound, and the resource's
validated segment extent. `FEditorBulkData` wraps the range with authored
instance/content identity and logical size; `FBulkData` wraps it with logical
size and residency state. The range never owns a hash, GUID, DDC key, schema,
target, asset path, or physical path.

Only the loose backend stores the mounted physical package path and derives the
stable `.dbulk` sibling. A read is admitted only when the resource is Active and
the checked range is within the validated segment extent.

Retirement changes the resource to Retiring, rejects new requests as Retired,
requests cancellation for admitted work, and waits for admitted callbacks to
publish one terminal result before becoming Retired. Package unload retires its
resource before object publication is withdrawn. Engine shutdown retires all
resources before filesystem and task services stop. Backend failures use the
stable vocabulary InvalidRange, MissingSegment, TruncatedSegment,
SegmentDigestMismatch, Cancelled, Retired, and IoError.

The loose backend validates the complete segment extent and XXH3-128 digest
once when registering a DAST v7 package. Registration streams at most 1 GiB and
uses at most 1 MiB of temporary I/O storage; it does not allocate payload-sized
memory. Construct-free full inspection and canonical resave perform the same
validation. A range request trusts that immutable registration snapshot but
checks before/after file size and rejects a changed segment. Metadata-only
header inspection reads no segment bytes and reports the declared closure
without admitting it for live access.

## DAST v7 authored wire contract

DAST v7 retains the DURF envelope and the eight required DAST sections in v6
order. Its 32-byte format header and 48-byte section entries are unchanged.
Public Summary version 2 contains, in order:

1. `uint32 SummaryVersion = 2`, `uint32 MainExportIndex = 1`.
2. `uint64 ImportCount`, `uint64 ExportCount`, `uint64 BulkFieldCount`.
3. `uint64 SegmentExtent`, `uint64 SegmentDigestLow`, and
   `uint64 SegmentDigestHigh`.
4. `uint32 SegmentFlags = 0`, `uint32 Reserved = 0`.
5. The existing asset-class and redirect-destination wire strings.

`SegmentExtent == 0` requires a zero digest and no `.dbulk` sibling.
`SegmentExtent > 0` requires a nonzero digest and exactly one stable `.dbulk`
sibling. The authored segment limit is 1 GiB, each package has at most 65,536
BulkData fields, and the complete `.dasset` remains bounded to 1 GiB.

Payload Directory version 2 begins with `uint32 Version = 2`, `uint32
EntryBytes = 72`, and `uint64 Count`. One entry per BulkData field follows in
canonical frozen object order and canonical reflected field order:

| Offset | Type | Meaning |
| ---: | --- | --- |
| 0 | `uint64` | One-based field index; equals directory position + 1 |
| 8 | `uint32` | Placement: 0 inline, 1 external raw segment |
| 12 | `uint32` | Storage flags; zero in M1 |
| 16 | `uint64` | Logical byte count |
| 24 | `uint64` | Stored byte count; equals logical size in M1 |
| 32 | `uint64` | Segment-relative offset; zero for inline |
| 40 | `uint32` | Alignment; 1 for inline, 16 for external authored data |
| 44 | `uint32` | Reserved zero |
| 48 | `uint64` | Content ID low word |
| 56 | `uint64` | Content ID high word |
| 64 | `uint64` | Reserved zero |

The logical BulkData field encoding is `uint64 FieldIndex`, `uint8 Placement`,
`uint8 StorageFlags`, `uint16 Alignment`, `uint32 ContentIdVersion = 1`, the
16-byte instance GUID, two `uint64` content-ID words, `uint64 LogicalSize`,
`uint64 StoredSize`, and `uint64 SegmentOffset`, followed only for inline
placement by exactly `StoredSize` bytes. It must agree byte-for-byte with its
directory entry. Zero-length fields are inline, have alignment 1 and offset 0,
and carry the canonical empty content ID.

Authored payloads of at most 256 KiB are inline. Larger payloads are external
and aligned to 16 bytes. External offsets are relative to byte zero of the raw
segment. The writer appends fields in directory order, inserts only zero bytes
to reach each declared alignment, assigns a distinct range to every nonempty
external field, and ends the segment at the final payload byte. There is no
leading header, directory, magic, generation ID, target, schema, or trailing
padding in `.dbulk`.

Validation rejects unsupported versions, placements or flags; noncanonical
field indexes; invalid sizes, alignments, offsets or arithmetic; duplicate,
overlapping or out-of-order ranges; nonzero padding; missing or trailing segment
bytes; digest disagreement; field/directory disagreement; and a segment whose
first bytes merely happen to resemble a container header no differently from
any other payload bytes. Segment content is opaque: a header-like payload is
valid when declared as payload, but undeclared header bytes before the first
range are nonzero padding and invalid.

## Cooked raw-field projection

A cooked Archive dispatches `DObject::SerializeCooked` and captures runtime
`FBulkData` fields through the same DAST v7 field grammar and raw-segment
placement used above. Cook supplies explicit platform/profile context, filters
editor-only state, and uses NoDelta planning with the same cooked dispatch.
Detached fields remain detached while capture copies their immutable bytes.
The package writer assigns transient field identities and content checks for
physical validation; neither becomes `FBulkData` semantic state.

`FCookContext::AddPackage(VirtualPath, Package)` publishes the resulting raw
segment and records it as `PackageBulk` in CMNF. This route has no
`FCookedPayloadDescriptor`, descriptor callback, DBLK header, payload table, or
family companion resolver. Cooked load validates the complete segment before
object publication, dispatches `SerializeCooked`, and attaches external fields
to the registered package resource. Metadata load performs zero range requests;
the first lock requests exactly the declared field range. The descriptor-aware
`AddPackage` production route is retired. `CookedBulk` manifest records and
DBLK v2 decoding remain accepted only by the bounded compatibility/inspection
surface.

## Publication, compatibility, and migration

A save captures immutable payload snapshots, lays out the segment without
mutating live fields, stages package and optional segment, and preserves the
prior stable segment as `.dbulk.durin-backup`. It publishes `.dbulk` first,
`.dasset` second, and catalog state last, validates the committed pair, then
removes the backup. Failure before catalog publication restores the previous
complete pair or removes a first uncommitted pair. Inline-only saves publish no
empty segment and remove a stale canonical segment only after the package is
committed.

DAST v6 plus DABK v2 remains a read-only compatibility input. Canonical resave
loads and verifies the complete legacy payload closure, writes only DAST v7 and
raw `.dbulk`, commits and catalogs the new pair, then removes the stable
`.dabulk`. A `.dasset` that declares v7 while only `.dabulk` exists, v6 while
only `.dbulk` exists, or either format with both stable siblings is a conflict;
normal load never guesses. Backup recovery considers only the suffix owned by
the package version. Git LFS pointer text, missing LFS content, partial clones,
and absent siblings are reported as missing or mismatched closure and never
published as live packages.

Move, duplicate, delete, inventory, orphan detection, and source-control
closure derive the stable companion from the validated package version and
summary. Transaction temporaries and `.durin-backup` files are recovery state,
not authored companions. New writers never emit DABK or `.dabulk`.

## Qualification budgets

`FPackageAssetTests.FieldBulkQualificationMeetsBoundedLooseFixtureBudgets`
freezes one 4 MiB uncompressed external authored field. The MacOS arm64 Debug
measurement on 2026-08-30 recorded 9.95 ms metadata load, 19.77 ms first access,
175.81 ms canonical v7 save, and 235.81 ms v6-to-v7 resave (16.96 MiB/s). The
enforced diagnostic ceilings are 500 ms metadata load, 500 ms first access,
2 seconds save, and 4 seconds resave, with a 0.25 MiB/s resave floor.

Metadata load retains zero field payload bytes, registers one logical package
resource, and issues zero range requests. First access returns one owned 4 MiB
buffer through exactly one 4 MiB request; the field remains nonresident. The
ordinary save preserves a 4 MiB headerless segment. Package unload returns the
registered-resource count to zero. Family fixtures separately require zero
source-range reads on validated warm DDC hits and one owned source snapshot on
cold build paths. These are bounded regression gates, not hardware benchmark
claims; rebaseline requires a dedicated qualification change with recorded host
and fixture evidence.
