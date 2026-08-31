# Serialization

Summary: Define canonical byte archives, object-aware logical serialization, object graphs, default-relative planning, and authored override intent.

Modules: Core, CoreDObject

Last reviewed: 2026-08-31

## Archive And Object Serialization

Core owns the generic byte Archive in `Serialization/Archive.h`. It provides
canonical little-endian primitives, raw transfer, bounded span regions,
position/remaining-byte queries, sticky structured failures, format/custom
versions, counting and hashing Archives, and bounded string, buffer, sequence,
alignment and zero-padding helpers. Persistent serializers use
`FCanonicalMemoryWriter`/`FCanonicalMemoryReader`; they never persist native
object representation, pointers, container capacity, ABI padding or unordered
iteration. `FArchiveState` independently carries direction, persistence, Cook,
editor-only filtering, bulk policy, purpose and target facts.

`Serialization/BinaryFormat.h` is the small convenience surface for explicit
binary families and reuses the same Archive byte substrate. Its typed integer
operations cover every non-boolean integral width and explicitly select little-
or big-endian encoding; named scalar helpers remain canonical little-endian.
Sequential readers and writers expose `Tell()`, bounded byte regions, canonical
unsigned VarInt plus ZigZag signed VarInt, and fixed GUID and XXH3-128 layouts.
VarInt readers reject overflow, truncation, and non-shortest encodings without
publishing a partial value. Configurable cursor limits bound the complete input
or output and each variable-width field; rejected writer operations append no
partial bytes.

One `FBinaryWriter` retains one canonical Archive bound to its owned byte vector
for its complete lifetime; scalar calls do not reconstruct Archive state. The
writer is neither copied nor moved, and `TakeBytes()` starts a new independent
sequence without invalidating later writes. Its
floating-point operations preserve exact IEEE-754 bits, including signed zero;
format-specific equivalence such as normalizing `-0.0` before a compatibility
hash remains the responsibility of the owning format. Bounded sequential
regions and random-access integer reads validate their complete source range
before publishing output. DAST, Cook state, and package BulkData reuse this byte
cursor but continue to own their schemas, semantic validation, diagnostics, and
publication policy.

`Serialization/BinaryEnvelope.h` owns the format-neutral `DURF` header-version-1
contract. The fixed 64-byte little-endian preamble encodes `DURF`, header and
preamble versions, a nonzero GUID `FormatId`, format version, required-feature
mask, exact front-header and physical-file extents, and an XXH3-128 header hash.
GUID words encode `A`, `B`, `C`, then `D`; the hash encodes low then high 64-bit
words. The hash covers the complete contiguous front header with its stored
16-byte hash field treated as zero and is an integrity check, not authenticity.

Prefix parsing consumes only the common preamble plus an independently known
physical file size and caller limits. It publishes the exact required front
header size only after validating magic, versions, identity, extents, limits,
and physical size. Complete validation receives that bounded front span and an
explicit immutable descriptor registry, then rejects unknown identities,
unsupported format versions or required features, descriptor-specific limits,
and hash mismatches before returning non-owning common and format-header views.
Registry construction copies descriptors, rejects invalid or duplicate IDs and
debug names independent of input order, and has no global registration or
constructor-order authority. Encoding, finalization, parsing, registry creation,
and validation replace caller outputs or destination bytes only on success.
Core never interprets format-owned sections, asset paths, schemas, codecs, or
publication policy.

Engine consumes this envelope through canonical DAST v9 object packages. An
authored or cooked `.dbulk` is deliberately not a DURF envelope: it is the raw
external BulkData segment bound by its owning package's Registry and Bulk
Directory. Embedded family payloads and raw DDC `.bin` values likewise do not
nest another DURF envelope; their owning asset slot supplies the codec and
schema.

Persistent values expose one bidirectional customization: member
`Serialize(FArchive&)`, free `Serialize(FArchive&, Value&)`, or an explicit
UE-style member taking a stable owner/context when the value cannot interpret
itself alone. Archive direction selects loading versus saving. A different
function is justified only for a materially different semantic layout such as
Cook streaming, not merely for the opposite direction.

CoreDObject layers `FObjectArchive` over that byte substrate. It owns reflected
logical descriptors, object/field/container scopes, hard and soft object
references and object-aware adapters. `DObject/Archive.h` forwards the Core
Archive API and temporarily aliases its prior memory-Archive spellings to the
object-aware canonical implementations for named migration consumers; it does
not contain a second primitive serializer.

