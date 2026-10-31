# Authored Property Overrides

Summary: Define immutable per-object replacement intent, manager queries, traversal lifetimes, and validated restoration.
Modules: CoreDObject, Engine

Last reviewed: 2026-10-31

## State and operations

`FOverridableManager` is the entry point for authored override state. Each
`DObject` lazily owns one immutable `FOverriddenPropertySet`; ordinary objects
allocate none. `GetOverriddenProperties` returns a shared read-only snapshot.
Readers may retain that snapshot while the owner publishes a replacement.
Atomic publication does not authorize concurrent live-object edits.

`OverrideProperty` validates a complete route before changing intent.
`RestoreOverriddenPropertyOperation(Object, Entries, ...)` is the strict bulk
replacement route: it validates Discovery/AuthoredPackage shape and every input
path, rejects exact duplicate inputs before normalization, then publishes once.
Failed validation leaves the prior snapshot intact. Nonempty edits reject
templates. Reset and restoration of empty state release the lazy allocation.

The supported operations are `None` and `Replace`. A value differing from its
default is not an authored operation. `EAuthoredOverrideProvenance::Forced`
records authoring provenance separately from the effective replacement operation.
Element Add/Remove, Modified and subobject shadowing mutation APIs are absent.
These names align responsibilities with Epic's public UE 5.8 API documentation,
not its ABI, internal node representation or experimental feature completeness.

`GetOverriddenPropertyOperation` validates a live route and returns a typed
query result. A replacement at an ancestor reports `Replace`, `bInherited` and
the replacement depth, without storing a child entry. An ancestor containing
only descendant replacements reports `None` with `bHasOverriddenDescendants`.
The immutable set's query compares canonical identities only; it cannot validate
live container bounds without an object. Use the manager for live input.

## Durable intent and content compatibility

Stored paths contain canonical declaring-type and field-name tokens, without
object/property/schema pointers. General inputs may include fixed-array, array
and Map-value selectors. Validation checks actual indices and canonical keys
before normalizing to the owning complete-container field. Parents subsume
children; no persistent per-element operation is implied by a selector.

Forced fields equal to their defaults remain replacements through save/load,
duplication and default changes. Nested Struct replacement preserves its own
boundary. Structural ancestors emit ordinary Explicit tags. Ordinary loaded
Explicit values carry no persistent intent and may disappear on a later delta
save if they equal defaults. Historical Forced tags retain conservative complete
replacement intent. The package format and enum encodings are unchanged.

NoDelta emits complete values without inventing intent; cooked loading does not
restore authored state. Clear, subtree clear and reset change intent only.
Indexed clears normalize to the container; clearing a child cannot create an
exception within a replaced parent. Duplication strictly revalidates the copied
boundaries after destination construction. Removed fields are ignored in default
planning; incompatible surviving paths fail. Deprecated migration code explicitly
transfers intent rather than inferring it from historical values.

## Transient paths

`FObjectArchive::GetSerializedPropertyChain` exposes the current object-relative
`FArchiveSerializedPropertyChain`. Core byte archives have no DObject dependency.
Object, field, fixed-array, array and Map scopes maintain this chain, including
error and exception unwinding. Reusable storage is retained by traversal depth;
inspection returns a span without copying a vector or walking the object again.

`FPropertyVisitorPath` is a checked borrowed view. Any chain mutation or archive
destruction invalidates it; durable conversion then returns `StaleTraversal`.
Spans obtained from a view have the same borrowed lifetime. Both reflected and
native fields use logical descriptors; no raw reflection binding escapes through
this interface. Metadata replacement cannot leave a dangling property pointer in
the path. `ToDurablePath` explicitly copies canonical tokens. Map keys themselves
cannot represent replacement routes; Map values require a notified canonical key.
Container positions are transient and do not establish persistent element identity.

## Restricted load restoration

General callers always use strict validation. Eligible package loading instead
uses a single `FOverriddenPropertyRestoreContext` created by an object archive.
The archive records Forced boundaries from validated field traversal, without
retaining complete logical values or bulk bytes. Completed scopes and successful
whole-object consumption precede publication.

Eligibility requires an exact native constructor-type proof that the concrete
object inherits `DObject::Serialize`, complete reflected nested schemas, managed
metadata identity, and no custom Struct serializer or PostDeserialize callback.
`InternalConstructor<T>` records the compiled proof; RTTI must match the concrete
object. This value-independent proof preserves the Discovery/AuthoredPackage
shape invariant. A successful custom load alone never proves that invariant.
Filtered/cooked contexts and unproven serializers retain strict restoration.

The context is consumed once in the synchronous validated-load callback while
the archive is alive. Its weak traversal lease expires on archive destruction or
chain mutation. Escaping the callback cannot preserve a reusable validation
capability. The manager rechecks metadata identities before normalizing and
atomically publishing; it exposes no trust/skip-validation flag or unchecked
path setter. The callback must not mutate values or reflection before consuming
the evidence. The Engine loader performs only deprecated-field bookkeeping there.

Restoration follows value application and Struct callbacks, before graph validation
and PostLoad. Later migration-owned changes remain authoritative. Failure, callback
rejection, cancellation and reload rollback discard candidate graphs and their
contexts. See [object loading](../Assets/Packages/ObjectLoading.md) for Engine
binding ownership, ordering and work counters.
