# Native Function Reflection

Summary: Define generated native function descriptors, inheritance, and unified object event dispatch.

Modules: CoreDObject

Last reviewed: 2026-10-16

## Authoring And Scope

`DFUNCTION()` on an ordinary method in a reflected `DCLASS` registers a native
callable. Its class requires `GENERATED_BODY()`. Concrete, non-static methods
support named parameters, const qualification, and value returns. Values may be
`bool`, fixed-width `int8` through `uint64`, `float`, `double`, `std::string`,
`FName`, or `FGuid`, reflected object pointers (`T*` and `TObjectPtr<T>`),
reflected `DENUM` values, `TSoftObjectPtr<T>`, `TWeakObjectPtr<T>`,
reflected `DSTRUCT` values, intrinsic math structs, `std::vector<T>`, and
`std::unordered_map<K, V>`. Containers may nest up to four levels, including
objects and structs as elements or mapped values. Map keys use supported scalar,
string, name, GUID, or reflected enum types. Soft/weak references may be elements
or mapped values, but cannot be map keys. Containers use the default allocator/hash/equality
forms; `std::vector<bool>` is unsupported. Ordinary `int`, `unsigned int`, short,
and signed/unsigned char
spellings map to the corresponding supported fixed-width type. Public,
protected, private, virtual, `final`, and `noexcept` methods are supported.
Parameters pass by value, `const T&` (input), or `T&` (input/output); returns
are by value or void. Object references also support `T*&` and `T* const&`;
const pointees are unsupported. Default arguments, non-object pointers, rvalue
references, reference returns, C arrays, unreflected enums/structs,
untyped `FSoftObjectPtr`/`FWeakObjectPtr`, `TStrongObjectPtr`, method ref qualifiers,
pure-virtual annotations, templates, variadics, declaring-class overloads,
free/static functions, and `DSTRUCT` methods are rejected. The definition must
link normally. Reflected parameter storage does not evaluate C++ default arguments.

```cpp
DFUNCTION(CallInEditor, Category = "Light", DisplayName = "Normalize Settings",
    ToolTip = "Clamp authored light settings to supported values.")
auto NormalizeSettings() -> void;
```

`CallInEditor` opts into the
[Details action adapter](../../Editor/Architecture/EditorFunctionActions.md).
Only void/no-argument signatures are eligible for that adapter, including explicit
layout placement and execution admission. Parameterized or returning functions
remain reflected even when marked `CallInEditor`, but do not produce buttons.
It grants no scripting, remote, console, or network exposure, does not remove
the declaration from game builds, and adds no editor module dependency. Typed
presentation metadata comprises `Category`, `DisplayName`, and `ToolTip` with
quoted strings. Category defaults to `Actions`; omitted display names use
`StringUtils::HumanizeName`. Unknown/duplicate specifiers fail generation.
Metadata uses flat macro arguments.

## Ownership And Invocation

`DFunction : DStructBase` is intrinsic reflected metadata. Its declaring `DClass`
keeps local functions reachable through GC; registered functions have stable
addresses and the class as their Outer. `DFunction` is the sole descriptor type.
`GetOuterDClass`, `GetReturnProperty`, `GetSuperFunction`,
`GetFunctionFlags`, `GetNativeFunc`, and `Invoke` expose the native function model.

`DStructBase` owns root property trees for classes, structs, and functions.
Function parameter/return properties belong to the function's local property chain,
never the declaring class's member chain. `Parm`, `ReferenceParm`, `ConstParm`,
`OutParm`, and `ReturnParm` describe their roles. This property chain is the sole
runtime signature. Virtual `DStructBase::Link` / `DFunction::Link` derives borrowed
input indexes and return metadata, validating roles, native types, bounds, alignment
and overlap before sealing the function layout. Parameter
buffers and frames bind directly to DFunction; generated aggregate lifecycle
callbacks operate on the complete native aggregate.
Each override has a complete local layout. `SuperStructBase` stores parent
identity and `GetSuperFunction` reads that identity. `GetInheritanceSuper` returns
null for functions; property enumeration, value iteration and GC schema assembly
therefore never add a second copy of the parent's parameters. Function properties do not create class
defaults or become serialized members of target instances.

`Native`, `Virtual`, `Final`, and `Const` flags are independent
of editor exposure. Signature compatibility compares value kinds, passing modes,
return type, and method constness; names and presentation do not participate.
Composite compatibility recursively compares element/key/value schemas, exact
reflected struct/enum identity, object target class, and raw versus `TObjectPtr`,
soft, or weak storage. Enum signedness and width come from the target compiler;
two different enums with the same underlying integer type remain distinct.
Identical size/alignment alone does not make two composite types compatible.
Virtual/final override policy is checked separately.

