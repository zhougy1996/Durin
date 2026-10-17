# Toolset Contracts

Summary: Define owned tool values, bounded schema validation, provider interfaces, and module-protected invocation for structured callers.

Modules: Core

Last reviewed: 2026-10-17

## Ownership and Scope

`Tools/Tool.h` defines transport-independent contracts. Core has no
dependency on editor objects, UI, network transports, or MCP. Domain providers
implement `IToolset` and call their existing typed services. Core does
not own a tool catalog, editor execution queue, transaction policy, or
asynchronous scheduler. Existing `FConsoleCommandRegistry` behavior is unchanged.

## Values and Schemas

`FToolValue` is a deep-copy value tree with null, boolean, signed 64-bit
integer, unsigned 64-bit integer, double, string, array, and lexically ordered
object alternatives. It contains no document views or engine pointers.
`FToolSchema` owns child properties, an optional array item schema, scalar
enums, defaults, numeric bounds, and size bounds. Copying a schema deep-copies
its array item as well as properties/defaults. Treat published metadata as
immutable. Validate provider metadata before retaining another catalog copy.

The schema vocabulary is closed. Object fields not declared in Properties are
rejected; Required must contain unique declared field names. Arrays require
exactly one Items schema. Enums contain unique scalar values of the exact
declared type. Numeric bounds have that same exact type; no signed/unsigned or
integer/double coercion occurs. Doubles must be finite. Size bounds apply to
string bytes, array elements, or normalized object fields. Inapplicable fields,
inverted bounds, invalid type tags, inconsistent enums, and invalid defaults
are rejected rather than ignored.

`ValidateToolValue` performs schema-independent bounded preflight before a
transport or host copies a detached submission. Defaults must be complete valid values without adding further defaults.
`PrepareToolArguments` validates the descriptor, exact local tool name and
version, raw argument budget, and normalized arguments. Only absent input
fields receive defaults; explicit null is not absence. It returns a detached
request on success and leaves the caller's request unchanged on failure.
`ValidateToolOutput` requires complete output and never inserts defaults.

Default limits are depth 32, 4096 nodes, 1024 entries per container, 64 KiB per
string, and 1 MiB total string bytes including object keys. Root depth is zero;
configurations above depth 64 or with zero node capacity are rejected. Schema
metadata (including embedded values), raw arguments, and normalized arguments
have independent budgets. Repeated default expansion is charged every time.
Output data is checked under the same value limits. Adapters must additionally
bound serialized input before parsing and bound diagnostic/result retention.

Validation returns one stable error code, message, and RFC 6901 pointer. Empty
Path names the root; `/input` and `/output` identify descriptor schema roots.
Limits report LimitExceeded; bad input/output reports InvalidArguments or
InvalidOutput. Schema/descriptor and contract version failures remain distinct.
JSON and other text adapters perform checked, schema-directed numeric conversion
and preserve opaque identities as strings; they must not silently lose 64-bit
precision or return a parser-owned node view.

## Descriptors and Invocation

Toolset names contain one or more dot-separated segments; local tool names are
a single segment and cannot contain dots. Both are at most 256 bytes each.
`ValidateToolsetName` and `ValidateToolName` enforce the respective grammars.
`FToolAddress` keeps the two names separate; it does not split or concatenate
names to infer ownership. Each segment starts
with an ASCII lowercase letter and continues with lowercase letters, digits,
or underscores. Names are case-sensitive and never normalized. ContractVersion
is nonzero and independent of module load generation. Inputs have an object
root. Descriptors declare description, output schema, effect flags, retry
semantics, and explicit external exposure metadata. Exposure is not an access
grant; calling hosts apply their own admission policy.

The calling host constructs `FToolContext` with trusted invocation/session
identity, source, and cancellation token. Business arguments cannot replace
these fields. A `FToolCancellationSource` owns the cooperative signal; tokens
can only observe it. Cancellation does not stop code or roll back domain edits.

An optional `FToolClientContext` carries a detached client ID and cancellation
token in `FToolContext.Client`, separately from the individual invocation token.
EditorToolService admits it through native parameters, not tool arguments. Closing
a transport client can therefore cancel its domain-owned asynchronous jobs after
the original invocation has completed, without retaining transport objects.

`IToolset` describes one named `FToolsetDescriptor` containing a description
and local `FToolDescriptor` entries, checks availability without side effects,
and calls already normalized requests. `FToolRequest.Address` identifies
both the owning toolset and the local tool; its contract version belongs to the
individual tool. Different toolsets may use the same local tool name. Availability is advisory; execution
must revalidate live targets and state. Host dispatch validates successful
output against the descriptor. Provider exceptions are contained by the
modular-feature invocation boundary, not reported as successful domain results.

