# Property Reflection

Summary: Define reflected property authoring, registration, value access, containers, and serialized field compatibility.

Modules: CoreDObject

Last reviewed: 2026-10-16

For type ownership and registration, see [Reflection system](ReflectionSystem.md).
Struct value capabilities are defined by [Reflected struct operations](StructOperations.md).

## Property Metadata

`DPROPERTY()` supports the following value families, including fixed C arrays
where permitted by the family's authoring restrictions:

| Family | Reflected values |
| --- | --- |
| Scalar | `bool`, fixed-width signed/unsigned integers, `float`, `double`, `std::byte` |
| Text and identifiers | `std::string`, `FName`, `FGuid` |
| Enum and struct | Reflected enums and `DSTRUCT` values, including intrinsic math structs |
| Object reference | `DObject*`, `TObjectPtr<T>`, `TSoftObjectPtr<T>`, transient `TWeakObjectPtr<T>` |
| Container | `std::vector<T>`, `std::unordered_map<K, V>` |
| Opaque storage | Direct `FByteBuffer` and distinct `FEditorBulkData` fields |

`FProperty` stores field identity, flags, array dimension, byte offset, element
size, kind, and ownership. Typed properties additionally resolve the referenced
class, enum, struct, or nested element descriptors. `DObject/Property.h` owns
the common base and value access helpers; `DObject/DurinPropertyTypes.h` owns
concrete typed accessors. Byte, Blob, and BulkData use dedicated kind and value
operations rather than numeric or ordinary Array semantics. The resource and
persistence contract for BulkData belongs to [Package bulk data](BulkData.md).

`DPROPERTY(AlwaysSerialize)` marks a required wire field. Default-delta planning
emits its complete value even when equal to the paired default. This flag is
independent of editor presentation and persistent Forced overrides.
`Transient` still excludes a field from authored serialization.

### Typed Authoring Metadata

First-party property presentation and numeric authoring constraints are written
as ordinary `DPROPERTY` specifiers:

```cpp
DPROPERTY(Edit, DisplayName = "Exposure", ToolTip = "Camera exposure bias.",
    Category = "Camera", Units = "Unitless", Step = "0.1", Precision = 2,
    ClampMin = "-10", ClampMax = "10", UIMin = "-5", UIMax = "5")
float Exposure = 0.0f;
```

`DisplayName`, `ToolTip`, and `Category` accept non-empty quoted strings on a
reflected direct or fixed-array field. `Units`, `Step`, `Precision`,
`ClampMin`, `ClampMax`, `UIMin`, and `UIMax` require `Edit` and apply to scalar
integer/float properties and the components of the intrinsic float/double
vectors and quaternions. They do not apply to bool, string, enum, object,
container, matrix, or arbitrary struct properties. Numeric metadata on a
containing property never implicitly flows into Array/Map elements or nested
struct fields.

`Units` accepts `Unitless`, `Percent`, `Degrees`, `Radians`, `Seconds`,
`Milliseconds`, `Meters`, `Centimeters`, `Millimeters`, or `Kilometers`.
`Step` must be positive and finite. `Precision` is limited to 0..9 for float
values and 0..17 for double values. UI bounds must be ordered and remain inside
hard bounds when both are present. A decimal float spelling is converted once
to its declared float or double channel; integer spellings must be integral and
fit the exact declared width. Consequently full-range `uint64` metadata never
passes through `double`.

DHT rejects duplicate selected keys, malformed or non-finite numbers, invalid
units, inapplicable keys, and inconsistent ranges with a source-located
`DHT-META` diagnostic. A selected typed key may not also be supplied through
`MetaData`; unrelated extension metadata remains supported. Generated
`FPropertyMetadataParams` records keep signed, unsigned, float, and double
numbers in distinct channels. Registration validates hand-authored records and
copies them into immutable `FPropertyMetadata`. The three presentation strings
are also mirrored into raw metadata lookup for compatibility, while first-party
consumers use `GetTypedMetadata()`.