`DurinCodeGen::FClassParams` accepts a trailing function pointer/count pair.
Generated statics helpers make type-correct casts and ordinary member calls;
the existing generated friend grants access to non-public methods. The generated
native ABI is `void(DObject&, FFunctionFrame&, void* Result)`. DHT emits a C++
aggregate with value storage for each parameter and, when present, a return field.
The target compiler supplies actual size, alignment, and offsets. Generated
member-function traits assert the extracted parameter types/count, return type,
and constness against the actual method declaration, preventing implicit native
conversions from hiding an extraction mismatch. Thunks read typed slots from the
frame and assign the returned value into already-initialized result storage.
Calls preserve ordinary C++ virtual dispatch and non-public generated access.

`RegisterFunctions` installs a local pointer list exactly once, including an empty
list. `ConstructDFunction` uses `FObjectInitializer` and builds owned properties
under RAII before publishing to the object registry. Derived parameter metadata
is finalized during linking, after construction and recursive type resolution. Generated
`FFunctionParams` point directly to property/layout records; no signature factory
callback constructs a separate descriptor graph.
Recursive property type resolvers can obtain an in-progress class identity;
the class construction guard prevents a second metadata installation or early
function linking during that recursion.
After the registration batch installs all class metadata, `LinkFunctions` seals
function layouts base-first before class default objects are constructed. It validates
native descriptors, signature compatibility, virtual overrides, and reflected
final declarations regardless of descriptor registration order. Errors leave
the failing class unlinked; successfully linked ancestors remain sealed. Linking
does not call generated parent construct helpers. Layout linking and class
admission are separate: a successful function Link seals its layout, while
ProcessEvent requires the complete owning-class override batch to pass. Failed
layout validation leaves fields mutable and clears partial derived metadata;
callers may repair the layout and retry. A declaring-class parent change after
function Link is rejected during class linking. Valid layouts remain sealed even if
another function or override causes the class batch to fail. No further registration is
allowed on a linked class. Manually registered classes link on their first lookup
or enumeration; registration and that first query must occur on the registration
thread, after ancestor metadata is installed.

`GetLinkDiagnostic` on the function and `GetFunctionLinkDiagnostic` on the class
retain the latest link failure: error code, originating class/function, property
path and reason. Nested paths use `.Inner`, `.Key` and `.Value`. Class linking
and automatic registration propagate that diagnostic, including failures in an
ancestor, instead of reducing the failure to an integer code. A successful retry
clears it. Signature comparison and override linking share the same comparison
implementation; names remain excluded from compatibility.

`DObject::ProcessEvent(const DFunction*, FMutableByteView)` is the single public
reflection dispatch entry. It checks target and descriptor liveness,
class compatibility, linked metadata, the validated signature, parameter size/alignment,
and native-entry availability before constructing a frame. There is no separate
no-argument overload or `InvokeVoidNoArgs` wrapper; callers pass an explicit empty
view for void/no-argument functions. A null descriptor returns `InvalidFunction`,
incorrect storage size/alignment returns `InvalidParameters`, and a function not installed by successful class linking returns
`UnlinkedFunction`, even if its owner already linked other functions.

```cpp
const auto* Function = Object->GetClass()->FindFunctionByName("NormalizeSettings");
const auto Error = Object->ProcessEvent(Function, {});
```

The virtual object entry permits interception/delegation. Its base implementation
dispatches the supplied descriptor; callers select an effective override through
class lookup. The single `Invoke(Target, Frame, Result)` native entry requires an
admitted frame matching the target and function. Editor actions use ProcessEvent
and the same virtual dispatch boundary. `FFunctionFrame` borrows
the target, function, and parameter view and links to the previous admitted frame
on the current thread. `GetCurrentFrame()` is restored on return or exception
unwinding. Frames and borrowed storage must not escape synchronous dispatch.

`EFunctionInvokeError::None` means dispatch returned successfully (the base
implementation returns it after the native call);
it does not convey domain success from a `void` body. Runtime invocation opens
no transactions and applies no editor exposure policy. Native exceptions are
not a recoverable action result or rollback mechanism.

## Parameter Storage