CoreDObject also owns the format-neutral construct-free package-linker
vocabulary in `DObject/PackageLinker.h`: checked null/import/export indices,
recursive serialized types, detached logical values, property tags, custom
versions, import/export records, and bounded table/path lookup. These records
contain no `DObject`, `FProperty`, AssetRegistry, Engine, or DAST-version
identity. Format adapters publish a complete `FLinkerTables` only after
validating table indices and Outer topology.

`DObject/CanonicalMapKey.h` is the sole low-level encoder for canonical Map-key
type tags, sortable signed and unsigned integers, zero-normalized IEEE floating
values, strings, names, GUIDs, enum storage, and Struct field framing. Both the
live reflected-property entry and construct-free decoded values use this
writer. Token construction is transactional: an unsupported type or invalid
shape leaves the caller's prior output unchanged.

`DObject/PackageFormat.h` owns the construct-free DAST v9 save boundary.
`FreezePackageV9(...)` validates and canonicalizes names, structural types,
schemas, imports, exports, property identities, references, and BulkData facts
into stable one-based ids. `WritePackageV9(...)` emits detached main and raw
external-bulk buffers and replaces neither caller output on failure. Values use
native `EValueKind` tags, Maps use the sole canonical-key writer, NaNs collapse
to one quiet pattern while signed zero is retained, and BulkData placement is
explicit detached input rather than live-object policy. This layer constructs
no `DObject` and depends on neither AssetRegistry nor Engine.

The same boundary owns bounded v9 reading. `ReadPackageV9Registry(...)`
validates an exact declared front-matter span, independently known main/bulk
extents, the caller-supplied mounted package identity, all directory facts, and
the header-resident Registry/names/imports before atomically publishing package
metadata. `ReadPackageV9(...)` validates complete section hashes, tables,
recursive native tags, package topology, references, canonical ordering, and
inline/external BulkData ranges and digests before publishing `FLinkerTables`.
Successful decode re-emits through the sole writer and requires byte-identical
main and bulk output, so noncanonical but otherwise interpretable bytes fail
closed. Neither API retains input spans in its published result.

`DObject::Serialize(FArchive&)` is the one complete-object state-transfer entry.
Its base implementation calls `SerializeDObjectProperties(...)`, which enters
stable reflected-field scopes and walks supported save-selected properties.
A derived override calls its superclass implementation exactly once and may add
native state only through explicitly named `FArchiveFieldDescriptor` scopes and
semantic value operations. Missing or duplicate base calls, duplicate field
identities, nested object entry, and unbalanced scopes are deterministic
Archive failures.

The shared save-selection predicate always omits `Transient` fields and
deprecated fields outside their migration window. It additionally omits a
reflected `DPROPERTY(EditorOnly)` when `Ar.IsFilterEditorOnly()` is true.
`EditorOnly` is persistence policy, not layout or behavior compilation: ordinary
authored packages, duplication, editable copy, snapshots, and transactions keep
the field unless their Archive explicitly selects filtering. The same predicate
is applied at object fields and at every reflected Struct fallback field, so a
Struct nested in a fixed array, Array, or Map value cannot bypass the policy.
The owning container field selects whether its complete value participates;
container elements do not carry a separate policy.

Purpose selects Discovery, ObjectGraph, Duplicate, PropertySnapshot,
EditableCopy, AuthoredPackage, DerivedDataKey, DerivedDataPayload,
CookedPackage, CookedPayload, or BulkData. Capabilities describe structured
fields, bounded raw payloads, canonical Map order, hard and soft references,
unknown-field retention, remaining-byte and position queries, custom versions,
and multi-pass discovery. Consumers branch on capabilities and Archive context
rather than implementation type. Discovery is save-like and may call a
serializer once before emission; a serializer must therefore expose the same
fields, logical types, versions, and references in both passes and must not
mutate persistent state.

`DObject::Serialize` remains the ordinary authored/object-state entry.
`DObject::SerializeCooked` is the materially different target projection entry;
its default delegates to `Serialize` for compatibility. A cooked package
Archive selects `SerializeCooked` during discovery, NoDelta logical planning,
payload capture, and cooked load, and supplies persistent/Cook/editor-filter and
explicit target facts in every pass. Overrides call the appropriate base entry
once, serialize a detached or stack-local PlatformData projection, and do not
mutate the live graph, package dirty state, build state, diagnostics, or
residency. Cooked native fields absent from reflection are admitted only as
provisional compatibility candidates; the exact cooked Archive manifest and
complete field consumption validate them before publication. Cooked loads do
not restore authored-override intent.

