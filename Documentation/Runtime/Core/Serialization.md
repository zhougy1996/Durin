# Serialization

Summary: Define canonical byte archives, object-aware logical serialization, object graphs, default-relative planning, and authored override intent.

Modules: Core, CoreDObject

Last reviewed: 2026-09-18

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

Engine consumes this envelope through canonical DAST v10 object packages. An
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

Texture and StaticMesh payload customizations load into
caller-owned storage. A failed destination is destructible but incomplete and
must be discarded; neither the destination nor the cursor is rolled back.
Successful loading replaces all serialized sequences, including optional
streams that are absent in the new value. Save, discovery, counting and hashing
calls do not change persistent source values.

The owning DDC value or BulkData slot supplies an exact memory region and checks
`RequireArchiveEnd` before publication. Nested physical records consume their
declared extents rather than the remainder of an enclosing archive. Core
`ReadRegion` borrows contiguous input from a canonical memory reader without
copying; archives that cannot lend storage fail with `UnsupportedCapability`.
The caller must retain the backing buffer or lease through interpretation.
Payloads retain owned decoded containers, not borrowed input views. A live
replacement operation owns its detached candidate and publishes it only after
complete decode and validation; serialization does not add another candidate.
These rules do not relax transactional Blob, bounded-sequence, BinaryEnvelope,
BulkData, reflected-Struct, or package-loading contracts.

Stored extents and decoded allocation counts are independently bounded before
growth. Families preserve structured Archive failures through internal calls;
`UnsupportedTarget` distinguishes target/context conflicts from unsupported
wire versions. Existing external cache/build boundaries may classify these as
recoverable misses or operation failures. Offset/hash formats retain bounded
layout staging on save; borrowing input does not imply allocation-free decoding.
Texture and mesh payloads resolve stable target facts exclusively from the
Archive and are invoked as `Serialize(Ar)` or `Ar << Value`. Texture requires
Win64 and Game/EditorValidation; mesh requires Win64 and retains its independent
borrowed cancellation callback. Missing or unsupported target facts fail before
payload transfer. Stored target identities still require validation on load.

`FCountingArchive` and `FHashingArchive` accept optional `FArchiveState` and
`FArchiveVersionContext`, matching canonical memory archive context inputs.
Callers supply the same target, filtering, bulk policy and versions for counting,
hashing and actual writing. Purpose still derives persistence/Cook defaults.
Counting/hashing force the save direction and advertise only raw-byte and
position capabilities; capabilities supplied in the context are not inherited.
Omitted context preserves existing target-independent callers. No payload-level
explicit target fallback remains.

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
shape leaves the caller's prior output unchanged. The live reflected APIs return
`FReflectedMapKeyResult`, with typed failure reasons, owned property identities,
array bounds, and an outer-to-inner property/index route. The detached package
API returns `FCanonicalMapKeyResult`. Neither API accepts diagnostic string
outputs; legacy Archive, snapshot, and Engine adapters format explicitly until
their own result contracts are migrated.

`DObject/PackageFormat.h` owns the construct-free DAST v10 save boundary.
`FreezePackage(...)` validates and canonicalizes names, structural types,
schemas, imports, exports, property identities, references, and BulkData facts
into stable one-based ids. `WritePackage(...)` emits detached main and raw
external-bulk buffers and replaces neither caller output on failure. Values use
native `EValueKind` tags, Maps use the sole canonical-key writer, NaNs collapse
to one quiet pattern while signed zero is retained, and BulkData placement is
explicit detached input rather than live-object policy. This layer constructs
no `DObject` and depends on neither AssetRegistry nor Engine.

The same boundary owns bounded v10 reading. `ReadPackageRegistry(...)`
validates an exact declared front-matter span, independently known main/bulk
extents, the caller-supplied mounted package identity, all directory facts, and
the header-resident Registry/names/imports before atomically publishing package
metadata. `ReadPackage(...)` validates complete section hashes, tables,
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
Reflected `FByteBuffer` uses this operation and the logical `Bytes`
descriptor. Structured package framing may add its own record length, but must
not reinterpret the Blob contents or persist vector capacity/allocator state.