Include `DObject/FunctionParameters.h` to use `FFunctionParameterBuffer`. It
allocates aligned aggregate storage, runs its generated constructor, and destroys
the aggregate at scope exit, including after native exception unwinding.
Strings and other nontrivial values are constructed values, never zeroed or
copied as raw bytes. Typed access checks the reflected kind, size, and alignment;
using the wrong type or index is a programmer-contract failure.
Structs and containers require native default construction, destruction, copy
construction, and assignment. Aggregate lifetimes and ordinary C++ member calls
use those native operations; custom struct archive/default-publication operations
do not replace native call semantics. Array/map property trees own their nested
property metadata and expose the existing container operations for introspection.

```cpp
// DFUNCTION() auto Combine(int32 Value, const std::string& Prefix,
//     std::string& Output) const -> std::string;
const auto* Function = Object->GetClass()->FindFunctionByName("Combine");
FFunctionParameterBuffer Parameters(*Function);
Parameters.GetParameter<int32>(0) = 42;
Parameters.GetParameter<std::string>(1) = "value=";
const auto Error = Object->ProcessEvent(Function, Parameters.GetView());
if (Error == EFunctionInvokeError::None)
{
    const auto& Output = Parameters.GetParameter<std::string>(2);
    const auto& ReturnValue = Parameters.GetReturnValue<std::string>();
}
```

Input/output references bind to the buffer's value slots; callers read or copy
back their final values after dispatch. Return values share the same aggregate,
with the native result pointer taken from the return property's offset. A return-only
function therefore still requires parameter storage. A void/no-argument buffer
is an empty view and requires no allocation.

`ProcessEvent` also accepts borrowed, already-initialized compiler aggregate
storage with exactly the selected descriptor's size, alignment, offsets, and
value types. It does not construct, destroy, or reinterpret the caller's value
lifetimes. Signature compatibility is semantic; it does not convert arbitrary
physical layouts. Buffer storage and descriptor/module lifetime must cover the
entire synchronous call. Function linking validates supported field kinds,
lifecycle, bounds, alignment, and non-overlap and caches that result; dispatch
does not rebuild or revalidate immutable layout metadata on each call. Owned
parameter buffer construction and descriptor compatibility reuse this cached
result as well. Linking rejects invalid layouts and missing native entries;
dispatch relies on that successful link instead of repeating those checks.

Function runtime implementation lives in `Private/DObject/Function.cpp`;
`DClass` retains registration, lookup, inheritance linking, and GC ownership edges.
`StructBase.h` separates metadata field/layout foundations from class/function
headers; `FunctionTypes.h` contains the invocation declarations without an object
include cycle. Internal `TNativePropertyTraits<T>` supplies type identity to typed
access and generated record helpers. `TNativePropertyParams<T>` owns nested
`FPropertyParamsBase`-derived records; both ordinary member registration and
`ConstructDFunction` use `ConstructProperties` to construct and attach records
in declaration order. It calls `ConstructProperty` for concrete field construction,
validation, metadata and value operations; AddOwnedProperty also attaches the root
through AddCppProperty, so ownership and chain installation cannot drift apart. The shared constructor explicitly
owns child trees with typed deleters and destroys parents before children.
`FField::GetOwnerVariant` exposes the non-owning owner; attaching an existing tree to a different metadata
scope is rejected instead of silently changing its logical owner. Handwritten
registration uses the same property/layout records and `ConstructDFunction` as
generated code. No detached signature or manual compatibility factories remain.

`ValidateNativeProperty` shares concrete field/native storage checks between
generated property construction and function linking: referenced types, native
size/alignment, container operations and required value operations. Construction
still validates record layout and resolvers before interpreting records or
creating fields. Function linking adds parameter-only restrictions, passing roles,
nesting limits and aggregate bounds/alignment/overlap. Ordinary fields retain
their supported generic, blob, bulk-data and accessor layouts; provisional struct
registration does not require native operations to be complete at construction.

Layout and chain storage are private to DStructBase. Checked layout, super and
chain APIs reject modification after Link. FField owner/next setters and property
flag, value-operation and container-schema setters check the owning metadata,
including nested fields. Borrowed native fixture chains use SetChildProperties
and retain caller-managed lifetime; generated trees use shared ownership.
Other class/struct registration and provisional-schema reassembly retain their
existing lifecycle unless explicitly linked. Parameter buffers require a linked
layout; callers should normally obtain functions through linked class lookup.

`ArePropertyTypesSame` provides strict recursive native property type identity,
including referenced descriptors and object pointer storage. Field names and
offsets do not participate. Snapshot compatibility retains its separate
name-based rules for restoring values across descriptor recreation.