`FArchiveLogicalTypeDescriptor` describes fixed-width scalars, enums, String,
Name, Guid, bytes, hard and soft objects, structs, Array, Map, and fixed arrays
without using C++ layout. There is no generic `sizeof(T)` serialization
fallback. `FArchiveFieldDescriptor` combines that recursive logical type with
the stable declaring type, field name, array dimension, and property flags;
offsets, padding, addresses, registration order, and RTTI are not persistent
identities. Raw bytes require explicit Archive support and, for a structured
authored Archive, an active field with a byte logical type.

`FArchive::SerializeByteBlob(...)` is the canonical owned binary-buffer
operation. It transfers a little-endian `uint64` byte count followed by the
exact bytes, rejects values above 1 GiB, validates the remaining input before
allocation, and loads into detached storage before replacing the destination.
Reflected `FByteArray` uses this operation and the logical `Bytes`
descriptor. Structured package framing may add its own record length, but must
not reinterpret the Blob contents or persist vector capacity/allocator state.

`FSharedByteBuffer` is Core's immutable, copy-shareable byte owner. It exposes
only a const span; replacement constructs a new allocation. Archive BulkData
serialization receives the field value plus explicit owner, element size,
alignment, storage policy, and Cook index. `Inline` transfers bounded bytes,
`Skip` performs no transfer, and `External` requires the owning Archive adapter
to capture or attach a logical package range. Core never resolves asset paths or
files. Runtime BulkData metadata is non-semantic and does not participate in
authored identical/default comparison; editor payload size and content identity
form the atomic authored logical value.

Object, field, array, and Map scopes maintain a structured diagnostic path.
`FArchive::Fail(...)` stores the first failure and later operations cannot clear
or replace it. Unsupported capabilities and types, malformed or truncated
payloads, invalid references and paths, unsupported versions, serializer
contract violations, and scope errors therefore abort the owning operation at
a stable path. A consumer owns construction, publication, rollback, and
destruction; those lifecycle steps are never hidden inside `Serialize`.

The semantic reflected-value layer is shared by object graphs, duplication,
property snapshots, editable copying, and authored-package Archives. Hard
references are delegated to the selected Archive and are never persisted as
process addresses. Soft references transfer only their bounded logical path.
Map writers that advertise canonical ordering use stable logical key tokens, so
supported Maps do not depend on bucket or insertion history.

### Default-Relative Logical Planning

`BuildDefaultDeltaPlan(...)` consumes the same Archive field and logical-type
descriptors as ordinary serialization and produces no package bytes. Discovery
and value capture run over the same virtual `Serialize` entry; the manifest,
types, container shapes, and canonical Map keys must agree before planning.
Reflected and native named fields enter one canonical order by declaring type
and field name. Captured values are detached logical nodes; published nodes do
not retain source-memory pointers.

A reflected Blob is one atomic logical `Bytes` node regardless of byte count.
Equality is size plus exact byte equality and a changed or forced Blob emits as
one complete field. It has no indexed authored-override paths. Planner
field-count limits therefore count the field once; Archive, package-size, and
allocation byte limits remain independent and authoritative.

A reflected authored-bulk value is a distinct atomic logical `BulkData` node.
Planning compares logical size and verified content identity, never domain
schema, physical placement, or authority state. Multi-megabyte values still
contribute one node in enabled and no-delta plans.

DAST v9 does not introduce a second logical Archive dialect. Engine captures
the ordinary object-aware Archive graph into detached `FLinkerTables`, and
CoreDObject emits the canonical tagged-value sections from that model. Each
logical BulkData field becomes one linker value with explicit inline/external
placement; capture never writes offsets, handles, flags, or residency back into
the live value. Complete package validation binds every range and segment before
Engine applies the linker. Section extents, placement, and generation remain
physical package concerns rather than reflected semantics.

Engine's private bounded manifest codec is not an Archive implementation.
It serializes only explicit little-endian fixed-width integers, GUID words, and
exact byte spans for CMNF physical framing. Archive continues to
own semantic object/value serialization, purposes, defaults, and reflected
field traversal; physical-container helpers are not public and do not admit
native structure layouts.

In `EDefaultDeltaMode::Enabled`, top-level fields compare with the paired class
default object. Once a Struct is emitted, its fields recursively compare with
the Struct type default, including Structs inside fixed arrays, Arrays, and Map
values. Containers are complete authored values rather than insert/remove
deltas. A class-specific non-type-default Struct can therefore emit an empty
Struct block when it is explicit but every child equals the type default.
Planning is transactional: missing defaults, unavailable identity, graph or
Archive failure, manifest drift, duplicate fields, and depth/count/path bounds
clear the output and return a typed diagnostic.