`FSharedByteBuffer` is Core's immutable, copy-shareable byte owner. It exposes
only an `FByteView`; replacement constructs a new allocation. Archive BulkData
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
Invalid decoded soft-reference paths retain an owned `FObjectError` in
`FObjectArchive` and through snapshot, property-copy, and graph results, alongside
the Archive code and field path. The destination path remains unchanged.
Serializers keep their `void` contract: the Archive records the first failure
internally, and the owning operation checks it before publication. Individual
field callers do not need to propagate a second result. Formatting occurs in
result formatters and the pending generic Archive diagnostic adapter.
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

DAST v10 does not introduce a second logical Archive dialect. Engine captures
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

In `EDefaultDeltaMode::Enabled`, fields compare with the paired class default
object, including the corresponding default subobject. Ordinary Structs pass
the paired default field down recursively. Arrays, fixed arrays, and Maps
replace their membership and ordering. Their reflected Struct elements compare
with the authoritative registered type default, never with a class-default
container element by index or key. Map keys retain complete canonical encoding.
Type-default captures are shared within planning and discarded afterwards.
Forced replacements also emit complete descendant values.
Dynamic owned roots establish their own class-default correspondence.
`AlwaysSerialize` reflected fields bypass default omission and emit a complete
value with ordinary provenance; required wire version sentinels use this flag.
It does not allocate an authored-override ledger. Required descendants also
keep their enclosing values present. Signed enum capture retains the underlying
signedness, so negative and positive enum changes participate in delta comparison.

DAST v10 carries an export baseline byte and a baseline byte before each Struct
value. Mode 0 is complete and uses constructor-initialized temporary storage;
mode 1 patches the initialized parent value; mode 2 copies the registered Struct
type default into managed temporary storage before applying present fields.
Complete values require all fields in their saved schema and never query the
registered type default. Type defaults can be nonzero and differ from the paired
parent value. Missing eligible defaults fail rather than silently changing modes.
Each present field
retains its own name, type, provenance, and value, independently of the shared
complete type descriptor. The reader validates field identities/types and
canonical form before constructing objects. The reader and writer support only
the current v10 contract; retired wire versions fail before graph construction.

An omitted type-relative field follows later changes to the registered default.
Complete saving or a whole-container Forced replacement pins
all saved fields. Per-element override editing and stable element identity remain
deferred; ordinary omission creates no ledger marks. Hard-reference-bearing
fields of a type-relative Struct remain complete, including their descendants,
so dependency discovery and reference binding stay explicit. The codec rejects
sparse type-relative values that omit these fields.

v10 authored loading copies reflected defaults for delta exports from the paired
CDO/default subobject with references remapped to loaded skeletons, then applies
saved fields. Native object fields are emitted completely because they lack a
reflected default-copy contract. Failure discards the unpublished graph. Cooked packages keep
complete values and do not require CDO initialization. This initialization
copies values only, never authored override state or PostLoad notifications.
A Struct with derived caches must invalidate them in PostDeserialize when
reflected fields change; FTextureSource detaches its decoded mip cache there.
Ordinary saving selects v10 delta mode; explicit complete exports skip default
initialization. Missing constructor-created default children reject delta saving.
Planning is transactional: missing defaults, unavailable identity, graph or
Archive failure, manifest drift, duplicate fields, and depth/count/path bounds
clear the output and return a typed diagnostic.

`EDefaultDeltaMode::NoDelta` does not read class or Struct defaults. It walks the
complete live Outer-owned graph and emits every supported non-`Transient`
logical field and child. Ordinary fields retain Explicit provenance; only
opt-in replacement boundaries carry Forced provenance. It is not a raw-memory fallback:
unstable descriptors, incomplete/custom reflected Structs, unsupported logical
values, malformed serializers, or limit violations still fail before output.