Owned buffers are game-thread scopes and register their initialized slots with
the GC root pass until destruction. Raw object and `TObjectPtr` function slots
(including container elements and return slots) retain their referenced objects.
Struct values follow their existing reflected reference schema and custom
reference collector: `TObjectPtr` fields retain objects, while ordinary raw
observer fields keep their established non-owning behavior. Clearing a slot or
destroying its buffer releases this temporary reachability; object destruction
still occurs through the normal GC safe point. Explicit garbage marking remains
authoritative.

Function Link compiles one immutable reference schema from its local parameters
and return property. Every owned buffer reuses it; collection traverses values
without rebuilding the property operation tree. This schema is separate from the
ordinary member schema because raw object parameter slots are strong. Cached
struct operations consult the struct's current member schema and native collector,
including when the member schema was empty or provisional at function linking.
Later struct registration therefore cannot leave a permanently pruned parameter
subtree. Weak/soft leaves emit no operation. Failed layout validation publishes
neither parameter indexes nor a parameter reference schema.

`ProcessEvent` runs on the game thread, defers physical GC throughout the native call, and restores the
deferral state on exceptions. This protects borrowed aggregates, the target,
native by-value copies, and temporary return values while C++ executes. A GC
request made inside a function remains pending for a later safe point. Borrowed
storage is not registered after return; retain its objects through an owned
buffer or another normal strong owner before allowing collection. Neither
parameter storage nor generated descriptors confer permanent object ownership.

Soft/weak slots preserve the [non-owning pointer semantics used by UE](https://dev.epicgames.com/documentation/en-us/unreal-engine/object-pointers-in-unreal-engine).
They never become GC roots, including inside nested containers or returned values.
Copying a soft reference preserves its authored path and weak loaded cache;
`ProcessEvent` never loads a resource implicitly. Null, pending, valid, and stale
states follow `TSoftObjectPtr`. Weak references preserve object identity/serial
keys and resolve to null after collection; copying an already-stale value is
valid. Passing by `const T&`, by value, or through an input/output reference does
not promote either wrapper to a strong owner. Code retaining a resolved object
across a later GC safe point must use an ordinary strong owner.

Include `DObject/NativeEnum.h` for `StaticEnum<T>()`. Generated enum sources
register a native RTTI identity and metadata resolver, then unregister that
resolver when their module retires. Lookup works for scoped and unscoped reflected
enums without injecting incomplete enum declarations into authored headers.
The mapping is process-local, is never serialized, and follows module lifetime;
an unreflected or retired native enum has no resolver and returns null. Runtime
metadata construction and lookup use the normal reflection registration thread.

## Inheritance And Lookup

`FindFunctionByName` returns the nearest local descriptor, optionally searching
ancestors. `ForEachFunction` returns one effective descriptor per reflected slot,
ordered by root declaring qualified class name, then method name. Excluding
ancestors enumerates local descriptors only. A slot's session identity is its
root declaring qualified class name and method name; `GetRootFunction()` returns
that root descriptor.

Linking binds each local descriptor's `GetSuperFunction()` to the nearest
reflected ancestor descriptor and builds the effective name index and ordered
enumeration list once. Parent/root access requires linked metadata. Derived-before-base
registration within a module batch preserves the same slot identity and receives
the same compatibility checks. Queries reuse the linked index/list instead of
rebuilding inheritance, duplicate suppression, and ordering on each editor draw.

A base action is inherited without another annotation. Unannotated native
virtual overrides preserve the slot and metadata and execute by ordinary virtual
dispatch. Generated calls never explicitly qualify the base implementation.
Annotated compatible virtual overrides replace the effective descriptor; their
annotation is complete, so omitted presentation uses defaults and omitting
`CallInEditor` removes the derived button. Non-virtual same-name hiding and
overloads against a reflected ancestor name are rejected in configured reflected
headers, including unannotated hiding. Diagnostics identify both locations.

Descriptors and executable invokers follow the owning class/module lifetime.
Consumers must not retain them after module retirement. Function identities are
session-only, never serialized. Persistent bindings, reload, scripting, and RPC
require separate contracts.

This design follows UE's [UFunction parameter layout](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/CoreUObject/UFunction)
and typed property descriptors, while using Durin's existing C++ containers,
struct metadata, and game-thread GC contracts.

## Related Documentation

- [Reflection generation](../../Development/Tooling/ReflectionGeneration.md)
- [Reflection system](ReflectionSystem.md)
- [Property reflection](PropertyReflection.md)
- [Reflected struct operations](StructOperations.md)