`EDefaultDeltaMode::NoDelta` does not read class or Struct defaults. It walks the
complete live Outer-owned graph and emits every supported non-`Transient`
logical field and child with forced provenance. It is not a raw-memory fallback:
unstable descriptors, incomplete/custom reflected Structs, unsupported logical
values, malformed serializers, or limit violations still fail before output.

### Authored Override Intent

An ordinary `DObject` may lazily own an `FAuthoredOverrideLedger`; an untouched
object allocates no ledger, and templates reject entries. The ledger uses
copy-on-write immutable snapshots for concurrent reads and contains no object,
property, schema, or memory pointer. A canonical path begins with a declaring
type and field name, then may use Struct field identities, fixed-array indices,
positional Array indices, or Map values selected by collision-checked canonical
key bytes. Mutation validates the current discovery/value schema, token form,
depth, byte length, provenance, bounds, and key availability before atomic
publication. Bulk replacement sorts complete paths and rejects duplicates
without changing the prior ledger.

Known intent is exactly `LoadedExplicit` or `Forced`; absence means no known
intent. Canonical v8 load rejects unknown or unsupported tagged values before
live publication rather than retaining an opaque Engine-side wire value.
Enabled planning applies `Forced`, then `LoadedExplicit`, then logical
difference, then omission. Nested intent emits every required parent record.
Forced state cannot be downgraded by a loaded-explicit update. Exact clear,
subtree clear, and reset change only intent, never the reflected value. Missing
Array positions and removed Map keys or fields are ignored during planning;
incompatible surviving routes fail closed. Array marks are intentionally not
remapped after structural edits, while Map marks survive iteration-order changes.

`DuplicateObject(...)` copies ledger snapshots only after the destination
graph exists and revalidates every path against the destination. GC ignores the
pointer-free tokens, object destruction releases the snapshot, and class/Struct
default teardown cannot invalidate it. Authored-package load creates no ledger
until an explicit/forced v8 property tag needs one; subsequent package capture
queries the same ledger when rebuilding canonical provenance.

Structs use the shared reflected save-selected field walk by default. A declared
`FDStructOps::Serialize(FArchive&, void*)` callback replaces that complete walk
for every Archive purpose and is invoked exactly once per value. Loading always
decodes into default-constructed managed storage. After the complete field walk
or custom serializer succeeds, an optional `PostDeserialize` callback receives
the Archive purpose and source format version. Only successful repair is
copy-assigned into the live destination, so truncation, missing capabilities,
or `PostDeserializeRejected` leaves the prior value unchanged. Hidden GC
references remain the separate responsibility of `CollectReferences`. A custom
Struct serializer whose reflected schema contains an `EditorOnly` field cannot
claim automatic editor-only filtering; a filtering Archive rejects that bypass
until the serializer honors `IsFilterEditorOnly()` itself or the Struct returns
to complete reflected traversal.

### Transient Object Graphs and Duplication

Object-graph v2 saving first runs a Discovery Archive over the same virtual
`DObject::Serialize` entries used for emission. Scope includes the root,
structural Outer descendants and required Outer chains, plus serialized hard
references; raw references, soft references, weak references, and GC-only hidden
references do not enlarge it. A transient reflected weak handle is emitted only
when its target already has a frozen graph id; otherwise it is encoded as null.
The discovered objects and ids are frozen before writing, and
late objects, fields, types, references, or versions fail without publishing
bytes. Emission retains deterministic root/Outer ordering.

Loading validates the v2 header and all record bounds, creates every object
skeleton before resolving reference ids, then invokes each object's virtual
serializer exactly once. A failure retires the entire constructed graph. The
format is process-local engine plumbing and has no v1 reader or migration path;
long-lived content uses the independently versioned, field-tagged `.dasset`
contract documented in [Asset Packages](../Assets/AssetPackages.md).

`DuplicateObject(...)` is the public typed duplication entry and uses
purpose-specific save and load Archives internally over
the same virtual entry. Hard references inside the duplicated Outer tree remap
to their duplicate, external hard references remain shared, and constructor-created
inners may be reused. Weak references remap only when their targets are already
duplicated for structural or hard-reference reasons; a weak-only external target
becomes null. Any failure retires the incomplete duplicate graph.
Property snapshots and editable copies operate on selected values rather than
pretending to serialize a complete object; snapshots root their captured hard
references and remain process-local and unversioned.

## Related Documentation

- [Generated Reflection System](ReflectionSystem.md)
- [Asset Packages](../Assets/AssetPackages.md)
- [Asset Data Lifecycle and Storage](../Assets/AssetDataLifecycle.md)