`ClampMin` and `ClampMax` constrain new editor-authored proposals through
`ValidatePropertyEditValue()`, which returns `std::expected<void, FPropertyEditValueError>` with
owned property routes, array bounds, and exact numeric channels. Formatting is
explicit through `ToString`. Validation happens on detached
draft storage before an edit session or transaction mutates the object. Package
loading deliberately does not call this property-edit validator and therefore does
not clamp or repair historical data. `UIMin` and `UIMax` only configure editor
presentation.

## Value Access And Inheritance

`DStructBase::GetChildProperties()` exposes only properties declared directly on that reflected type. Storage inheritance follows `GetInheritanceSuper()`; functions retain parent metadata identity without inheriting parameter storage. Layout and chain changes use checked APIs and are rejected after `Link()`. Use `FindPropertyByName(...)` or the property iteration helpers when inherited properties should be visible.

`FProperty::ContainerPtrToValuePtr<T>(...)` and `GetValuePtr(...)` address a
field within its owner. `FObjectProperty` reads and writes hard references
through `GetObjectPropertyValue(...)` and `SetObjectPropertyValue(...)`.
`FSoftObjectProperty::GetSoftObjectPtr(...)` exposes the bounded untyped wrapper;
use `GetPath()` and `SetPath(FObjectPath)` for authored identity.
`FWeakObjectProperty::GetWeakObjectPtr(...)` exposes its exact wrapper and expected
class. `FStringProperty` provides a `std::string*` helper.

