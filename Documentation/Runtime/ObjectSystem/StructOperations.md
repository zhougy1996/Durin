# Reflected Struct Operations

Summary: Define struct value capabilities, detached storage, logical identity, type defaults, and hidden-reference collection.

Modules: CoreDObject

Last reviewed: 2026-10-16

`DSTRUCT()` opts a value type into metadata; it does not promise that generic
code can construct, copy, compare, serialize, repair, or trace that type. Each
`DStruct` instead owns one immutable, process-lifetime `FDStructOps` table. The
table advertises default construction, destruction (including the
separate trivially-destructible fact), copy construction, copy assignment, zero
construction, logical identity, runtime Archive serialization,
post-deserialize repair, hidden-reference collection, and completeness of the
authored reflected-field representation. `DStruct` exposes matching capability
queries and never changes the table after registration.

Generated structs register `GetDStructOps<T>()`; externally described intrinsic
structs use the same path. `TDStructOpsTraits<T>` derives safe mechanical
defaults from the C++ type and lets an audited specialization disable or replace
an operation or enable an optional callback. The specialization is
compile-checked against the callback signature before its operation is emitted.
Current non-mechanical specializations provide deterministic mathematical
defaults for vector, quaternion, matrix, and color structs.

Consequently, seeing a reflected struct as a direct property, fixed array
element, vector element, or map key/value never requests a C++ operation from
the property declaration itself. Construction, destruction, copying,
alignment, identity, serialization, repair, and hidden-reference traversal are
all capability-checked through the referenced `DStruct` and its `FDStructOps`.

## Detached Value Storage

Construction callbacks require aligned uninitialized storage; copy assignment
requires a live destination. `FReflectedValueStorage` is the common aligned RAII
owner used for detached reflected values and tracks whether exactly one value is
live. It destroys only a successfully constructed value. Generic storage,
container mutation, editor drafts, Archive loading, and authored struct loading
preflight the operations they require before allocation or mutation. An
unsupported request returns `DStructOperationUnavailable` with the operation
and qualified struct name.

## Logical Identity And Type Defaults

Default struct identity recursively compares every non-`Transient` reflected
field. Arrays are ordered, maps compare key/value associations rather than
iteration order, object values use pointer identity, and `float`/`double`
compare complete bit patterns. Thus signed zero remains distinct and NaNs are
identical only when sign, exponent, and payload bits match. A declared
`Identical` callback is authoritative for the complete struct value and is not
combined with a field walk.

Eligible `DStruct` values also publish one immutable deterministic type default
during the registration batch. Eligibility requires complete authored fields,
deterministic default construction and destruction, and no custom Archive
serializer; unavailable structs retain a stable reason rather than a partial
value. Publication is atomic, aligned storage is destroyed exactly once, hidden
strong references are rooted through the struct's collection contract, and
module teardown releases defaults before their callbacks can unload. Explicit
Struct values compare against this type default, never against nested memory in
a containing class default.

`ComparePropertyValues(...)` returns `Identical`, `Different`, or `Unsupported`
with a stable logical path, kind, and reason. Unsupported operations are not an
ordinary difference and cannot authorize omission. The comparison grammar is
recursive and bounded to depth 64: fixed arrays and Arrays are positional, Maps
use canonical logical key tokens and compare key/value associations independent
of iteration order, hard references may use a supplied default-object graph,
soft references compare canonical paths, and floating-point values compare
their complete bits. Descriptor cycles, missing container operations,
incomplete/custom Struct semantics, duplicate Map keys, excessive counts, and
malformed values fail with a typed diagnostic.

## Hidden References

Reflected GC traversal visits the compiled schema first and then invokes one
optional `CollectReferences` callback for strong references outside reflected
fields. Callback authors must not repeat reflected references. Detached
property snapshots root the deduplicated union of schema and callback
references and compare materialized values through the same logical-identity
rules.

## Related Documentation

- [Reflection system](ReflectionSystem.md)
- [Property reflection](PropertyReflection.md)
- [Serialization](Serialization.md)
- [Garbage collection](GarbageCollection.md)