`FToolResult` distinguishes synchronous success and failure and carries
owned Data and diagnostics. Its default is failure. OperationId is reserved for
future domain-owned long work and implies no scheduler. Queued/Running/Completed/
Cancelled/Expired invocation states describe host submission, not another
provider execution protocol. The domain owns transactions and reports the actual
completion stage; dispatcher code must not promise automatic rollback or Undo.

## Provider Lifetime

Modules register through
`FModularFeatureRegistry::Get().RegisterFeature<IToolset>` and retain the
returned token. Reset it at a main-thread safe point before destroying the
provider. Enumerate through `InvokeAll<IToolset>` and retain only owned
descriptions and RegistrationIdentity, never the visitor reference or a callback.

`InvokeRegistered<IToolset>(Identity, Visitor)` routes to that exact
registered entry. Zero, stale, unregistered, and wrong-interface identities are
unavailable; a replacement is never substituted. Registry mutation and module
shutdown/unload are forbidden during dispatch. Nested read/invocation is allowed,
and dispatch scopes unwind on exceptions. All registry operations require the
module-control thread before GameThread initialization, then GameThread.

`GetPublicationRevision()` changes on registration and actual unregistration.
A catalog rebuilds when that global revision changes; unrelated features may
conservatively invalidate it. The catalog and provider visitation remain on
GameThread. External callers enqueue detached requests into the editor
service and read host-owned result snapshots instead of using this registry.

There is no feature retirement or wait protocol. Modules own producer shutdown,
worker synchronization, callbacks, retained storage, and code lifetime. Returning
from a visitor or unregistering a provider does not drain asynchronous work.
See [modular features and module shutdown](ModularFeaturesAndModuleRetirement.md).

## Explicit Native Authoring

`Tools/NativeToolset.h` provides immutable `FNativeToolset` groups implementing
`IToolset`. An `FToolBinding` combines an owned descriptor, a normalized-request
handler, and an optional advisory availability callback. Construction rejects
invalid metadata, duplicate local names, and missing handlers. Register the group
through the same typed feature registry and keep its token in the owning module.
The calling host prepares arguments once with `PrepareToolArguments` before
checking availability or invoking `Call`. `FNativeToolset::Call` checks ownership
and contract version, then forwards that same prepared request to the binding;
it does not validate, apply defaults, or copy the argument tree again. Direct
native callers have the same preparation precondition as editor hosts.
The host remains responsible for availability checks, exception containment,
output validation, and execution scheduling; direct calls do not replace that host.

`BindTool` explicitly binds a free/static function with signature
`R(const FToolContext&, Args...)`. It generates object input fields and the output
schema from supported C++ types. Names, descriptions, defaults, version, effects,
retry policy, and external exposure remain explicit authoring metadata. Input
arguments can be values or const references; mutable/output references and engine
pointers require explicit adapters. Void returns become null output.

Built-in `TToolValueCodec<T>` mappings cover bool, signed/unsigned integers up to
64 bits, float/double, string, and recursively typed vectors. Narrow integer and
float schemas carry representable bounds. Conversion follows validation without
numeric coercion. Additional owned types require a codec specialization supplying
Schema, Decode, and Encode, or a manually authored binding. Unsupported types fail
at compilation instead of falling back to strings.

```cpp
auto Add(const FToolContext&, int32 Left, int32 Right) -> int64
{
    return static_cast<int64>(Left) + Right;
}

auto Binding = BindTool(FToolDescriptor{.Name = "add"}, &Add,
    std::array{FToolParameter{"left"}, FToolParameter{"right", "", FToolValue{0}}});
FNativeToolset Math("math", "Arithmetic tools", {std::move(Binding)});
auto Registration = FModularFeatureRegistry::Get().RegisterFeature(Math);
// Submit FToolRequest{{"math", "add"}, 1, FToolValue::FObject{{"left", 42}}}
// through the editor tool service. Reset Registration before destroying Math.
```

This path uses explicit function bindings and C++ type deduction, not runtime
function reflection. It does not extend DHT or introduce DFunction metadata.
A future C++ reflection or Python signature adapter can produce the same owned
toolset descriptors and normalized-request handlers. No embedded Python runtime,
Python registration API, or automatic function discovery is implemented yet.