Array and Map access uses the checked [container operations](#container-properties).
The `Resize`, `Insert`, and `RenameKey` convenience APIs return
`std::expected<void, FPropertyContainerError>` with backend codes, unavailable
lifecycle requirements, owned property/Struct identities, and bounds/counts.
Map keys remain immutable in place; edits copy, validate uniqueness, and rename
the node to preserve hashing and equality invariants.

## Soft And Weak References

DurinHeaderTool recognizes direct and fixed-array `TSoftObjectPtr<T>` fields and
soft values nested through supported Array, Map-value, and reflected-struct
paths. `T` must resolve to a reflected `DObject` subclass. Raw wrapper aliases,
unresolved or non-object targets, soft Map keys, and unsupported qualifiers are
rejected rather than emitted with incomplete metadata. `FSoftObjectProperty`
is a distinct property kind: serialization and editor access use its exact
authored `FObjectPath`, while GC deliberately excludes it from the strong-reference
schema. DHT also recognizes typed `TWeakObjectPtr<T>` in direct, fixed-array,
Array, Map-value, and reflected-Struct positions. The top-level property must
be explicitly `Transient`, and weak Map keys are invalid. Generated weak
properties retain exact wrapper lifecycle operations and the expected class;
they are non-owning runtime state and never acquire a soft path identity.

## Leaf Property Registration

Built-in leaf records use typed authoring descriptors while owner property
arrays remain `const FPropertyParamsBase* const*`. The common base is a
non-authorable dispatch header for field identity, flags, array dimension,
offset, fixed kind/layout discriminants, paired value accessors, metadata and
compatibility names, and optional native value operations. Runtime registration
validates the layout and common pairs before reading a family descriptor.

Bool, Byte, fixed-width signed/unsigned/floating-point Numerical, String, Name, and
Guid records are constrained `TPlainPropertyParams<TValue, Kind>` aliases. A
direct or nested generated record supplies field identity, flags, array dimension,
offset, and optional metadata; kind, layout, size, alignment, and lifecycle
callbacks are fixed by the alias. External intrinsic
registration uses the same constructor or `WithAccessors(...)` for a paired
accessor-backed field with offset zero.

Enum records add one `DEnum` resolver. The resolved descriptor is the authority
for the supported underlying kind and size. Object records use
`FObjectPropertyParams::Raw<T>(...)` or `ObjectPtr<T>(...)` and add one class
resolver. Those factories install exact `T*` or `TObjectPtr<T>` size,
alignment, value operations, and logical get/set callbacks. Consequently
`FObjectProperty` never interprets `TObjectPtr<T>` storage as `FObjectPtr`;
Archive, Engine, and GC still use `IsObjectPtrWrapper()` to preserve the
intentional raw-versus-strong-reference policy distinction.

Every built-in leaf property supports default construction, destruction, copy
construction, and copy assignment. Scalars and Enums use their exact fixed
type, String/Name/Guid use their C++ value type, raw Object defaults to null,
and ObjectPtr operations use the declared wrapper specialization. Fixed arrays
apply those operations per element through the property stride. Intentional
custom `None` registrations use `FGenericPropertyParams` and must explicitly
supply element/value size, alignment, and all four operations; those facts are
not part of the common base.

Generated leaf records do not emit element-size expressions. Plain aliases own
their exact C++ value type, Enum records resolve their underlying type, and
Object/SoftObject factories instantiate their declared storage type. Reflected
struct properties likewise emit no element-size expression because their
resolved `DStruct` owns size and alignment. This keeps ABI ownership with the
compiler-built descriptor and prevents synthetic parser declarations or host
STL contents from changing runtime property sizes.

## Struct Property Registration

Struct-property registration describes schema and field access, while the
referenced `DStruct` describes the value stored there. The typed parameter
record fixes the generated kind/layout pair to `Struct`; runtime construction
validates that pair, the resolver and descriptor, descriptor size/alignment,
the optional accessor pair, and the optional metadata pair before publishing
the property. `FStructProperty` retains the resolved descriptor and derives its
element stride from that descriptor rather than from generated C++ layout or
lifecycle callbacks.

DurinHeaderTool authors direct and nested records automatically. External
intrinsic registration uses the same contract: construct
`FStructPropertyParams` with name, flags, array dimension, byte offset, and a
resolver returning the referenced `DStruct`. Use
`FStructPropertyParams::WithAccessors(...)` only when the containing type must
expose paired mutable/const accessors instead of a byte offset; accessor-backed
records keep offset `0`. Optional metadata is the final pointer/count pair.
Neither form supplies value construction, destruction, copying, size, or
alignment. The intrinsic struct's own `FStructParams` and `FDStructOps` remain
the authority for those facts.

Transactional struct loading also supports an accessor-backed struct field.
It constructs a temporary offset-zero property using the same `DStruct`, loads
and repairs that detached value, then copy-assigns through the original field's
accessor. This keeps matrix-column loading atomic without pretending that a GLM
column has a portable byte offset.

## Byte And Blob Properties

`std::byte` is a distinct reflected one-byte leaf. It is not numeric: generic
property editing presents hexadecimal byte data and does not apply arithmetic,
range, or decimal controls. Fixed C arrays of `std::byte` retain fixed-array
shape.

A direct `FByteBuffer` is generated as one owned Blob property with logical
Archive kind `Bytes`; it is not an `FArrayProperty` and exposes only a read-only
byte-count summary in generic Details. `std::vector<std::byte>` is the private
implementation of `FByteBuffer`, not a second reflected spelling. Blob
construction, destruction, copy, exact equality, snapshots, duplication, and
Archive loading operate on the complete array. Blob values contain no object
references and cannot be Map keys or targets of indexed authored overrides.

`std::vector<uint8>` remains the existing numeric `Array<UInt8>` contract.
DurinHeaderTool does not infer Blob intent from a field name or size. Blob
properties admit only a direct field: nested Blob containers, custom
allocators, and Blob Map participation are rejected during generation.
`FByteView` and `FMutableByteView` are borrowed API types and are not reflected
properties; reflected byte storage must use `FByteBuffer`.

## Container Properties

DurinHeaderTool currently recognizes these standard library container spellings for `DPROPERTY()` fields:

- `std::vector<T>`
- `std::unordered_map<K, V>`

`std::vector<T>` creates an `FArrayProperty` whose `Inner` points at the generated property metadata for `T`.

`std::unordered_map<K, V>` creates an `FMapProperty` whose `KeyProp` and `ValueProp` describe the key and value types.

Generated Array and Map records are typed `FArrayPropertyParams` and
`FMapPropertyParams`. Each record owns the nested logical property descriptors
and resolves one immutable, process-lifetime `FArrayOps` or `FMapOps` table for
its concrete storage specialization. Generated source does not contain
property-local count, element, traversal, construction, insertion, or removal
functions. The current reusable adapters accept only default-form
`std::vector<T>` and `std::unordered_map<K, V>`; storage is a replaceable C++
backend and is not part of reflected type identity or serialized schema.

The operation tables advertise individual capabilities instead of
assuming every container can perform every operation. Consumers request and
check only what they need: count and const/mutable traversal, indexed Array
element access, resizing, Map lookup/insertion/removal/key rename, and detached
construction plus transactional commit. Missing capability slots return a
typed `EContainerOpResult`; a property is never treated as empty and a mutation
is never silently skipped. Map traversal is single-pass and callback-scoped.
There is no indexed Map API, no persistent iterator identity, and no permission
to retain entry pointers across structural mutation.

Array/Map tables also advertise optional `CopyConstruct` and `CopyAssign`
callbacks. Generated properties install these callbacks into the ordinary
`FProperty` value lifecycle, so `FReflectedValueStorage::CopyConstruct` works
for copyable containers, including nested containers. Copies own independent
container storage; object references retain their normal pointer semantics.
The standard adapters check nested element copyability instead of trusting a
container's declared copy constructor. Unsupported copies remain explicit
capability failures. Copy assignment builds a temporary and uses a nonthrowing
swap, preserving the destination if copying fails and supporting self-assignment.
The `FProperty` copy boundary reports thrown copy callbacks as failure; failed
construction leaves value storage non-live. The strong assignment guarantee
belongs to these container adapters, not arbitrary user-defined struct assignment.

Array/Map loading uses `FDetachedContainerStorage`: input is decoded into a
managed temporary container, counts are bounded before allocation, duplicate
logical Map keys are rejected, and the destination is committed only after all
nested values and post-deserialize work succeed. Failure leaves the destination
logically unchanged. GC, runtime Archive, snapshots, Engine, and editor
property access all use the same checked operations.

Containers may be nested up to `MAX_CONTAINER_PROPERTY_DEPTH` in DurinHeaderTool, currently 4. For example:

```cpp
DPROPERTY()
std::vector<std::vector<int32>> NestedScores;

DPROPERTY()
std::unordered_map<std::string, std::vector<DObject*>> Groups;
```

Only the top-level field uses `STRUCT_OFFSET`. Nested inner/key/value properties use offset `0` because they describe an element type, not a field within the reflected class. Struct descriptors at either level still resolve the same referenced `DStruct` and do not carry local value-operation thunks.

Container key restrictions are intentionally stricter than value restrictions. Map keys may be numeric, `bool`, `std::string`, `FName`, `FGuid`, reflected enum, or reflected struct values. Object references and containers cannot be keys. Array elements and Map values accept supported leaf, reference, struct, and nested-container kinds within their authoring restrictions and the depth limit.

Canonical writers do not persist `std::unordered_map` iteration or bucket
order. They build a logical token for each supported key and sort entries by
that token. The canonical key domain includes bool, integers, floating-point
values, reflected enums, strings, `FName`, `FGuid`, and complete ordinary
reflected structs composed recursively from supported fields. Object/container
keys and structs with hidden state or custom identity/serialization operations
cannot be canonicalized and fail before output. Ordinary streaming Archives
retain traversal order unless they explicitly advertise canonical Map output.

Unsupported `DPROPERTY()` types fail DHT with stable diagnostics. This includes
unknown templates, smart pointers, non-reflected object pointers, references,
containers nested deeper than the configured limit, and aliases whose meaning
is available only from a neutralized include. `std::vector<bool>`, custom vector
allocators, custom unordered-map hash/equality/allocator arguments, and
unsupported Map key kinds are rejected during generation rather than failing
later in generated C++.

Runtime nested metadata can be walked with `ForEachNestedProperty(...)`. The helper visits array inner properties and map key/value properties recursively. Nested properties also expose their containing property through `FProperty::GetOwnerProperty()`.

## Serialized Property Aliases

Load-local bindings reuse canonical schema identity and typed compatibility routes,
while container positions and consumption remain per object. Reflection metadata
must remain immutable during a load; replacement invalidates its scoped bindings.
Ownership and unregistered-metadata fallback are defined in
[Object Package Loading](../Assets/Packages/ObjectLoading.md#binding-ownership).

A durable reflected field may declare read-only compatibility names when its C++
member is renamed:

```cpp
DPROPERTY(LegacyNames = "OldExposure;ExposureValue")
float ExposureEV = 0.0f;
```

DHT requires an explicitly quoted semicolon-separated list of unqualified C++
identifiers. Within one declaring class or struct, an alias must differ from its
property's current name and must not collide with any current property name or
another property alias. Generated property descriptors carry the aliases as
first-class compatibility data, and runtime registration repeats the collision
checks for manually authored descriptors.

`FindPropertyByName(...)` remains current-name-only. Serialized readers use
`FindPropertyBySerializedName(...)`, scoped to the field's declaring class or
struct, and canonicalize a recognized alias to the property's current name at
the bytes-to-runtime boundary. Type signatures are still checked independently;
an alias permits a rename only and does not make a type change compatible.
Canonical saves, authored-override paths, compatibility catalogs, and reference
routes use the current property name. If one stored schema contains both a
current name and an alias that resolve to the same property, the schema is
rejected as ambiguous rather than applying two values in record order. Moving a
field between declaring types is not covered by property aliases.

## Deprecated Properties

Use a deprecated property when a stored field cannot be handled by a
`LegacyNames` rename because its logical type or meaning changed:

```cpp
DPROPERTY()
float Distance = 0.0f;

DPROPERTY(Deprecated)
int32 Distance_DEPRECATED = 0;
```

`Deprecated` is explicit; the `_DEPRECATED` suffix alone has no special
behavior. An explicitly deprecated field must use that suffix, cannot be
`Edit` or `Transient`. The historical stored name is inferred by removing the
suffix; deprecated-property annotations do not accept migration targets,
custom-version domains, or version bounds.

Routing is exact on declaring type, inferred historical name, and logical type
signature. It happens before a reused current name is considered, so an old
integer `Distance` loads into `Distance_DEPRECATED` while the current float
`Distance` remains untouched. The owning class performs semantic conversion in
`PostLoad`; reflected structs do so in `PostDeserialize`. Those callbacks can
use `WasDeprecatedPropertyLoaded` to distinguish a consumed old route from an
unloaded deprecated member holding its C++ default. Any rejection rolls back
the complete load.

If the stored and current fields have the same name and logical type, keep one
current property and perform any version-dependent in-place conversion in the
owning callback. A deprecated route is for a historical schema shape that can
be distinguished by reflection, not for every semantic version change.

Deprecated fields are load-only: current Archive saves, default deltas,
canonical schemas, Details panels, and current authored ledgers exclude them.
Migration code owns both value conversion and any authored-intent transfer. It
can call `FOverridableManager::OverrideProperty` for each current path whose explicit or forced
state must survive the resave; splits, merges, and conditional targets therefore
remain ordinary migration logic rather than reflection metadata. Compatibility
and canonical-resave reports expose each consumed route as remaining migration
debt. A route should be removed only after the supported content baseline
contains no matching evidence and the normal compatibility-policy gate has
approved retirement.

## Related Documentation

- [Reflection generation](../../Development/Tooling/ReflectionGeneration.md)
- [Serialization](Serialization.md)
- [Reflected property editing](../../Editor/Architecture/PropertyEditing/ReflectedPropertyEditing.md)