### Authored Override Intent

Ordinary authored-package loading restores values without creating override
state. Enabled planning compares values with defaults again, so an ordinary
value changed back to its default can be omitted. The Engine SavePackage entry
selects v10 delta saving; explicit `EAssetPackageSaveMode::Complete` selects NoDelta; emission mode and
persistent replacement state are independent. A saved Explicit tag is evidence of a value
in that package, not a persistent request to keep overriding the default.

A `DObject` may opt into complete field replacement through
`SetAuthoredOverride(..., Forced)`. It lazily owns an `FAuthoredOverrideLedger`;
ordinary objects allocate none and templates reject marks. The ledger uses
copy-on-write immutable snapshots and contains no object, property, schema, or
memory pointer. Each stored path contains declaring-type/field-name tokens.
Mutation validates the full supplied discovery/value route, token form, depth,
length, provenance, container bounds, and canonical Map key before publication.
Indexed routes normalize to the owning container field. A parent replacement
subsumes children; Arrays, fixed arrays, and Maps therefore remain replacements
across insertion, removal, sorting, and key changes. Bulk replacement rejects
exact duplicate input paths transactionally before normalizing/coalescing.

Forced fields emit their complete supported value, including default-valued
children. A nested Struct replacement causes its parents to be emitted with
ordinary Explicit tags, without promoting them into independent replacements.
Only replacement boundaries receive Forced tags. NoDelta writes complete values
without manufacturing overrides and retains explicitly requested boundaries;
cooked planning/loading does not carry runtime override state.

Canonical loading restores only Forced boundaries and stops below a replaced
field. Existing Explicit values remain readable but may be omitted on a later
ordinary resave if equal to defaults. Historical Forced tags do not distinguish
old NoDelta emission from user intent; preserve them conservatively as complete
replacement boundaries. Do not infer intent or require a bulk asset rewrite.
Struct reconstruction uses the explicit baseline rules above.

Clear, subtree clear, and reset change intent only, never values. Indexed clear
routes normalize to their container. Clearing a child cannot carve an exception
out of a replaced parent; clear the parent first. Removed fields are ignored
while planning; incompatible surviving routes fail closed. `DuplicateObject`
copies and revalidates sparse marks after constructing the destination graph.
GC ignores pointer-free tokens, destruction releases snapshots, and default
teardown cannot invalidate them. General callers and forced package restoration
retain validated publication; no unchecked setter is exposed.

Structs use the shared reflected save-selected field walk by default. A declared
`FDStructOps::Serialize(FArchive&, void*)` callback replaces that complete walk
for every Archive purpose and is invoked exactly once per value. Loading decodes into managed storage initialized according to the explicit
Struct baseline described above. After the complete field walk
or custom serializer succeeds, an optional `PostDeserialize` callback receives
the Archive purpose and source format version and returns `FObjectValidationResult`.
The context has no error-text slot or text rejection helper. `FDStructOps` version 2
requires this typed callback signature. Failures retain the Struct identity,
source version and module-owned cause through Archive/snapshot results. Only successful repair is
copy-assigned into the live destination, so truncation, missing capabilities,
or `PostDeserializeRejected` leaves the prior value unchanged. Hidden GC
references remain the separate responsibility of `CollectReferences`. A custom
Struct serializer whose reflected schema contains an `EditorOnly` field cannot
claim automatic editor-only filtering; a filtering Archive rejects that bypass
until the serializer honors `IsFilterEditorOnly()` itself or the Struct returns
to complete reflected traversal.

### Transient Object Graphs and Duplication

`SaveObjectGraphToMemory` and `LoadObjectGraphFromMemory` return
`FObjectGraphResult`; success derives from `FObjectGraphError::Code`. Load
publishes its root through `Object` only on success. Failures retain owned object
and class identities, record ids, byte counts, header versions, and underlying
Archive/property/map/graph-validation causes. Failed saves preserve the caller
byte buffer; failed loads retire all candidates. `FormatObjectGraphError` is an
explicit presentation boundary.

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
serializer exactly once. Once all values and Outer links are restored, each
object's read-only `ValidateLoadedObjectGraph` hook may reject invariants requiring
populated children. The hook returns `FObjectValidationResult`, with owned object
identity and an optional module-owned typed `IObjectValidationCause`. Material
adapters preserve their typed error and complete program diagnostics; the Core
framework has no Engine dependency. Formatting is explicit at presentation or
pending Archive adapters. A failure retires the entire constructed graph. The
format is process-local engine plumbing and has no v1 reader or migration path;
long-lived content uses the independently versioned, field-tagged `.dasset`
contract documented in [Asset Packages](../Assets/AssetPackages.md).

`DuplicateObject(...)` returns `TObjectGraphResult<T>` (or `FObjectGraphResult`
for the untyped overload), with success derived from its error code and the
duplicate in `Object`. Failures retain Archive/property causes, authored override
reason/path, or the complete graph-validation cause. The optional source-to-copy
map is cleared on entry and published only after success. Material graph command
and program errors retain an owned duplication cause; pending asset-operation
and Play error adapters format explicitly. Duplication uses purpose-specific
save and load Archives internally over
the same virtual entry. Hard references inside the duplicated Outer tree remap
to their duplicate, external hard references remain shared, and constructor-created
inners may be reused. Weak references remap only when their targets are already
duplicated for structural or hard-reference reasons; a weak-only external target
becomes null. After all values and authored ledgers are copied, graph-validation
hooks run before any duplicate PostLoad notification. A rejection retires the
whole duplicate graph. Any failure retires the incomplete duplicate graph.
`FProperty` value construction/copy and `FReflectedValueStorage` operations return
`FPropertyValueResult`. Errors own property and Struct names, the requested array
index and dimension, layout facts, and the failed operation. Missing inputs,
unavailable operations, invalid storage, and copy exceptions are distinct codes.
No diagnostic-output overload remains. Copy failure context survives detached
storage cleanup; failed attempts to construct into live storage preserve its
value. Pending Archive and editor contracts format explicitly at their adapters.

Property snapshot capture/restore APIs return `FPropertySnapshotResult`, with
owned validation context, operation and array indices, exact reference-table
failure reasons/counts, and Archive code/path context. The Archive retains its first local failure; snapshot and copy boundaries
materialize its diagnostic text while the Archive is alive, without copying a
nested cause tree. `FormatPropertySnapshotError` presents that owned text. Capture
publishes its payload only on success; restore retains the existing detached
Struct/container commit and hard-reference resolution rules. Shared property
error records live in `DObject/PropertyDiagnostic.h`.

`InitializeObjectFromDefaults` and `CopyEditableObjectProperties` return
`FObjectPropertyCopyResult`, without string error outputs. They retain owned
source/destination types and identities, nested reference routes, and diagnostic
text captured from property-value, snapshot, container and Archive failures. Default initialization
still delegates graph rollback to its caller. Editable copying restores earlier
fields on failure, retains the original failure plus the first rollback failure,
and marks the destination dirty only on success. The first rollback failure
remains a separate optional snapshot outcome. Engine/editor callers
format explicitly with `FormatObjectPropertyCopyError`.

Property snapshots and editable copies operate on selected values rather than
pretending to serialize a complete object; snapshots root their captured hard
references and remain process-local and unversioned.

Engine package field application formats the first Archive failure and any value
cause into an owned load diagnostic before candidate cleanup. The diagnostic
includes object and field identity and preserves the original Archive message;
it does not retain a resolver operation or require a recursive result hierarchy.

## Related Documentation

- [Package persistence](PackagePersistence.md): reflected capture, save operations and staged commit.

- [Generated Reflection System](ReflectionSystem.md)
- [Asset Packages](../Assets/AssetPackages.md)
- [Asset Data Lifecycle and Storage](../Assets/AssetDataLifecycle.md)
